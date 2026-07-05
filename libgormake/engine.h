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
 * distributed under the License is distributed on an "AS IS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GORMAKE_LIBGORMAKE_ENGINE_H_
#define GORMAKE_LIBGORMAKE_ENGINE_H_

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "vardb.h"
#include "ruledb.h"

namespace gormake {

struct MakeOptions {
  std::string makefilePath = "Makefile";
  std::vector<std::string> goals;       // targets to build
  std::vector<std::string> cmdLineVars; // VAR=value from command line
  std::string directory;                // -C directory
  bool dryRun = false;                  // -n: don't execute, just print
  bool silent = false;                  // -s: don't echo commands
  bool keepGoing = false;               // -k: keep going on errors
  bool ignoreErrors = false;            // -i: ignore recipe errors
  bool alwaysMake = false;              // -B: unconditionally rebuild
  bool printDir = false;               // -w: print directory
  int jobs = 1;                         // -j: parallel jobs (1=serial)
};

class Engine {
 public:
  Engine();
  ~Engine();

  // Run the full make pipeline. Returns 0 on success, non-zero on error.
  int Run(const MakeOptions& opts);

 private:
  // Parse a makefile and populate vars_ and rules_.
  bool ParseMakefile(const std::string& path);

  // Parse a single line from a makefile.
  // Returns true if the line was handled.
  bool ProcessLine(const std::string& line, int lineNum);

  // Process include directive.
  void ProcessInclude(const std::string& args);

  // Process conditional directive (ifeq, ifneq, ifdef, ifndef).
  // Returns true if subsequent lines should be processed.
  bool ProcessConditional(const std::string& directive,
                          const std::string& args);

  // Build a target with dependency resolution.
  // Returns true if target was built or is up-to-date.
  bool BuildTarget(const std::string& target,
                   std::unordered_set<std::string>& visited,
                   std::unordered_set<std::string>& building);

  // Execute the recipe for a rule.
  bool ExecuteRecipe(const Rule* rule, const std::string& target,
                      const std::string& stem);

  // Check if target needs rebuilding based on timestamps.
  bool NeedsRebuild(const std::string& target,
                    const std::vector<std::string>& prereqs) const;

  // Get file modification time. Returns 0 if file doesn't exist.
  long GetFileMtime(const std::string& path) const;

  VariableDB vars_;
  RuleDB rules_;
  std::vector<MakeOptions> optsStack_;  // for nested make calls
  const MakeOptions* opts_ = nullptr;

  // Conditional state stack for ifeq/else/endif
  struct CondState {
    bool active;       // whether lines should be processed
    bool parentActive;  // whether parent conditional is active
    bool elseSeen;      // whether else has been seen
  };
  std::vector<CondState> condStack_;

  // Track targets currently being built (cycle detection)
  std::unordered_set<std::string> building_;

  // Current default goal
  std::string defaultGoal_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_ENGINE_H_
