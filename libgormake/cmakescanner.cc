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

#include "cmakescanner.h"
#include "buildenginebase.h"

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
  current_.srcDir = buildutil::DirName(path);
  pendingLine_.clear();
  parenDepth_ = 0;

  std::string line;
  while (std::getline(file, line)) {
    // Accumulate multi-line commands (CMake commands can span lines
    // until parens are balanced)
    if (parenDepth_ > 0) {
      pendingLine_ += " " + line;
    } else {
      pendingLine_ = line;
    }

    // Recompute paren depth from scratch (string-aware)
    // to handle multi-line commands correctly.
    parenDepth_ = 0;
    bool inString = false;
    for (size_t i = 0; i < pendingLine_.size(); ++i) {
      char c = pendingLine_[i];
      if (c == '"' && (i == 0 || pendingLine_[i - 1] != '\\')) {
        inString = !inString;
      } else if (!inString) {
        if (c == '(') parenDepth_++;
        else if (c == ')') parenDepth_ = (parenDepth_ > 0) ? parenDepth_ - 1 : 0;
      }
    }

    if (parenDepth_ > 0) continue;

    // Process the complete command
    std::string fullLine = pendingLine_;
    pendingLine_.clear();
    ProcessLine(fullLine);
  }

  // Process any remaining pending line
  if (!pendingLine_.empty()) {
    ProcessLine(pendingLine_);
  }

  return true;
}

void CmakeScanner::ScanDirectory(const std::string& dirPath) {
  for (auto& entry : fs::recursive_directory_iterator(dirPath)) {
    if (entry.path().filename() == "CMakeLists.txt") {
      std::string entryStr = entry.path().string();
      if (entryStr.find("/out/") != std::string::npos) continue;
      if (entryStr.find("/bazel-") != std::string::npos) continue;
      if (entryStr.find("/.git/") != std::string::npos) continue;
      if (entryStr.find("/build/") != std::string::npos) continue;
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

const std::vector<CmakeTarget>& CmakeScanner::GetTargets() const {
  return targets_;
}

std::string CmakeScanner::StripComment(const std::string& line) const {
  std::string result;
  bool inString = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
      inString = !inString;
    }
    if (c == '#' && !inString) break;
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
        std::string varName = s.substr(i + 2, close - i - 2);
        auto it = variables_.find(varName);
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
  bool inString = false;

  for (size_t i = 0; i < args.size(); ++i) {
    char c = args[i];
    if (c == '"' && (i == 0 || args[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (!inString && (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                       c == '(' || c == ')')) {
      if (!current.empty()) {
        result.push_back(ExpandVars(current));
        current.clear();
      }
    } else if (c == '$' && i + 1 < args.size() && args[i + 1] == '{') {
      // Variable reference
      size_t close = args.find('}', i + 2);
      if (close != std::string::npos) {
        std::string varName = args.substr(i + 2, close - i - 2);
        auto it = variables_.find(varName);
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
  size_t parenPos = trimmed.find('(');
  if (parenPos == std::string::npos) return false;

  *cmd = ToLower(Trim(trimmed.substr(0, parenPos)));

  // Find the matching closing paren
  int depth = 1;
  size_t endPos = parenPos + 1;
  while (endPos < trimmed.size() && depth > 0) {
    if (trimmed[endPos] == '(') depth++;
    else if (trimmed[endPos] == ')') depth--;
    if (depth > 0) endPos++;
  }

  if (depth != 0) return false;

  *args = trimmed.substr(parenPos + 1, endPos - parenPos - 1);
  return true;
}

void CmakeScanner::ProcessLine(const std::string& rawLine) {
  std::string cmd, args;
  if (!ParseCommand(rawLine, &cmd, &args)) {
    // Not a command — could be a continuation or empty
    return;
  }

  auto argList = ParseArgs(args);
  if (argList.empty()) return;

  // Handle conditionals — always process all branches (lenient mode).
  // We're scanning, not evaluating CMake logic, so we want to see
  // targets in both if and else branches.
  if (cmd == "if") {
    condStack_.push_back(true);
    return;
  } else if (cmd == "elseif") {
    return;
  } else if (cmd == "else") {
    return;
  } else if (cmd == "endif") {
    if (!condStack_.empty()) condStack_.pop_back();
    return;
  }

  // Handle target creation commands
  if (cmd == "add_executable") {
    // add_executable(name [source1 source2 ...])
    if (argList.size() >= 1) {
      CmakeTarget t;
      t.path = current_.path;
      t.srcDir = current_.srcDir;
      t.name = argList[0];
      t.type = "executable";
      for (size_t i = 1; i < argList.size(); ++i) {
        t.srcs.push_back(argList[i]);
      }
      targets_.push_back(t);
    }
    return;
  }

  if (cmd == "add_library") {
    // add_library(name [STATIC|SHARED|MODULE|INTERFACE] [source1 ...])
    if (argList.size() >= 1) {
      CmakeTarget t;
      t.path = current_.path;
      t.srcDir = current_.srcDir;
      t.name = argList[0];

      // Determine library type
      std::string libType = "static_library";
      size_t srcStart = 1;
      if (argList.size() >= 2) {
        std::string typeArg = ToUpper(argList[1]);
        if (typeArg == "STATIC") {
          libType = "static_library";
          srcStart = 2;
        } else if (typeArg == "SHARED") {
          libType = "shared_library";
          srcStart = 2;
        } else if (typeArg == "MODULE") {
          libType = "module_library";
          srcStart = 2;
        } else if (typeArg == "INTERFACE") {
          libType = "interface_library";
          srcStart = 2;
        } else if (typeArg == "OBJECT") {
          libType = "object_library";
          srcStart = 2;
        }
      }
      t.type = libType;

      for (size_t i = srcStart; i < argList.size(); ++i) {
        t.srcs.push_back(argList[i]);
      }
      targets_.push_back(t);
    }
    return;
  }

  // Handle target property commands (apply to current target)
  if (cmd == "target_sources") {
    // target_sources(target [PRIVATE|PUBLIC|INTERFACE] source1 ...)
    if (argList.size() >= 1) {
      std::string targetName = argList[0];
      // Find the target and add sources
      for (auto& t : targets_) {
        if (t.name == targetName) {
          for (size_t i = 1; i < argList.size(); ++i) {
            std::string a = ToUpper(argList[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.srcs.push_back(argList[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_link_libraries") {
    // target_link_libraries(target [PRIVATE|PUBLIC|INTERFACE] lib1 ...)
    if (argList.size() >= 1) {
      std::string targetName = argList[0];
      for (auto& t : targets_) {
        if (t.name == targetName) {
          for (size_t i = 1; i < argList.size(); ++i) {
            std::string a = ToUpper(argList[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.linkLibs.push_back(argList[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_include_directories") {
    if (argList.size() >= 1) {
      std::string targetName = argList[0];
      for (auto& t : targets_) {
        if (t.name == targetName) {
          for (size_t i = 1; i < argList.size(); ++i) {
            std::string a = ToUpper(argList[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            if (a == "BEFORE" || a == "SYSTEM") continue;
            t.includeDirs.push_back(argList[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_compile_definitions") {
    if (argList.size() >= 1) {
      std::string targetName = argList[0];
      for (auto& t : targets_) {
        if (t.name == targetName) {
          for (size_t i = 1; i < argList.size(); ++i) {
            std::string a = ToUpper(argList[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.defines.push_back(argList[i]);
          }
          return;
        }
      }
    }
    return;
  }

  if (cmd == "target_compile_options") {
    if (argList.size() >= 1) {
      std::string targetName = argList[0];
      for (auto& t : targets_) {
        if (t.name == targetName) {
          for (size_t i = 1; i < argList.size(); ++i) {
            std::string a = ToUpper(argList[i]);
            if (a == "PRIVATE" || a == "PUBLIC" || a == "INTERFACE") continue;
            t.compileOptions.push_back(argList[i]);
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
    for (const auto& a : argList) {
      variables_["CMAKE_INCLUDE_DIRS"] =
          variables_["CMAKE_INCLUDE_DIRS"] + " " + a;
    }
    return;
  }

  if (cmd == "add_compile_options") {
    for (const auto& a : argList) {
      variables_["CMAKE_COMPILE_OPTIONS"] =
          variables_["CMAKE_COMPILE_OPTIONS"] + " " + a;
    }
    return;
  }

  if (cmd == "add_definitions") {
    for (const auto& a : argList) {
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
    if (cmd == "set" && argList.size() >= 2) {
      std::string varName = argList[0];
      std::string varValue;
      for (size_t i = 1; i < argList.size(); ++i) {
        if (i > 1) varValue += ";";
        varValue += argList[i];
      }
      variables_[varName] = varValue;
    }
    return;
  }

  // add_subdirectory — scan the subdirectory
  if (cmd == "add_subdirectory") {
    if (!argList.empty()) {
      std::string subdir = argList[0];
      // Make relative to current srcDir
      if (subdir[0] != '/') {
        subdir = current_.srcDir + "/" + subdir;
      }
      std::string cmakeFile = subdir + "/CMakeLists.txt";
      if (buildutil::FileExists(cmakeFile)) {
        // Save/restore state
        auto savedVars = variables_;
        auto savedCurrent = current_;
        bool savedInTarget = inTarget_;
        std::string savedTargetName = currentTargetName_;

        ScanFile(cmakeFile);

        variables_ = savedVars;
        current_ = savedCurrent;
        inTarget_ = savedInTarget;
        currentTargetName_ = savedTargetName;
      }
    }
    return;
  }

  // add_custom_target — skip but don't crash
  if (cmd == "add_custom_target") {
    // Create a target entry for visibility
    if (!argList.empty()) {
      CmakeTarget t;
      t.name = argList[0];
      t.type = "custom_target";
      t.srcDir = current_.srcDir;
      t.path = current_.path;
      targets_.push_back(t);
    }
    return;
  }
}

void CmakeScanner::FlushTarget(const std::string& type,
                                const std::string& name) {
  if (!inTarget_) return;
  current_.type = type;
  current_.name = name;
  targets_.push_back(current_);
  inTarget_ = false;
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
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(t.srcDir).c_str());
    printf("      \"path\": \"%s\",\n", JsonEscape(t.path).c_str());

    printf("      \"srcs\": ");
    OutputArray(t.srcs);
    printf(",\n");

    printf("      \"link_libs\": ");
    OutputArray(t.linkLibs);
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

    printf("      \"compile_options\": ");
    OutputArray(t.compileOptions);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

// --- Build implementation ---

static std::string ReplaceExt(const std::string& path,
                               const std::string& newExt) {
  size_t dot = path.find_last_of('.');
  std::string base = (dot == std::string::npos) ? path : path.substr(0, dot);
  return base + newExt;
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
  if (!dryRun_) if (!buildutil::MkdirP("build")) {
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
  std::string objDir = "build/obj/" + target.name;
  if (!dryRun_) if (!buildutil::MkdirP(objDir)) {
    fprintf(stderr, "gor_make: *** Failed to create obj directory: %s\n",
            objDir.c_str());
    return false;
  }

  // Compile all source files
  for (const auto& src : target.srcs) {
    std::string objFile = GetObjectPath(target, src);
    if (!CompileSource(target, src, objFile)) {
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
                                  const std::string& objFile) {
  // Resolve source path relative to target's source directory
  std::string srcPath = src;
  if (!srcPath.empty() && srcPath[0] != '/') {
    srcPath = target.srcDir + "/" + src;
  }

  if (!buildutil::FileExists(srcPath)) {
    fprintf(stderr, "gor_make: *** Source file not found: %s\n",
            srcPath.c_str());
    return false;
  }

  // Ensure obj directory exists
  std::string objDir = buildutil::DirName(objFile);
  if (!dryRun_) if (!buildutil::MkdirP(objDir)) {
    fprintf(stderr, "gor_make: *** Failed to create directory: %s\n",
            objDir.c_str());
    return false;
  }

  // Check if recompilation is needed
  if (!NeedsRecompile(objFile, srcPath)) {
    return true;  // Up to date
  }

  // Choose compiler based on file extension
  std::string compiler = buildutil::GetCompiler(srcPath);

  // Build compilation command
  std::string cmd = compiler + " -MMD -MP -c";

  // Add cflags
  for (const auto& f : target.cflags) {
    cmd += " " + f;
  }

  // Add compile options
  for (const auto& opt : target.compileOptions) {
    cmd += " " + opt;
  }

  // Add include directories
  for (const auto& dir : target.includeDirs) {
    // Resolve relative to source directory
    std::string incDir = dir;
    if (!incDir.empty() && incDir[0] != '/') {
      incDir = target.srcDir + "/" + incDir;
    }
    cmd += " -I" + incDir;
  }

  // Add defines
  for (const auto& def : target.defines) {
    cmd += " -D" + def;
  }

  // Add source and output
  cmd += " -o " + objFile + " " + srcPath;

  // Print and execute
  printf("%s\n", cmd.c_str());
  fflush(stdout);

  if (!ExecuteCmd(cmd)) {
    fprintf(stderr, "gor_make: *** Compilation failed for %s\n",
            srcPath.c_str());
    return false;
  }

  return true;
}

bool CmakeScanner::LinkTarget(const CmakeTarget& target) {
  std::string outPath = GetOutputPath(target);

  // Ensure output directory exists
  std::string outDir = buildutil::DirName(outPath);
  if (!dryRun_) if (!buildutil::MkdirP(outDir)) {
    fprintf(stderr, "gor_make: *** Failed to create output directory: %s\n",
            outDir.c_str());
    return false;
  }

  std::string cmd;

  if (target.type == "static_library") {
    // Create static library with ar
    cmd = "ar rcs " + outPath;
    for (const auto& src : target.srcs) {
      cmd += " " + GetObjectPath(target, src);
    }
  } else if (target.type == "shared_library") {
    // Link shared library
    cmd = "g++ -shared -o " + outPath;

    // Add ldflags
    for (const auto& f : target.ldflags) {
      cmd += " " + f;
    }

    // Add object files
    for (const auto& src : target.srcs) {
      cmd += " " + GetObjectPath(target, src);
    }

    // Add link libraries
    for (const auto& lib : target.linkLibs) {
      const CmakeTarget* depTarget = FindTarget(targets_, lib);
      if (depTarget) {
        // Local target — add its output path
        cmd += " " + GetOutputPath(*depTarget);
      } else {
        // System library — add as -l flag
        cmd += " -l" + lib;
      }
    }
  } else {
    // Executable (default)
    cmd = "g++ -o " + outPath;

    // Add ldflags
    for (const auto& f : target.ldflags) {
      cmd += " " + f;
    }

    // Add object files
    for (const auto& src : target.srcs) {
      cmd += " " + GetObjectPath(target, src);
    }

    // Add link libraries
    for (const auto& lib : target.linkLibs) {
      const CmakeTarget* depTarget = FindTarget(targets_, lib);
      if (depTarget) {
        // Local target — add its output path
        cmd += " " + GetOutputPath(*depTarget);
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
    fprintf(stderr, "gor_make: *** Linking failed for %s\n", outPath.c_str());
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
  std::string baseName = buildutil::BaseName(src);
  return "build/obj/" + target.name + "/" + ReplaceExt(baseName, ".o");
}

bool CmakeScanner::ExecuteCmd(const std::string& cmd) {
  if (dryRun_) { std::printf("  %s\n", cmd.c_str()); return true; }
  return buildutil::ExecuteCmd(cmd);
}

bool CmakeScanner::NeedsRecompile(const std::string& objFile,
                                   const std::string& srcFile) const {
  return buildutil::NeedsRecompile(objFile, srcFile);
}

}  // namespace gormake
