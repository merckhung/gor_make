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

#ifndef GORMAKE_LIBGORMAKE_SCONSCANNER_H_
#define GORMAKE_LIBGORMAKE_SCONSCANNER_H_

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
  std::string srcDir;
  std::vector<std::string> srcs;
  std::vector<std::string> cflags;
  std::vector<std::string> cppflags;
  std::vector<std::string> ldflags;
  std::vector<std::string> includeDirs;
  std::vector<std::string> defines;
  std::vector<std::string> linkLibs;
  std::string path;
};

// Scans SConstruct/SConscript files and extracts target definitions.
class SconScanner {
 public:
  SconScanner();
  ~SconScanner();

  bool ScanFile(const std::string& path);
  void ScanDirectory(const std::string& dirPath);
  const std::vector<SconTarget>& GetTargets() const;
  void OutputJson() const;
  void SetDryRun(bool v) { dryRun_ = v; }

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
  bool MatchFunc(const std::string& line, const std::string& funcName,
                 std::string* args) const;

  // JSON helpers
  static std::string JsonEscape(const std::string& s);
  static void OutputArray(const std::vector<std::string>& arr);

  // --- Build engine methods ---
  bool BuildTarget(const SconTarget& target);
  bool CompileSource(const SconTarget& target, const std::string& src,
                     const std::string& objFile);
  bool LinkTarget(const SconTarget& target);
  std::string GetOutputPath(const SconTarget& target) const;
  std::string GetObjectPath(const SconTarget& target,
                            const std::string& src) const;
  bool ExecuteCmd(const std::string& cmd);
  bool NeedsRecompile(const std::string& objFile,
                      const std::string& srcFile) const;

  std::vector<SconTarget> targets_;

  // Track visited files to avoid infinite recursion
  std::unordered_set<std::string> visitedFiles_;

  // Environment variables (simulated SCons env)
  std::vector<std::string> envCflags_;
  std::vector<std::string> envIncludeDirs_;
  std::vector<std::string> envDefines_;
  std::vector<std::string> envLibs_;

  // Current file state
  std::string currentPath_;
  std::string currentSrcDir_;
  bool dryRun_ = false;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_SCONSCANNER_H_
