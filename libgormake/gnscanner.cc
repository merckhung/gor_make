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

#include "gnscanner.h"
#include "buildenginebase.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unordered_set>

namespace gormake {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

static std::string DirName(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

static std::string Trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() &&
         (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n')) {
    start++;
  }
  size_t end = s.size();
  while (end > start &&
         (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ||
          s[end - 1] == '\n')) {
    end--;
  }
  return s.substr(start, end - start);
}

static bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool IsIdentStartChar(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static std::string JsonEscape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
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

static void OutputJsonArray(const std::vector<std::string>& arr) {
  printf("[");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) printf(", ");
    printf("\"%s\"", JsonEscape(arr[i]).c_str());
  }
  printf("]");
}

static bool IsTargetType(const std::string& name) {
  // Accept any identifier that's not a GN keyword/control statement.
  // GN has many target types beyond the basic ones: rust_static_library,
  // buildflag_header, java_library, android_library, etc.
  if (name.empty()) return false;

  static const std::unordered_set<std::string> kKeywords = {
    "if", "else", "for", "foreach", "import", "template",
    "assert", "print", "defined", "not", "and", "or",
  };
  if (kKeywords.count(name) > 0) return false;

  // Must be a valid identifier (letters, digits, underscores)
  for (char c : name) {
    if (!isalnum(c) && c != '_') return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// GnScanner public methods
// ---------------------------------------------------------------------------

GnScanner::GnScanner() : inTarget_(false), braceDepth_(0) {}

GnScanner::~GnScanner() {}

bool GnScanner::ScanFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }

  current_.path = path;
  current_.srcDir = DirName(path);

  // Read line by line, joining line continuations (backslash-newline).
  std::string buffer;
  std::string line;
  bool continuation = false;

  while (std::getline(file, line)) {
    // Strip trailing \r if present (Windows line endings).
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (continuation) {
      buffer += line;
    } else {
      buffer = line;
    }

    // Check for line continuation: backslash at end of line.
    std::string trimmed = Trim(buffer);
    if (!trimmed.empty() && trimmed.back() == '\\') {
      // Remove the backslash and continue accumulating.
      buffer = trimmed.substr(0, trimmed.size() - 1);
      continuation = true;
      continue;
    }

    continuation = false;
    ProcessLine(buffer);
    buffer.clear();
  }

  // Process any remaining buffered line.
  if (!buffer.empty()) {
    ProcessLine(buffer);
  }

  return true;
}

void GnScanner::ScanDirectory(const std::string& dirPath) {
  std::error_code ec;
  for (auto& entry : fs::recursive_directory_iterator(dirPath, ec)) {
    if (ec) continue;
    if (entry.path().filename() == "BUILD.gn") {
      std::string entryStr = entry.path().string();
      // Skip common output/build directories.
      if (entryStr.find("/out/") != std::string::npos) continue;
      if (entryStr.find("/bazel-") != std::string::npos) continue;
      if (entryStr.find("/.git/") != std::string::npos) continue;
      try {
        ScanFile(entryStr);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "gor_make: [warning] error parsing %s: %s\n",
                     entryStr.c_str(), e.what());
      } catch (...) {
        std::fprintf(stderr, "gor_make: [warning] unknown error parsing %s\n",
                     entryStr.c_str());
      }
    }
  }
}

const std::vector<GnTarget>& GnScanner::GetTargets() const {
  return targets_;
}

// ---------------------------------------------------------------------------
// Comment stripping
// ---------------------------------------------------------------------------

std::string GnScanner::StripComment(const std::string& line) const {
  std::string result;
  bool inString = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
      inString = !inString;
    }
    if (c == '#' && !inString) {
      break;
    }
    result += c;
  }
  return result;
}

// ---------------------------------------------------------------------------
// List parsing
// ---------------------------------------------------------------------------

// Parses a GN list literal like:  ["a", "b", "c"]
// Also handles a bare string: "hello"
// Also handles comma/space separated bare tokens as a fallback.
std::vector<std::string> GnScanner::ParseList(const std::string& s) const {
  std::vector<std::string> result;
  std::string str = Trim(s);

  if (str.empty()) return result;

  // Case: list literal [ ... ]
  if (str.front() == '[') {
    size_t i = 1;
    while (i < str.size()) {
      // Skip whitespace and commas.
      while (i < str.size() &&
             (str[i] == ' ' || str[i] == '\t' || str[i] == ',' ||
              str[i] == '\n' || str[i] == '\r')) {
        i++;
      }
      if (i >= str.size() || str[i] == ']') break;

      if (str[i] == '"') {
        // String literal.
        i++;  // skip opening quote
        std::string token;
        while (i < str.size() && str[i] != '"') {
          if (str[i] == '\\' && i + 1 < str.size()) {
            char next = str[i + 1];
            switch (next) {
              case 'n':
                token += '\n';
                break;
              case 't':
                token += '\t';
                break;
              case 'r':
                token += '\r';
                break;
              case '\\':
                token += '\\';
                break;
              case '"':
                token += '"';
                break;
              default:
                token += next;
                break;
            }
            i += 2;
          } else {
            token += str[i];
            i++;
          }
        }
        if (i < str.size()) i++;  // skip closing quote
        result.push_back(token);
      } else {
        // Bare token (e.g., a variable reference or number).
        std::string token;
        while (i < str.size() && str[i] != ',' && str[i] != ' ' &&
               str[i] != '\t' && str[i] != ']') {
          token += str[i];
          i++;
        }
        if (!token.empty()) result.push_back(token);
      }
    }
    return result;
  }

  // Case: single string literal "..."
  if (str.front() == '"') {
    std::string token;
    size_t i = 1;
    while (i < str.size() && str[i] != '"') {
      if (str[i] == '\\' && i + 1 < str.size()) {
        char next = str[i + 1];
        switch (next) {
          case 'n':
            token += '\n';
            break;
          case 't':
            token += '\t';
            break;
          case 'r':
            token += '\r';
            break;
          case '\\':
            token += '\\';
            break;
          case '"':
            token += '"';
            break;
          default:
            token += next;
            break;
        }
        i += 2;
      } else {
        token += str[i];
        i++;
      }
    }
    result.push_back(token);
    return result;
  }

  // Case: bare token (could be a variable reference to a list).
  // Return it as a single-element list; the caller may resolve it.
  result.push_back(str);
  return result;
}

// ---------------------------------------------------------------------------
// Variable expansion
// ---------------------------------------------------------------------------

std::string GnScanner::ExpandVars(const std::string& s) const {
  std::string result;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '$' && i + 1 < s.size()) {
      if (s[i + 1] == '{') {
        size_t close = s.find('}', i + 2);
        if (close != std::string::npos) {
          std::string varName = s.substr(i + 2, close - i - 2);
          auto it = variables_.find(varName);
          if (it != variables_.end()) {
            result += it->second;
          }
          i = close;
          continue;
        }
      } else if (IsIdentStartChar(s[i + 1])) {
        size_t j = i + 1;
        while (j < s.size() && IsIdentChar(s[j])) j++;
        std::string varName = s.substr(i + 1, j - i - 1);
        auto it = variables_.find(varName);
        if (it != variables_.end()) {
          result += it->second;
        }
        i = j - 1;
        continue;
      }
    }
    result += s[i];
  }
  return result;
}

std::vector<std::string> GnScanner::ResolveValue(const std::string& s) const {
  std::string trimmed = Trim(s);

  // If it's a list literal or string, parse directly.
  if (!trimmed.empty() && (trimmed.front() == '[' || trimmed.front() == '"')) {
    return ParseList(trimmed);
  }

  // If it's a bare identifier that is a known list variable, return that list.
  {
    auto it = listVariables_.find(trimmed);
    if (it != listVariables_.end()) {
      return it->second;
    }
  }

  // If it's a bare identifier that is a known scalar variable, return
  // it as a single-element list.
  {
    auto it = variables_.find(trimmed);
    if (it != variables_.end() && !it->second.empty()) {
      // If the variable's value looks like a list, try to parse it.
      std::string val = Trim(it->second);
      if (!val.empty() && val.front() == '[') {
        return ParseList(val);
      }
      return {val};
    }
  }

  // Fallback: parse as a list (handles comma/space-separated bare tokens).
  return ParseList(trimmed);
}

// ---------------------------------------------------------------------------
// Property assignment
// ---------------------------------------------------------------------------

void GnScanner::AssignProperty(const std::string& name,
                               const std::vector<std::string>& values,
                               bool append) {
  if (name == "sources") {
    if (append) {
      current_.srcs.insert(current_.srcs.end(), values.begin(), values.end());
    } else {
      current_.srcs = values;
    }
  } else if (name == "deps") {
    if (append) {
      current_.deps.insert(current_.deps.end(), values.begin(), values.end());
    } else {
      current_.deps = values;
    }
  } else if (name == "public_deps") {
    if (append) {
      current_.publicDeps.insert(current_.publicDeps.end(), values.begin(),
                                 values.end());
    } else {
      current_.publicDeps = values;
    }
  } else if (name == "cflags") {
    if (append) {
      current_.cflags.insert(current_.cflags.end(), values.begin(),
                             values.end());
    } else {
      current_.cflags = values;
    }
  } else if (name == "cflags_cc") {
    if (append) {
      current_.cppflags.insert(current_.cppflags.end(), values.begin(),
                               values.end());
    } else {
      current_.cppflags = values;
    }
  } else if (name == "ldflags") {
    if (append) {
      current_.ldflags.insert(current_.ldflags.end(), values.begin(),
                               values.end());
    } else {
      current_.ldflags = values;
    }
  } else if (name == "include_dirs") {
    if (append) {
      current_.includeDirs.insert(current_.includeDirs.end(), values.begin(),
                                   values.end());
    } else {
      current_.includeDirs = values;
    }
  } else if (name == "defines") {
    if (append) {
      current_.defines.insert(current_.defines.end(), values.begin(),
                               values.end());
    } else {
      current_.defines = values;
    }
  } else if (name == "configs" || name == "public_configs") {
    if (append) {
      current_.configs.insert(current_.configs.end(), values.begin(),
                              values.end());
    } else {
      current_.configs = values;
    }
  }
  // Other properties (e.g., output_name, testonly) are ignored.
}

// ---------------------------------------------------------------------------
// Line processing
// ---------------------------------------------------------------------------

// Attempts to parse a target block header like:
//   executable("name") {
//   static_library("name") {
//   config("name") {
// On success, sets targetType and targetName. Returns true.
static bool ParseTargetHeader(const std::string& trimmed,
                              std::string* targetType,
                              std::string* targetName) {
  // Find the opening paren.
  size_t paren = trimmed.find('(');
  if (paren == std::string::npos) return false;

  // The type is the identifier before the paren.
  std::string type = Trim(trimmed.substr(0, paren));
  if (!IsTargetType(type)) return false;

  // Find the closing paren.
  size_t closeParen = trimmed.find(')', paren + 1);
  if (closeParen == std::string::npos) return false;

  // Extract the argument (the target name string).
  std::string arg = Trim(trimmed.substr(paren + 1, closeParen - paren - 1));

  // Remove surrounding quotes if present.
  if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
    arg = arg.substr(1, arg.size() - 2);
  }

  *targetType = type;
  *targetName = arg;
  return true;
}

void GnScanner::ProcessLine(const std::string& rawLine) {
  // Strip comments.
  std::string line = StripComment(rawLine);
  std::string trimmed = Trim(line);

  if (trimmed.empty()) return;

  // ---- Detect target block start: type("name") { ----
  if (!inTarget_) {
    // Check if this line starts a target block.
    // It must contain a target type call followed by '{'.
    size_t bracePos = trimmed.find('{');
    if (bracePos != std::string::npos) {
      std::string beforeBrace = Trim(trimmed.substr(0, bracePos));
      std::string targetType;
      std::string targetName;
      if (ParseTargetHeader(beforeBrace, &targetType, &targetName)) {
        // Start a new target. Preserve path/srcDir set by ScanFile.
        std::string savedPath = current_.path;
        std::string savedSrcDir = current_.srcDir;
        current_ = GnTarget();
        current_.type = targetType;
        current_.name = targetName;
        current_.path = savedPath;
        current_.srcDir = savedSrcDir;

        inTarget_ = true;
        braceDepth_ = 1;

        // Process any content after the opening brace on the same line.
        std::string afterBrace = trimmed.substr(bracePos + 1);
        if (!Trim(afterBrace).empty()) {
          ProcessLine(afterBrace);
        }
        return;
      }
    }
  } else {
    // ---- Inside a target block ----
    // Track brace depth to handle nested blocks.
    // We need to scan for braces, respecting strings.
    bool inString = false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
      char c = trimmed[i];
      if (c == '"' && (i == 0 || trimmed[i - 1] != '\\')) {
        inString = !inString;
      }
      if (!inString) {
        if (c == '{') {
          braceDepth_++;
        } else if (c == '}') {
          braceDepth_--;
          if (braceDepth_ == 0) {
            // End of target block.
            // Process any content before the closing brace.
            std::string before = trimmed.substr(0, i);
            if (!Trim(before).empty()) {
              ProcessLine(before);
            }
            // Flush the target.
            if (!current_.name.empty()) {
              targets_.push_back(current_);
            }
            // Preserve path/srcDir for the next target in the same file.
            std::string savedPath = current_.path;
            std::string savedSrcDir = current_.srcDir;
            inTarget_ = false;
            current_ = GnTarget();
            current_.path = savedPath;
            current_.srcDir = savedSrcDir;

            // Process any content after the closing brace (rare).
            std::string after = trimmed.substr(i + 1);
            if (!Trim(after).empty()) {
              ProcessLine(after);
            }
            return;
          }
        }
      }
    }

    // Still inside the target block at depth >= 1.
    // Parse property assignments: name = value  OR  name += value
    size_t eqPos = std::string::npos;
    bool isAppend = false;
    bool inStr = false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
      char c = trimmed[i];
      if (c == '"' && (i == 0 || trimmed[i - 1] != '\\')) {
        inStr = !inStr;
      }
      if (!inStr && c == '=') {
        if (i > 0 && trimmed[i - 1] == '+') {
          isAppend = true;
          eqPos = i - 1;
        } else {
          isAppend = false;
          eqPos = i;
        }
        break;
      }
    }

    if (eqPos != std::string::npos) {
      std::string propName = Trim(trimmed.substr(0, eqPos));
      size_t valStart = isAppend ? eqPos + 2 : eqPos + 1;
      std::string rawValue = Trim(trimmed.substr(valStart));

      // Expand variable references in the value.
      std::string expanded = ExpandVars(rawValue);

      // Resolve to a list of strings.
      std::vector<std::string> values = ResolveValue(expanded);

      // Assign to the appropriate property.
      AssignProperty(propName, values, isAppend);

      // Also store as a local variable for later references.
      if (isAppend) {
        auto it = listVariables_.find(propName);
        if (it != listVariables_.end()) {
          it->second.insert(it->second.end(), values.begin(), values.end());
        } else {
          listVariables_[propName] = values;
        }
      } else {
        listVariables_[propName] = values;
      }
      // Also keep a scalar form.
      std::string scalar;
      for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) scalar += " ";
        scalar += values[i];
      }
      variables_[propName] = scalar;
      return;
    }

    // If not an assignment, it might be a nested function call or
    // a sub-block (e.g., if/else, foreach). We ignore those for now.
    return;
  }

  // ---- Top-level (outside any target block) ----
  // Parse top-level variable assignments: myvar = value  OR  myvar += value
  size_t eqPos = std::string::npos;
  bool isAppend = false;
  bool inStr = false;
  for (size_t i = 0; i < trimmed.size(); ++i) {
    char c = trimmed[i];
    if (c == '"' && (i == 0 || trimmed[i - 1] != '\\')) {
      inStr = !inStr;
    }
    if (!inStr && c == '=') {
      if (i > 0 && trimmed[i - 1] == '+') {
        isAppend = true;
        eqPos = i - 1;
      } else {
        isAppend = false;
        eqPos = i;
      }
      break;
    }
  }

  if (eqPos != std::string::npos) {
    std::string varName = Trim(trimmed.substr(0, eqPos));
    size_t valStart = isAppend ? eqPos + 2 : eqPos + 1;
    std::string rawValue = Trim(trimmed.substr(valStart));
    std::string expanded = ExpandVars(rawValue);

    // Store as a list variable if the value is a list literal.
    if (!expanded.empty() && expanded.front() == '[') {
      std::vector<std::string> values = ParseList(expanded);
      if (isAppend) {
        auto it = listVariables_.find(varName);
        if (it != listVariables_.end()) {
          it->second.insert(it->second.end(), values.begin(), values.end());
        } else {
          listVariables_[varName] = values;
        }
      } else {
        listVariables_[varName] = values;
      }
      // Also keep a scalar form.
      std::string scalar;
      for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) scalar += " ";
        scalar += values[i];
      }
      variables_[varName] = scalar;
    } else {
      // Scalar variable.
      if (isAppend) {
        variables_[varName] += " " + expanded;
      } else {
        variables_[varName] = expanded;
      }
    }
    return;
  }

  // Handle import("//path/file.gni") — scan the imported file
  if (trimmed.substr(0, 6) == "import") {
    // Extract the string argument
    size_t openParen = trimmed.find('(');
    size_t closeParen = trimmed.rfind(')');
    if (openParen != std::string::npos && closeParen != std::string::npos) {
      std::string arg = trimmed.substr(openParen + 1, closeParen - openParen - 1);
      arg = Trim(arg);
      // Remove quotes
      if (arg.size() >= 2 && (arg[0] == '"' || arg[0] == '\'')) {
        arg = arg.substr(1, arg.size() - 2);
      }
      // Resolve //path to filesystem path
      std::string importPath;
      if (arg.substr(0, 2) == "//") {
        // Try relative to rootDir_ if set, otherwise current srcDir
        // Walk up to find the source root
        importPath = current_.srcDir + "/../" + arg.substr(2);
      } else if (arg[0] == '/') {
        importPath = arg;
      } else {
        importPath = current_.srcDir + "/" + arg;
      }
      // Check if file exists
      struct stat st;
      if (stat(importPath.c_str(), &st) == 0) {
        // Avoid re-importing the same file
        if (visitedFiles_.find(importPath) == visitedFiles_.end()) {
          visitedFiles_.insert(importPath);
          ScanFile(importPath);
        }
      }
    }
    return;
  }

  // Other top-level constructs (template definitions, etc.)
  // are not handled in this simplified scanner.
}

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

void GnScanner::OutputJson() const {
  printf("{\n");
  printf("  \"format\": \"build.gn\",\n");
  printf("  \"target_count\": %zu,\n", targets_.size());
  printf("  \"targets\": [\n");

  bool first = true;
  for (const auto& tgt : targets_) {
    if (!first) printf(",\n");
    first = false;

    printf("    {\n");
    printf("      \"name\": \"%s\",\n", JsonEscape(tgt.name).c_str());
    printf("      \"type\": \"%s\",\n", JsonEscape(tgt.type).c_str());
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(tgt.srcDir).c_str());
    printf("      \"path\": \"%s\",\n", JsonEscape(tgt.path).c_str());

    printf("      \"srcs\": ");
    OutputJsonArray(tgt.srcs);
    printf(",\n");

    printf("      \"deps\": ");
    OutputJsonArray(tgt.deps);
    printf(",\n");

    printf("      \"public_deps\": ");
    OutputJsonArray(tgt.publicDeps);
    printf(",\n");

    printf("      \"cflags\": ");
    OutputJsonArray(tgt.cflags);
    printf(",\n");

    printf("      \"cppflags\": ");
    OutputJsonArray(tgt.cppflags);
    printf(",\n");

    printf("      \"ldflags\": ");
    OutputJsonArray(tgt.ldflags);
    printf(",\n");

    printf("      \"include_dirs\": ");
    OutputJsonArray(tgt.includeDirs);
    printf(",\n");

    printf("      \"defines\": ");
    OutputJsonArray(tgt.defines);
    printf(",\n");

    printf("      \"configs\": ");
    OutputJsonArray(tgt.configs);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

}  // namespace gormake

// =====================================================================
// GnScanner — build engine (compile + link without ninja)
// =====================================================================

#include <sys/stat.h>
#include <unistd.h>

namespace gormake {

std::string GnScanner::GetOutputPath(const GnTarget& target) const {
  std::string dir = target.srcDir.empty() ? "." : target.srcDir;
  return dir + "/build/" + target.name;
}

std::string GnScanner::GetObjectPath(const GnTarget& target,
                                      const std::string& src) const {
  std::string dir = target.srcDir.empty() ? "." : target.srcDir;
  return dir + "/build/obj/" + target.name + "/" + buildutil::BaseName(src) + ".o";
}

bool GnScanner::NeedsRecompile(const std::string& objFile,
                                const std::string& srcFile) const {
  return buildutil::NeedsRecompile(objFile, srcFile);
}

bool GnScanner::ExecuteCmd(const std::string& cmd) {
  if (dryRun_) { std::printf("  %s\n", cmd.c_str()); return true; }
  std::printf("  %s\n", cmd.c_str());
  return system(cmd.c_str()) == 0;
}

bool GnScanner::CompileSource(const GnTarget& target, const std::string& src,
                               const std::string& objFile) {
  if (!NeedsRecompile(objFile, src)) {
    std::printf("  [skip] %s (up-to-date)\n", src.c_str());
    return true;
  }

  std::string srcPath = src;
  if (srcPath[0] != '/') {
    std::string dir = target.srcDir.empty() ? "." : target.srcDir;
    srcPath = dir + "/" + srcPath;
  }

  std::string compiler = buildutil::GetCompiler(src);
  std::string cmd = compiler + " -c -o " + objFile + " " + srcPath;

  for (const auto& f : target.cflags) cmd += " " + f;
  for (const auto& f : target.cppflags) cmd += " " + f;
  for (const auto& d : target.defines) cmd += " -D" + d;
  for (const auto& inc : target.includeDirs) {
    if (inc[0] == '/') cmd += " -I" + inc;
    else {
      std::string dir = target.srcDir.empty() ? "." : target.srcDir;
      cmd += " -I" + dir + "/" + inc;
    }
  }
  cmd += " -Wall";

  std::string objDir = objFile.substr(0, objFile.find_last_of('/'));
  if (!dryRun_) buildutil::MkdirP(objDir);
  return ExecuteCmd(cmd);
}

bool GnScanner::LinkTarget(const GnTarget& target) {
  std::string outputPath = GetOutputPath(target);
  std::string dir = target.srcDir.empty() ? "." : target.srcDir;
  if (!dryRun_) buildutil::MkdirP(dir + "/build");

  std::string objFiles;
  for (const auto& src : target.srcs) {
    objFiles += " " + GetObjectPath(target, src);
  }

  if (target.type == "static_library") {
    std::string cmd = "ar rcs " + outputPath + ".a" + objFiles;
    return ExecuteCmd(cmd);
  }

  if (target.type == "shared_library") {
    std::string cmd = "g++ -shared -o " + outputPath + ".so" + objFiles;
    for (const auto& f : target.ldflags) cmd += " " + f;
    return ExecuteCmd(cmd);
  }

  if (target.type == "executable") {
    std::string cmd = "g++ -o " + outputPath + objFiles;
    for (const auto& f : target.ldflags) cmd += " " + f;

    // Link deps (static/shared libraries built by us)
    for (const auto& dep : target.deps) {
      std::string depName = dep;
      if (!depName.empty() && depName[0] == ':') depName = depName.substr(1);
      for (const auto& t : targets_) {
        if (t.name == depName) {
          std::string path = GetOutputPath(t);
          if (t.type == "static_library") cmd += " " + path + ".a";
          else if (t.type == "shared_library") cmd += " " + path + ".so";
          break;
        }
      }
    }
    return ExecuteCmd(cmd);
  }

  return true;
}

bool GnScanner::BuildTarget(const GnTarget& target) {
  if (target.type == "config" || target.type == "group" ||
      target.type == "action" || target.type == "action_foreach" ||
      target.type == "template") {
    return true;
  }
  if (target.srcs.empty()) {
    std::printf("  [skip] %s (%s) — no sources\n",
                target.name.c_str(), target.type.c_str());
    return true;
  }

  std::printf("Building: %s (%s)\n", target.name.c_str(), target.type.c_str());

  for (const auto& src : target.srcs) {
    std::string objFile = GetObjectPath(target, src);
    if (!CompileSource(target, src, objFile)) {
      std::fprintf(stderr, "gor_make: *** [%s] Error compiling %s\n",
                   target.name.c_str(), src.c_str());
      return false;
    }
  }

  if (!LinkTarget(target)) {
    std::fprintf(stderr, "gor_make: *** [%s] Error linking\n",
                 target.name.c_str());
    return false;
  }
  return true;
}

int GnScanner::BuildAll() {
  std::printf("Building %zu targets...\n", targets_.size());

  // Build static/shared libraries first, then executables
  for (const auto& t : targets_) {
    if (t.type == "static_library" || t.type == "shared_library") {
      if (!BuildTarget(t)) return 1;
    }
  }
  for (const auto& t : targets_) {
    if (t.type == "executable" || t.type == "source_set") {
      if (!BuildTarget(t)) return 1;
    }
  }

  std::printf("Build complete.\n");
  return 0;
}

}  // namespace gormake
