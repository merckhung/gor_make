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

#include "mkscanner.h"
#include "buildenginebase.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

namespace gormake {

namespace fs = std::filesystem;

// Helper: trim whitespace
static std::string Trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
  size_t end = s.size();
  while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || 
                          s[end-1] == '\r' || s[end-1] == '\n')) end--;
  return s.substr(start, end - start);
}

// Helper: check if string starts with prefix
static bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Helper: escape a string for JSON output
static std::string JsonEscape(const std::string& s) {
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

static void OutputJsonArray(const std::vector<std::string>& arr) {
  printf("[");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) printf(", ");
    printf("\"%s\"", JsonEscape(arr[i]).c_str());
  }
  printf("]");
}

MkScanner::MkScanner() {}
MkScanner::~MkScanner() {}

bool MkScanner::ScanFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }

  current_.path = path;
  current_.srcDir = buildutil::DirName(path);
  variables_["LOCAL_PATH"] = "";  // Will be set by $(call my-dir) expansion

  std::string line;
  while (std::getline(file, line)) {
    ProcessLine(line);
  }

  return true;
}

void MkScanner::ScanDirectory(const std::string& dirPath) {
  for (auto& entry : fs::recursive_directory_iterator(dirPath)) {
    if (entry.path().filename() == "Android.mk") {
      std::string entryStr = entry.path().string();
      // Skip common output/build directories
      if (entryStr.find("/out/") != std::string::npos) continue;
      if (entryStr.find("/bazel-") != std::string::npos) continue;
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

const std::vector<MkModule>& MkScanner::GetModules() const {
  return modules_;
}

std::string MkScanner::StripComment(const std::string& line) const {
  std::string result;
  bool inString = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && (i == 0 || line[i-1] != '\\')) {
      inString = !inString;
    }
    if (c == '#' && !inString) {
      break;
    }
    result += c;
  }
  return result;
}

std::vector<std::string> MkScanner::SplitList(const std::string& s) const {
  std::vector<std::string> result;
  std::string current;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) result.push_back(current);
  return result;
}

std::string MkScanner::ExpandVars(const std::string& s) const {
  std::string result;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '$' && i + 1 < s.size()) {
      if (s[i+1] == '(') {
        size_t close = s.find(')', i + 2);
        if (close != std::string::npos) {
          std::string varName = s.substr(i + 2, close - i - 2);
          // Handle $(call my-dir) -> return the directory
          if (StartsWith(varName, "call my-dir")) {
            result += current_.srcDir;
          } else if (StartsWith(varName, "LOCAL_PATH")) {
            result += current_.srcDir;
          } else {
            auto it = variables_.find(varName);
            if (it != variables_.end()) {
              result += it->second;
            }
            // If not found, just skip (empty expansion)
          }
          i = close;
          continue;
        }
      } else if (s[i+1] == '$') {
        result += '$';
        i++;
        continue;
      }
    }
    result += s[i];
  }
  return result;
}

bool MkScanner::ParseAssignment(const std::string& line, std::string* name,
                                 std::string* value, char* op) {
  // Find the operator
  size_t opPos = std::string::npos;
  *op = '=';

  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '=') {
      if (i > 0 && (line[i-1] == ':' || line[i-1] == '?' || line[i-1] == '+')) {
        opPos = i - 1;
        *op = line[i-1];
      } else {
        opPos = i;
        *op = '=';
      }
      break;
    }
    if (line[i] == ' ' || line[i] == '\t') {
      // Continue looking
    }
  }

  if (opPos == std::string::npos) return false;

  *name = Trim(line.substr(0, opPos));
  size_t valStart = opPos + 1;
  if (*op != '=') valStart = opPos + 2;  // Skip the operator char and =
  *value = Trim(line.substr(valStart));

  return true;
}

void MkScanner::ProcessConditional(const std::string& line) {
  std::string trimmed = Trim(line);

  if (StartsWith(trimmed, "ifdef ") || StartsWith(trimmed, "ifndef ")) {
    bool isIfndef = StartsWith(trimmed, "ifndef ");
    std::string varName = Trim(trimmed.substr(6));
    bool defined = variables_.count(varName) > 0 &&
                   !variables_[varName].empty();
    // Also check environment
    if (!defined) {
      const char* env = getenv(varName.c_str());
      if (env) defined = true;
    }
    bool result = isIfndef ? !defined : defined;
    condStack_.push_back(result);
  } else if (StartsWith(trimmed, "ifeq ") || StartsWith(trimmed, "ifneq ")) {
    // Simplified: just push true (we can't fully evaluate)
    condStack_.push_back(true);
  } else if (trimmed == "else") {
    if (!condStack_.empty()) {
      condStack_.back() = !condStack_.back();
    }
  } else if (trimmed == "endif") {
    if (!condStack_.empty()) condStack_.pop_back();
  }

  // Recompute active_
  active_ = true;
  for (bool v : condStack_) {
    if (!v) { active_ = false; break; }
  }
}

// Helper: normalize a build type string by stripping $(...) wrappers and
// mapping to canonical build type names.
static std::string NormalizeBuildType(const std::string& raw) {
  std::string name = raw;

  // Step 1: Strip $(...) wrapper if the entire string is wrapped
  if (name.size() >= 3 && name[0] == '$' && name[1] == '(' &&
      name.back() == ')') {
    name = name.substr(2, name.size() - 3);
  }

  // Step 2: Check against known build types (all map to themselves)
  static const std::vector<std::string> knownTypes = {
      "BUILD_EXECUTABLE",
      "BUILD_HOST_EXECUTABLE",
      "BUILD_STATIC_LIBRARY",
      "BUILD_HOST_STATIC_LIBRARY",
      "BUILD_SHARED_LIBRARY",
      "BUILD_HOST_SHARED_LIBRARY",
      "BUILD_HOST_JAVA_LIBRARY",
      "BUILD_NATIVE_TEST",
      "BUILD_HOST_DALVIK_JAVA_LIBRARY",
  };
  for (const auto& known : knownTypes) {
    if (name == known) return known;
  }

  // Step 3: BUILD_HOST_$(build_target) and similar -> BUILD_HOST_VARIABLE
  if (StartsWith(name, "BUILD_HOST_")) {
    return "BUILD_HOST_VARIABLE";
  }

  // Step 4: For any other pattern, keep the (stripped) inner name
  return name;
}

void MkScanner::FlushModule(const std::string& buildType) {
  if (!inModule_) return;

  // Normalize the build type: strip $(...) and map to canonical name
  current_.buildType = NormalizeBuildType(buildType);
  if (!current_.name.empty()) {
    modules_.push_back(current_);
  }

  // Reset current module
  current_ = MkModule();
  current_.path = current_.path;  // Keep path
  current_.srcDir = current_.srcDir;
  inModule_ = false;
}

void MkScanner::ProcessLine(const std::string& rawLine) {
  // Strip comments
  std::string line = StripComment(rawLine);
  std::string trimmed = Trim(line);

  if (trimmed.empty()) return;

  // Handle line continuation (backslash at end)
  // Note: multi-line continuations are handled by the caller joining lines

  // Handle conditionals
  if (StartsWith(trimmed, "ifdef ") || StartsWith(trimmed, "ifndef ") ||
      StartsWith(trimmed, "ifeq ") || StartsWith(trimmed, "ifneq ") ||
      trimmed == "else" || trimmed == "endif") {
    ProcessConditional(trimmed);
    return;
  }

  // Skip lines inside inactive conditionals
  if (!active_) return;

  // Handle include directives
  if (StartsWith(trimmed, "include ") || StartsWith(trimmed, "include\t")) {
    std::string includeTarget = Trim(trimmed.substr(8));

    // Check for CLEAR_VARS before expansion
    if (includeTarget.find("CLEAR_VARS") != std::string::npos) {
      // Start a new module, preserving path/srcDir
      std::string savedPath = current_.path;
      std::string savedSrcDir = current_.srcDir;
      current_ = MkModule();
      current_.path = savedPath;
      current_.srcDir = savedSrcDir;
      inModule_ = true;
      return;
    }

    // Check for BUILD_* includes before expansion.
    // Check BUILD_HOST_* first — host build type names contain their non-host
    // counterparts as substrings (e.g. BUILD_HOST_EXECUTABLE contains
    // BUILD_EXECUTABLE), so they must be checked before the non-host types.
    if (includeTarget.find("BUILD_HOST_") != std::string::npos) {
      FlushModule(includeTarget);
      return;
    }
    if (includeTarget.find("BUILD_STATIC_LIBRARY") != std::string::npos) {
      FlushModule(includeTarget);
      return;
    }
    if (includeTarget.find("BUILD_SHARED_LIBRARY") != std::string::npos) {
      FlushModule(includeTarget);
      return;
    }
    if (includeTarget.find("BUILD_EXECUTABLE") != std::string::npos) {
      FlushModule(includeTarget);
      return;
    }
    if (includeTarget.find("BUILD_NATIVE_TEST") != std::string::npos) {
      FlushModule(includeTarget);
      return;
    }

    // Expand variables for other includes
    includeTarget = ExpandVars(includeTarget);

    // Try to read the file
    if (buildutil::FileExists(includeTarget)) {
      // Recursively scan included file
      auto savedVars = variables_;
      auto savedCurrent = current_;
      bool savedInModule = inModule_;

      ScanFile(includeTarget);

      variables_ = savedVars;
      current_ = savedCurrent;
      inModule_ = savedInModule;
    }
    return;
  }

  // Handle define/endef
  if (StartsWith(trimmed, "define ")) {
    // Skip multi-line define blocks
    return;
  }

  // Parse variable assignments
  std::string varName, varValue;
  char op;
  if (ParseAssignment(trimmed, &varName, &varValue, &op)) {
    // Expand variables in value
    varValue = ExpandVars(varValue);

    // Handle LOCAL_* variables when inside a module
    if (inModule_ && StartsWith(varName, "LOCAL_")) {
      if (varName == "LOCAL_MODULE") {
        current_.name = varValue;
      } else if (varName == "LOCAL_SRC_FILES") {
        auto srcs = SplitList(varValue);
        if (op == '+') {
          current_.srcs.insert(current_.srcs.end(), srcs.begin(), srcs.end());
        } else {
          current_.srcs = srcs;
        }
      } else if (varName == "LOCAL_CFLAGS") {
        auto flags = SplitList(varValue);
        if (op == '+') {
          current_.cflags.insert(current_.cflags.end(), flags.begin(), flags.end());
        } else {
          current_.cflags = flags;
        }
      } else if (varName == "LOCAL_CPPFLAGS") {
        auto flags = SplitList(varValue);
        if (op == '+') {
          current_.cppflags.insert(current_.cppflags.end(), flags.begin(), flags.end());
        } else {
          current_.cppflags = flags;
        }
      } else if (varName == "LOCAL_LDFLAGS") {
        auto flags = SplitList(varValue);
        if (op == '+') {
          current_.ldflags.insert(current_.ldflags.end(), flags.begin(), flags.end());
        } else {
          current_.ldflags = flags;
        }
      } else if (varName == "LOCAL_SHARED_LIBRARIES") {
        auto libs = SplitList(varValue);
        if (op == '+') {
          current_.sharedLibs.insert(current_.sharedLibs.end(), libs.begin(), libs.end());
        } else {
          current_.sharedLibs = libs;
        }
      } else if (varName == "LOCAL_STATIC_LIBRARIES") {
        auto libs = SplitList(varValue);
        if (op == '+') {
          current_.staticLibs.insert(current_.staticLibs.end(), libs.begin(), libs.end());
        } else {
          current_.staticLibs = libs;
        }
      } else if (varName == "LOCAL_WHOLE_STATIC_LIBRARIES") {
        auto libs = SplitList(varValue);
        if (op == '+') {
          current_.wholeStaticLibs.insert(current_.wholeStaticLibs.end(), libs.begin(), libs.end());
        } else {
          current_.wholeStaticLibs = libs;
        }
      } else if (varName == "LOCAL_C_INCLUDES") {
        auto dirs = SplitList(varValue);
        if (op == '+') {
          current_.includeDirs.insert(current_.includeDirs.end(), dirs.begin(), dirs.end());
        } else {
          current_.includeDirs = dirs;
        }
      } else if (varName == "LOCAL_EXPORT_C_INCLUDE_DIRS") {
        auto dirs = SplitList(varValue);
        if (op == '+') {
          current_.exportIncludeDirs.insert(current_.exportIncludeDirs.end(), dirs.begin(), dirs.end());
        } else {
          current_.exportIncludeDirs = dirs;
        }
      }
      // Also store in variables_ for expansion
      variables_[varName] = varValue;
    } else {
      // Regular variable assignment
      if (op == '+') {
        variables_[varName] += " " + varValue;
      } else if (op == '?') {
        if (variables_.count(varName) == 0) {
          variables_[varName] = varValue;
        }
      } else {
        variables_[varName] = varValue;
      }
    }
    return;
  }

  // Handle $(call ...) and other function calls at top level
  if (StartsWith(trimmed, "$(") || StartsWith(trimmed, "$(call")) {
    // Evaluate and ignore (or store result)
    return;
  }
}

void MkScanner::OutputJson() const {
  printf("{\n");
  printf("  \"format\": \"android.mk\",\n");
  printf("  \"module_count\": %zu,\n", modules_.size());
  printf("  \"modules\": [\n");

  bool first = true;
  for (const auto& mod : modules_) {
    if (!first) printf(",\n");
    first = false;

    // Determine module type string
    std::string typeStr = "unknown";
    if (mod.buildType == "BUILD_STATIC_LIBRARY") typeStr = "cc_library_static";
    else if (mod.buildType == "BUILD_SHARED_LIBRARY") typeStr = "cc_library_shared";
    else if (mod.buildType == "BUILD_EXECUTABLE") typeStr = "cc_binary";
    else if (mod.buildType == "BUILD_NATIVE_TEST") typeStr = "cc_test";
    else typeStr = mod.buildType;

    printf("    {\n");
    printf("      \"name\": \"%s\",\n", JsonEscape(mod.name).c_str());
    printf("      \"type\": \"%s\",\n", JsonEscape(typeStr).c_str());
    printf("      \"build_type\": \"%s\",\n", JsonEscape(mod.buildType).c_str());
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(mod.srcDir).c_str());
    printf("      \"path\": \"%s\",\n", JsonEscape(mod.path).c_str());

    printf("      \"srcs\": ");
    OutputJsonArray(mod.srcs);
    printf(",\n");

    printf("      \"shared_libs\": ");
    OutputJsonArray(mod.sharedLibs);
    printf(",\n");

    printf("      \"static_libs\": ");
    OutputJsonArray(mod.staticLibs);
    printf(",\n");

    printf("      \"whole_static_libs\": ");
    OutputJsonArray(mod.wholeStaticLibs);
    printf(",\n");

    printf("      \"cflags\": ");
    OutputJsonArray(mod.cflags);
    printf(",\n");

    printf("      \"cppflags\": ");
    OutputJsonArray(mod.cppflags);
    printf(",\n");

    printf("      \"ldflags\": ");
    OutputJsonArray(mod.ldflags);
    printf(",\n");

    printf("      \"include_dirs\": ");
    OutputJsonArray(mod.includeDirs);
    printf(",\n");

    printf("      \"export_include_dirs\": ");
    OutputJsonArray(mod.exportIncludeDirs);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

}

// =====================================================================
// MkScanner — build engine (compile + link without ninja)
// =====================================================================

#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

namespace gormake {

std::string MkScanner::GetOutputPath(const MkModule& module) const {
  std::string dir = module.srcDir.empty() ? "." : module.srcDir;
  return dir + "/build/" + module.name;
}

std::string MkScanner::GetObjectPath(const MkModule& module,
                                      const std::string& src) const {
  std::string dir = module.srcDir.empty() ? "." : module.srcDir;
  return dir + "/build/obj/" + module.name + "/" + buildutil::BaseName(src) + ".o";
}

bool MkScanner::NeedsRecompile(const std::string& objFile,
                                const std::string& srcFile) const {
  return buildutil::NeedsRecompile(objFile, srcFile);
}

bool MkScanner::ExecuteCmd(const std::string& cmd) {
  if (dryRun_) { std::printf("  %s\n", cmd.c_str()); return true; }
  return buildutil::ExecuteCmd(cmd);
}

bool MkScanner::CompileSource(const MkModule& module, const std::string& src,
                               const std::string& objFile) {
  if (!NeedsRecompile(objFile, src)) {
    std::printf("  [skip] %s (up-to-date)\n", src.c_str());
    return true;
  }

  // Resolve source path relative to srcDir
  std::string srcPath = src;
  if (srcPath[0] != '/') {
    std::string dir = module.srcDir.empty() ? "." : module.srcDir;
    srcPath = dir + "/" + srcPath;
  }

  std::string compiler = buildutil::GetCompiler(src);

  std::string cmd = compiler + " -MMD -MP -c -o " + objFile + " " + srcPath;

  // Add cflags
  for (const auto& flag : module.cflags) {
    cmd += " " + flag;
  }
  // Add cppflags
  for (const auto& flag : module.cppflags) {
    cmd += " " + flag;
  }
  // Add include dirs
  for (const auto& inc : module.includeDirs) {
    if (inc[0] == '/') {
      cmd += " -I" + inc;
    } else {
      std::string dir = module.srcDir.empty() ? "." : module.srcDir;
      cmd += " -I" + dir + "/" + inc;
    }
  }
  // Add export include dirs
  for (const auto& inc : module.exportIncludeDirs) {
    if (inc[0] == '/') {
      cmd += " -I" + inc;
    } else {
      std::string dir = module.srcDir.empty() ? "." : module.srcDir;
      cmd += " -I" + dir + "/" + inc;
    }
  }

  cmd += " -Wall";

  // Create output directory
  std::string objDir = objFile.substr(0, objFile.find_last_of('/'));
  if (!dryRun_) buildutil::MkdirP(objDir);

  return ExecuteCmd(cmd);
}

bool MkScanner::LinkModule(const MkModule& module) {
  std::string outputPath = GetOutputPath(module);
  std::string dir = module.srcDir.empty() ? "." : module.srcDir;
  std::string outputDir = dir + "/build";
  if (!dryRun_) buildutil::MkdirP(outputDir);

  // Collect all object files
  std::string objFiles;
  for (const auto& src : module.srcs) {
    std::string objPath = GetObjectPath(module, src);
    objFiles += " " + objPath;
  }

  if (module.buildType == "BUILD_STATIC_LIBRARY") {
    std::string cmd = "ar rcs " + outputPath + ".a" + objFiles;
    return ExecuteCmd(cmd);
  }

  if (module.buildType == "BUILD_SHARED_LIBRARY") {
    std::string cmd = "g++ -shared -o " + outputPath + ".so" + objFiles;
    for (const auto& flag : module.ldflags) cmd += " " + flag;
    // Link shared libs
    for (const auto& lib : module.sharedLibs) {
      std::string libName = lib;
      if (libName.substr(0, 3) == "lib") libName = libName.substr(3);
      cmd += " -l" + libName;
    }
    return ExecuteCmd(cmd);
  }

  if (module.buildType == "BUILD_EXECUTABLE") {
    std::string cmd = "g++ -o " + outputPath + objFiles;
    for (const auto& flag : module.ldflags) cmd += " " + flag;

    // Link shared libs
    for (const auto& lib : module.sharedLibs) {
      std::string libName = lib;
      if (libName.substr(0, 3) == "lib") libName = libName.substr(3);
      cmd += " -l" + libName;
    }
    // Link static libs — use the .a file directly from the build output
    for (const auto& lib : module.staticLibs) {
      // Find the built static library
      std::string libPath;
      for (const auto& m : modules_) {
        if (m.name == lib && m.buildType == "BUILD_STATIC_LIBRARY") {
          libPath = GetOutputPath(m) + ".a";
          break;
        }
      }
      if (!libPath.empty()) {
        cmd += " " + libPath;
      } else {
        // Fallback to -l flag
        std::string libName = lib;
        if (libName.substr(0, 3) == "lib") libName = libName.substr(3);
        cmd += " -l" + libName;
      }
    }
    return ExecuteCmd(cmd);
  }

  // Unknown build type — skip
  return true;
}

bool MkScanner::BuildModule(const MkModule& module) {
  if (module.srcs.empty()) {
    std::printf("  [skip] %s (no sources)\n", module.name.c_str());
    return true;
  }

  std::printf("Building: %s (%s)\n", module.name.c_str(),
              module.buildType.c_str());

  // Compile all sources
  for (const auto& src : module.srcs) {
    std::string objFile = GetObjectPath(module, src);
    if (!CompileSource(module, src, objFile)) {
      std::fprintf(stderr, "gor_make: *** [%s] Error compiling %s\n",
                   module.name.c_str(), src.c_str());
      return false;
    }
  }

  // Link
  if (!LinkModule(module)) {
    std::fprintf(stderr, "gor_make: *** [%s] Error linking\n",
                 module.name.c_str());
    return false;
  }

  return true;
}

int MkScanner::BuildAll() {
  std::printf("Building %zu modules...\n", modules_.size());

  // Build static libraries first, then shared, then executables
  std::vector<std::string> order = {"BUILD_STATIC_LIBRARY",
                                     "BUILD_SHARED_LIBRARY",
                                     "BUILD_EXECUTABLE"};
  for (const auto& buildType : order) {
    for (const auto& module : modules_) {
      if (module.buildType == buildType) {
        if (!BuildModule(module)) return 1;
      }
    }
  }

  // Build any remaining modules
  for (const auto& module : modules_) {
    if (module.buildType != "BUILD_STATIC_LIBRARY" &&
        module.buildType != "BUILD_SHARED_LIBRARY" &&
        module.buildType != "BUILD_EXECUTABLE") {
      if (!BuildModule(module)) return 1;
    }
  }

  std::printf("Build complete.\n");
  return 0;
}

}  // namespace gormake
