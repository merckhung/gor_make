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

#include "engine.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gormake {

// Helpers for line processing --------------------------------------------

// Strip leading whitespace from a string (returns a view-like substring).
static std::string LStrip(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

// Strip trailing whitespace.
static std::string RStrip(const std::string& s) {
  size_t end = s.size();
  while (end > 0 && (s[end-1] == ' ' || s[end-1] == '\t' ||
                     s[end-1] == '\r' || s[end-1] == '\n')) {
    end--;
  }
  return s.substr(0, end);
}

static std::string Strip(const std::string& s) {
  return RStrip(LStrip(s));
}

// Split a string by whitespace into words.
static std::vector<std::string> SplitWords(const std::string& s) {
  std::vector<std::string> result;
  std::string current;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) result.push_back(current);
  return result;
}

// Handle line continuation (backslash-newline).
static std::string JoinContinuations(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\\' && i + 1 < input.size()) {
      if (input[i+1] == '\n') {
        // Line continuation: replace with single space, skip newline
        result += ' ';
        i++;  // skip newline
        // Skip leading whitespace on next line
        while (i + 1 < input.size() && (input[i+1] == ' ' || input[i+1] == '\t')) {
          i++;
        }
      } else if (input[i+1] == '\r' && i + 2 < input.size() &&
                 input[i+2] == '\n') {
        result += ' ';
        i += 2;
        while (i + 1 < input.size() && (input[i+1] == ' ' || input[i+1] == '\t')) {
          i++;
        }
      } else {
        result += input[i];
      }
    } else {
      result += input[i];
    }
  }
  return result;
}

// Trim leading tabs from a recipe line (keeping recipe context).
static std::string TrimRecipePrefix(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && s[i] == '\t') i++;
  return s.substr(i);
}

// Parse a recipe line, extracting @, -, + prefixes.
static RecipeLine ParseRecipeLine(const std::string& raw) {
  RecipeLine recipe;
  std::string text = raw;
  // Strip leading whitespace but not tabs (tabs indicate recipe)
  text = LStrip(text);

  // Process prefix chars
  while (!text.empty()) {
    if (text[0] == '@') {
      recipe.silent = true;
      text = text.substr(1);
    } else if (text[0] == '-') {
      recipe.ignore_error = true;
      text = text.substr(1);
    } else if (text[0] == '+') {
      recipe.always_run = true;
      text = text.substr(1);
    } else {
      break;
    }
    text = LStrip(text);
  }
  recipe.text = text;
  return recipe;
}

// Engine implementation --------------------------------------------------

Engine::Engine() {
  vars_.ImportEnvironment();
}

Engine::~Engine() {
}

int Engine::Run(const MakeOptions& opts) {
  opts_ = &opts;

  // Change directory if requested
  if (!opts.directory.empty()) {
    if (chdir(opts.directory.c_str()) != 0) {
      fprintf(stderr, "gor_make: cannot chdir to %s: %s\n",
              opts.directory.c_str(), strerror(errno));
      return 1;
    }
  }

  // Process command-line variable assignments
  for (const auto& cv : opts.cmd_line_vars) {
    // Parse VAR=value or VAR:=value or VAR+=value or VAR?=value
    size_t op_pos = std::string::npos;
    char op = '=';
    VarFlavor flavor = VarFlavor::FLAVOR_RECURSIVE;

    // Find the operator
    for (size_t i = 0; i < cv.size(); ++i) {
      if (cv[i] == '=' ) {
        // Check if previous char is : ? or +
        if (i > 0 && (cv[i-1] == ':' || cv[i-1] == '?' || cv[i-1] == '+')) {
          op_pos = i - 1;
          op = cv[i-1];
          flavor = VarFlavor::FLAVOR_SIMPLE;
        } else {
          op_pos = i;
          op = '=';
          flavor = VarFlavor::FLAVOR_RECURSIVE;
        }
        break;
      }
    }

    if (op_pos != std::string::npos) {
      std::string name, value;
      if (op == '=') {
        name = cv.substr(0, op_pos);
        value = cv.substr(op_pos + 1);
      } else {
        name = cv.substr(0, op_pos);
        value = cv.substr(op_pos + 2);
      }
      name = Strip(name);
      value = Strip(value);
      bool append = (op == '+');
      if (op == '?') {
        if (vars_.IsDefined(name)) continue;
      }
      vars_.Set(name, value, flavor, VarOrigin::ORIGIN_COMMAND, append);
    }
  }

  // Parse the makefile
  if (!ParseMakefile(opts.makefile_path)) {
    fprintf(stderr, "gor_make: *** No rule to make target '%s'. Stop.\n",
            opts.makefile_path.c_str());
    return 2;
  }

  // JSON output mode: print rule relationships and exit
  if (opts.json_output) {
    OutputJson();
    return 0;
  }

  // Determine goals
  std::vector<std::string> goals = opts.goals;
  if (goals.empty()) {
    std::string dg = vars_.Expand("$(.DEFAULT_GOAL)");
    if (!dg.empty()) {
      goals = SplitWords(dg);
    }
    if (goals.empty()) {
      dg = rules_.GetDefaultGoal();
      if (!dg.empty()) {
        goals.push_back(dg);
      }
    }
  }

  if (goals.empty()) {
    fprintf(stderr, "gor_make: *** No targets.  Stop.\n");
    return 2;
  }

  // Build each goal
  int result = 0;
  for (const auto& goal : goals) {
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> building;
    if (!BuildTarget(goal, visited, building)) {
      result = 1;
      if (!opts.keep_going) break;
    }
  }

  return result;
}

bool Engine::ParseMakefile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    // Try GNUmakefile, makefile
    file.open("GNUmakefile");
    if (!file.is_open()) {
      file.open("makefile");
      if (!file.is_open()) {
        return false;
      }
    }
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
  file.close();

  // Handle line continuations
  content = JoinContinuations(content);

  // Process line by line
  std::istringstream stream(content);
  std::string line;
  int line_num = 0;
  Rule* current_rule = nullptr;

  while (std::getline(stream, line)) {
    line_num++;

    // Check if conditional is active
    bool cond_active = true;
    for (const auto& cs : cond_stack_) {
      if (!cs.active) { cond_active = false; break; }
    }

    // Handle conditionals first (even if inactive, to track nesting)
    std::string stripped = Strip(line);

    // Check for directives
    if (!stripped.empty() && stripped[0] != '\t') {
      // Directives must start at column 0 (or after whitespace that's not tab)

      // Check for conditional directives
      if (stripped.substr(0, 5) == "ifeq " || stripped == "ifeq" ||
          stripped.substr(0, 6) == "ifneq " || stripped == "ifneq" ||
          stripped.substr(0, 6) == "ifdef " || stripped == "ifdef" ||
          stripped.substr(0, 7) == "ifndef " || stripped == "ifndef" || stripped.substr(0, 7) == "ifndef") {
        std::string directive, args;
        size_t sp = stripped.find_first_of(" \t");
        if (sp != std::string::npos) {
          directive = stripped.substr(0, sp);
          args = Strip(stripped.substr(sp + 1));
        } else {
          directive = stripped;
        }

        bool parent_active = cond_active;
        if (parent_active) {
          bool condition = ProcessConditional(directive, args);
          cond_stack_.push_back({condition, parent_active, false});
        } else {
          cond_stack_.push_back({false, false, false});
        }
        continue;
      }

      if (stripped == "else" || stripped.substr(0, 5) == "else ") {
        if (!cond_stack_.empty()) {
          auto& cs = cond_stack_.back();
          if (!cs.else_seen) {
            cs.else_seen = true;
            if (cs.parent_active) {
              // Toggle: if was active, now inactive, and vice versa
              std::string args;
              size_t sp = stripped.find_first_of(" \t");
              if (sp != std::string::npos) {
                std::string rest = Strip(stripped.substr(sp + 1));
                if (!rest.empty()) {
                  // else if <condition>
                  if (rest.substr(0, 5) == "ifeq " || rest.substr(0, 6) == "ifdef " ||
                      rest.substr(0, 6) == "ifndef" || rest.substr(0, 5) == "ifneq") {
                    cs.active = ProcessConditional(rest.substr(0, rest.find_first_of(" \t")),
                                                   Strip(rest.substr(rest.find_first_of(" \t"))));
                  }
                }
              } else {
                cs.active = !cs.active;
              }
            }
          }
        }
        continue;
      }

      if (stripped == "endif") {
        if (!cond_stack_.empty()) cond_stack_.pop_back();
        continue;
      }

      if (!cond_active) continue;

      // Include directive
      if (stripped.substr(0, 8) == "include " || stripped == "include" ||
          stripped.substr(0, 9) == "-include " || stripped.substr(0, 2) == "-!") {
        std::string args = Strip(stripped.substr(stripped.find_first_of(" \t")));
        ProcessInclude(args);
        continue;
      }

      // export/unexport/override/unexport
      if (stripped.substr(0, 9) == "override " || stripped == "override") {
        std::string rest = Strip(stripped.substr(9));
        // Treat as normal assignment with override origin
        // Fall through to assignment handling below
        stripped = "override " + rest;
      }

      if (stripped.substr(0, 7) == "export " || stripped == "export" ||
          stripped.substr(0, 9) == "unexport " || stripped == "unexport" ||
          stripped.substr(0, 6) == "define " || stripped == "define") {
        // Simplified: skip export/unexport, handle define later
        if (stripped.substr(0, 6) == "define") {
          // Skip until endef
          while (std::getline(stream, line)) {
            line_num++;
            if (Strip(line) == "endef") break;
          }
        }
        continue;
      }

      // .PHONY etc.
      if (stripped[0] == '.') {
        // Check for special targets like .PHONY: target
        size_t colon = stripped.find(':');
        if (colon != std::string::npos) {
          std::string targets = Strip(stripped.substr(0, colon));
          std::string prereqs = Strip(stripped.substr(colon + 1));
          if (targets == ".PHONY") {
            auto words = SplitWords(vars_.Expand(prereqs));
            for (const auto& w : words) {
              rules_.MarkPhony(w);
            }
            continue;
          }
          if (targets == ".DEFAULT_GOAL") {
            vars_.Set(".DEFAULT_GOAL", vars_.Expand(prereqs),
                      VarFlavor::FLAVOR_RECURSIVE, VarOrigin::ORIGIN_FILE, false);
            continue;
          }
          // Fall through: treat as regular rule
        }
      }
    }

    if (!cond_active) continue;

    // Check if it's a recipe line (starts with tab)
    if (!line.empty() && line[0] == '\t') {
      if (current_rule != nullptr) {
        RecipeLine recipe = ParseRecipeLine(TrimRecipePrefix(line));
        if (!recipe.text.empty()) {
          current_rule->recipes.push_back(recipe);
        }
      }
      continue;
    }

    // Empty line or comment
    if (stripped.empty() || stripped[0] == '#') {
      continue;
    }

    // Strip inline comments (not after #)
    std::string processed_line;
    for (size_t i = 0; i < stripped.size(); ++i) {
      if (stripped[i] == '#' && (i == 0 || stripped[i-1] != '\\')) {
        break;
      }
      if (stripped[i] == '\\' && i + 1 < stripped.size() && stripped[i+1] == '#') {
        processed_line += '#';
        i++;
      } else {
        processed_line += stripped[i];
      }
    }
    processed_line = Strip(processed_line);
    if (processed_line.empty()) continue;

    // Check for variable assignment: VAR = / := / += / ?=
    bool is_assignment = false;
    for (size_t i = 0; i < processed_line.size(); ++i) {
      if (processed_line[i] == '=') {
        is_assignment = true;
        break;
      }
      if (processed_line[i] == ':') {
        // Could be := or a target rule
        if (i + 1 < processed_line.size() && processed_line[i+1] == '=') {
          is_assignment = true;
        }
        break;  // : without = means it's a target
      }
    }

    if (is_assignment) {
      // Parse variable assignment
      size_t op_pos = std::string::npos;
      VarFlavor flavor = VarFlavor::FLAVOR_RECURSIVE;
      bool append = false;
      bool conditional = false;

      for (size_t i = 0; i < processed_line.size(); ++i) {
        if (processed_line[i] == '=' && op_pos == std::string::npos) {
          op_pos = i;
          flavor = VarFlavor::FLAVOR_RECURSIVE;
          break;
        }
        if (processed_line[i] == ':' && i + 1 < processed_line.size() &&
            processed_line[i+1] == '=') {
          op_pos = i;
          flavor = VarFlavor::FLAVOR_SIMPLE;
          break;
        }
        if (processed_line[i] == '+' && i + 1 < processed_line.size() &&
            processed_line[i+1] == '=') {
          op_pos = i;
          flavor = VarFlavor::FLAVOR_RECURSIVE;
          append = true;
          break;
        }
        if (processed_line[i] == '?' && i + 1 < processed_line.size() &&
            processed_line[i+1] == '=') {
          op_pos = i;
          flavor = VarFlavor::FLAVOR_RECURSIVE;
          conditional = true;
          break;
        }
      }

      if (op_pos != std::string::npos) {
        std::string name = Strip(processed_line.substr(0, op_pos));
        std::string value;
        size_t val_start = op_pos + 1;
        if (flavor == VarFlavor::FLAVOR_SIMPLE || append || conditional) {
          val_start = op_pos + 2;
        }
        if (val_start <= processed_line.size()) {
          value = Strip(processed_line.substr(val_start));
        }

        if (conditional && vars_.IsDefined(name)) {
          // ?= and already defined: skip
        } else {
          VarOrigin origin = VarOrigin::ORIGIN_FILE;
          if (name.substr(0, 9) == "override ") {
            name = Strip(name.substr(9));
            origin = VarOrigin::ORIGIN_OVERRIDE;
          }
          vars_.Set(name, value, flavor, origin, append);
        }
        current_rule = nullptr;
        continue;
      }
    }

    // It must be a rule: target : prereqs
    size_t colon = processed_line.find(':');
    if (colon != std::string::npos) {
      // Check for ::= (double colon)
      bool double_colon = false;
      size_t rule_start = colon + 1;
      if (rule_start < processed_line.size() && processed_line[rule_start] == ':') {
        double_colon = true;
        rule_start++;
      }

      std::string targets_str = Strip(processed_line.substr(0, colon));
      std::string prereqs_str = Strip(processed_line.substr(rule_start));

      // Expand targets and prereqs
      std::string expanded_targets = vars_.Expand(targets_str);
      std::string expanded_prereqs = vars_.Expand(prereqs_str);

      auto targets = SplitWords(expanded_targets);
      auto all_prereqs = SplitWords(expanded_prereqs);

      // Split into normal prereqs and order-only prereqs at | separator
      std::vector<std::string> normal_prereqs;
      std::vector<std::string> order_only_prereqs;
      bool past_pipe = false;
      for (const auto& p : all_prereqs) {
        if (p == "|") {
          past_pipe = true;
        } else if (past_pipe) {
          order_only_prereqs.push_back(p);
        } else {
          normal_prereqs.push_back(p);
        }
      }

      if (!targets.empty()) {
        auto rule = std::make_unique<Rule>();
        rule->targets = targets;
        rule->prereqs = normal_prereqs;
        rule->order_only_prereqs = order_only_prereqs;
        rule->is_double_colon = double_colon;

        // Check for pattern rules
        for (const auto& t : targets) {
          if (t.find('%') != std::string::npos) {
            rule->is_pattern = true;
            break;
          }
        }

        current_rule = rule.get();
        rules_.AddRule(std::move(rule));
        continue;
      }
    }

    // Unrecognized line, skip
    current_rule = nullptr;
  }

  return true;
}

void Engine::ProcessInclude(const std::string& args) {
  std::string expanded = vars_.Expand(args);
  auto files = SplitWords(expanded);
  for (const auto& f : files) {
    ParseMakefile(f);
  }
}

bool Engine::ProcessConditional(const std::string& directive,
                                 const std::string& args) {
  if (directive == "ifdef" || directive == "ifndef") {
    std::string var_name = Strip(args);
    // Expand the variable name itself
    var_name = vars_.Expand(var_name);
    bool defined = vars_.IsDefined(var_name);
    const Variable* v = vars_.Get(var_name);
    if (defined && v) {
      defined = !v->value.empty();
    }
    return (directive == "ifdef") ? defined : !defined;
  }

  if (directive == "ifeq" || directive == "ifneq") {
    // Parse: (arg1, arg2) or "arg1" "arg2"
    std::string a, b;
    std::string s = Strip(args);

    if (!s.empty() && s[0] == '(') {
      // (arg1, arg2) form - find matching close paren handling nesting
      int depth = 0;
      size_t close = std::string::npos;
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
          depth--;
          if (depth == 0) { close = i; break; }
        }
      }
      if (close == std::string::npos) return false;
      std::string inner = s.substr(1, close - 1);
      // Split by comma at depth 0 (respecting nested parens)
      depth = 0;
      size_t comma = std::string::npos;
      for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '(') depth++;
        else if (inner[i] == ')') depth--;
        else if (inner[i] == ',' && depth == 0) {
          comma = i;
          break;
        }
      }
      if (comma == std::string::npos) return false;
      a = vars_.Expand(Strip(inner.substr(0, comma)));
      b = vars_.Expand(Strip(inner.substr(comma + 1)));
    } else {
      // "str1" "str2" form
      size_t q1_start = s.find('"');
      if (q1_start == std::string::npos) return false;
      size_t q1_end = s.find('"', q1_start + 1);
      if (q1_end == std::string::npos) return false;
      size_t q2_start = s.find('"', q1_end + 1);
      if (q2_start == std::string::npos) return false;
      size_t q2_end = s.find('"', q2_start + 1);
      if (q2_end == std::string::npos) return false;
      a = vars_.Expand(s.substr(q1_start + 1, q1_end - q1_start - 1));
      b = vars_.Expand(s.substr(q2_start + 1, q2_end - q2_start - 1));
    }

    bool equal = (a == b);
    return (directive == "ifeq") ? equal : !equal;
  }

  return false;
}

bool Engine::BuildTarget(const std::string& target,
                         std::unordered_set<std::string>& visited,
                         std::unordered_set<std::string>& building) {
  // Cycle detection
  if (building.count(target) > 0) {
    fprintf(stderr, "gor_make: Circular dependency detected for '%s'.\n",
            target.c_str());
    return false;
  }

  // Already built?
  if (visited.count(target) > 0) return true;

  // Find the rule for this target
  Rule* rule = rules_.FindFirstRule(target);
  std::string stem;
  bool is_pattern = false;

  if (!rule) {
    // Try pattern rules
    rule = rules_.FindPatternRule(target, stem);
    if (rule) {
      is_pattern = true;
    }
  }

  // If no rule and file exists, it's a source file — nothing to do
  if (!rule) {
    struct stat st;
    if (stat(target.c_str(), &st) == 0) {
      visited.insert(target);
      return true;
    }
    fprintf(stderr, "gor_make: *** No rule to make target '%s'.  Stop.\n",
            target.c_str());
    return false;
  }

  building.insert(target);

  // Build prerequisites first
  std::vector<std::string> expanded_prereqs;
  for (const auto& prereq : rule->prereqs) {
    std::string expanded;
    if (is_pattern) {
      // In pattern rules, replace % with stem in prereqs
      std::string p = prereq;
      size_t pct = p.find('%');
      if (pct != std::string::npos) {
        p = p.substr(0, pct) + stem + p.substr(pct + 1);
      }
      expanded = vars_.Expand(p);
    } else {
      expanded = vars_.Expand(prereq);
    }
    auto words = SplitWords(expanded);
    for (const auto& w : words) {
      expanded_prereqs.push_back(w);
      if (!BuildTarget(w, visited, building)) {
        building.erase(target);
        if (!opts_->keep_going) return false;
      }
    }
  }

  // Build order-only prerequisites (they don't affect timestamp checking)
  for (const auto& prereq : rule->order_only_prereqs) {
    std::string expanded = vars_.Expand(prereq);
    auto words = SplitWords(expanded);
    for (const auto& w : words) {
      BuildTarget(w, visited, building);
    }
  }

  building.erase(target);
  visited.insert(target);

  // Check if we need to rebuild
  bool need_rebuild = opts_->always_make;
  if (!need_rebuild) {
    need_rebuild = NeedsRebuild(target, expanded_prereqs);
  }

  // If target is .PHONY, always rebuild
  if (rules_.IsPhony(target)) {
    need_rebuild = true;
  }

#ifdef DEBUG_GORMAKE
  fprintf(stderr, "[DEBUG] BuildTarget '%s' need_rebuild=%d is_phony=%d recipes=%zu\n",
          target.c_str(), need_rebuild, rules_.IsPhony(target), rule->recipes.size());
#endif

  // If no rebuild needed, nothing to do
  if (!need_rebuild) {
    return true;
  }

  if (rule->recipes.empty()) {
    return true;  // Nothing to do
  }

  // Execute the recipe
  if (!rule->recipes.empty()) {
    // Set automatic variables
    vars_.PushAutomaticScope();

    std::string prereq_str;
    for (size_t i = 0; i < expanded_prereqs.size(); ++i) {
      if (i > 0) prereq_str += " ";
      prereq_str += expanded_prereqs[i];
    }
    std::string first_prereq = expanded_prereqs.empty() ? "" : expanded_prereqs[0];

    vars_.SetAutomatic("@", target);
    vars_.SetAutomatic("<", first_prereq);
    vars_.SetAutomatic("^", prereq_str);
    vars_.SetAutomatic("?", prereq_str);  // simplified
    vars_.SetAutomatic("*", stem);

    bool success = ExecuteRecipe(rule, target, stem);

    vars_.PopAutomaticScope();

    if (!success) {
      fprintf(stderr, "gor_make: *** [%s] Error\n", target.c_str());
      if (!opts_->ignore_errors && !opts_->keep_going) {
        return false;
      }
    }
  }

  return true;
}

bool Engine::ExecuteRecipe(const Rule* rule, const std::string& target,
                            const std::string& stem) {
  for (const auto& recipe : rule->recipes) {
    std::string cmd = vars_.Expand(recipe.text, target, "", stem);

    // Get automatic variable values for ^ and <
    const Variable* v_less = vars_.Get("<");
    const Variable* v_caret = vars_.Get("^");
    std::string prereq_str = v_caret ? v_caret->value : "";
    std::string first_prereq = v_less ? v_less->value : "";

    // Re-expand with proper context
    cmd = vars_.Expand(recipe.text, target, prereq_str, stem);

    if (cmd.empty()) continue;

    bool silent = recipe.silent || opts_->silent;
    bool ignore_error = recipe.ignore_error || opts_->ignore_errors;

    if (!silent || opts_->dry_run) {
      printf("%s\n", cmd.c_str());
      fflush(stdout);
    }

    if (opts_->dry_run && !recipe.always_run) {
      continue;
    }

    // Execute via /bin/sh -c
    std::string shell_cmd = vars_.Expand("$(SHELL)");
    std::string shell_flags = vars_.Expand("$(.SHELLFLAGS)");

    int status = system(cmd.c_str());
    if (status == -1) {
      if (!ignore_error) return false;
    } else if (WIFEXITED(status)) {
      int code = WEXITSTATUS(status);
      if (code != 0) {
        if (!ignore_error) return false;
      }
    } else if (WIFSIGNALED(status)) {
      if (!ignore_error) return false;
    }
  }
  return true;
}

bool Engine::NeedsRebuild(const std::string& target,
                           const std::vector<std::string>& prereqs) const {
  long target_mtime = GetFileMtime(target);

  // If target doesn't exist, it needs to be built
  if (target_mtime == 0) return true;

  // Check if any prerequisite is newer
  for (const auto& prereq : prereqs) {
    long prereq_mtime = GetFileMtime(prereq);
    if (prereq_mtime > target_mtime) return true;
  }

  return false;
}

long Engine::GetFileMtime(const std::string& path) const {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return st.st_mtime;
}

// Helper: escape a string for JSON output
static std::string MkJsonEscape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

static void MkOutputJsonArray(const std::vector<std::string>& arr) {
  printf("[");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) printf(", ");
    printf("\"%s\"", MkJsonEscape(arr[i]).c_str());
  }
  printf("]");
}

void Engine::OutputJson() const {
  const auto& all_rules = rules_.GetAllRules();
  const auto& pattern_rules = rules_.GetPatternRules();

  printf("{\n");
  printf("  \"format\": \"makefile\",\n");
  printf("  \"makefile\": \"%s\",\n", MkJsonEscape(opts_ ? opts_->makefile_path : "").c_str());
  printf("  \"target_count\": %zu,\n", all_rules.size() + pattern_rules.size());
  printf("  \"targets\": [\n");

  bool first = true;
  for (const auto& [target, rule_list] : all_rules) {
    for (const auto* rule : rule_list) {
      if (!first) printf(",\n");
      first = false;

      printf("    {\n");
      printf("      \"name\": \"%s\",\n", MkJsonEscape(target).c_str());
      printf("      \"type\": \"%s\",\n",
             rules_.IsPhony(target) ? "phony" : "explicit");
      printf("      \"is_pattern\": false,\n");

      // Collect all targets for this rule
      std::vector<std::string> targets;
      for (const auto& t : rule->targets) targets.push_back(t);
      printf("      \"targets\": ");
      MkOutputJsonArray(targets);
      printf(",\n");

      printf("      \"prereqs\": ");
      MkOutputJsonArray(rule->prereqs);
      printf(",\n");

      printf("      \"order_only_prereqs\": ");
      MkOutputJsonArray(rule->order_only_prereqs);
      printf(",\n");

      // Output recipe commands
      printf("      \"recipes\": [");
      for (size_t i = 0; i < rule->recipes.size(); ++i) {
        if (i > 0) printf(", ");
        printf("\"%s\"", MkJsonEscape(rule->recipes[i].text).c_str());
      }
      printf("],\n");

      printf("      \"is_double_colon\": %s,\n",
             rule->is_double_colon ? "true" : "false");
      printf("      \"is_phony\": %s\n",
             rules_.IsPhony(target) ? "true" : "false");

      printf("    }");
    }
  }

  // Output pattern rules
  for (const auto& rule : pattern_rules) {
    if (!first) printf(",\n");
    first = false;

    printf("    {\n");
    printf("      \"name\": \"");
    for (size_t i = 0; i < rule->targets.size(); ++i) {
      if (i > 0) printf(", ");
      printf("%s", MkJsonEscape(rule->targets[i]).c_str());
    }
    printf("\",\n");
    printf("      \"type\": \"pattern\",\n");
    printf("      \"is_pattern\": true,\n");

    printf("      \"targets\": ");
    MkOutputJsonArray(rule->targets);
    printf(",\n");

    printf("      \"prereqs\": ");
    MkOutputJsonArray(rule->prereqs);
    printf(",\n");

    printf("      \"order_only_prereqs\": ");
    MkOutputJsonArray(rule->order_only_prereqs);
    printf(",\n");

    printf("      \"recipes\": [");
    for (size_t i = 0; i < rule->recipes.size(); ++i) {
      if (i > 0) printf(", ");
      printf("\"%s\"", MkJsonEscape(rule->recipes[i].text).c_str());
    }
    printf("],\n");

    printf("      \"is_double_colon\": %s,\n",
           rule->is_double_colon ? "true" : "false");
    printf("      \"is_phony\": false\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

}  // namespace gormake
