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

#ifndef GORMAKE_LIBGORMAKE_MK_SCANNER_H_
#define GORMAKE_LIBGORMAKE_MK_SCANNER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gormake {

// Represents a module extracted from an Android.mk file.
// Android.mk uses LOCAL_* variables within include $(CLEAR_VARS) ... 
// include $(BUILD_*) blocks to define modules.
struct MkModule {
  std::string name;              // LOCAL_MODULE
  std::string build_type;         // BUILD_STATIC_LIBRARY, BUILD_SHARED_LIBRARY, etc.
  std::string src_dir;            // Directory of the Android.mk file
  std::vector<std::string> srcs;  // LOCAL_SRC_FILES
  std::vector<std::string> cflags;       // LOCAL_CFLAGS
  std::vector<std::string> cppflags;    // LOCAL_CPPFLAGS
  std::vector<std::string> ldflags;     // LOCAL_LDFLAGS
  std::vector<std::string> shared_libs;  // LOCAL_SHARED_LIBRARIES
  std::vector<std::string> static_libs;  // LOCAL_STATIC_LIBRARIES
  std::vector<std::string> whole_static_libs;  // LOCAL_WHOLE_STATIC_LIBRARIES
  std::vector<std::string> include_dirs;       // LOCAL_C_INCLUDES
  std::vector<std::string> export_include_dirs;  // LOCAL_EXPORT_C_INCLUDE_DIRS
  std::string path;              // Path to the Android.mk file
};

// Scans Android.mk files and extracts module definitions.
// Unlike the full Make engine, this is a lightweight scanner that looks for
// LOCAL_* variable assignments within CLEAR_VARS / BUILD_* blocks.
class MkScanner {
 public:
  MkScanner();
  ~MkScanner();

  // Scan a single Android.mk file.
  // Returns true on success (even if no modules found).
  bool ScanFile(const std::string& path);

  // Scan all Android.mk files in a directory tree.
  void ScanDirectory(const std::string& dir_path);

  // Get all extracted modules.
  const std::vector<MkModule>& GetModules() const;

  // Output all modules as JSON to stdout.
  void OutputJson() const;
  void SetDryRun(bool v) { dry_run_ = v; }
  void SetJobs(int j) { jobs_ = j; }

  // Build all modules. Returns 0 on success.
  int BuildAll();

 private:
  // Process a line, tracking LOCAL_* variables.
  void ProcessLine(const std::string& line);

  // Build a single module (compile + link).
  bool BuildModule(const MkModule& module);

  // Compile a source file into an object file.
  bool CompileSource(const MkModule& module, const std::string& src,
                     const std::string& obj_file);

  // Link a module into its final output.
  bool LinkModule(const MkModule& module);

  // Get output path for a module.
  std::string GetOutputPath(const MkModule& module) const;

  // Get object file path for a source file.
  std::string GetObjectPath(const MkModule& module,
                            const std::string& src) const;

  // Execute a command.
  bool ExecuteCmd(const std::string& cmd);

  // Check if file needs recompilation.
  bool NeedsRecompile(const std::string& obj_file,
                      const std::string& src_file) const;

  // Flush the current module when BUILD_* is encountered.
  void FlushModule(const std::string& build_type);

  // Parse a variable assignment (VAR := value or VAR = value or VAR += value).
  bool ParseAssignment(const std::string& line, std::string* name,
                       std::string* value, char* op);

  // Expand simple $(VAR) references in a value string.
  std::string ExpandVars(const std::string& s) const;

  // Split a string by whitespace, handling line continuations.
  std::vector<std::string> SplitList(const std::string& s) const;

  // Strip comments from a line.
  std::string StripComment(const std::string& line) const;

  // Handle ifdef/ifndef/ifeq/ifneq/else/endif
  void ProcessConditional(const std::string& line);

  std::vector<MkModule> modules_;
  std::map<std::string, std::string> variables_;  // Regular variables

  // Current module being built
  MkModule current_;
  bool in_module_ = false;  // True between CLEAR_VARS and BUILD_*

  // Conditional stack: true = active, false = inactive
  std::vector<bool> cond_stack_;
  bool dry_run_ = false;
  int jobs_ = 1;
  bool active_ = true;  // Combined result of cond_stack_
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_MK_SCANNER_H_
