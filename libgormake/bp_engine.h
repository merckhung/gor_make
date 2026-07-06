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

#ifndef GORMAKE_LIBGORMAKE_BP_ENGINE_H_
#define GORMAKE_LIBGORMAKE_BP_ENGINE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bp_parser.h"

namespace gormake {

// Build configuration options for Android.bp builds.
struct BpBuildOptions {
  std::string bp_file_path = "Android.bp";
  std::vector<std::string> goals;       // modules to build
  bool dry_run = false;                   // -n: print commands without executing
  bool silent = false;                   // -s: don't echo commands
  bool verbose = false;                  // -v: show all commands
  bool keep_going = false;                 // -k: keep going on errors
  bool clean = false;
  bool json_output = false;               // --json: output relationship JSON
  std::string build_dir = "out";          // output directory
  std::string arch = "x86_64";           // target architecture
  std::vector<std::string> cmd_line_vars;  // variable overrides
};

// Represents a resolved build module after defaults expansion.
struct BpBuildModule {
  std::string type;       // cc_binary, cc_library, cc_library_shared, etc.
  std::string name;       // module name
  std::string src_dir;     // directory containing the Android.bp file

  // Build properties (after defaults expansion)
  std::vector<std::string> srcs;
  std::vector<std::string> shared_libs;
  std::vector<std::string> static_libs;
  std::vector<std::string> whole_static_libs;
  std::vector<std::string> header_libs;
  std::vector<std::string> cflags;
  std::vector<std::string> cppflags;
  std::vector<std::string> ldflags;
  std::vector<std::string> include_dirs;
  std::vector<std::string> local_include_dirs;
  std::vector<std::string> export_include_dirs;
  std::vector<std::string> system_shared_libs;
  std::string stl;                    // "libc++", "libstdc++", "none", ""
  bool is_static = false;
  bool is_shared = false;
  bool is_binary = false;
  bool is_test = false;

  // Computed during build
  std::vector<std::string> object_files;
  std::string output_file;
  std::vector<std::string> resolved_shared_libs;
  std::vector<std::string> resolved_static_libs;

  // For genrule
  std::string gen_cmd;
  std::vector<std::string> gen_out;
  std::vector<std::string> gen_srcs;
};

// The main Android.bp build engine.
class BpEngine {
 public:
  BpEngine();
  ~BpEngine();

  // Run the build. Returns 0 on success, non-zero on error.
  int Run(const BpBuildOptions& opts);

  // Output all modules and their relationships as JSON to stdout.
  void OutputJson() const;

 private:
  // Parse all Android.bp files in the tree starting from the given path.
  bool ParseBpFiles(const std::string& root_path);

  // Parse all Android.bp files in a directory tree.
  void ParseBpDirectory(const std::string& dir_path);

  // Parse a single Android.bp file.
  bool ParseSingleBp(const std::string& path);

  // Resolve cc_defaults expansion for all modules.
  void ApplyDefaults();

  // Apply defaults from a cc_defaults module to a target module.
  void ApplyDefaultsToModule(const BpModule& defaults, BpBuildModule* module);

  // Convert a parsed BpModule to a BpBuildModule.
  std::unique_ptr<BpBuildModule> ConvertModule(const BpModule& bp_module,
                                                  const std::string& src_dir);

  // Resolve property as string list from BpValue.
  std::vector<std::string> GetStringList(const BpValue& val,
                                          const std::string& src_dir);

  // Resolve a single string property, expanding globs.
  std::vector<std::string> ResolveSrcs(const BpValue& val,
                                        const std::string& src_dir);

  // Find a module by name.
  BpBuildModule* FindModule(const std::string& name);

  // Build a module and all its dependencies.
  bool BuildModule(const std::string& name,
                   std::unordered_set<std::string>& visited,
                   std::unordered_set<std::string>& building);

  // Compile source files into object files.
  bool CompileModule(BpBuildModule* module);

  // Link a module into its final output.
  bool LinkModule(BpBuildModule* module);

  // Build a genrule.
  bool BuildGenrule(BpBuildModule* module);

  // Clean build outputs.
  void Clean();

  // Get the output path for a module.
  std::string GetOutputPath(const BpBuildModule& module) const;

  // Get the object file path for a source file.
  std::string GetObjectPath(const BpBuildModule& module,
                            const std::string& src_file) const;

  // Check if a file needs recompilation.
  bool NeedsRecompile(const std::string& obj_file,
                      const std::string& src_file,
                      const std::vector<std::string>& headers) const;

  // Execute a command, handling dry_run and silent flags.
  bool ExecuteCmd(const std::string& cmd, bool silent = false);

  // All parsed modules indexed by name.
  std::unordered_map<std::string, std::unique_ptr<BpBuildModule>> modules_;

  // Track which files have been parsed to avoid duplicates.
  std::unordered_set<std::string> parsed_files_;

  // All parsed BpFiles.
  std::vector<BpFile> bp_files_;

  // Build options.
  const BpBuildOptions* opts_ = nullptr;

  // Compiler and linker to use.
  std::string cc_ = "gcc";
  std::string cxx_ = "g++";
  std::string ar_ = "ar";

  // Common cflags added to all compilations.
  std::vector<std::string> common_cflags_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_BP_ENGINE_H_
