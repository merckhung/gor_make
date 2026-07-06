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

#include "bp_parser.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gormake {

// =====================================================================
// BpValue helpers
// =====================================================================

std::string BpValue::AsString() const {
  if (type == STRING) {
    return str_val;
  }
  return "";
}

std::vector<std::string> BpValue::AsStringList() const {
  std::vector<std::string> result;
  if (type == LIST) {
    for (const auto& v : list_val) {
      if (v.type == STRING) {
        result.push_back(v.str_val);
      }
    }
  }
  return result;
}

// static
BpValue BpValue::String(const std::string& s) {
  BpValue v;
  v.type = STRING;
  v.str_val = s;
  return v;
}

// static
BpValue BpValue::List(const std::vector<std::string>& vec) {
  BpValue v;
  v.type = LIST;
  for (const auto& s : vec) {
    v.list_val.push_back(BpValue::String(s));
  }
  return v;
}

// =====================================================================
// BpParser public interface
// =====================================================================

BpParser::BpParser() = default;
BpParser::~BpParser() = default;

bool BpParser::ParseFile(const std::string& path, BpFile* result) {
  std::ifstream in(path);
  if (!in.is_open()) {
    error_ = "cannot open file: " + path + ": " + std::strerror(errno);
    return false;
  }

  std::stringstream ss;
  ss << in.rdbuf();
  std::string source = ss.str();

  // Determine the directory of the file for glob expansion.
  size_t slash = path.find_last_of('/');
  base_dir_ = (slash != std::string::npos) ? path.substr(0, slash) : ".";

  return ParseSource(source, path, result);
}

bool BpParser::ParseSource(const std::string& source,
                            const std::string& path, BpFile* result) {
  tokens_.clear();
  pos_ = 0;
  error_.clear();

  if (!Tokenize(source, &tokens_)) {
    return false;
  }

  if (base_dir_.empty()) {
    size_t slash = path.find_last_of('/');
    base_dir_ = (slash != std::string::npos) ? path.substr(0, slash) : ".";
  }

  variables_ = &result->variables;

  if (!ParseTopLevel(result)) {
    return false;
  }

  return true;
}

const std::string& BpParser::GetError() const { return error_; }

// =====================================================================
// Tokenizer
// =====================================================================

bool BpParser::Tokenize(const std::string& source,
                          std::vector<Token>* tokens) {
  size_t i = 0;
  int line = 1;
  const size_t n = source.size();

  while (i < n) {
    char c = source[i];

    // ---- Whitespace ----
    if (c == '\n') {
      ++line;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }

    // ---- Comments ----
    if (c == '/' && i + 1 < n && source[i + 1] == '/') {
      // Single-line comment: skip to end of line.
      while (i < n && source[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && source[i + 1] == '*') {
      // Multi-line comment: skip to matching */.
      int start_line = line;
      i += 2;  // skip /*
      while (i < n) {
        if (source[i] == '\n') {
          ++line;
        }
        if (source[i] == '*' && i + 1 < n && source[i + 1] == '/') {
          i += 2;
          break;
        }
        ++i;
      }
      if (i >= n) {
        error_ = "unterminated /* comment starting at line " +
                 std::to_string(start_line);
        return false;
      }
      continue;
    }

    // ---- Single-char tokens ----
    Token tok;
    tok.line = line;
    switch (c) {
      case '{':
        tok.type = TOK_LBRACE;
        break;
      case '}':
        tok.type = TOK_RBRACE;
        break;
      case '[':
        tok.type = TOK_LBRACKET;
        break;
      case ']':
        tok.type = TOK_RBRACKET;
        break;
      case ':':
        tok.type = TOK_COLON;
        break;
      case ',':
        tok.type = TOK_COMMA;
        break;
      case '+':
        tok.type = TOK_PLUS;
        break;
      case '=':
        tok.type = TOK_ASSIGN;
        break;
      case '(':
        tok.type = TOK_LPAREN;
        break;
      case ')':
        tok.type = TOK_RPAREN;
        break;
      default:
        tok.type = TOK_EOF;  // sentinel; handled below
        break;
    }
    if (tok.type != TOK_EOF) {
      tok.value = std::string(1, c);
      tokens->push_back(tok);
      ++i;
      continue;
    }

    // ---- String literal ----
    if (c == '"') {
      int start_line = line;
      ++i;  // skip opening quote
      std::string val;
      bool closed = false;
      while (i < n) {
        char ch = source[i];
        if (ch == '\\' && i + 1 < n) {
          char next = source[i + 1];
          switch (next) {
            case 'n':
              val.push_back('\n');
              break;
            case 't':
              val.push_back('\t');
              break;
            case 'r':
              val.push_back('\r');
              break;
            case '"':
              val.push_back('"');
              break;
            case '\\':
              val.push_back('\\');
              break;
            case '\n':
              ++line;
              break;  // line continuation
            default:
              val.push_back(next);
              break;
          }
          i += 2;
          continue;
        }
        if (ch == '"') {
          ++i;
          closed = true;
          break;
        }
        if (ch == '\n') {
          ++line;
        }
        val.push_back(ch);
        ++i;
      }
      if (!closed) {
        error_ = "unterminated string starting at line " +
                 std::to_string(start_line);
        return false;
      }
      tok.type = TOK_STRING;
      tok.value = val;
      tokens->push_back(tok);
      continue;
    }

    // ---- Integer literal ----
    if (c >= '0' && c <= '9') {
      std::string num;
      while (i < n && source[i] >= '0' && source[i] <= '9') {
        num.push_back(source[i]);
        ++i;
      }
      tok.type = TOK_INT;
      tok.value = num;
      tokens->push_back(tok);
      continue;
    }

    // ---- Identifier ----
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      std::string ident;
      while (i < n) {
        char ch = source[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') {
          ident.push_back(ch);
          ++i;
        } else {
          break;
        }
      }
      tok.type = TOK_IDENT;
      tok.value = ident;
      tokens->push_back(tok);
      continue;
    }

    // ---- Unknown character ----
    error_ = "unexpected character '" + std::string(1, c) + "' at line " +
             std::to_string(line);
    return false;
  }

  // End-of-file sentinel.
  Token eof;
  eof.type = TOK_EOF;
  eof.value = "";
  eof.line = line;
  tokens->push_back(eof);
  return true;
}

// =====================================================================
// Token helpers
// =====================================================================

const BpParser::Token& BpParser::Current() const {
  // tokens_ always ends with TOK_EOF, so pos_ is always in range.
  return tokens_[pos_];
}

void BpParser::Advance() {
  if (pos_ < tokens_.size() - 1) {
    ++pos_;
  }
}

bool BpParser::AtEnd() const { return Current().type == TOK_EOF; }

bool BpParser::Peek(TokenType type) const { return Current().type == type; }

bool BpParser::Accept(TokenType type) {
  if (Current().type == type) {
    Advance();
    return true;
  }
  return false;
}

bool BpParser::Expect(TokenType type, const std::string& what) {
  if (Current().type == type) {
    Advance();
    return true;
  }
  return Error("expected " + what + " but got '" + Current().value + "' at line " +
               std::to_string(Current().line));
}

bool BpParser::Error(const std::string& msg) {
  error_ = msg;
  return false;
}

bool BpParser::ErrorAt(const Token& tok, const std::string& msg) {
  error_ = msg + " at line " + std::to_string(tok.line);
  return false;
}

// =====================================================================
// Parser — top level
// =====================================================================

bool BpParser::ParseTopLevel(BpFile* result) {
  while (!AtEnd()) {
    const Token& tok = Current();

    // An identifier at the top level is either a module type or a variable
    // name, depending on whether it is followed by '{' or '='.
    if (tok.type != TOK_IDENT) {
      return Error("expected module type or variable name at line " +
                   std::to_string(tok.line) + ", got '" + tok.value + "'");
    }

    // Look ahead at the next token to decide.
    // We need to peek two tokens: current is IDENT, next determines kind.
    size_t next = pos_ + 1;
    if (next >= tokens_.size()) {
      return Error("unexpected end of input after identifier '" + tok.value +
                   "' at line " + std::to_string(tok.line));
    }

    const Token& lookahead = tokens_[next];
    if (lookahead.type == TOK_LBRACE) {
      // Module definition.
      BpModule module;
      if (!ParseModule(&module)) {
        return false;
      }
      result->modules.push_back(std::move(module));
    } else if (lookahead.type == TOK_ASSIGN) {
      // Top-level variable assignment.
      std::string var_name = tok.value;
      Advance();  // consume IDENT
      Advance();  // consume '='
      BpValue val;
      if (!ParseValue(&val)) {
        return false;
      }
      (*variables_)[var_name] = val;
    } else if (lookahead.type == TOK_PLUS) {
      // += append?  Blueprint doesn't have +=, but handle gracefully.
      return Error("unexpected '+', did you mean '='? at line " +
                   std::to_string(tok.line));
    } else {
      return Error("expected '{' or '=' after identifier '" + tok.value +
                   "' at line " + std::to_string(tok.line));
    }
  }
  return true;
}

// =====================================================================
// Parser — modules
// =====================================================================

bool BpParser::ParseModule(BpModule* module) {
  // We are positioned at the module type identifier.
  module->type = Current().value;
  Advance();  // consume type

  if (!Expect(TOK_LBRACE, "'{'")) {
    return false;
  }

  // Parse properties.
  while (!AtEnd() && Current().type != TOK_RBRACE) {
    if (!ParseProperty(&module->properties)) {
      return false;
    }
    // Trailing comma is allowed.
    Accept(TOK_COMMA);
  }

  if (!Expect(TOK_RBRACE, "'}'")) {
    return false;
  }

  // Extract the module name for convenience.
  auto it = module->properties.find("name");
  if (it != module->properties.end() && it->second.IsString()) {
    module->name = it->second.str_val;
  }

  return true;
}

bool BpParser::ParseProperty(std::map<std::string, BpValue>* props) {
  if (Current().type != TOK_IDENT) {
    return Error("expected property name but got '" + Current().value +
                 "' at line " + std::to_string(Current().line));
  }

  std::string key = Current().value;
  Advance();  // consume key

  if (!Expect(TOK_COLON, "':'")) {
    return false;
  }

  BpValue val;
  if (!ParseValue(&val)) {
    return false;
  }

  // If the key already exists and both are lists, merge (Blueprint appends).
  auto it = props->find(key);
  if (it != props->end() && it->second.IsList() && val.IsList()) {
    for (auto& e : val.list_val) {
      it->second.list_val.push_back(std::move(e));
    }
  } else {
    (*props)[key] = std::move(val);
  }

  return true;
}

// =====================================================================
// Parser — values
// =====================================================================

bool BpParser::ParseValue(BpValue* value) {
  // Parse the first primary.
  BpValue left;
  if (!ParsePrimary(&left)) {
    return false;
  }

  // Handle + operator chains: a + b + c ...
  while (Current().type == TOK_PLUS) {
    Advance();  // consume '+'
    BpValue right;
    if (!ParsePrimary(&right)) {
      return false;
    }

    // Concatenate based on types.
    if (left.type == BpValue::STRING && right.type == BpValue::STRING) {
      // String concatenation.
      left.str_val += right.str_val;
    } else if (left.type == BpValue::LIST && right.type == BpValue::LIST) {
      // List concatenation.
      for (const auto& e : right.list_val) {
        left.list_val.push_back(e);
      }
    } else if (left.type == BpValue::MAP && right.type == BpValue::MAP) {
      // Map merge: right overrides left for duplicate keys.
      for (const auto& [k, v] : right.map_val) {
        left.map_val[k] = v;
      }
    } else if (left.type == BpValue::STRING && right.type == BpValue::LIST) {
      // String + list: prepend the string as a single-element list.
      left.list_val.insert(left.list_val.begin(), BpValue::String(left.str_val));
      left.type = BpValue::LIST;
      left.str_val.clear();
      for (const auto& e : right.list_val) {
        left.list_val.push_back(e);
      }
    } else if (left.type == BpValue::LIST && right.type == BpValue::STRING) {
      // List + string: append.
      left.list_val.push_back(BpValue::String(right.str_val));
    } else if (left.type == BpValue::NONE) {
      // Unresolved variable; adopt right.
      left = right;
    } else if (right.type == BpValue::NONE) {
      // Unresolved variable on right; keep left.
      // (No-op.)
    } else {
      return Error("cannot concatenate types " +
                   std::to_string(left.type) + " and " +
                   std::to_string(right.type) + " at line " +
                   std::to_string(Current().line));
    }
  }

  *value = std::move(left);
  return true;
}

bool BpParser::ParsePrimary(BpValue* value) {
  const Token& tok = Current();

  switch (tok.type) {
    case TOK_STRING:
      return ParseStringLiteral(value);

    case TOK_INT: {
      value->type = BpValue::INT;
      // Use strtoll for safety on large numbers.
      errno = 0;
      char* end = nullptr;
      long long ll = std::strtoll(tok.value.c_str(), &end, 10);
      if (errno != 0 || end == tok.value.c_str()) {
        return Error("invalid integer '" + tok.value + "' at line " +
                     std::to_string(tok.line));
      }
      value->int_val = static_cast<int64_t>(ll);
      Advance();
      return true;
    }

    case TOK_LBRACKET:
      return ParseList(value);

    case TOK_LBRACE:
      return ParseMap(value);

    case TOK_IDENT: {
      // Could be: true, false, a variable reference, or a function call.
      if (tok.value == "true") {
        value->type = BpValue::BOOL;
        value->bool_val = true;
        Advance();
        return true;
      }
      if (tok.value == "false") {
        value->type = BpValue::BOOL;
        value->bool_val = false;
        Advance();
        return true;
      }
      // Check if this is a function call: ident(...)
      if (pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].type == TOK_LPAREN) {
        return ParseFunctionCall(value);
      }
      // Variable reference.
      BpValue resolved = ResolveVariable(tok.value);
      if (resolved.type == BpValue::NONE) {
        // Unknown variable — treat as empty string to be lenient
        *value = BpValue::String("");
        Advance();
        return true;
      }
      *value = resolved;
      Advance();
      return true;
    }

    default:
      return Error("expected a value but got '" + tok.value + "' at line " +
                   std::to_string(tok.line));
  }
}

bool BpParser::SkipParenthesized() {
  // Skip everything until matching TOK_RPAREN, handling nesting
  int depth = 1;
  while (!AtEnd() && depth > 0) {
    if (Current().type == TOK_LPAREN) depth++;
    else if (Current().type == TOK_RPAREN) depth--;
    if (depth > 0) Advance();
  }
  if (depth != 0) {
    return Error("unmatched '(' in function call");
  }
  // Consume the closing ')'
  Advance();
  return true;
}

bool BpParser::ParseFunctionCall(BpValue* value) {
  // ident(args) — handle known functions
  std::string func_name = Current().value;
  Advance();  // consume identifier

  // Expect '('
  if (!Expect(TOK_LPAREN, "'('")) {
    return false;
  }

  // For select() calls, try to extract a default value.
  // select({key: value, ...}) — we pick "default" key or first value.
  if (func_name == "select") {
    BpValue select_arg;
    if (ParsePrimary(&select_arg)) {
      if (select_arg.type == BpValue::MAP) {
        auto it = select_arg.map_val.find("default");
        if (it != select_arg.map_val.end()) {
          *value = it->second;
          return true;
        }
        if (!select_arg.map_val.empty()) {
          *value = select_arg.map_val.begin()->second;
          return true;
        }
      } else if (select_arg.type == BpValue::LIST ||
                 select_arg.type == BpValue::STRING) {
        *value = select_arg;
        return true;
      }
    }
    value->type = BpValue::NONE;
    return true;
  }

  // For other function calls (release_flag(), etc.), skip arguments
  if (!SkipParenthesized()) {
    return false;
  }

  value->type = BpValue::NONE;
  return true;
}

bool BpParser::ParseStringLiteral(BpValue* value) {
  const Token& tok = Current();
  value->type = BpValue::STRING;
  value->str_val = tok.value;
  Advance();
  return true;
}

bool BpParser::ParseList(BpValue* value) {
  value->type = BpValue::LIST;
  value->list_val.clear();

  if (!Expect(TOK_LBRACKET, "'['")) {
    return false;
  }

  while (!AtEnd() && Current().type != TOK_RBRACKET) {
    BpValue elem;
    if (!ParseValue(&elem)) {
      return false;
    }

    // Glob expansion: if the element is a string containing wildcard
    // characters, expand it.
    if (elem.type == BpValue::STRING &&
        (elem.str_val.find('*') != std::string::npos ||
         elem.str_val.find('?') != std::string::npos)) {
      std::vector<std::string> matched = ExpandGlob(elem.str_val);
      for (const auto& m : matched) {
        value->list_val.push_back(BpValue::String(m));
      }
    } else {
      value->list_val.push_back(std::move(elem));
    }

    // Expect comma or ']'.
    if (Current().type == TOK_COMMA) {
      Advance();
    } else if (Current().type != TOK_RBRACKET) {
      return Error("expected ',' or ']' but got '" + Current().value +
                   "' at line " + std::to_string(Current().line));
    }
  }

  if (!Expect(TOK_RBRACKET, "']'")) {
    return false;
  }

  return true;
}

bool BpParser::ParseMap(BpValue* value) {
  value->type = BpValue::MAP;
  value->map_val.clear();

  if (!Expect(TOK_LBRACE, "'{'")) {
    return false;
  }

  while (!AtEnd() && Current().type != TOK_RBRACE) {
    if (Current().type != TOK_IDENT) {
      return Error("expected map key but got '" + Current().value +
                   "' at line " + std::to_string(Current().line));
    }
    std::string key = Current().value;
    Advance();

    if (!Expect(TOK_COLON, "':'")) {
      return false;
    }

    BpValue val;
    if (!ParseValue(&val)) {
      return false;
    }

    value->map_val[key] = std::move(val);

    // Trailing comma is allowed.
    if (Current().type == TOK_COMMA) {
      Advance();
    } else if (Current().type != TOK_RBRACE) {
      return Error("expected ',' or '}' but got '" + Current().value +
                   "' at line " + std::to_string(Current().line));
    }
  }

  if (!Expect(TOK_RBRACE, "'}'")) {
    return false;
  }

  return true;
}

// =====================================================================
// Variable resolution
// =====================================================================

BpValue BpParser::ResolveVariable(const std::string& name) const {
  if (variables_ == nullptr) {
    return BpValue();
  }
  auto it = variables_->find(name);
  if (it == variables_->end()) {
    return BpValue();
  }
  return it->second;
}

// =====================================================================
// Glob expansion
// =====================================================================

// static
bool BpParser::GlobMatch(const std::string& text, const std::string& glob) {
  // Standard recursive glob matcher supporting * (any sequence, including
  // empty) and ? (any single character).
  size_t t = 0;  // text index
  size_t g = 0;  // glob index
  size_t star_t = std::string::npos;  // text position when * was seen
  size_t star_g = std::string::npos;  // glob position of *

  while (t < text.size()) {
    if (g < glob.size() && (glob[g] == '?' || glob[g] == text[t])) {
      ++t;
      ++g;
    } else if (g < glob.size() && glob[g] == '*') {
      star_g = g;
      star_t = t;
      ++g;
    } else if (star_g != std::string::npos) {
      // Backtrack: let * consume one more character.
      g = star_g + 1;
      t = star_t + 1;
      star_t = t;
    } else {
      return false;
    }
  }
  // Consume trailing * in the glob.
  while (g < glob.size() && glob[g] == '*') {
    ++g;
  }
  return g == glob.size();
}

void BpParser::GlobWalk(const std::string& dir, const std::string& pattern,
                          std::vector<std::string>* out) {
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) {
    return;
  }

  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") {
      continue;
    }

    std::string full = dir + "/" + name;

    struct stat st;
    if (stat(full.c_str(), &st) != 0) {
      continue;
    }

    // If pattern has a '/', check if this directory component matches the
    // first segment.
    size_t slash = pattern.find('/');
    if (slash != std::string::npos) {
      std::string head = pattern.substr(0, slash);
      std::string rest = pattern.substr(slash + 1);

      if (S_ISDIR(st.st_mode) && GlobMatch(name, head)) {
        // Check for ** (recursive glob).
        if (head == "**") {
          // ** matches zero or more directories.
          // 1. Try matching rest in the current directory.
          GlobWalk(full, rest, out);
          // 2. Also recurse deeper with ** still active.
          GlobWalk(full, pattern, out);
        } else {
          // Normal directory match: recurse with the remainder.
          if (!rest.empty()) {
            GlobWalk(full, rest, out);
          }
        }
      } else if (head == "**" && !rest.empty()) {
        // ** with more pattern: try matching rest here.
        GlobWalk(full, rest, out);
        // And recurse deeper.
        GlobWalk(full, pattern, out);
      }
    } else {
      // No slash: match file name directly.
      if (!S_ISDIR(st.st_mode) && GlobMatch(name, pattern)) {
        out->push_back(full);
      }
    }
  }

  closedir(d);
}

std::vector<std::string> BpParser::ExpandGlob(const std::string& pattern) {
  std::vector<std::string> result;

  std::string full_pattern = pattern;
  // Make relative to base_dir_.
  std::string dir = base_dir_;

  GlobWalk(dir, full_pattern, &result);

  // Sort for deterministic output.
  std::sort(result.begin(), result.end());

  // Strip the base_dir_ prefix so results are relative, matching how
  // Blueprint / Soong represents file paths.
  if (!dir.empty()) {
    std::string prefix = dir + "/";
    for (auto& p : result) {
      if (p.size() > prefix.size() &&
          p.compare(0, prefix.size(), prefix) == 0) {
        p = p.substr(prefix.size());
      }
    }
  }

  return result;
}

}  // namespace gormake
