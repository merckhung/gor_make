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

#include "sconscanner.h"

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

static std::string DirName(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

static std::string StripComment(const std::string& line) {
  // SCons files use Python syntax: # comments
  std::string result;
  bool inString = false;
  char stringChar = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (inString) {
      result += c;
      if (c == stringChar && (i == 0 || line[i - 1] != '\\'))
        inString = false;
    } else {
      if (c == '#') break;
      if (c == '"' || c == '\'') {
        inString = true;
        stringChar = c;
      }
      result += c;
    }
  }
  return result;
}

static std::string BaseName(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  return (dot == std::string::npos) ? base : base.substr(0, dot);
}

static bool IsCppSource(const std::string& src) {
  size_t dot = src.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = src.substr(dot);
  return ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C";
}

static bool IsCSource(const std::string& src) {
  size_t dot = src.find_last_of('.');
  if (dot == std::string::npos) return false;
  return src.substr(dot) == ".c";
}

static bool SconFileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static bool SconMkdirP(const std::string& path) {
  std::string cmd = "mkdir -p " + path;
  return system(cmd.c_str()) == 0;
}

// --- SconScanner implementation ---

SconScanner::SconScanner() {}
SconScanner::~SconScanner() {}

bool SconScanner::ScanFile(const std::string& path) {
  // Avoid re-scanning the same file (prevents infinite recursion)
  if (visitedFiles_.count(path) > 0) return true;
  visitedFiles_.insert(path);

  std::ifstream file(path);
  if (!file.is_open()) return false;

  currentPath_ = path;
  currentSrcDir_ = DirName(path);

  std::string line;
  while (std::getline(file, line)) {
    ProcessLine(line);
  }

  return true;
}

void SconScanner::ScanDirectory(const std::string& dirPath) {
  for (auto& entry : fs::recursive_directory_iterator(dirPath)) {
    std::string filename = entry.path().filename().string();
    if (filename == "SConstruct" || filename == "SConscript" ||
        filename == "SConscript.*") {
      std::string entryStr = entry.path().string();
      if (entryStr.find("/out/") != std::string::npos) continue;
      if (entryStr.find("/build/") != std::string::npos) continue;
      if (entryStr.find("/.git/") != std::string::npos) continue;
      try {
        ScanFile(entryStr);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "gor_make: [warning] error parsing %s: %s\n",
                     entryStr.c_str(), e.what());
      } catch (...) {
        std::fprintf(stderr, "gor_make: [warning] unknown error parsing %s\n",
                     entryStr.c_str());
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
  bool inString = false;
  char stringChar = 0;
  std::string current;

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (!inString) {
      if (c == '"' || c == '\'') {
        inString = true;
        stringChar = c;
        current.clear();
      }
    } else {
      if (c == stringChar && (i == 0 || s[i - 1] != '\\')) {
        inString = false;
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
                              const std::string& funcName,
                              std::string* args) const {
  // Match patterns: FuncName(...), env.FuncName(...), envx.FuncName(...)
  std::string trimmed = Trim(StripComment(line));
  if (trimmed.empty()) return false;

  // Find the function name
  std::string func = ExtractFuncName(trimmed);
  if (func != funcName) return false;

  // Find the opening paren after the function name
  size_t parenPos = trimmed.find('(');
  if (parenPos == std::string::npos) return false;

  // Find matching closing paren
  int depth = 0;
  size_t endPos = parenPos;
  for (size_t i = parenPos; i < trimmed.size(); ++i) {
    if (trimmed[i] == '(') depth++;
    else if (trimmed[i] == ')') {
      depth--;
      if (depth == 0) {
        endPos = i;
        break;
      }
    }
  }

  *args = trimmed.substr(parenPos + 1, endPos - parenPos - 1);
  return true;
}

void SconScanner::ProcessLine(const std::string& rawLine) {
  std::string line = StripComment(rawLine);
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
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      // Add to a "source" target
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "source";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      t.srcs.push_back(stringArgs[0]);
      targets_.push_back(t);
    }
    return;
  }

  // GTest('name', 'test.cc', ...) — test target
  if (MatchFunc(trimmed, "GTest", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "gtest";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      for (size_t i = 1; i < stringArgs.size(); ++i) {
        t.srcs.push_back(stringArgs[i]);
      }
      targets_.push_back(t);
    }
    return;
  }

  // SimObject('file.py', ...) — Python sim object definition
  if (MatchFunc(trimmed, "SimObject", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "simobject";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      targets_.push_back(t);
    }
    return;
  }

  // env.Library('name', [...sources...]) or env.Library(target='name', source=[...])
  if (MatchFunc(trimmed, "Library", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "library";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      // Sources may be in a list or individual strings
      for (size_t i = 1; i < stringArgs.size(); ++i) {
        if (IsCppSource(stringArgs[i]) || IsCSource(stringArgs[i])) {
          t.srcs.push_back(stringArgs[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libsPos = args.find("LIBS");
      if (libsPos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eqPos = args.find('=', libsPos);
        if (eqPos != std::string::npos) {
          std::string libsPart = args.substr(eqPos + 1);
          auto libs = ExtractStringArgs(libsPart);
          for (const auto& lib : libs) {
            t.linkLibs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.SharedLibrary('name', [...sources...])
  if (MatchFunc(trimmed, "SharedLibrary", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "shared_library";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      for (size_t i = 1; i < stringArgs.size(); ++i) {
        if (IsCppSource(stringArgs[i]) || IsCSource(stringArgs[i])) {
          t.srcs.push_back(stringArgs[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libsPos = args.find("LIBS");
      if (libsPos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eqPos = args.find('=', libsPos);
        if (eqPos != std::string::npos) {
          std::string libsPart = args.substr(eqPos + 1);
          auto libs = ExtractStringArgs(libsPart);
          for (const auto& lib : libs) {
            t.linkLibs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.StaticLibrary('name', [...sources...])
  if (MatchFunc(trimmed, "StaticLibrary", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "library";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      for (size_t i = 1; i < stringArgs.size(); ++i) {
        if (IsCppSource(stringArgs[i]) || IsCSource(stringArgs[i])) {
          t.srcs.push_back(stringArgs[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libsPos = args.find("LIBS");
      if (libsPos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eqPos = args.find('=', libsPos);
        if (eqPos != std::string::npos) {
          std::string libsPart = args.substr(eqPos + 1);
          auto libs = ExtractStringArgs(libsPart);
          for (const auto& lib : libs) {
            t.linkLibs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.Program('name', [...sources...]) — executable
  if (MatchFunc(trimmed, "Program", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "program";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
      for (size_t i = 1; i < stringArgs.size(); ++i) {
        if (IsCppSource(stringArgs[i]) || IsCSource(stringArgs[i])) {
          t.srcs.push_back(stringArgs[i]);
        }
      }
      // Capture LIBS from keyword argument: LIBS=['math'] or LIBS=['math', 'foo']
      size_t libsPos = args.find("LIBS");
      if (libsPos != std::string::npos) {
        // Find the list after LIBS= (skip the = sign)
        size_t eqPos = args.find('=', libsPos);
        if (eqPos != std::string::npos) {
          std::string libsPart = args.substr(eqPos + 1);
          auto libs = ExtractStringArgs(libsPart);
          for (const auto& lib : libs) {
            t.linkLibs.push_back(lib);
          }
        }
      }
      targets_.push_back(t);
    }
    return;
  }

  // env.SharedObject('file.c') — compile to .o
  if (MatchFunc(trimmed, "SharedObject", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    for (const auto& s : stringArgs) {
      if (IsCppSource(s) || IsCSource(s)) {
        SconTarget t;
        t.name = s;
        t.type = "object";
        t.srcDir = currentSrcDir_;
        t.path = currentPath_;
        t.srcs.push_back(s);
        targets_.push_back(t);
      }
    }
    return;
  }

  // env.Object('file.cc') — compile to .o
  if (MatchFunc(trimmed, "Object", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    for (const auto& s : stringArgs) {
      if (IsCppSource(s) || IsCSource(s)) {
        SconTarget t;
        t.name = s;
        t.type = "object";
        t.srcDir = currentSrcDir_;
        t.path = currentPath_;
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
        if (f[0] == '-') envCflags_.push_back(f);
      }
    }
    if (args.find("CPPDEFINES") != std::string::npos) {
      auto defs = ExtractList(args);
      for (const auto& d : defs) {
        envDefines_.push_back(d);
      }
    }
    if (args.find("LIBS") != std::string::npos) {
      auto libs = ExtractList(args);
      for (const auto& l : libs) {
        envLibs_.push_back(l);
      }
    }
    return;
  }

  // env.Prepend(CPPPATH=Dir('./include')) — include directories
  if (MatchFunc(trimmed, "Prepend", &args)) {
    if (args.find("CPPPATH") != std::string::npos) {
      auto dirs = ExtractList(args);
      for (const auto& d : dirs) {
        envIncludeDirs_.push_back(d);
      }
    }
    return;
  }

  // SConscript('path/SConscript') — recursive include
  if (MatchFunc(trimmed, "SConscript", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    for (const auto& s : stringArgs) {
      // Resolve relative to current dir
      std::string scriptPath;
      if (s[0] == '/') {
        scriptPath = s;
      } else {
        scriptPath = currentSrcDir_ + "/" + s;
      }
      if (SconFileExists(scriptPath)) {
        ScanFile(scriptPath);
      } else if (SconFileExists(scriptPath + "/SConscript")) {
        ScanFile(scriptPath + "/SConscript");
      }
    }
    return;
  }

  // SourceLib('libname', tags=[...]) — library dependency
  if (MatchFunc(trimmed, "SourceLib", &args)) {
    auto stringArgs = ExtractStringArgs(args);
    if (!stringArgs.empty()) {
      SconTarget t;
      t.name = stringArgs[0];
      t.type = "source_lib";
      t.srcDir = currentSrcDir_;
      t.path = currentPath_;
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
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(t.srcDir).c_str());
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
    OutputArray(t.includeDirs);
    printf(",\n");

    printf("      \"defines\": ");
    OutputArray(t.defines);
    printf(",\n");

    printf("      \"link_libs\": ");
    OutputArray(t.linkLibs);
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
  std::string dir = target.srcDir.empty() ? "." : target.srcDir;
  return dir + "/build/" + target.name;
}

std::string SconScanner::GetObjectPath(const SconTarget& target,
                                        const std::string& src) const {
  std::string dir = target.srcDir.empty() ? "." : target.srcDir;
  return dir + "/build/obj/" + target.name + "/" + BaseName(src) + ".o";
}

bool SconScanner::NeedsRecompile(const std::string& objFile,
                                  const std::string& srcFile) const {
  if (!SconFileExists(objFile)) return true;
  struct stat objStat, srcStat;
  if (stat(objFile.c_str(), &objStat) != 0) return true;
  if (stat(srcFile.c_str(), &srcStat) != 0) return true;
  return srcStat.st_mtime > objStat.st_mtime;
}

bool SconScanner::ExecuteCmd(const std::string& cmd) {
  std::printf("  %s\n", cmd.c_str());
  return system(cmd.c_str()) == 0;
}

bool SconScanner::CompileSource(const SconTarget& target,
                                  const std::string& src,
                                  const std::string& objFile) {
  if (!NeedsRecompile(objFile, src)) {
    std::printf("  [skip] %s (up-to-date)\n", src.c_str());
    return true;
  }

  std::string srcPath = src;
  if (srcPath[0] != '/') {
    std::string dir = target.srcDir.empty() ? "." : target.srcDir;
    srcPath = dir + "/" + srcPath;
  }

  bool isCpp = IsCppSource(src);
  std::string compiler = isCpp ? "g++" : "gcc";
  std::string cmd = compiler + " -c -o " + objFile + " " + srcPath;

  // Add env cflags
  for (const auto& f : envCflags_) cmd += " " + f;
  // Add target cflags
  for (const auto& f : target.cflags) cmd += " " + f;
  // Add defines
  for (const auto& d : envDefines_) cmd += " -D" + d;
  for (const auto& d : target.defines) cmd += " -D" + d;
  // Add include dirs
  for (const auto& inc : envIncludeDirs_) {
    cmd += " -I" + inc;
  }
  for (const auto& inc : target.includeDirs) {
    if (inc[0] == '/') cmd += " -I" + inc;
    else {
      std::string dir = target.srcDir.empty() ? "." : target.srcDir;
      cmd += " -I" + dir + "/" + inc;
    }
  }
  cmd += " -Wall";

  std::string objDir = objFile.substr(0, objFile.find_last_of('/'));
  SconMkdirP(objDir);
  return ExecuteCmd(cmd);
}

bool SconScanner::LinkTarget(const SconTarget& target) {
  std::string outputPath = GetOutputPath(target);
  std::string dir = target.srcDir.empty() ? "." : target.srcDir;
  SconMkdirP(dir + "/build");

  std::string objFiles;
  for (const auto& src : target.srcs) {
    objFiles += " " + GetObjectPath(target, src);
  }

  if (target.type == "library") {
    std::string cmd = "ar rcs " + outputPath + ".a" + objFiles;
    return ExecuteCmd(cmd);
  }

  if (target.type == "shared_library") {
    std::string cmd = "g++ -shared -o " + outputPath + ".so" + objFiles;
    for (const auto& f : target.ldflags) cmd += " " + f;
    return ExecuteCmd(cmd);
  }

  if (target.type == "program") {
    std::string cmd = "g++ -o " + outputPath + objFiles;
    for (const auto& f : target.ldflags) cmd += " " + f;

    // Link env libs
    for (const auto& lib : envLibs_) {
      cmd += " -l" + lib;
    }
    // Link target libs — find local .a files first
    for (const auto& lib : target.linkLibs) {
      std::string libPath;
      for (const auto& t : targets_) {
        if (t.name == lib && t.type == "library") {
          libPath = GetOutputPath(t) + ".a";
          break;
        }
      }
      if (!libPath.empty()) {
        cmd += " " + libPath;
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
    std::string objFile = GetObjectPath(target, src);
    if (!CompileSource(target, src, objFile)) {
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
