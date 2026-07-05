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

#ifndef GORMAKE_GNSCANNER_H_
#define GORMAKE_GNSCANNER_H_

#include <map>
#include <string>
#include <vector>

namespace gormake {

// Represents a single GN target parsed from a BUILD.gn file.
// Target types include: executable, static_library, shared_library,
// source_set, group, config, action.
struct GnTarget {
  std::string name;
  std::string type;  // executable, static_library, shared_library, source_set,
                     // group, config, action
  std::string srcDir;
  std::vector<std::string> srcs;
  std::vector<std::string> deps;
  std::vector<std::string> cflags;
  std::vector<std::string> cppflags;
  std::vector<std::string> ldflags;
  std::vector<std::string> includeDirs;
  std::vector<std::string> defines;
  std::vector<std::string> configs;      // public_configs
  std::vector<std::string> publicDeps;
  std::string path;
};

// Scans GN (BUILD.gn) build files and extracts target information.
//
// Supported GN syntax features:
//   - Target types: executable, static_library, shared_library, source_set,
//     group, config, action
//   - String literals: "string"
//   - Lists: ["a", "b"]
//   - Variable assignment: myvar = ["a"]
//   - Variable reference: myvar (bare name) or $myvar
//   - += operator: cflags += ["-O2"]
//   - Comments: # single line (stripped, but not inside strings)
//   - Block scope: target_type("name") { ... }
//   - Line continuations (backslash at end of line)
//   - Nested braces tracked by depth
class GnScanner {
 public:
  GnScanner();
  ~GnScanner();

  // Scans a single BUILD.gn file. Returns true on success.
  bool ScanFile(const std::string& path);

  // Recursively scans a directory tree for BUILD.gn files.
  // Skips directories matching /out/, /bazel-*, /.git/.
  void ScanDirectory(const std::string& dirPath);

  // Returns all targets collected from scanning.
  const std::vector<GnTarget>& GetTargets() const;

  // Outputs the collected targets as JSON to stdout.
  // The "format" field is set to "build.gn".
  void OutputJson() const;

  // Build all targets. Returns 0 on success.
  int BuildAll();

 private:
  // Processes a single logical line (after line-continuation joining).
  void ProcessLine(const std::string& line);

  // Strips # comments from a line, respecting string literals.
  std::string StripComment(const std::string& line) const;

  // Parses a GN list literal: ["a", "b", ...] returning the string elements.
  std::vector<std::string> ParseList(const std::string& s) const;

  // Expands variable references in a value string.
  // Supports both bare-name references and $name / ${name} forms.
  std::string ExpandVars(const std::string& s) const;

  // Resolves a possibly-bare variable reference to a list of strings.
  // If the name is a known list variable, returns its value.
  // Otherwise returns s split as a fallback.
  std::vector<std::string> ResolveValue(const std::string& s) const;

  // Helper: assigns/appends a list value to a target property.
  void AssignProperty(const std::string& name,
                      const std::vector<std::string>& values, bool append);

  // --- Build engine methods ---
  bool BuildTarget(const GnTarget& target);
  bool CompileSource(const GnTarget& target, const std::string& src,
                     const std::string& objFile);
  bool LinkTarget(const GnTarget& target);
  std::string GetOutputPath(const GnTarget& target) const;
  std::string GetObjectPath(const GnTarget& target,
                            const std::string& src) const;
  bool ExecuteCmd(const std::string& cmd);
  bool NeedsRecompile(const std::string& objFile,
                      const std::string& srcFile) const;

  std::vector<GnTarget> targets_;
  // Top-level (and target-local) variables: name -> string value.
  std::map<std::string, std::string> variables_;
  // List-typed variables: name -> list of strings.
  std::map<std::string, std::vector<std::string>> listVariables_;

  // Current target being built (empty when not inside a target block).
  GnTarget current_;
  bool inTarget_;
  int braceDepth_;  // Nesting depth inside the current target block.
};

}  // namespace gormake

#endif  // GORMAKE_GNSCANNER_H_
