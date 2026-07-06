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

#ifndef GORMAKE_LIBGORMAKE_VAR_DB_H_
#define GORMAKE_LIBGORMAKE_VAR_DB_H_

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gormake {

// Variable origin tracks where a variable was defined.
enum class VarOrigin {
  ORIGIN_UNDEFINED,
  ORIGIN_DEFAULT,    // built-in default
  ORIGIN_ENVIRONMENT,
  ORIGIN_ENVIRONMENT_OVERRIDE,
  ORIGIN_FILE,       // defined in a makefile
  ORIGIN_COMMAND,    // defined on the command line
  ORIGIN_OVERRIDE,   // defined with override directive
  ORIGIN_AUTOMATIC,  // automatic variable ($@, $<, etc.)
};

// Flavor: recursive (deferred expansion) vs. simple (immediate expansion).
enum class VarFlavor {
  FLAVOR_RECURSIVE,  // VAR = value  (expanded when referenced)
  FLAVOR_SIMPLE,     // VAR := value (expanded at assignment time)
};

struct Variable {
  std::string name;
  std::string value;
  VarFlavor flavor = VarFlavor::FLAVOR_RECURSIVE;
  VarOrigin origin = VarOrigin::ORIGIN_UNDEFINED;
  bool from_env = false;  // came from environment

  Variable() = default;
  Variable(std::string n, std::string v, VarFlavor f, VarOrigin o)
      : name(std::move(n)), value(std::move(v)), flavor(f), origin(o) {}
};

// VariableDB stores all variables and provides expansion.
class VariableDB {
 public:
  VariableDB();
  ~VariableDB();

  // Initialize built-in default variables (CC, CXX, MAKE, etc.)
  void InitDefaults();

  // Import environment variables.
  void ImportEnvironment();

  // Set a variable.  If append is true, value is appended (+= semantics).
  void Set(const std::string& name, const std::string& value,
           VarFlavor flavor, VarOrigin origin, bool append);

  // Set an automatic variable (always simple flavor, automatic origin).
  void SetAutomatic(const std::string& name, const std::string& value);

  // Get a variable.  Returns nullptr if undefined.
  const Variable* Get(const std::string& name) const;

  // Check if a variable is defined (for ifdef/ifndef).
  bool IsDefined(const std::string& name) const;

  // Expand variable references in a string:  $(VAR), ${VAR}, $X
  // Also handles automatic variables and functions.
  std::string Expand(const std::string& str) const;

  // Expand with a context for automatic variables.
  std::string Expand(const std::string& str,
                     const std::string& target,
                     const std::string& prereqs,
                     const std::string& stem) const;

  // Register a function handler for $(function args).
  // Known functions: wildcard, shell, subst, patsubst, strip, etc.
  using FunctionHandler =
      std::function<std::string(const std::vector<std::string>&)>;
  void RegisterFunction(const std::string& name, FunctionHandler handler);

  // Push/pop automatic variable scope (for recipe execution).
  void PushAutomaticScope();
  void PopAutomaticScope();

 private:
  // Expand a single $(...) or ${...} reference.
  std::string ExpandRef(const std::string& ref,
                        const std::string& target,
                        const std::string& prereqs,
                        const std::string& stem) const;

  // Handle built-in functions.
  std::string CallFunction(const std::string& name,
                           const std::string& raw_args,
                           const std::string& target,
                           const std::string& prereqs,
                           const std::string& stem) const;

  // Split function arguments by comma (respecting nested parens).
  static std::vector<std::string> SplitArgs(const std::string& s);

  // Registered function handlers.
  std::unordered_map<std::string, FunctionHandler> functions_;

  // Main variable storage.
  std::unordered_map<std::string, Variable> vars_;

  // Scope stack for automatic variables (pushed/popped per recipe).
  std::vector<std::unordered_map<std::string, std::string>> auto_scope_;

  // True while inside Expand() to prevent infinite recursion.
  mutable int expanding_depth_ = 0;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_VAR_DB_H_
