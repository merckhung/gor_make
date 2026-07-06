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

#include "var_db.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glob.h>
#include <sstream>

namespace gormake {

// Helper: get first word of a space-separated string
static std::string FirstWord(const std::string& s) {
  size_t sp = s.find(' ');
  return (sp == std::string::npos) ? s : s.substr(0, sp);
}

VariableDB::VariableDB() {
  InitDefaults();
}

VariableDB::~VariableDB() {
}

void VariableDB::InitDefaults() {
  // Built-in variables that GNU Make defines by default.
  struct DefaultVar {
    const char* name;
    const char* value;
    VarFlavor flavor;
  };
  static const DefaultVar defaults[] = {
    {"MAKE",           "make",         VarFlavor::FLAVOR_SIMPLE},
    {"MAKECMDGOALS",   "",             VarFlavor::FLAVOR_SIMPLE},
    {"SHELL",          "/bin/sh",      VarFlavor::FLAVOR_SIMPLE},
    {".SHELLFLAGS",    "-c",           VarFlavor::FLAVOR_SIMPLE},
    {"CURDIR",         ".",            VarFlavor::FLAVOR_SIMPLE},
    {"MAKEFLAGS",      "",             VarFlavor::FLAVOR_RECURSIVE},
    {"MAKELEVEL",      "0",            VarFlavor::FLAVOR_SIMPLE},
    {"MAKEFILES",      "",             VarFlavor::FLAVOR_RECURSIVE},
    {"VPATH",          "",             VarFlavor::FLAVOR_RECURSIVE},
    {".DEFAULT_GOAL",  "",             VarFlavor::FLAVOR_RECURSIVE},

    // Built-in implicit-rule variables
    {"CC",             "cc",           VarFlavor::FLAVOR_RECURSIVE},
    {"CXX",            "g++",          VarFlavor::FLAVOR_RECURSIVE},
    {"CFLAGS",         "",             VarFlavor::FLAVOR_RECURSIVE},
    {"CXXFLAGS",       "",             VarFlavor::FLAVOR_RECURSIVE},
    {"CPPFLAGS",       "",             VarFlavor::FLAVOR_RECURSIVE},
    {"LDFLAGS",        "",             VarFlavor::FLAVOR_RECURSIVE},
    {"LDLIBS",         "",             VarFlavor::FLAVOR_RECURSIVE},
    {"TARGET_ARCH",    "",             VarFlavor::FLAVOR_RECURSIVE},
    {"AR",             "ar",           VarFlavor::FLAVOR_RECURSIVE},
    {"ARFLAGS",        "rv",           VarFlavor::FLAVOR_RECURSIVE},
    {"RM",             "rm -f",        VarFlavor::FLAVOR_RECURSIVE},
    {"LINK.o",         "$(CC) $(LDFLAGS)",   VarFlavor::FLAVOR_RECURSIVE},
    {"LINK.c",         "$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)",  VarFlavor::FLAVOR_RECURSIVE},
    {"LINK.cc",        "$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS)", VarFlavor::FLAVOR_RECURSIVE},
    {"COMPILE.c",      "$(CC) $(CFLAGS) $(CPPFLAGS) -c",  VarFlavor::FLAVOR_RECURSIVE},
    {"COMPILE.cc",     "$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c", VarFlavor::FLAVOR_RECURSIVE},
    {"OUTPUT_OPTION",  "-o $@",        VarFlavor::FLAVOR_RECURSIVE},
    {"OBJCFLAGS",      "",             VarFlavor::FLAVOR_RECURSIVE},
    {"FC",             "f77",          VarFlavor::FLAVOR_RECURSIVE},
    {"FFLAGS",         "",             VarFlavor::FLAVOR_RECURSIVE},
    {"LEX",            "lex",          VarFlavor::FLAVOR_RECURSIVE},
    {"LFLAGS",         "",             VarFlavor::FLAVOR_RECURSIVE},
    {"YACC",           "yacc",         VarFlavor::FLAVOR_RECURSIVE},
    {"YFLAGS",         "",             VarFlavor::FLAVOR_RECURSIVE},
    {"AS",             "as",           VarFlavor::FLAVOR_RECURSIVE},
    {"ASFLAGS",        "",             VarFlavor::FLAVOR_RECURSIVE},
  };

  for (const auto& d : defaults) {
    vars_[d.name] = Variable(d.name, d.value, d.flavor, VarOrigin::ORIGIN_DEFAULT);
  }

  // Register built-in functions.
  functions_["wildcard"] = [](const std::vector<std::string>& args) -> std::string {
    std::string result;
    for (const auto& arg : args) {
      glob_t globbuf;
      if (glob(arg.c_str(), 0, nullptr, &globbuf) == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc; ++i) {
          if (!result.empty()) result += " ";
          result += globbuf.gl_pathv[i];
        }
        globfree(&globbuf);
      }
    }
    return result;
  };

  functions_["shell"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    std::string cmd = args[0];
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      result += buffer;
    }
    pclose(pipe);
    // Strip trailing newlines
    while (!result.empty() && result.back() == '\n') {
      result.pop_back();
    }
    // Replace internal newlines with spaces (Make behavior)
    std::replace(result.begin(), result.end(), '\n', ' ');
    return result;
  };

  functions_["subst"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 3) return "";
    std::string from = args[0];
    std::string to = args[1];
    std::string text = args[2];
    if (from.empty()) return text;
    std::string result;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t found = text.find(from, pos);
      if (found == std::string::npos) {
        result += text.substr(pos);
        break;
      }
      result += text.substr(pos, found - pos);
      result += to;
      pos = found + from.size();
    }
    return result;
  };

  functions_["patsubst"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 3) return "";
    std::string pattern = args[0];
    std::string replacement = args[1];
    std::string text = args[2];
    // Simple % pattern: prefix%suffix
    size_t pct_pos = pattern.find('%');
    std::string result;
    size_t start = 0;
    while (start < text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start)
          : text.substr(start, sp - start);
      if (!word.empty()) {
        if (pct_pos != std::string::npos) {
          std::string prefix = pattern.substr(0, pct_pos);
          std::string suffix = pattern.substr(pct_pos + 1);
          if (word.size() >= prefix.size() + suffix.size()
              && word.compare(0, prefix.size(), prefix) == 0
              && word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::string stem = word.substr(prefix.size(),
                                           word.size() - prefix.size() - suffix.size());
            size_t r_pct = replacement.find('%');
            std::string replaced;
            if (r_pct != std::string::npos) {
              replaced = replacement.substr(0, r_pct) + stem +
                         replacement.substr(r_pct + 1);
            } else {
              replaced = replacement;
            }
            result += replaced;
          } else {
            result += word;
          }
        } else {
          if (word == pattern) {
            result += replacement;
          } else {
            result += word;
          }
        }
      }
      if (sp == std::string::npos) break;
      result += " ";
      start = sp + 1;
    }
    return result;
  };

  functions_["strip"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    std::string s = args[0];
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
  };

  functions_["findstring"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 2) return "";
    if (args[1].find(args[0]) != std::string::npos) return args[0];
    return "";
  };

  functions_["filter"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 2) return "";
    std::string patterns = args[0];
    std::string text = args[1];
    std::vector<std::string> pat_list;
    std::string cur;
    for (char c : patterns) {
      if (c == ' ' || c == '\t') {
        if (!cur.empty()) { pat_list.push_back(cur); cur.clear(); }
      } else { cur += c; }
    }
    if (!cur.empty()) pat_list.push_back(cur);

    std::string result;
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start)
          : text.substr(start, sp - start);
      for (const auto& pat : pat_list) {
        size_t pct = pat.find('%');
        bool match = false;
        if (pct != std::string::npos) {
          std::string prefix = pat.substr(0, pct);
          std::string suffix = pat.substr(pct + 1);
          if (word.size() >= prefix.size() + suffix.size()
              && word.compare(0, prefix.size(), prefix) == 0
              && word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0) {
            match = true;
          }
        } else if (word == pat) {
          match = true;
        }
        if (match) {
          if (!result.empty()) result += " ";
          result += word;
          break;
        }
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["filter-out"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 2) return "";
    std::string patterns = args[0];
    std::string text = args[1];
    std::vector<std::string> pat_list;
    std::string cur;
    for (char c : patterns) {
      if (c == ' ' || c == '\t') {
        if (!cur.empty()) { pat_list.push_back(cur); cur.clear(); }
      } else { cur += c; }
    }
    if (!cur.empty()) pat_list.push_back(cur);

    std::string result;
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start)
          : text.substr(start, sp - start);
      bool filtered = false;
      for (const auto& pat : pat_list) {
        size_t pct = pat.find('%');
        if (pct != std::string::npos) {
          std::string prefix = pat.substr(0, pct);
          std::string suffix = pat.substr(pct + 1);
          if (word.size() >= prefix.size() + suffix.size()
              && word.compare(0, prefix.size(), prefix) == 0
              && word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0) {
            filtered = true;
            break;
          }
        } else if (word == pat) {
          filtered = true;
          break;
        }
      }
      if (!filtered && !word.empty()) {
        if (!result.empty()) result += " ";
        result += word;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["notdir"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    std::string result;
    std::string text = args[0];
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        size_t slash = word.find_last_of('/');
        if (!result.empty()) result += " ";
        result += (slash != std::string::npos)
            ? word.substr(slash + 1) : word;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["dir"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    std::string result;
    std::string text = args[0];
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        size_t slash = word.find_last_of('/');
        if (!result.empty()) result += " ";
        result += (slash != std::string::npos)
            ? word.substr(0, slash + 1) : "./";
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["basename"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    std::string result;
    std::string text = args[0];
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        size_t dot = word.find_last_of('.');
        size_t slash = word.find_last_of('/');
        if (slash != std::string::npos && dot != std::string::npos && dot < slash) {
          dot = std::string::npos;
        }
        if (!result.empty()) result += " ";
        result += (dot != std::string::npos)
            ? word.substr(0, dot) : word;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["suffix"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    std::string result;
    std::string text = args[0];
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        size_t dot = word.find_last_of('.');
        size_t slash = word.find_last_of('/');
        if (slash != std::string::npos && dot != std::string::npos && dot < slash) {
          dot = std::string::npos;
        }
        if (dot != std::string::npos) {
          if (!result.empty()) result += " ";
          result += word.substr(dot);
        }
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["addprefix"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 2) return "";
    std::string prefix = args[0];
    std::string text = args[1];
    std::string result;
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        if (!result.empty()) result += " ";
        result += prefix + word;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["addsuffix"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 2) return "";
    std::string suffix = args[0];
    std::string text = args[1];
    std::string result;
    size_t start = 0;
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        if (!result.empty()) result += " ";
        result += word + suffix;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  };

  functions_["words"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "0";
    int count = 0;
    bool in_word = false;
    for (char c : args[0]) {
      if (c == ' ' || c == '\t') {
        in_word = false;
      } else if (!in_word) {
        in_word = true;
        count++;
      }
    }
    return std::to_string(count);
  };

  functions_["word"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.size() < 2) return "";
    int n = atoi(args[0].c_str());
    if (n < 1) return "";
    int count = 0;
    size_t start = 0;
    std::string text = args[1];
    while (start <= text.size()) {
      size_t sp = text.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? text.substr(start) : text.substr(start, sp - start);
      if (!word.empty()) {
        count++;
        if (count == n) return word;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return "";
  };

  functions_["firstword"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    size_t sp = args[0].find(' ');
    return (sp == std::string::npos) ? args[0] : args[0].substr(0, sp);
  };

  functions_["lastword"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    size_t sp = args[0].find_last_of(' ');
    return (sp == std::string::npos) ? args[0] : args[0].substr(sp + 1);
  };

  functions_["foreach"] = [](const std::vector<std::string>& args) -> std::string {
    // Simplified: real foreach needs variable scope, handled in ExpandRef
    return "";
  };

  functions_["if"] = [](const std::vector<std::string>& args) -> std::string {
    // Simplified: real if needs deferred expansion, handled in ExpandRef
    return "";
  };

  functions_["call"] = [](const std::vector<std::string>& args) -> std::string {
    // Simplified: handled in ExpandRef
    return "";
  };

  functions_["error"] = [](const std::vector<std::string>& args) -> std::string {
    if (!args.empty()) {
      fprintf(stderr, "gor_make: *** %s.  Stop.\n", args[0].c_str());
      exit(2);
    }
    return "";
  };

  functions_["warning"] = [](const std::vector<std::string>& args) -> std::string {
    if (!args.empty()) {
      fprintf(stderr, "gor_make: %s\n", args[0].c_str());
    }
    return "";
  };

  functions_["info"] = [](const std::vector<std::string>& args) -> std::string {
    if (!args.empty()) {
      printf("%s\n", args[0].c_str());
    }
    return "";
  };

  functions_["abspath"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    char* abs = realpath(args[0].c_str(), nullptr);
    if (abs) {
      std::string result = abs;
      free(abs);
      return result;
    }
    return args[0];
  };

  functions_["realpath"] = [](const std::vector<std::string>& args) -> std::string {
    if (args.empty()) return "";
    char* real = realpath(args[0].c_str(), nullptr);
    if (real) {
      std::string result = real;
      free(real);
      return result;
    }
    return "";
  };

  functions_["origin"] = [](const std::vector<std::string>& args) -> std::string {
    // Can't access vars_ here, return "file" as a default
    return "file";
  };
}

}  // namespace gormake

extern char** environ;

namespace gormake {

void VariableDB::ImportEnvironment() {
  for (char** env = environ; *env != nullptr; ++env) {
    char* eq = strchr(*env, '=');
    if (eq != nullptr) {
      std::string name(*env, eq - *env);
      std::string value(eq + 1);
      vars_[name] = Variable(name, value, VarFlavor::FLAVOR_RECURSIVE,
                            VarOrigin::ORIGIN_ENVIRONMENT);
      vars_[name].from_env = true;
    }
  }
}

void VariableDB::Set(const std::string& name, const std::string& value,
                      VarFlavor flavor, VarOrigin origin, bool append) {
  auto it = vars_.find(name);
  if (append && it != vars_.end()) {
    // += semantics: append to existing value
    // For recursive vars, append the raw text. For simple vars, expand and append.
    if (it->second.flavor == VarFlavor::FLAVOR_SIMPLE) {
      it->second.value += " " + Expand(value);
    } else {
      it->second.value += " " + value;
    }
    it->second.flavor = flavor;  // Update flavor to match assignment type
  } else {
    vars_[name] = Variable(name, value, flavor, origin);
  }
}

void VariableDB::SetAutomatic(const std::string& name, const std::string& value) {
  if (!auto_scope_.empty()) {
    auto_scope_.back()[name] = value;
  } else {
    vars_[name] = Variable(name, value, VarFlavor::FLAVOR_SIMPLE,
                           VarOrigin::ORIGIN_AUTOMATIC);
  }
}

const Variable* VariableDB::Get(const std::string& name) const {
  // Check automatic scope first
  if (!auto_scope_.empty()) {
    auto it = auto_scope_.back().find(name);
    if (it != auto_scope_.back().end()) {
      static thread_local Variable auto_var;
      auto_var = Variable(name, it->second, VarFlavor::FLAVOR_SIMPLE,
                         VarOrigin::ORIGIN_AUTOMATIC);
      return &auto_var;
    }
  }
  auto it = vars_.find(name);
  if (it != vars_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool VariableDB::IsDefined(const std::string& name) const {
  if (!auto_scope_.empty()) {
    if (auto_scope_.back().count(name) > 0) return true;
  }
  return vars_.count(name) > 0;
}

std::string VariableDB::Expand(const std::string& str) const {
  return Expand(str, "", "", "");
}

std::string VariableDB::Expand(const std::string& str,
                               const std::string& target,
                               const std::string& prereqs,
                               const std::string& stem) const {
  if (expanding_depth_ > 50) {
    return str;  // Prevent infinite recursion
  }
  expanding_depth_++;

  std::string result;
  size_t i = 0;
  while (i < str.size()) {
    char c = str[i];

    // Handle $(...) and ${...}
    if (c == '$' && i + 1 < str.size()) {
      char open = str[i + 1];
      if (open == '(' || open == '{') {
        char close = (open == '(') ? ')' : '}';
        // Find matching close, handling nesting
        int depth = 1;
        size_t j = i + 2;
        while (j < str.size() && depth > 0) {
          if (str[j] == open) depth++;
          else if (str[j] == close) depth--;
          if (depth > 0) j++;
        }
        if (depth == 0) {
          std::string ref = str.substr(i + 2, j - i - 2);
          result += ExpandRef(ref, target, prereqs, stem);
          i = j + 1;
          continue;
        }
      } else if (open == '$') {
        result += '$';
        i += 2;
        continue;
      } else {
        // Single-char variable: $X
        std::string var_name(1, open);
        const Variable* v = Get(var_name);
        if (v) {
          if (v->flavor == VarFlavor::FLAVOR_SIMPLE) {
            result += v->value;
          } else {
            result += Expand(v->value, target, prereqs, stem);
          }
        }
        i += 2;
        continue;
      }
    }

    result += c;
    i++;
  }

  expanding_depth_--;
  return result;
}

std::string VariableDB::ExpandRef(const std::string& ref,
                                  const std::string& target,
                                  const std::string& prereqs,
                                  const std::string& stem) const {
  // First, expand any nested references inside ref
  std::string expanded_ref = Expand(ref, target, prereqs, stem);

  // Check for function call:  function-name args
  size_t space_pos = expanded_ref.find_first_of(" \t");
  if (space_pos != std::string::npos) {
    std::string name = expanded_ref.substr(0, space_pos);
    std::string raw_args = expanded_ref.substr(space_pos + 1);
    // Check if it's a known function
    if (functions_.count(name) > 0 || name == "if" || name == "foreach" ||
        name == "call" || name == "origin" || name == "value") {
      return CallFunction(name, raw_args, target, prereqs, stem);
    }
    // Otherwise it's a variable reference like $(VAR:substitution)
  }

  // Handle automatic variables
  if (expanded_ref == "@") return target;
  if (expanded_ref == "<") return FirstWord(prereqs);
  if (expanded_ref == "^") return prereqs;
  if (expanded_ref == "?") return prereqs;  // simplified
  if (expanded_ref == "*") return stem;
  if (expanded_ref == ".") return "";  // suffix, simplified

  // Handle substitution reference: $(VAR:pattern=replacement)
  size_t colon = expanded_ref.find(':');
  size_t equals = expanded_ref.find('=', colon + 1);
  if (colon != std::string::npos && equals != std::string::npos && colon < equals) {
    std::string var_name = expanded_ref.substr(0, colon);
    std::string pattern = expanded_ref.substr(colon + 1, equals - colon - 1);
    std::string replacement = expanded_ref.substr(equals + 1);
    const Variable* v = Get(var_name);
    if (v) {
      std::string val = (v->flavor == VarFlavor::FLAVOR_SIMPLE)
          ? v->value : Expand(v->value, target, prereqs, stem);
      // Apply patsubst
      std::vector<std::string> args;
      if (pattern.find('%') != std::string::npos) {
        args.push_back(pattern);
        args.push_back(replacement);
        args.push_back(val);
        if (functions_.count("patsubst")) {
          return functions_.at("patsubst")(args);
        }
      } else {
        // Simple suffix substitution
        args.push_back(pattern);
        args.push_back(replacement);
        args.push_back(val);
        if (functions_.count("subst")) {
          return functions_.at("subst")(args);
        }
      }
    }
    return "";
  }

  // Regular variable reference
  const Variable* v = Get(expanded_ref);
  if (v) {
    if (v->flavor == VarFlavor::FLAVOR_SIMPLE) {
      return v->value;
    } else {
      return Expand(v->value, target, prereqs, stem);
    }
  }
  return "";
}

std::string VariableDB::CallFunction(const std::string& name,
                                     const std::string& raw_args,
                                     const std::string& target,
                                     const std::string& prereqs,
                                     const std::string& stem) const {
  // Handle control-flow functions specially since they need deferred expansion
  if (name == "if") {
    // $(if condition,then-part[,else-part])
    auto args = SplitArgs(raw_args);
    if (args.empty()) return "";
    std::string cond = Expand(args[0], target, prereqs, stem);
    if (!cond.empty()) {
      return args.size() > 1 ? Expand(args[1], target, prereqs, stem) : "";
    }
    return args.size() > 2 ? Expand(args[2], target, prereqs, stem) : "";
  }

  if (name == "foreach") {
    // $(foreach var,list,text)
    auto args = SplitArgs(raw_args);
    if (args.size() < 3) return "";
    std::string var_name = Expand(args[0], target, prereqs, stem);
    std::string list = Expand(args[1], target, prereqs, stem);
    std::string text = args[2];
    std::string result;
    size_t start = 0;
    while (start <= list.size()) {
      size_t sp = list.find(' ', start);
      std::string word = (sp == std::string::npos)
          ? list.substr(start) : list.substr(start, sp - start);
      if (!word.empty()) {
        const_cast<VariableDB*>(this)->SetAutomatic(var_name, word);
        if (!result.empty()) result += " ";
        result += Expand(text, target, prereqs, stem);
        const_cast<VariableDB*>(this)->auto_scope_.back().erase(var_name);
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    return result;
  }

  if (name == "call") {
    // $(call function,arg1,arg2,...)
    auto args = SplitArgs(raw_args);
    if (args.empty()) return "";
    std::string func_name = Expand(args[0], target, prereqs, stem);
    const Variable* func_var = Get(func_name);
    if (!func_var) return "";
    std::string body = func_var->value;
    // Set $1, $2, ... for arguments
    const_cast<VariableDB*>(this)->PushAutomaticScope();
    for (size_t i = 1; i < args.size(); ++i) {
      std::string arg_name = std::to_string(i);
      const_cast<VariableDB*>(this)->SetAutomatic(arg_name,
          Expand(args[i], target, prereqs, stem));
    }
    std::string result = Expand(body, target, prereqs, stem);
    const_cast<VariableDB*>(this)->PopAutomaticScope();
    return result;
  }

  if (name == "origin") {
    auto args = SplitArgs(raw_args);
    if (args.empty()) return "undefined";
    const Variable* v = Get(args[0]);
    if (!v) return "undefined";
    switch (v->origin) {
      case VarOrigin::ORIGIN_UNDEFINED: return "undefined";
      case VarOrigin::ORIGIN_DEFAULT: return "default";
      case VarOrigin::ORIGIN_ENVIRONMENT: return "environment";
      case VarOrigin::ORIGIN_ENVIRONMENT_OVERRIDE: return "environment override";
      case VarOrigin::ORIGIN_FILE: return "file";
      case VarOrigin::ORIGIN_COMMAND: return "command line";
      case VarOrigin::ORIGIN_OVERRIDE: return "override";
      case VarOrigin::ORIGIN_AUTOMATIC: return "automatic";
    }
  }

  if (name == "value") {
    auto args = SplitArgs(raw_args);
    if (args.empty()) return "";
    const Variable* v = Get(args[0]);
    return v ? v->value : "";
  }

  // Standard function: expand args first, then call handler
  auto args = SplitArgs(raw_args);
  for (auto& a : args) {
    a = Expand(a, target, prereqs, stem);
  }
  auto it = functions_.find(name);
  if (it != functions_.end()) {
    return it->second(args);
  }
  return "";
}

// static
std::vector<std::string> VariableDB::SplitArgs(const std::string& s) {
  std::vector<std::string> result;
  std::string current;
  int paren_depth = 0;
  bool in_word = false;
  for (char c : s) {
    if (c == '(' || c == '{') {
      paren_depth++;
      current += c;
      in_word = true;
    } else if (c == ')' || c == '}') {
      paren_depth--;
      current += c;
      in_word = true;
    } else if ((c == ',' && paren_depth == 0)) {
      result.push_back(current);
      current.clear();
      in_word = false;
    } else {
      current += c;
      if (c != ' ' && c != '\t') in_word = true;
    }
  }
  if (in_word || !current.empty()) {
    result.push_back(current);
  }
  return result;
}

void VariableDB::PushAutomaticScope() {
  auto_scope_.emplace_back();
}

void VariableDB::PopAutomaticScope() {
  if (!auto_scope_.empty()) {
    auto_scope_.pop_back();
  }
}

}  // namespace gormake
