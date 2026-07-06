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

#ifndef GORMAKE_LIBGORMAKE_CMAKE_SCANNER_H_
#define GORMAKE_LIBGORMAKE_CMAKE_SCANNER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gormake {

// Represents a target extracted from a CMakeLists.txt file.
struct CmakeTarget {
  std::string name;
  std::string type;       // executable, static_library, shared_library, interface_library
  std::string src_dir;
  std::vector<std::string> srcs;
  std::vector<std::string> cflags;
  std::vector<std::string> cppflags;
  std::vector<std::string> ldflags;
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> link_libs;       // target_link_libraries
  std::vector<std::string> compile_options;
  std::string path;
};

// Scans CMakeLists.txt files and extracts target definitions.
class CmakeScanner {
 public:
  CmakeScanner();
  ~CmakeScanner();

  bool ScanFile(const std::string& path);
  void ScanDirectory(const std::string& dir_path);
  const std::vector<CmakeTarget>& GetTargets() const;
  void OutputJson() const;
  void SetDryRun(bool v) { dry_run_ = v; }
  void SetJobs(int j) { jobs_ = j; }

  // Build all targets. Returns 0 on success.
  int BuildAll();

 private:
  void ProcessLine(const std::string& line);
  void FlushTarget(const std::string& type, const std::string& name);

  // Parse a CMake command: command(args)
  bool ParseCommand(const std::string& line, std::string* cmd,
                    std::string* args);

  // Split CMake arguments (handles quoted strings, variables)
  std::vector<std::string> ParseArgs(const std::string& args);

  // Expand ${VAR} references
  std::string ExpandVars(const std::string& s) const;

  // Strip comments
  std::string StripComment(const std::string& line) const;

  // JSON helpers
  static std::string JsonEscape(const std::string& s);
  static void OutputArray(const std::vector<std::string>& arr);

  // Build a single target (compile + link).
  bool BuildTarget(const CmakeTarget& target);

  // Compile a source file into an object file.
  bool CompileSource(const CmakeTarget& target, const std::string& src,
                     const std::string& obj_file);

  // Link a target into its final output.
  bool LinkTarget(const CmakeTarget& target);

  // Get output path for a target.
  std::string GetOutputPath(const CmakeTarget& target) const;

  // Get object file path for a source file.
  std::string GetObjectPath(const CmakeTarget& target,
                            const std::string& src) const;

  // Execute a command.
  bool ExecuteCmd(const std::string& cmd);

  // Check if file needs recompilation.
  bool NeedsRecompile(const std::string& obj_file,
                      const std::string& src_file) const;

  std::vector<CmakeTarget> targets_;
  std::map<std::string, std::string> variables_;

  // Current target being built
  CmakeTarget current_;
  bool in_target_ = false;
  std::string current_target_name_;

  // Track multi-line commands
  std::string pending_line_;
  int paren_depth_ = 0;

  // If/else stack
  std::vector<bool> cond_stack_;
  bool dry_run_ = false;
  int jobs_ = 1;
  bool active_ = true;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_CMAKE_SCANNER_H_
