/*
 * Copyright (C) 2015 GORMAKE project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scons_scanner.h"
#include "build_engine_base.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

namespace gormake {

namespace fs = std::filesystem;

// --- Static helpers ---

static std::string Trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && isspace(static_cast<unsigned char>(s[start])))
    start++;
  size_t end = s.size();
  while (end > start && isspace(static_cast<unsigned char>(s[end - 1])))
    end--;
  return s.substr(start, end - start);
}

static std::string StripComment(const std::string& line) {
  // SCons files use Python syntax: # comments
  std::string result;
  bool in_string = false;
  char string_char = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_string) {
      result += c;
      if (c == string_char && (i == 0 || line[i - 1] != '\\'))
        in_string = false;
    } else {
      if (c == '#') break;
      if (c == '"' || c == '\'') {
        in_string = true;
        string_char = c;
      }
      result += c;
    }
  }
  return result;
}

// --- SconScanner implementation ---

SconScanner::SconScanner() {}
SconScanner::~SconScanner() {}

bool SconScanner::ScanFile(const std::string& path) {
  // Avoid re-scanning the same file (prevents infinite recursion)
  if (visited_files_.count(path) > 0) return true;
  visited_files_.insert(path);

  std::ifstream file(path);
  if (!file.is_open()) return false;

  current_path_ = path;
  current_src_dir_ = buildutil::DirName(path);

  std::string line;
  while (std::getline(file, line)) {
    ProcessLine(line);
  }

  return true;
}

void SconScanner::ScanDirectory(const std::string& dir_path) {
  for (auto& entry : fs::recursive_directory_iterator(dir_path)) {
    std::string filename = entry.path().filename().string();
    if (filename == "SConstruct" || filename == "SConscript" ||
        filename == "SConscript.*") {
      std::string entry_str = entry.path().string();
      if (entry_str.find("/out/") != std::string::npos) continue;
      if (entry_str.find("/build/") != std::string::npos) continue;
      if (entry_str.find("/.git/") != std::string::npos) continue;
      try {
        ScanFile(entry_str);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "gor_make: [warning] error parsing %s: %s\n",
                     entry_str.c_str(), e.what());
      } catch (...) {
        std::fprintf(stderr, "gor_make: [warning] unknown error parsing %s\n",
                     entry_str.c_str());
      }
    }
  }
}

const std::vector<SconTarget>& SconScanner::GetTargets() const {
  return targets_;
}

std::string SconScanner::ExtractFuncName(const std::string& s) const {
  // Extract function name from patterns like:
  //   Source('x.cc') -> "Source"
  //   env.Library('lib', [...]) -> "Library"
  //   fpenv.SharedObject('fp64.c') -> "SharedObject"

  std::string trimmed = Trim(s);

  // Skip env. prefix
  size_t dot = trimmed.find('.');
  if (dot != std::string::npos) {
    // Check if it's env.Method() pattern
    std::string prefix = trimmed.substr(0, dot);
    // Only skip known env variable prefixes
    if (prefix == "env" || prefix == "fpenv" || prefix.find("env") == 0) {
      trimmed = trimmed.substr(dot + 1);
    }
  }

  // Extract the identifier
  std::string func;
  for (char c : trimmed) {
    if (isalnum(static_cast<unsigned char>(c)) || c == '_') {
      func += c;
    } else {
      break;
    }
  }
  return func;
}

std::vector<std::string> SconScanner::ExtractStringArgs(const std::string& s) const {
  // Extract quoted string arguments from a function call
  std::vector<std::string> result;
  bool in_string = false;
  char string_char = 0;
  std::string current;

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (!in_string) {
      if (c == '"' || c == '\'') {
        in_string = true;
        string_char = c;
        current.clear();
      }
    } else {
      if (c == string_char && (i == 0 || s[i - 1] != '\\')) {
        in_string = false;
        result.push_back(current);
        current.clear();
      } else {
        current += c;
      }
    }
  }
  return result;
}

std::vector<std::string> SconScanner::ExtractList(const std::string& s) const {
  // Extract elements from a list like ['a', 'b', 'c']
  return ExtractStringArgs(s);
}

bool SconScanner::MatchFunc(const std::string& line,
                              const std::string& func_name,
                              std::string* args) const {
  // Match patterns: FuncName(...), env.FuncName(...), envx.FuncName(...)
  std::string trimmed = Trim(StripComment(line));
  if (trimmed.empty()) return false;

  // Find the function name
  std::string func = ExtractFuncName(trimmed);
  if (func != func_name) return false;

  // Find the opening paren after the function name
  size_t paren_pos = trimmed.find('(');
  if (paren_pos == std::string::npos) return false;

  // Find matching closing paren
  int depth = 0;
  size_t end_pos = paren_pos;
  for (size_t i = paren_pos; i < trimmed.size(); ++i) {
    if (trimmed[i] == '(') depth++;
    else if (trimmed[i] == ')') {
      depth--;
      if (depth == 0) {
        end_pos = i;
        break;
      }
    }
  }

  *args = trimmed.substr(paren_pos + 1, end_pos - paren_pos - 1);
  return true;
}

void SconScanner::ProcessLine(const std::string& raw_line) {
  std::string line = StripComment(raw_line);
  std::string trimmed = Trim(line);
  if (trimmed.empty()) return;

  // Skip comments and imports
  if (trimmed[0] == '#') return;
  if (trimmed.substr(0, 6) == "Import") return;
  if (trimmed.substr(0, 6) == "Export") return;
  if (trimmed.substr(0, 6) == "from " || trimmed.substr(0, 7) == "import ") return;
  if (trimmed[0] == '@') return;  // decorators

  // Skip Python control structures
  if (trimmed.substr(0, 3) == "if " || trimmed.substr(0, 4) == "elif" ||
      trimmed.substr(0, 4) == "else" || trimmed.substr(0, 3) == "for" ||
      trimmed.substr(0, 5) == "while" || trimmed.substr(0, 3) == "try" ||
      trimmed.substr(0, 6) == "return" || trimmed == "pass" ||
      trimmed[0] == '{' || trimmed[0] == '}') {
    return;
  }

  std::string args;

  // Source('file.cc', ...) — adds a source file to the current module
  if (MatchFunc(trimmed, "Source", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      // Add to a "source" target
      SconTarget t;
      t.name = string_args[0];
      t.type = "source";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      t.srcs.push_back(string_args[0]);
      targets_.push_back(t);
    }
    return;
  }

  // GTest('name', 'test.cc', ...) — test target
  if (MatchFunc(trimmed, "GTest", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "gtest";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      for (size_t i = 1; i < string_args.size(); ++i) {
        t.srcs.push_back(string_args[i]);
      }
      targets_.push_back(t);
    }
    return;
  }

  // SimObject('file.py', ...) — Python sim object definition
  if (MatchFunc(trimmed, "SimObject", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "simobject";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      targets_.push_back(t);
    }
    return;
  }

  // env.Library('name', [...sources...]) or env.Library(target='name', source=[...])
  if (MatchFunc(trimmed, "Library", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "library";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      // Sources may be in a list or individual strings
      for (size_t i = 1; i < string_args.size(); ++i) {
        if (buildutil::IsCppSource(string_args[i]) || buildutil::IsCSource(string_args[i])) {
          t.srcs.push_back(string_args[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libs_pos = args.find("LIBS");
      if (libs_pos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eq_pos = args.find('=', libs_pos);
        if (eq_pos != std::string::npos) {
          std::string libs_part = args.substr(eq_pos + 1);
          auto libs = ExtractStringArgs(libs_part);
          for (const auto& lib : libs) {
            t.link_libs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.SharedLibrary('name', [...sources...])
  if (MatchFunc(trimmed, "SharedLibrary", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "shared_library";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      for (size_t i = 1; i < string_args.size(); ++i) {
        if (buildutil::IsCppSource(string_args[i]) || buildutil::IsCSource(string_args[i])) {
          t.srcs.push_back(string_args[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libs_pos = args.find("LIBS");
      if (libs_pos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eq_pos = args.find('=', libs_pos);
        if (eq_pos != std::string::npos) {
          std::string libs_part = args.substr(eq_pos + 1);
          auto libs = ExtractStringArgs(libs_part);
          for (const auto& lib : libs) {
            t.link_libs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.StaticLibrary('name', [...sources...])
  if (MatchFunc(trimmed, "StaticLibrary", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "library";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      for (size_t i = 1; i < string_args.size(); ++i) {
        if (buildutil::IsCppSource(string_args[i]) || buildutil::IsCSource(string_args[i])) {
          t.srcs.push_back(string_args[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libs_pos = args.find("LIBS");
      if (libs_pos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eq_pos = args.find('=', libs_pos);
        if (eq_pos != std::string::npos) {
          std::string libs_part = args.substr(eq_pos + 1);
          auto libs = ExtractStringArgs(libs_part);
          for (const auto& lib : libs) {
            t.link_libs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.Program('name', [...sources...]) — executable
  if (MatchFunc(trimmed, "Program", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "program";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      for (size_t i = 1; i < string_args.size(); ++i) {
        if (buildutil::IsCppSource(string_args[i]) || buildutil::IsCSource(string_args[i])) {
          t.srcs.push_back(string_args[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libs_pos = args.find("LIBS");
      if (libs_pos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eq_pos = args.find('=', libs_pos);
        if (eq_pos != std::string::npos) {
          std::string libs_part = args.substr(eq_pos + 1);
          auto libs = ExtractStringArgs(libs_part);
          for (const auto& lib : libs) {
            t.link_libs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.SharedObject('file.c') — compile to .o
  if (MatchFunc(trimmed, "SharedObject", &args)) {
    auto string_args = ExtractStringArgs(args);
    for (const auto& s : string_args) {
      if (buildutil::IsCppSource(s) || buildutil::IsCSource(s)) {
        SconTarget t;
        t.name = s;
        t.type = "object";
        t.src_dir = current_src_dir_;
        t.path = current_path_;
        t.srcs.push_back(s);
        targets_.push_back(t);
      }
    }
    return;
  }

  // env.Object('file.cc') — compile to .o
  if (MatchFunc(trimmed, "Object", &args)) {
    auto string_args = ExtractStringArgs(args);
    for (const auto& s : string_args) {
      if (buildutil::IsCppSource(s) || buildutil::IsCSource(s)) {
        SconTarget t;
        t.name = s;
        t.type = "object";
        t.src_dir = current_src_dir_;
        t.path = current_path_;
        t.srcs.push_back(s);
        targets_.push_back(t);
      }
    }
    return;
  }

  // env.Append(CCFLAGS=[...]) — add compiler flags
  if (MatchFunc(trimmed, "Append", &args)) {
    // Parse CCFLAGS, CPPPATH, LIBS, CPPDEFINES
    if (args.find("CCFLAGS") != std::string::npos) {
      auto flags = ExtractList(args);
      for (const auto& f : flags) {
        if (f[0] == '-') env_cflags_.push_back(f);
      }
    }
    if (args.find("CPPDEFINES") != std::string::npos) {
      auto defs = ExtractList(args);
      for (const auto& d : defs) {
        env_defines_.push_back(d);
      }
    }
    if (args.find("LIBS") != std::string::npos) {
      auto libs = ExtractList(args);
      for (const auto& l : libs) {
        env_libs_.push_back(l);
      }
    }
    return;
  }

  // env.Prepend(CPPPATH=Dir('./include')) — include directories
  if (MatchFunc(trimmed, "Prepend", &args)) {
    if (args.find("CPPPATH") != std::string::npos) {
      auto dirs = ExtractList(args);
      for (const auto& d : dirs) {
        env_include_dirs_.push_back(d);
      }
    }
    return;
  }

  // SConscript('path/SConscript') — recursive include
  if (MatchFunc(trimmed, "SConscript", &args)) {
    auto string_args = ExtractStringArgs(args);
    for (const auto& s : string_args) {
      // Resolve relative to current dir
      std::string script_path;
      if (s[0] == '/') {
        script_path = s;
      } else {
        script_path = current_src_dir_ + "/" + s;
      }
      if (buildutil::FileExists(script_path)) {
        ScanFile(script_path);
      } else if (buildutil::FileExists(script_path + "/SConscript")) {
        ScanFile(script_path + "/SConscript");
      }
    }
    return;
  }

  // SourceLib('libname', tags=[...]) — library dependency
  if (MatchFunc(trimmed, "SourceLib", &args)) {
    auto string_args = ExtractStringArgs(args);
    if (!string_args.empty()) {
      SconTarget t;
      t.name = string_args[0];
      t.type = "source_lib";
      t.src_dir = current_src_dir_;
      t.path = current_path_;
      targets_.push_back(t);
    }
    return;
  }

  // Skip other Python/SCons calls we don't handle
}

// --- JSON output ---

std::string SconScanner::JsonEscape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

void SconScanner::OutputArray(const std::vector<std::string>& arr) {
  printf("[");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) printf(", ");
    printf("\"%s\"", JsonEscape(arr[i]).c_str());
  }
  printf("]");
}

void SconScanner::OutputJson() const {
  printf("{\n");
  printf("  \"format\": \"scons\",\n");
  printf("  \"target_count\": %zu,\n", targets_.size());
  printf("  \"targets\": [\n");

  bool first = true;
  for (const auto& t : targets_) {
    if (!first) printf(",\n");
    first = false;

    printf("    {\n");
    printf("      \"name\": \"%s\",\n", JsonEscape(t.name).c_str());
    printf("      \"type\": \"%s\",\n", JsonEscape(t.type).c_str());
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(t.src_dir).c_str());
    printf("      \"path\": \"%s\",\n", JsonEscape(t.path).c_str());

    printf("      \"srcs\": ");
    OutputArray(t.srcs);
    printf(",\n");

    printf("      \"cflags\": ");
    OutputArray(t.cflags);
    printf(",\n");

    printf("      \"cppflags\": ");
    OutputArray(t.cppflags);
    printf(",\n");

    printf("      \"ldflags\": ");
    OutputArray(t.ldflags);
    printf(",\n");

    printf("      \"include_dirs\": ");
    OutputArray(t.include_dirs);
    printf(",\n");

    printf("      \"defines\": ");
    OutputArray(t.defines);
    printf(",\n");

    printf("      \"link_libs\": ");
    OutputArray(t.link_libs);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

// =====================================================================
// SconScanner — build engine (compile + link without ninja)
// =====================================================================

std::string SconScanner::GetOutputPath(const SconTarget& target) const {
  std::string dir = target.src_dir.empty() ? "." : target.src_dir;
  return dir + "/build/" + target.name;
}

std::string SconScanner::GetObjectPath(const SconTarget& target,
                                        const std::string& src) const {
  std::string dir = target.src_dir.empty() ? "." : target.src_dir;
  return dir + "/build/obj/" + target.name + "/" + buildutil::BaseName(src) + ".o";
}

bool SconScanner::NeedsRecompile(const std::string& obj_file,
                                  const std::string& src_file) const {
  return buildutil::NeedsRecompile(obj_file, src_file);
}

bool SconScanner::ExecuteCmd(const std::string& cmd) {
  if (dry_run_) { std::printf("  %s\n", cmd.c_str()); return true; }
  return buildutil::ExecuteCmd(cmd);
}

bool SconScanner::CompileSource(const SconTarget& target,
                                  const std::string& src,
                                  const std::string& obj_file) {
  if (!NeedsRecompile(obj_file, src)) {
    std::printf("  [skip] %s (up-to-date)\n", src.c_str());
    return true;
  }

  std::string src_path = src;
  if (src_path[0] != '/') {
    std::string dir = target.src_dir.empty() ? "." : target.src_dir;
    src_path = dir + "/" + src_path;
  }

  std::string compiler = buildutil::GetCompiler(src);
  std::string cmd = compiler + " -MMD -MP -c -o " + obj_file + " " + src_path;

  // Add env cflags
  for (const auto& f : env_cflags_) cmd += " " + f;
  // Add target cflags
  for (const auto& f : target.cflags) cmd += " " + f;
  // Add defines
  for (const auto& d : env_defines_) cmd += " -D" + d;
  for (const auto& d : target.defines) cmd += " -D" + d;
  // Add include dirs
  for (const auto& inc : env_include_dirs_) {
    cmd += " -I" + inc;
  }
  for (const auto& inc : target.include_dirs) {
    if (inc[0] == '/') cmd += " -I" + inc;
    else {
      std::string dir = target.src_dir.empty() ? "." : target.src_dir;
      cmd += " -I" + dir + "/" + inc;
    }
  }
  cmd += " -Wall";

  std::string obj_dir = obj_file.substr(0, obj_file.find_last_of('/'));
  if (!dry_run_) buildutil::MkdirP(obj_dir);
  return ExecuteCmd(cmd);
}

bool SconScanner::LinkTarget(const SconTarget& target) {
  std::string output_path = GetOutputPath(target);
  std::string dir = target.src_dir.empty() ? "." : target.src_dir;
  if (!dry_run_) buildutil::MkdirP(dir + "/build");

  std::string obj_files;
  for (const auto& src : target.srcs) {
    obj_files += " " + GetObjectPath(target, src);
  }

  if (target.type == "library") {
    std::string cmd = "ar rcs " + output_path + ".a" + obj_files;
    return ExecuteCmd(cmd);
  }

  if (target.type == "shared_library") {
    std::string cmd = "g++ -shared -o " + output_path + ".so" + obj_files;
    for (const auto& f : target.ldflags) cmd += " " + f;
    return ExecuteCmd(cmd);
  }

  if (target.type == "program") {
    std::string cmd = "g++ -o " + output_path + obj_files;
    for (const auto& f : target.ldflags) cmd += " " + f;

    // Link env libs
    for (const auto& lib : env_libs_) {
      cmd += " -l" + lib;
    }
    // Link target libs — find local .a files first
    for (const auto& lib : target.link_libs) {
      std::string lib_path;
      for (const auto& t : targets_) {
        if (t.name == lib && t.type == "library") {
          lib_path = GetOutputPath(t) + ".a";
          break;
        }
      }
      if (!lib_path.empty()) {
        cmd += " " + lib_path;
      } else {
        cmd += " -l" + lib;
      }
    }
    return ExecuteCmd(cmd);
  }

  // Object targets: already compiled, no link needed
  return true;
}

bool SconScanner::BuildTarget(const SconTarget& target) {
  // Skip non-buildable targets
  if (target.type == "simobject" || target.type == "source_lib" ||
      target.type == "source" || target.type == "gtest") {
    if (target.srcs.empty()) return true;
  }

  if (target.srcs.empty()) {
    std::printf("  [skip] %s (%s) — no sources\n",
                target.name.c_str(), target.type.c_str());
    return true;
  }

  std::printf("Building: %s (%s)\n", target.name.c_str(),
              target.type.c_str());

  for (const auto& src : target.srcs) {
    std::string obj_file = GetObjectPath(target, src);
    if (!CompileSource(target, src, obj_file)) {
      std::fprintf(stderr, "gor_make: *** [%s] Error compiling %s\n",
                   target.name.c_str(), src.c_str());
      return false;
    }
  }

  if (!LinkTarget(target)) {
    std::fprintf(stderr, "gor_make: *** [%s] Error linking\n",
                 target.name.c_str());
    return false;
  }
  return true;
}

int SconScanner::BuildAll() {
  std::printf("Building %zu targets...\n", targets_.size());

  // Build libraries first, then programs
  for (const auto& t : targets_) {
    if (t.type == "library" || t.type == "shared_library") {
      if (!BuildTarget(t)) return 1;
    }
  }
  for (const auto& t : targets_) {
    if (t.type == "program") {
      if (!BuildTarget(t)) return 1;
    }
  }

  std::printf("Build complete.\n");
  return 0;
}

}  // namespace gormake
