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

#ifndef GORMAKE_LIBGORMAKE_BPENGINE_H_
#define GORMAKE_LIBGORMAKE_BPENGINE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bpparser.h"

namespace gormake {

// Build configuration options for Android.bp builds.
struct BpBuildOptions {
  std::string bpFilePath = "Android.bp";
  std::vector<std::string> goals;       // modules to build
  bool dryRun = false;                   // -n: print commands without executing
  bool silent = false;                   // -s: don't echo commands
  bool verbose = false;                  // -v: show all commands
  bool keepGoing = false;                 // -k: keep going on errors
  bool clean = false;                    // clean build outputs
  std::string buildDir = "out";          // output directory
  std::string arch = "x86_64";           // target architecture
  std::vector<std::string> cmdLineVars;  // variable overrides
};

// Represents a resolved build module after defaults expansion.
struct BpBuildModule {
  std::string type;       // cc_binary, cc_library, cc_library_shared, etc.
  std::string name;       // module name
  std::string srcDir;     // directory containing the Android.bp file

  // Build properties (after defaults expansion)
  std::vector<std::string> srcs;
  std::vector<std::string> sharedLibs;
  std::vector<std::string> staticLibs;
  std::vector<std::string> wholeStaticLibs;
  std::vector<std::string> headerLibs;
  std::vector<std::string> cflags;
  std::vector<std::string> cppflags;
  std::vector<std::string> ldflags;
  std::vector<std::string> includeDirs;
  std::vector<std::string> localIncludeDirs;
  std::vector<std::string> exportIncludeDirs;
  std::vector<std::string> systemSharedLibs;
  std::string stl;                    // "libc++", "libstdc++", "none", ""
  bool isStatic = false;
  bool isShared = false;
  bool isBinary = false;
  bool isTest = false;

  // Computed during build
  std::vector<std::string> objectFiles;
  std::string outputFile;
  std::vector<std::string> resolvedSharedLibs;
  std::vector<std::string> resolvedStaticLibs;

  // For genrule
  std::string genCmd;
  std::vector<std::string> genOut;
  std::vector<std::string> genSrcs;
};

// The main Android.bp build engine.
class BpEngine {
 public:
  BpEngine();
  ~BpEngine();

  // Run the build. Returns 0 on success, non-zero on error.
  int Run(const BpBuildOptions& opts);

 private:
  // Parse all Android.bp files in the tree starting from the given path.
  bool ParseBpFiles(const std::string& rootPath);

  // Parse a single Android.bp file.
  bool ParseSingleBp(const std::string& path);

  // Resolve cc_defaults expansion for all modules.
  void ApplyDefaults();

  // Apply defaults from a cc_defaults module to a target module.
  void ApplyDefaultsToModule(const BpModule& defaults, BpBuildModule* module);

  // Convert a parsed BpModule to a BpBuildModule.
  std::unique_ptr<BpBuildModule> ConvertModule(const BpModule& bpModule,
                                                  const std::string& srcDir);

  // Resolve property as string list from BpValue.
  std::vector<std::string> GetStringList(const BpValue& val,
                                          const std::string& srcDir);

  // Resolve a single string property, expanding globs.
  std::vector<std::string> ResolveSrcs(const BpValue& val,
                                        const std::string& srcDir);

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
                            const std::string& srcFile) const;

  // Check if a file needs recompilation.
  bool NeedsRecompile(const std::string& objFile,
                      const std::string& srcFile,
                      const std::vector<std::string>& headers) const;

  // Execute a command, handling dryRun and silent flags.
  bool ExecuteCmd(const std::string& cmd, bool silent = false);

  // All parsed modules indexed by name.
  std::unordered_map<std::string, std::unique_ptr<BpBuildModule>> modules_;

  // Track which files have been parsed to avoid duplicates.
  std::unordered_set<std::string> parsedFiles_;

  // All parsed BpFiles.
  std::vector<BpFile> bpFiles_;

  // Build options.
  const BpBuildOptions* opts_ = nullptr;

  // Compiler and linker to use.
  std::string cc_ = "gcc";
  std::string cxx_ = "g++";
  std::string ar_ = "ar";

  // Common cflags added to all compilations.
  std::vector<std::string> commonCflags_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_BPENGINE_H_
