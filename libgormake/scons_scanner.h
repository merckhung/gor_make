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

#ifndef GORMAKE_LIBGORMAKE_SCONS_SCANNER_H_
#define GORMAKE_LIBGORMAKE_SCONS_SCANNER_H_

#include <cstdint>
#include <map>
#include <unordered_set>
#include <string>
#include <vector>

namespace gormake {

// Represents a target extracted from SCons (SConstruct/SConscript) files.
struct SconTarget {
  std::string name;
  std::string type;  // library, shared_library, program, source, gtest, simobject
  std::string src_dir;
  std::vector<std::string> srcs;
  std::vector<std::string> cflags;
  std::vector<std::string> cppflags;
  std::vector<std::string> ldflags;
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> link_libs;
  std::string path;
};

// Scans SConstruct/SConscript files and extracts target definitions.
class SconScanner {
 public:
  SconScanner();
  ~SconScanner();

  bool ScanFile(const std::string& path);
  void ScanDirectory(const std::string& dir_path);
  const std::vector<SconTarget>& GetTargets() const;
  void OutputJson() const;
  void SetDryRun(bool v) { dry_run_ = v; }
  void SetJobs(int j) { jobs_ = j; }

  // Build all targets. Returns 0 on success.
  int BuildAll();

 private:
  void ProcessLine(const std::string& line);

  // Extract string arguments from a function call: foo('a', 'b') -> ['a', 'b']
  std::vector<std::string> ExtractStringArgs(const std::string& s) const;

  // Extract the function name from: foo(...) or env.foo(...)
  std::string ExtractFuncName(const std::string& s) const;

  // Extract list literal: ['a', 'b', 'c']
  std::vector<std::string> ExtractList(const std::string& s) const;

  // Check if a function call matches a pattern
  bool MatchFunc(const std::string& line, const std::string& func_name,
                 std::string* args) const;

  // JSON helpers
  static std::string JsonEscape(const std::string& s);
  static void OutputArray(const std::vector<std::string>& arr);

  // --- Build engine methods ---
  bool BuildTarget(const SconTarget& target);
  bool CompileSource(const SconTarget& target, const std::string& src,
                     const std::string& obj_file);
  bool LinkTarget(const SconTarget& target);
  std::string GetOutputPath(const SconTarget& target) const;
  std::string GetObjectPath(const SconTarget& target,
                            const std::string& src) const;
  bool ExecuteCmd(const std::string& cmd);
  bool NeedsRecompile(const std::string& obj_file,
                      const std::string& src_file) const;

  std::vector<SconTarget> targets_;

  // Track visited files to avoid infinite recursion
  std::unordered_set<std::string> visited_files_;

  // Environment variables (simulated SCons env)
  std::vector<std::string> env_cflags_;
  std::vector<std::string> env_include_dirs_;
  std::vector<std::string> env_defines_;
  std::vector<std::string> env_libs_;

  // Current file state
  std::string current_path_;
  std::string current_src_dir_;
  bool dry_run_ = false;
  int jobs_ = 1;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_SCONS_SCANNER_H_
