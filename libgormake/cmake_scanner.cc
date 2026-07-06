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

#include "cmake_scanner.h"
#include "build_engine_base.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gormake {

namespace fs = std::filesystem;

static std::string Trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                               s[start] == '\r' || s[start] == '\n'))
    start++;
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                          s[end - 1] == '\r' || s[end - 1] == '\n'))
    end--;
  return s.substr(start, end - start);
}

static std::string ToUpper(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

static std::string ToLower(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

// --- CmakeScanner implementation ---

CmakeScanner::CmakeScanner() {}
CmakeScanner::~CmakeScanner() {}

bool CmakeScanner::ScanFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return false;

  current_.path = path;
  current_.src_dir = buildutil::DirName(path);
  pending_line_.clear();
  paren_depth_ = 0;

  std::string line;
  while (std::getline(file, line)) {
    // Accumulate multi-line commands (CMake commands can span lines
    // until parens are balanced)
    if (paren_depth_ > 0) {
      pending_line_ += " " + line;
    } else {
      pending_line_ = line;
    }

    // Recompute paren depth from scratch (string-aware)
    // to handle multi-line commands correctly.
    paren_depth_ = 0;
    bool in_string = false;
    for (size_t i = 0; i < pending_line_.size(); ++i) {
      char c = pending_line_[i];
      if (c == '"' && (i == 0 || pending_line_[i - 1] != '\\')) {
        in_string = !in_string;
      } else if (!in_string) {
        if (c == '(') paren_depth_++;
        else if (c == ')') paren_depth_ = (paren_depth_ > 0) ? paren_depth_ - 1 : 0;
      }
    }

    if (paren_depth_ > 0) continue;

    // Process the complete command
    std::string full_line = pending_line_;
    pending_line_.clear();
    ProcessLine(full_line);
  }

  // Process any remaining pending line
  if (!pending_line_.empty()) {
    ProcessLine(pending_line_);
  }

  return true;
}

void CmakeScanner::ScanDirectory(const std::string& dir_path) {
  for (auto& entry : fs::recursive_directory_iterator(dir_path)) {
    if (entry.path().filename() == "CMakeLists.txt") {
      std::string entry_str = entry.path().string();
      if (entry_str.find("/out/") != std::string::npos) continue;
      if (entry_str.find("/bazel-") != std::string::npos) continue;
      if (entry_str.find("/.git/") != std::string::npos) continue;
      if (entry_str.find("/build/") != std::string::npos) continue;
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

const std::vector<CmakeTarget>& CmakeScanner::GetTargets() const {
  return targets_;
}

std::string CmakeScanner::StripComment(const std::string& line) const {
  std::string result;
  bool in_string = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
      in_string = !in_string;
    }
    if (c == '#' && !in_string) break;
    result += c;
  }
  return result;
}

std::string CmakeScanner::ExpandVars(const std::string& s) const {
  std::string result;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
      size_t close = s.find('}', i + 2);
      if (close != std::string::npos) {
        std::string var_name = s.substr(i + 2, close - i - 2);
        auto it = variables_.find(var_name);
        if (it != variables_.end()) {
          result += it->second;
        }
        i = close;
        continue;
      }
    }
    result += s[i];
  }
  return result;
}

std::vector<std::string> CmakeScanner::ParseArgs(const std::string& args) {
  std::vector<std::string> result;
  std::string current;
  bool in_string = false;

  for (size_t i = 0; i < args.size(); ++i) {
    char c = args[i];
    if (c == '"' && (i == 0 || args[i - 1] != '\\')) {
      in_string = !in_string;
      continue;
    }
    if (!in_string && (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                       c == '(' || c == ')')) {
      if (!current.empty()) {
        result.push_back(ExpandVars(current));
        current.clear();
      }
    } else if (c == '$' && i + 1 < args.size() && args[i + 1] == '{') {
      // Variable reference
      size_t close = args.find('}', i + 2);
      if (close != std::string::npos) {
        std::string var_name = args.substr(i + 2, close - i - 2);
        auto it = variables_.find(var_name);
        if (it != variables_.end()) {
          current += it->second;
        }
        i = close;
      } else {
        current += c;
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) result.push_back(ExpandVars(current));
  return result;
}

bool CmakeScanner::ParseCommand(const std::string& line, std::string* cmd,
                                std::string* args) {
  std::string trimmed = Trim(StripComment(line));
  if (trimmed.empty()) return false;

  // Find the opening paren
  size_t paren_pos = trimmed.find('(');
  if (paren_pos == std::string::npos) return false;

  *cmd = ToLower(Trim(trimmed.substr(0, paren_pos)));

  // Find the matching closing paren
  int depth = 1;
  size_t end_pos = paren_pos + 1;
  while (end_pos < trimmed.size() && depth > 0) {
    if (trimmed[end_pos] == '(') depth++;
    else if (trimmed[end_pos] == ')') depth--;
    if (depth > 0) end_pos++;
  }

  if (depth != 0) return false;

  *args = trimmed.substr(paren_pos + 1, end_pos - paren_pos - 1);
  return true;
}

void CmakeScanner::ProcessLine(const std::string& raw_line) {
  std::string cmd, args;
  if (!ParseCommand(raw_line, &cmd, &args)) {
    // Not a command — could be a continuation or empty
    return;
  }

  auto arg_list = ParseArgs(args);
  if (arg_list.empty()) return;

  // Handle conditionals — always process all branches (lenient mode).
  // We're scanning, not evaluating CMake logic, so we want to see
  // targets in both if and else branches.
  if (cmd == "if") {
    cond_stack_.push_back(true);
    return;
  } else if (cmd == "elseif") {
    return;
  } else if (cmd == "else") {
    return;
  } else if (cmd == "endif") {
    if (!cond_stack_.empty()) cond_stack_.pop_back();
    return;
  }

  // Handle target creation commands
  if (cmd == "add_executable") {
    // add_executable(name [source1 source2 ...])
    if (arg_list.size() >= 1) {
      CmakeTarget t;
      t.path = current_.path;
      t.src_dir = current_.src_dir;
      t.name = arg_list[0];
      t.type = "executable";
      for (size_t i = 1; i < arg_list.size(); ++i) {
        t.srcs.push_back(arg_list[i]);
      }
      targets_.push_back(t);
    }
    return;
  }

  if (cmd == "add_library") {
    // add_library(name [STATIC|SHARED|MODULE|INTERFACE] [source1 ...])
    if (arg_list.size() >= 1) {
      CmakeTarget t;
      t.path = current_.path;
      t.src_dir = current_.src_dir;
      t.name = arg_list[0];

      // Determine library type
      std::string lib_type = "static_library";
      size_t src_start = 1;
      if (arg_list.size() >= 2) {
        std::string type_arg = ToUpper(arg_list[1]);
        if (type_arg == "STATIC") {
          lib_type = "static_library";
          src_start = 2;
        } else if (type_arg == "SHARED") {
          lib_type = "shared_library";
          src_start = 2;
        } else if (type_arg == "MODULE") {
          lib_type = "module_library";
          src_start = 2;
        } else if (type_arg == "INTERFACE") {
          lib_type = "interface_library";
          src_start = 2;
        } else if (type_arg == "OBJECT") {
          lib_type = "object_library";
          src_start = 2;
        }
      }
      t.type = lib_type;

      for (size_t i = src_start; i < arg_list.size(); ++i) {
        t.srcs.push_back(arg_list[i]);
      }
      targets_.push_back(t);
    }
    return;
  }

  // Handle target property commands (apply to current target)
  if (cmd == "target_sources") {
    // target_sources(target [PRIVATE|PUBLIC|INTERFACE] source1 ...)
    if (arg_list.size() >= 1) {
      std::string target_name = arg_list[0];
      // Find the target and add sources
      for (auto& t : targets_) {
        if (t.name == target_name) {
          for (size_t i = 1; i < arg_list.size(); ++i) {
            std::string a = ToUpper(arg_list[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.srcs.push_back(arg_list[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_link_libraries") {
    // target_link_libraries(target [PRIVATE|PUBLIC|INTERFACE] lib1 ...)
    if (arg_list.size() >= 1) {
      std::string target_name = arg_list[0];
      for (auto& t : targets_) {
        if (t.name == target_name) {
          for (size_t i = 1; i < arg_list.size(); ++i) {
            std::string a = ToUpper(arg_list[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.link_libs.push_back(arg_list[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_include_directories") {
    if (arg_list.size() >= 1) {
      std::string target_name = arg_list[0];
      for (auto& t : targets_) {
        if (t.name == target_name) {
          for (size_t i = 1; i < arg_list.size(); ++i) {
            std::string a = ToUpper(arg_list[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            if (a == "BEFORE" || a == "SYSTEM") continue;
            t.include_dirs.push_back(arg_list[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_compile_definitions") {
    if (arg_list.size() >= 1) {
      std::string target_name = arg_list[0];
      for (auto& t : targets_) {
        if (t.name == target_name) {
          for (size_t i = 1; i < arg_list.size(); ++i) {
            std::string a = ToUpper(arg_list[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.defines.push_back(arg_list[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_compile_options") {
    if (arg_list.size() >= 1) {
      std::string target_name = arg_list[0];
      for (auto& t : targets_) {
        if (t.name == target_name) {
          for (size_t i = 1; i < arg_list.size(); ++i) {
            std::string a = ToUpper(arg_list[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.compile_options.push_back(arg_list[i]);
          }
          return;
        }
      }
    }
    return;
  }

  // Global commands
  if (cmd == "include_directories") {
    // Apply to all subsequent targets — store as a global var
    for (const auto& a : arg_list) {
      variables_["CMAKE_INCLUDE_DIRS"] =
          variables_["CMAKE_INCLUDE_DIRS"] + " " + a;
    }
    return;
  }

  if (cmd == "add_compile_options") {
    for (const auto& a : arg_list) {
      variables_["CMAKE_COMPILE_OPTIONS"] =
          variables_["CMAKE_COMPILE_OPTIONS"] + " " + a;
    }
    return;
  }

  if (cmd == "add_definitions") {
    for (const auto& a : arg_list) {
      variables_["CMAKE_DEFINITIONS"] =
          variables_["CMAKE_DEFINITIONS"] + " " + a;
    }
    return;
  }

  if (cmd == "link_directories") {
    // Ignored for now
    return;
  }

  if (cmd == "cmake_minimum_required" || cmd == "project" ||
      cmd == "set" || cmd == "unset" || cmd == "message" ||
      cmd == "option" || cmd == "find_package" || cmd == "enable_testing" ||
      cmd == "enable_language" || cmd == "cmake_policy") {
    // Handle set() for variable tracking
    if (cmd == "set" && arg_list.size() >= 2) {
      std::string var_name = arg_list[0];
      std::string var_value;
      for (size_t i = 1; i < arg_list.size(); ++i) {
        if (i > 1) var_value += ";";
        var_value += arg_list[i];
      }
      variables_[var_name] = var_value;
    }
    return;
  }

  // add_subdirectory — scan the subdirectory
  if (cmd == "add_subdirectory") {
    if (!arg_list.empty()) {
      std::string subdir = arg_list[0];
      // Make relative to current src_dir
      if (subdir[0] != '/') {
        subdir = current_.src_dir + "/" + subdir;
      }
      std::string cmake_file = subdir + "/CMakeLists.txt";
      if (buildutil::FileExists(cmake_file)) {
        // Save/restore state
        auto saved_vars = variables_;
        auto saved_current = current_;
        bool saved_in_target = in_target_;
        std::string saved_target_name = current_target_name_;

        ScanFile(cmake_file);

        variables_ = saved_vars;
        current_ = saved_current;
        in_target_ = saved_in_target;
        current_target_name_ = saved_target_name;
      }
    }
    return;
  }

  // add_custom_target — skip but don't crash
  if (cmd == "add_custom_target") {
    // Create a target entry for visibility
    if (!arg_list.empty()) {
      CmakeTarget t;
      t.name = arg_list[0];
      t.type = "custom_target";
      t.src_dir = current_.src_dir;
      t.path = current_.path;
      targets_.push_back(t);
    }
    return;
  }
}

void CmakeScanner::FlushTarget(const std::string& type,
                                const std::string& name) {
  if (!in_target_) return;
  current_.type = type;
  current_.name = name;
  targets_.push_back(current_);
  in_target_ = false;
}

// --- JSON helpers ---

std::string CmakeScanner::JsonEscape(const std::string& s) {
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

void CmakeScanner::OutputArray(const std::vector<std::string>& arr) {
  printf("[");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) printf(", ");
    printf("\"%s\"", JsonEscape(arr[i]).c_str());
  }
  printf("]");
}

void CmakeScanner::OutputJson() const {
  printf("{\n");
  printf("  \"format\": \"cmake\",\n");
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

    printf("      \"link_libs\": ");
    OutputArray(t.link_libs);
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

    printf("      \"compile_options\": ");
    OutputArray(t.compile_options);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

// --- Build implementation ---

static std::string ReplaceExt(const std::string& path,
                               const std::string& new_ext) {
  size_t dot = path.find_last_of('.');
  std::string base = (dot == std::string::npos) ? path : path.substr(0, dot);
  return base + new_ext;
}

static const CmakeTarget* FindTarget(const std::vector<CmakeTarget>& targets,
                                      const std::string& name) {
  for (const auto& t : targets) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

int CmakeScanner::BuildAll() {
  // Create build directory
  if (!dry_run_) if (!buildutil::MkdirP("build")) {
    fprintf(stderr, "gor_make: *** Failed to create build directory.\n");
    return 1;
  }

  int result = 0;
  for (const auto& target : targets_) {
    if (!BuildTarget(target)) {
      result = 1;
    }
  }

  if (result == 0) {
    fprintf(stderr, "Build completed successfully.\n");
  }

  return result;
}

bool CmakeScanner::BuildTarget(const CmakeTarget& target) {
  // Skip non-compilable targets
  if (target.type == "custom_target") {
    return true;
  }
  if (target.type == "interface_library") {
    // Header-only, nothing to build
    return true;
  }
  if (target.srcs.empty()) {
    return true;
  }

  // Create object directory for this target
  std::string obj_dir = "build/obj/" + target.name;
  if (!dry_run_) if (!buildutil::MkdirP(obj_dir)) {
    fprintf(stderr, "gor_make: *** Failed to create obj directory: %s\n",
            obj_dir.c_str());
    return false;
  }

  // Compile all source files
  for (const auto& src : target.srcs) {
    std::string obj_file = GetObjectPath(target, src);
    if (!CompileSource(target, src, obj_file)) {
      return false;
    }
  }

  // Link (except object libraries)
  if (target.type != "object_library") {
    if (!LinkTarget(target)) {
      return false;
    }
  }

  return true;
}

bool CmakeScanner::CompileSource(const CmakeTarget& target,
                                  const std::string& src,
                                  const std::string& obj_file) {
  // Resolve source path relative to target's source directory
  std::string src_path = src;
  if (!src_path.empty() && src_path[0] != '/') {
    src_path = target.src_dir + "/" + src;
  }

  if (!buildutil::FileExists(src_path)) {
    fprintf(stderr, "gor_make: *** Source file not found: %s\n",
            src_path.c_str());
    return false;
  }

  // Ensure obj directory exists
  std::string obj_dir = buildutil::DirName(obj_file);
  if (!dry_run_) if (!buildutil::MkdirP(obj_dir)) {
    fprintf(stderr, "gor_make: *** Failed to create directory: %s\n",
            obj_dir.c_str());
    return false;
  }

  // Check if recompilation is needed
  if (!NeedsRecompile(obj_file, src_path)) {
    return true;  // Up to date
  }

  // Choose compiler based on file extension
  std::string compiler = buildutil::GetCompiler(src_path);

  // Build compilation command
  std::string cmd = compiler + " -MMD -MP -c";

  // Add cflags
  for (const auto& f : target.cflags) {
    cmd += " " + f;
  }

  // Add compile options
  for (const auto& opt : target.compile_options) {
    cmd += " " + opt;
  }

  // Add include directories
  for (const auto& dir : target.include_dirs) {
    // Resolve relative to source directory
    std::string inc_dir = dir;
    if (!inc_dir.empty() && inc_dir[0] != '/') {
      inc_dir = target.src_dir + "/" + inc_dir;
    }
    cmd += " -I" + inc_dir;
  }

  // Add defines
  for (const auto& def : target.defines) {
    cmd += " -D" + def;
  }

  // Add source and output
  cmd += " -o " + obj_file + " " + src_path;

  // Print and execute
  printf("%s\n", cmd.c_str());
  fflush(stdout);

  if (!ExecuteCmd(cmd)) {
    fprintf(stderr, "gor_make: *** Compilation failed for %s\n",
            src_path.c_str());
    return false;
  }

  return true;
}

bool CmakeScanner::LinkTarget(const CmakeTarget& target) {
  std::string out_path = GetOutputPath(target);

  // Ensure output directory exists
  std::string out_dir = buildutil::DirName(out_path);
  if (!dry_run_) if (!buildutil::MkdirP(out_dir)) {
    fprintf(stderr, "gor_make: *** Failed to create output directory: %s\n",
            out_dir.c_str());
    return false;
  }

  std::string cmd;

  if (target.type == "static_library") {
    // Create static library with ar
    cmd = "ar rcs " + out_path;
    for (const auto& src : target.srcs) {
      cmd += " " + GetObjectPath(target, src);
    }
  } else if (target.type == "shared_library") {
    // Link shared library
    cmd = "g++ -shared -o " + out_path;

    // Add ldflags
    for (const auto& f : target.ldflags) {
      cmd += " " + f;
    }

    // Add object files
    for (const auto& src : target.srcs) {
      cmd += " " + GetObjectPath(target, src);
    }

    // Add link libraries
    for (const auto& lib : target.link_libs) {
      const CmakeTarget* dep_target = FindTarget(targets_, lib);
      if (dep_target) {
        // Local target — add its output path
        cmd += " " + GetOutputPath(*dep_target);
      } else {
        // System library — add as -l flag
        cmd += " -l" + lib;
      }
    }
  } else {
    // Executable (default)
    cmd = "g++ -o " + out_path;

    // Add ldflags
    for (const auto& f : target.ldflags) {
      cmd += " " + f;
    }

    // Add object files
    for (const auto& src : target.srcs) {
      cmd += " " + GetObjectPath(target, src);
    }

    // Add link libraries
    for (const auto& lib : target.link_libs) {
      const CmakeTarget* dep_target = FindTarget(targets_, lib);
      if (dep_target) {
        // Local target — add its output path
        cmd += " " + GetOutputPath(*dep_target);
      } else {
        // System library — add as -l flag
        cmd += " -l" + lib;
      }
    }
  }

  // Print and execute
  printf("%s\n", cmd.c_str());
  fflush(stdout);

  if (!ExecuteCmd(cmd)) {
    fprintf(stderr, "gor_make: *** Linking failed for %s\n", out_path.c_str());
    return false;
  }

  return true;
}

std::string CmakeScanner::GetOutputPath(const CmakeTarget& target) const {
  if (target.type == "static_library") {
    return "build/" + target.name + ".a";
  }
  if (target.type == "shared_library") {
    return "build/" + target.name + ".so";
  }
  // Executable
  return "build/" + target.name;
}

std::string CmakeScanner::GetObjectPath(const CmakeTarget& target,
                                         const std::string& src) const {
  std::string base_name = buildutil::BaseName(src);
  return "build/obj/" + target.name + "/" + ReplaceExt(base_name, ".o");
}

bool CmakeScanner::ExecuteCmd(const std::string& cmd) {
  if (dry_run_) { std::printf("  %s\n", cmd.c_str()); return true; }
  return buildutil::ExecuteCmd(cmd);
}

bool CmakeScanner::NeedsRecompile(const std::string& obj_file,
                                   const std::string& src_file) const {
  return buildutil::NeedsRecompile(obj_file, src_file);
}

}  // namespace gormake
