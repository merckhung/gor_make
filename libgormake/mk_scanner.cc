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

#include "mk_scanner.h"
#include "build_engine_base.h"

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
  current_.src_dir = buildutil::DirName(path);
  variables_["LOCAL_PATH"] = "";  // Will be set by $(call my-dir) expansion

  std::string line;
  while (std::getline(file, line)) {
    ProcessLine(line);
  }

  return true;
}

void MkScanner::ScanDirectory(const std::string& dir_path) {
  for (auto& entry : fs::recursive_directory_iterator(dir_path)) {
    if (entry.path().filename() == "Android.mk") {
      std::string entry_str = entry.path().string();
      // Skip common output/build directories
      if (entry_str.find("/out/") != std::string::npos) continue;
      if (entry_str.find("/bazel-") != std::string::npos) continue;
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

const std::vector<MkModule>& MkScanner::GetModules() const {
  return modules_;
}

std::string MkScanner::StripComment(const std::string& line) const {
  std::string result;
  bool in_string = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && (i == 0 || line[i-1] != '\\')) {
      in_string = !in_string;
    }
    if (c == '#' && !in_string) {
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
          std::string var_name = s.substr(i + 2, close - i - 2);
          // Handle $(call my-dir) -> return the directory
          if (StartsWith(var_name, "call my-dir")) {
            result += current_.src_dir;
          } else if (StartsWith(var_name, "LOCAL_PATH")) {
            result += current_.src_dir;
          } else {
            auto it = variables_.find(var_name);
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
  size_t op_pos = std::string::npos;
  *op = '=';

  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '=') {
      if (i > 0 && (line[i-1] == ':' || line[i-1] == '?' || line[i-1] == '+')) {
        op_pos = i - 1;
        *op = line[i-1];
      } else {
        op_pos = i;
        *op = '=';
      }
      break;
    }
    if (line[i] == ' ' || line[i] == '\t') {
      // Continue looking
    }
  }

  if (op_pos == std::string::npos) return false;

  *name = Trim(line.substr(0, op_pos));
  size_t val_start = op_pos + 1;
  if (*op != '=') val_start = op_pos + 2;  // Skip the operator char and =
  *value = Trim(line.substr(val_start));

  return true;
}

void MkScanner::ProcessConditional(const std::string& line) {
  std::string trimmed = Trim(line);

  if (StartsWith(trimmed, "ifdef ") || StartsWith(trimmed, "ifndef ")) {
    bool is_ifndef = StartsWith(trimmed, "ifndef ");
    std::string var_name = Trim(trimmed.substr(6));
    bool defined = variables_.count(var_name) > 0 &&
                   !variables_[var_name].empty();
    // Also check environment
    if (!defined) {
      const char* env = getenv(var_name.c_str());
      if (env) defined = true;
    }
    bool result = is_ifndef ? !defined : defined;
    cond_stack_.push_back(result);
  } else if (StartsWith(trimmed, "ifeq ") || StartsWith(trimmed, "ifneq ")) {
    // Simplified: just push true (we can't fully evaluate)
    cond_stack_.push_back(true);
  } else if (trimmed == "else") {
    if (!cond_stack_.empty()) {
      cond_stack_.back() = !cond_stack_.back();
    }
  } else if (trimmed == "endif") {
    if (!cond_stack_.empty()) cond_stack_.pop_back();
  }

  // Recompute active_
  active_ = true;
  for (bool v : cond_stack_) {
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
  static const std::vector<std::string> known_types = {
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
  for (const auto& known : known_types) {
    if (name == known) return known;
  }

  // Step 3: BUILD_HOST_$(build_target) and similar -> BUILD_HOST_VARIABLE
  if (StartsWith(name, "BUILD_HOST_")) {
    return "BUILD_HOST_VARIABLE";
  }

  // Step 4: For any other pattern, keep the (stripped) inner name
  return name;
}

void MkScanner::FlushModule(const std::string& build_type) {
  if (!in_module_) return;

  // Normalize the build type: strip $(...) and map to canonical name
  current_.build_type = NormalizeBuildType(build_type);
  if (!current_.name.empty()) {
    modules_.push_back(current_);
  }

  // Reset current module
  current_ = MkModule();
  current_.path = current_.path;  // Keep path
  current_.src_dir = current_.src_dir;
  in_module_ = false;
}

void MkScanner::ProcessLine(const std::string& raw_line) {
  // Strip comments
  std::string line = StripComment(raw_line);
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
    std::string include_target = Trim(trimmed.substr(8));

    // Check for CLEAR_VARS before expansion
    if (include_target.find("CLEAR_VARS") != std::string::npos) {
      // Start a new module, preserving path/src_dir
      std::string saved_path = current_.path;
      std::string saved_src_dir = current_.src_dir;
      current_ = MkModule();
      current_.path = saved_path;
      current_.src_dir = saved_src_dir;
      in_module_ = true;
      return;
    }

    // Check for BUILD_* includes before expansion.
    // Check BUILD_HOST_* first — host build type names contain their non-host
    // counterparts as substrings (e.g. BUILD_HOST_EXECUTABLE contains
    // BUILD_EXECUTABLE), so they must be checked before the non-host types.
    if (include_target.find("BUILD_HOST_") != std::string::npos) {
      FlushModule(include_target);
      return;
    }
    if (include_target.find("BUILD_STATIC_LIBRARY") != std::string::npos) {
      FlushModule(include_target);
      return;
    }
    if (include_target.find("BUILD_SHARED_LIBRARY") != std::string::npos) {
      FlushModule(include_target);
      return;
    }
    if (include_target.find("BUILD_EXECUTABLE") != std::string::npos) {
      FlushModule(include_target);
      return;
    }
    if (include_target.find("BUILD_NATIVE_TEST") != std::string::npos) {
      FlushModule(include_target);
      return;
    }

    // Expand variables for other includes
    include_target = ExpandVars(include_target);

    // Try to read the file
    if (buildutil::FileExists(include_target)) {
      // Recursively scan included file
      auto saved_vars = variables_;
      auto saved_current = current_;
      bool saved_in_module = in_module_;

      ScanFile(include_target);

      variables_ = saved_vars;
      current_ = saved_current;
      in_module_ = saved_in_module;
    }
    return;
  }

  // Handle define/endef
  if (StartsWith(trimmed, "define ")) {
    // Skip multi-line define blocks
    return;
  }

  // Parse variable assignments
  std::string var_name, var_value;
  char op;
  if (ParseAssignment(trimmed, &var_name, &var_value, &op)) {
    // Expand variables in value
    var_value = ExpandVars(var_value);

    // Handle LOCAL_* variables when inside a module
    if (in_module_ && StartsWith(var_name, "LOCAL_")) {
      if (var_name == "LOCAL_MODULE") {
        current_.name = var_value;
      } else if (var_name == "LOCAL_SRC_FILES") {
        auto srcs = SplitList(var_value);
        if (op == '+') {
          current_.srcs.insert(current_.srcs.end(), srcs.begin(), srcs.end());
        } else {
          current_.srcs = srcs;
        }
      } else if (var_name == "LOCAL_CFLAGS") {
        auto flags = SplitList(var_value);
        if (op == '+') {
          current_.cflags.insert(current_.cflags.end(), flags.begin(), flags.end());
        } else {
          current_.cflags = flags;
        }
      } else if (var_name == "LOCAL_CPPFLAGS") {
        auto flags = SplitList(var_value);
        if (op == '+') {
          current_.cppflags.insert(current_.cppflags.end(), flags.begin(), flags.end());
        } else {
          current_.cppflags = flags;
        }
      } else if (var_name == "LOCAL_LDFLAGS") {
        auto flags = SplitList(var_value);
        if (op == '+') {
          current_.ldflags.insert(current_.ldflags.end(), flags.begin(), flags.end());
        } else {
          current_.ldflags = flags;
        }
      } else if (var_name == "LOCAL_SHARED_LIBRARIES") {
        auto libs = SplitList(var_value);
        if (op == '+') {
          current_.shared_libs.insert(current_.shared_libs.end(), libs.begin(), libs.end());
        } else {
          current_.shared_libs = libs;
        }
      } else if (var_name == "LOCAL_STATIC_LIBRARIES") {
        auto libs = SplitList(var_value);
        if (op == '+') {
          current_.static_libs.insert(current_.static_libs.end(), libs.begin(), libs.end());
        } else {
          current_.static_libs = libs;
        }
      } else if (var_name == "LOCAL_WHOLE_STATIC_LIBRARIES") {
        auto libs = SplitList(var_value);
        if (op == '+') {
          current_.whole_static_libs.insert(current_.whole_static_libs.end(), libs.begin(), libs.end());
        } else {
          current_.whole_static_libs = libs;
        }
      } else if (var_name == "LOCAL_C_INCLUDES") {
        auto dirs = SplitList(var_value);
        if (op == '+') {
          current_.include_dirs.insert(current_.include_dirs.end(), dirs.begin(), dirs.end());
        } else {
          current_.include_dirs = dirs;
        }
      } else if (var_name == "LOCAL_EXPORT_C_INCLUDE_DIRS") {
        auto dirs = SplitList(var_value);
        if (op == '+') {
          current_.export_include_dirs.insert(current_.export_include_dirs.end(), dirs.begin(), dirs.end());
        } else {
          current_.export_include_dirs = dirs;
        }
      }
      // Also store in variables_ for expansion
      variables_[var_name] = var_value;
    } else {
      // Regular variable assignment
      if (op == '+') {
        variables_[var_name] += " " + var_value;
      } else if (op == '?') {
        if (variables_.count(var_name) == 0) {
          variables_[var_name] = var_value;
        }
      } else {
        variables_[var_name] = var_value;
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
    std::string type_str = "unknown";
    if (mod.build_type == "BUILD_STATIC_LIBRARY") type_str = "cc_library_static";
    else if (mod.build_type == "BUILD_SHARED_LIBRARY") type_str = "cc_library_shared";
    else if (mod.build_type == "BUILD_EXECUTABLE") type_str = "cc_binary";
    else if (mod.build_type == "BUILD_NATIVE_TEST") type_str = "cc_test";
    else type_str = mod.build_type;

    printf("    {\n");
    printf("      \"name\": \"%s\",\n", JsonEscape(mod.name).c_str());
    printf("      \"type\": \"%s\",\n", JsonEscape(type_str).c_str());
    printf("      \"build_type\": \"%s\",\n", JsonEscape(mod.build_type).c_str());
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(mod.src_dir).c_str());
    printf("      \"path\": \"%s\",\n", JsonEscape(mod.path).c_str());

    printf("      \"srcs\": ");
    OutputJsonArray(mod.srcs);
    printf(",\n");

    printf("      \"shared_libs\": ");
    OutputJsonArray(mod.shared_libs);
    printf(",\n");

    printf("      \"static_libs\": ");
    OutputJsonArray(mod.static_libs);
    printf(",\n");

    printf("      \"whole_static_libs\": ");
    OutputJsonArray(mod.whole_static_libs);
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
    OutputJsonArray(mod.include_dirs);
    printf(",\n");

    printf("      \"export_include_dirs\": ");
    OutputJsonArray(mod.export_include_dirs);
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
  std::string dir = module.src_dir.empty() ? "." : module.src_dir;
  return dir + "/build/" + module.name;
}

std::string MkScanner::GetObjectPath(const MkModule& module,
                                      const std::string& src) const {
  std::string dir = module.src_dir.empty() ? "." : module.src_dir;
  return dir + "/build/obj/" + module.name + "/" + buildutil::BaseName(src) + ".o";
}

bool MkScanner::NeedsRecompile(const std::string& obj_file,
                                const std::string& src_file) const {
  return buildutil::NeedsRecompile(obj_file, src_file);
}

bool MkScanner::ExecuteCmd(const std::string& cmd) {
  if (dry_run_) { std::printf("  %s\n", cmd.c_str()); return true; }
  return buildutil::ExecuteCmd(cmd);
}

bool MkScanner::CompileSource(const MkModule& module, const std::string& src,
                               const std::string& obj_file) {
  if (!NeedsRecompile(obj_file, src)) {
    std::printf("  [skip] %s (up-to-date)\n", src.c_str());
    return true;
  }

  // Resolve source path relative to src_dir
  std::string src_path = src;
  if (src_path[0] != '/') {
    std::string dir = module.src_dir.empty() ? "." : module.src_dir;
    src_path = dir + "/" + src_path;
  }

  std::string compiler = buildutil::GetCompiler(src);

  std::string cmd = compiler + " -MMD -MP -c -o " + obj_file + " " + src_path;

  // Add cflags
  for (const auto& flag : module.cflags) {
    cmd += " " + flag;
  }
  // Add cppflags
  for (const auto& flag : module.cppflags) {
    cmd += " " + flag;
  }
  // Add include dirs
  for (const auto& inc : module.include_dirs) {
    if (inc[0] == '/') {
      cmd += " -I" + inc;
    } else {
      std::string dir = module.src_dir.empty() ? "." : module.src_dir;
      cmd += " -I" + dir + "/" + inc;
    }
  }
  // Add export include dirs
  for (const auto& inc : module.export_include_dirs) {
    if (inc[0] == '/') {
      cmd += " -I" + inc;
    } else {
      std::string dir = module.src_dir.empty() ? "." : module.src_dir;
      cmd += " -I" + dir + "/" + inc;
    }
  }

  cmd += " -Wall";

  // Create output directory
  std::string obj_dir = obj_file.substr(0, obj_file.find_last_of('/'));
  if (!dry_run_) buildutil::MkdirP(obj_dir);

  return ExecuteCmd(cmd);
}

bool MkScanner::LinkModule(const MkModule& module) {
  std::string output_path = GetOutputPath(module);
  std::string dir = module.src_dir.empty() ? "." : module.src_dir;
  std::string output_dir = dir + "/build";
  if (!dry_run_) buildutil::MkdirP(output_dir);

  // Collect all object files
  std::string obj_files;
  for (const auto& src : module.srcs) {
    std::string obj_path = GetObjectPath(module, src);
    obj_files += " " + obj_path;
  }

  if (module.build_type == "BUILD_STATIC_LIBRARY") {
    std::string cmd = "ar rcs " + output_path + ".a" + obj_files;
    return ExecuteCmd(cmd);
  }

  if (module.build_type == "BUILD_SHARED_LIBRARY") {
    std::string cmd = "g++ -shared -o " + output_path + ".so" + obj_files;
    for (const auto& flag : module.ldflags) cmd += " " + flag;
    // Link shared libs
    for (const auto& lib : module.shared_libs) {
      std::string lib_name = lib;
      if (lib_name.substr(0, 3) == "lib") lib_name = lib_name.substr(3);
      cmd += " -l" + lib_name;
    }
    return ExecuteCmd(cmd);
  }

  if (module.build_type == "BUILD_EXECUTABLE") {
    std::string cmd = "g++ -o " + output_path + obj_files;
    for (const auto& flag : module.ldflags) cmd += " " + flag;

    // Link shared libs
    for (const auto& lib : module.shared_libs) {
      std::string lib_name = lib;
      if (lib_name.substr(0, 3) == "lib") lib_name = lib_name.substr(3);
      cmd += " -l" + lib_name;
    }
    // Link static libs — use the .a file directly from the build output
    for (const auto& lib : module.static_libs) {
      // Find the built static library
      std::string lib_path;
      for (const auto& m : modules_) {
        if (m.name == lib && m.build_type == "BUILD_STATIC_LIBRARY") {
          lib_path = GetOutputPath(m) + ".a";
          break;
        }
      }
      if (!lib_path.empty()) {
        cmd += " " + lib_path;
      } else {
        // Fallback to -l flag
        std::string lib_name = lib;
        if (lib_name.substr(0, 3) == "lib") lib_name = lib_name.substr(3);
        cmd += " -l" + lib_name;
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
              module.build_type.c_str());

  // Compile all sources
  for (const auto& src : module.srcs) {
    std::string obj_file = GetObjectPath(module, src);
    if (!CompileSource(module, src, obj_file)) {
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
  for (const auto& build_type : order) {
    for (const auto& module : modules_) {
      if (module.build_type == build_type) {
        if (!BuildModule(module)) return 1;
      }
    }
  }

  // Build any remaining modules
  for (const auto& module : modules_) {
    if (module.build_type != "BUILD_STATIC_LIBRARY" &&
        module.build_type != "BUILD_SHARED_LIBRARY" &&
        module.build_type != "BUILD_EXECUTABLE") {
      if (!BuildModule(module)) return 1;
    }
  }

  std::printf("Build complete.\n");
  return 0;
}

}  // namespace gormake
