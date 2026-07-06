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

#include "rule_db.h"

#include <memory>

namespace gormake {

RuleDB::RuleDB() {
}

RuleDB::~RuleDB() {
}

void RuleDB::AddRule(std::unique_ptr<Rule> rule) {
  Rule* raw = rule.get();

  // Check if it's a pattern rule
  bool has_pattern = false;
  for (const auto& t : rule->targets) {
    if (t.find('%') != std::string::npos) {
      has_pattern = true;
      break;
    }
  }

  if (has_pattern) {
    rule->is_pattern = true;
    pattern_rules_.push_back(std::move(rule));
  } else {
    for (const auto& t : rule->targets) {
      target_to_rules_[t].push_back(raw);
    }
    rules_.push_back(std::move(rule));
  }
}

void RuleDB::AddRecipe(const std::string& target, RecipeLine recipe) {
  auto it = target_to_rules_.find(target);
  if (it != target_to_rules_.end() && !it->second.empty()) {
    it->second.back()->recipes.push_back(std::move(recipe));
  }
}

const std::vector<Rule*> RuleDB::FindRules(const std::string& target) const {
  auto it = target_to_rules_.find(target);
  if (it != target_to_rules_.end()) {
    return it->second;
  }
  return {};
}

Rule* RuleDB::FindFirstRule(const std::string& target) const {
  auto it = target_to_rules_.find(target);
  if (it != target_to_rules_.end() && !it->second.empty()) {
    return it->second.front();
  }
  return nullptr;
}

Rule* RuleDB::FindPatternRule(const std::string& target,
                               std::string& stem) const {
  for (const auto& rule : pattern_rules_) {
    for (const auto& pat : rule->targets) {
      size_t pct = pat.find('%');
      if (pct == std::string::npos) continue;
      std::string prefix = pat.substr(0, pct);
      std::string suffix = pat.substr(pct + 1);
      if (target.size() >= prefix.size() + suffix.size()
          && target.compare(0, prefix.size(), prefix) == 0
          && target.compare(target.size() - suffix.size(),
                            suffix.size(), suffix) == 0) {
        stem = target.substr(prefix.size(),
                             target.size() - prefix.size() - suffix.size());
        // Check that at least one prerequisite exists or can be made
        return rule.get();
      }
    }
  }
  return nullptr;
}

void RuleDB::MarkPhony(const std::string& target) {
  phony_targets_.insert(target);
}

bool RuleDB::IsPhony(const std::string& target) const {
  return phony_targets_.count(target) > 0;
}

std::string RuleDB::GetDefaultGoal() const {
  for (const auto& rule : rules_) {
    for (const auto& t : rule->targets) {
      if (t.find('%') == std::string::npos && !t.empty()) {
        return t;
      }
    }
  }
  return "";
}

}  // namespace gormake
