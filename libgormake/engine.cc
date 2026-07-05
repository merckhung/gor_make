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

// Check if a character can be part of a variable name.
static bool IsVarNameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
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
      recipe.ignoreError = true;
      text = text.substr(1);
    } else if (text[0] == '+') {
      recipe.alwaysRun = true;
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
  for (const auto& cv : opts.cmdLineVars) {
    // Parse VAR=value or VAR:=value or VAR+=value or VAR?=value
    size_t opPos = std::string::npos;
    char op = '=';
    VarFlavor flavor = VarFlavor::FLAVOR_RECURSIVE;

    // Find the operator
    for (size_t i = 0; i < cv.size(); ++i) {
      if (cv[i] == '=' ) {
        // Check if previous char is : ? or +
        if (i > 0 && (cv[i-1] == ':' || cv[i-1] == '?' || cv[i-1] == '+')) {
          opPos = i - 1;
          op = cv[i-1];
          flavor = VarFlavor::FLAVOR_SIMPLE;
        } else {
          opPos = i;
          op = '=';
          flavor = VarFlavor::FLAVOR_RECURSIVE;
        }
        break;
      }
    }

    if (opPos != std::string::npos) {
      std::string name, value;
      if (op == '=') {
        name = cv.substr(0, opPos);
        value = cv.substr(opPos + 1);
      } else {
        name = cv.substr(0, opPos);
        value = cv.substr(opPos + 2);
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
  if (!ParseMakefile(opts.makefilePath)) {
    fprintf(stderr, "gor_make: *** No rule to make target '%s'. Stop.\n",
            opts.makefilePath.c_str());
    return 2;
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
      if (!opts.keepGoing) break;
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
  int lineNum = 0;
  // Track if we're inside a recipe (tab-indented lines after a target)
  bool inRecipe = false;
  Rule* currentRule = nullptr;

  while (std::getline(stream, line)) {
    lineNum++;

    // Check if conditional is active
    bool condActive = true;
    for (const auto& cs : condStack_) {
      if (!cs.active) { condActive = false; break; }
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

        bool parentActive = condActive;
        if (parentActive) {
          bool condition = ProcessConditional(directive, args);
          condStack_.push_back({condition, parentActive, false});
        } else {
          condStack_.push_back({false, false, false});
        }
        continue;
      }

      if (stripped == "else" || stripped.substr(0, 5) == "else ") {
        if (!condStack_.empty()) {
          auto& cs = condStack_.back();
          if (!cs.elseSeen) {
            cs.elseSeen = true;
            if (cs.parentActive) {
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
        if (!condStack_.empty()) condStack_.pop_back();
        continue;
      }

      if (!condActive) continue;

      // Include directive
      if (stripped.substr(0, 8) == "include " || stripped == "include" ||
          stripped.substr(0, 9) == "-include " || stripped.substr(0, 2) == "-!") {
        std::string args = Strip(stripped.substr(stripped.find_first_of(" \t")));
        bool ignoreMissing = (stripped[0] == '-');
        ProcessInclude(args);
        inRecipe = false;
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
            lineNum++;
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

    if (!condActive) continue;

    // Check if it's a recipe line (starts with tab)
    if (!line.empty() && line[0] == '\t') {
      if (currentRule != nullptr) {
        RecipeLine recipe = ParseRecipeLine(TrimRecipePrefix(line));
        if (!recipe.text.empty()) {
          currentRule->recipes.push_back(recipe);
        }
      }
      continue;
    }

    // Empty line or comment
    if (stripped.empty() || stripped[0] == '#') {
      continue;
    }

    // Strip inline comments (not after #)
    std::string processedLine;
    for (size_t i = 0; i < stripped.size(); ++i) {
      if (stripped[i] == '#' && (i == 0 || stripped[i-1] != '\\')) {
        break;
      }
      if (stripped[i] == '\\' && i + 1 < stripped.size() && stripped[i+1] == '#') {
        processedLine += '#';
        i++;
      } else {
        processedLine += stripped[i];
      }
    }
    processedLine = Strip(processedLine);
    if (processedLine.empty()) continue;

    // Check for variable assignment: VAR = / := / += / ?=
    bool isAssignment = false;
    for (size_t i = 0; i < processedLine.size(); ++i) {
      if (processedLine[i] == '=') {
        isAssignment = true;
        break;
      }
      if (processedLine[i] == ':') {
        // Could be := or a target rule
        if (i + 1 < processedLine.size() && processedLine[i+1] == '=') {
          isAssignment = true;
        }
        break;  // : without = means it's a target
      }
    }

    if (isAssignment) {
      // Parse variable assignment
      size_t opPos = std::string::npos;
      VarFlavor flavor = VarFlavor::FLAVOR_RECURSIVE;
      bool append = false;
      bool conditional = false;

      for (size_t i = 0; i < processedLine.size(); ++i) {
        if (processedLine[i] == '=' && opPos == std::string::npos) {
          opPos = i;
          flavor = VarFlavor::FLAVOR_RECURSIVE;
          break;
        }
        if (processedLine[i] == ':' && i + 1 < processedLine.size() &&
            processedLine[i+1] == '=') {
          opPos = i;
          flavor = VarFlavor::FLAVOR_SIMPLE;
          break;
        }
        if (processedLine[i] == '+' && i + 1 < processedLine.size() &&
            processedLine[i+1] == '=') {
          opPos = i;
          flavor = VarFlavor::FLAVOR_RECURSIVE;
          append = true;
          break;
        }
        if (processedLine[i] == '?' && i + 1 < processedLine.size() &&
            processedLine[i+1] == '=') {
          opPos = i;
          flavor = VarFlavor::FLAVOR_RECURSIVE;
          conditional = true;
          break;
        }
      }

      if (opPos != std::string::npos) {
        std::string name = Strip(processedLine.substr(0, opPos));
        std::string value;
        size_t valStart = opPos + 1;
        if (flavor == VarFlavor::FLAVOR_SIMPLE || append || conditional) {
          valStart = opPos + 2;
        }
        if (valStart <= processedLine.size()) {
          value = Strip(processedLine.substr(valStart));
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
        currentRule = nullptr;
        continue;
      }
    }

    // It must be a rule: target : prereqs
    size_t colon = processedLine.find(':');
    if (colon != std::string::npos) {
      // Check for ::= (double colon)
      bool doubleColon = false;
      size_t ruleStart = colon + 1;
      if (ruleStart < processedLine.size() && processedLine[ruleStart] == ':') {
        doubleColon = true;
        ruleStart++;
      }

      std::string targetsStr = Strip(processedLine.substr(0, colon));
      std::string prereqsStr = Strip(processedLine.substr(ruleStart));

      // Expand targets and prereqs
      std::string expandedTargets = vars_.Expand(targetsStr);
      std::string expandedPrereqs = vars_.Expand(prereqsStr);

      auto targets = SplitWords(expandedTargets);
      auto allPrereqs = SplitWords(expandedPrereqs);

      // Split into normal prereqs and order-only prereqs at | separator
      std::vector<std::string> normalPrereqs;
      std::vector<std::string> orderOnlyPrereqs;
      bool pastPipe = false;
      for (const auto& p : allPrereqs) {
        if (p == "|") {
          pastPipe = true;
        } else if (pastPipe) {
          orderOnlyPrereqs.push_back(p);
        } else {
          normalPrereqs.push_back(p);
        }
      }

      if (!targets.empty()) {
        auto rule = std::make_unique<Rule>();
        rule->targets = targets;
        rule->prereqs = normalPrereqs;
        rule->orderOnlyPrereqs = orderOnlyPrereqs;
        rule->isDoubleColon = doubleColon;

        // Check for pattern rules
        for (const auto& t : targets) {
          if (t.find('%') != std::string::npos) {
            rule->isPattern = true;
            break;
          }
        }

        currentRule = rule.get();
        rules_.AddRule(std::move(rule));
        continue;
      }
    }

    // Unrecognized line, skip
    currentRule = nullptr;
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
    std::string varName = Strip(args);
    // Expand the variable name itself
    varName = vars_.Expand(varName);
    bool defined = vars_.IsDefined(varName);
    const Variable* v = vars_.Get(varName);
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
      size_t q1Start = s.find('"');
      if (q1Start == std::string::npos) return false;
      size_t q1End = s.find('"', q1Start + 1);
      if (q1End == std::string::npos) return false;
      size_t q2Start = s.find('"', q1End + 1);
      if (q2Start == std::string::npos) return false;
      size_t q2End = s.find('"', q2Start + 1);
      if (q2End == std::string::npos) return false;
      a = vars_.Expand(s.substr(q1Start + 1, q1End - q1Start - 1));
      b = vars_.Expand(s.substr(q2Start + 1, q2End - q2Start - 1));
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
  bool isPattern = false;

  if (!rule) {
    // Try pattern rules
    rule = rules_.FindPatternRule(target, stem);
    if (rule) {
      isPattern = true;
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
  std::vector<std::string> expandedPrereqs;
  for (const auto& prereq : rule->prereqs) {
    std::string expanded;
    if (isPattern) {
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
      expandedPrereqs.push_back(w);
      if (!BuildTarget(w, visited, building)) {
        building.erase(target);
        if (!opts_->keepGoing) return false;
      }
    }
  }

  // Build order-only prerequisites (they don't affect timestamp checking)
  for (const auto& prereq : rule->orderOnlyPrereqs) {
    std::string expanded = vars_.Expand(prereq);
    auto words = SplitWords(expanded);
    for (const auto& w : words) {
      BuildTarget(w, visited, building);
    }
  }

  building.erase(target);
  visited.insert(target);

  // Check if we need to rebuild
  bool needRebuild = opts_->alwaysMake;
  if (!needRebuild) {
    needRebuild = NeedsRebuild(target, expandedPrereqs);
  }

  // If target is .PHONY, always rebuild
  if (rules_.IsPhony(target)) {
    needRebuild = true;
  }

#ifdef DEBUG_GORMAKE
  fprintf(stderr, "[DEBUG] BuildTarget '%s' needRebuild=%d isPhony=%d recipes=%zu\n",
          target.c_str(), needRebuild, rules_.IsPhony(target), rule->recipes.size());
#endif

  // If no rebuild needed, nothing to do
  if (!needRebuild) {
    return true;
  }

  if (rule->recipes.empty()) {
    return true;  // Nothing to do
  }

  // Execute the recipe
  if (!rule->recipes.empty()) {
    // Set automatic variables
    vars_.PushAutomaticScope();

    std::string prereqStr;
    for (size_t i = 0; i < expandedPrereqs.size(); ++i) {
      if (i > 0) prereqStr += " ";
      prereqStr += expandedPrereqs[i];
    }
    std::string firstPrereq = expandedPrereqs.empty() ? "" : expandedPrereqs[0];

    vars_.SetAutomatic("@", target);
    vars_.SetAutomatic("<", firstPrereq);
    vars_.SetAutomatic("^", prereqStr);
    vars_.SetAutomatic("?", prereqStr);  // simplified
    vars_.SetAutomatic("*", stem);

    bool success = ExecuteRecipe(rule, target, stem);

    vars_.PopAutomaticScope();

    if (!success) {
      fprintf(stderr, "gor_make: *** [%s] Error\n", target.c_str());
      if (!opts_->ignoreErrors && !opts_->keepGoing) {
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
    const Variable* vAt = vars_.Get("@");
    const Variable* vLess = vars_.Get("<");
    const Variable* vCaret = vars_.Get("^");
    std::string prereqStr = vCaret ? vCaret->value : "";
    std::string firstPrereq = vLess ? vLess->value : "";

    // Re-expand with proper context
    cmd = vars_.Expand(recipe.text, target, prereqStr, stem);

    if (cmd.empty()) continue;

    bool silent = recipe.silent || opts_->silent;
    bool ignoreError = recipe.ignoreError || opts_->ignoreErrors;

    if (!silent || opts_->dryRun) {
      printf("%s\n", cmd.c_str());
      fflush(stdout);
    }

    if (opts_->dryRun && !recipe.alwaysRun) {
      continue;
    }

    // Execute via /bin/sh -c
    std::string shellCmd = vars_.Expand("$(SHELL)");
    std::string shellFlags = vars_.Expand("$(.SHELLFLAGS)");

    int status = system(cmd.c_str());
    if (status == -1) {
      if (!ignoreError) return false;
    } else if (WIFEXITED(status)) {
      int code = WEXITSTATUS(status);
      if (code != 0) {
        if (!ignoreError) return false;
      }
    } else if (WIFSIGNALED(status)) {
      if (!ignoreError) return false;
    }
  }
  return true;
}

bool Engine::NeedsRebuild(const std::string& target,
                           const std::vector<std::string>& prereqs) const {
  long targetMtime = GetFileMtime(target);

  // If target doesn't exist, it needs to be built
  if (targetMtime == 0) return true;

  // Check if any prerequisite is newer
  for (const auto& prereq : prereqs) {
    long prereqMtime = GetFileMtime(prereq);
    if (prereqMtime > targetMtime) return true;
  }

  return false;
}

long Engine::GetFileMtime(const std::string& path) const {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return st.st_mtime;
}

}  // namespace gormake
