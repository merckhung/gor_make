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

#ifndef GORMAKE_LIBGORMAKE_RULEDB_H_
#define GORMAKE_LIBGORMAKE_RULEDB_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gormake {

// A single recipe line (one command in a rule).
struct RecipeLine {
  std::string text;       // raw command text (before variable expansion)
  bool silent = false;     // @ prefix: don't echo
  bool ignoreError = false; // - prefix: ignore errors
  bool alwaysRun = false;  // + prefix: always run even with -n
};

// A rule: target(s) -> prerequisites + recipe lines.
struct Rule {
  std::vector<std::string> targets;
  std::vector<std::string> prereqs;
  std::vector<std::string> orderOnlyPrereqs;  // after | separator
  std::vector<RecipeLine> recipes;
  bool isPhony = false;
  bool isDoubleColon = false;  // ::= vs : syntax
  bool isPattern = false;     // contains % in target
  std::string patternStem;    // for pattern rules

  Rule() = default;
};

// RuleDB stores all rules and provides lookup by target name.
class RuleDB {
 public:
  RuleDB();
  ~RuleDB();

  // Add a complete rule.  Takes ownership of its data.
  void AddRule(std::unique_ptr<Rule> rule);

  // Add a recipe to the most recently added rule with matching target.
  void AddRecipe(const std::string& target, RecipeLine recipe);

  // Find all rules for a given target.
  const std::vector<Rule*> FindRules(const std::string& target) const;

  // Find the first rule for a target (for simple cases).
  Rule* FindFirstRule(const std::string& target) const;

  // Find a pattern rule that matches the target.
  // Returns the matched rule and sets stem.
  Rule* FindPatternRule(const std::string& target, std::string& stem) const;

  // Mark a target as phony.
  void MarkPhony(const std::string& target);

  // Check if a target is phony.
  bool IsPhony(const std::string& target) const;

  // Get all targets.
  const std::unordered_map<std::string, std::vector<Rule*>>& GetAllRules() const {
    return targetToRules_;
  }

  // Get pattern rules.
  const std::vector<std::unique_ptr<Rule>>& GetPatternRules() const {
    return patternRules_;
  }

  // Get the default goal (first non-pattern target).
  std::string GetDefaultGoal() const;

 private:
  // Owned rule storage.
  std::vector<std::unique_ptr<Rule>> rules_;

  // Pattern rules (targets with %).
  std::vector<std::unique_ptr<Rule>> patternRules_;

  // Map: target name -> list of rules.
  std::unordered_map<std::string, std::vector<Rule*>> targetToRules_;

  // Set of phony targets.
  std::unordered_set<std::string> phonyTargets_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_RULEDB_H_
