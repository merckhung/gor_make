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

#ifndef GORMAKE_LIBGORMAKE_BPPARSER_H_
#define GORMAKE_LIBGORMAKE_BPPARSER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gormake {

// A value in the Blueprint type system.  Every literal, variable reference, or
// evaluated expression in an Android.bp file reduces to one of these.
struct BpValue {
  enum Type { STRING, INT, BOOL, LIST, MAP, NONE };

  Type type = NONE;
  std::string strVal;
  int64_t intVal = 0;
  bool boolVal = false;
  std::vector<BpValue> listVal;
  std::map<std::string, BpValue> mapVal;

  // Helpers
  bool IsString() const { return type == STRING; }
  bool IsList() const { return type == LIST; }
  bool IsMap() const { return type == MAP; }

  // Returns the string representation for STRING-typed values, or the empty
  // string otherwise.
  std::string AsString() const;

  // Returns a vector of strings for LIST-typed values whose elements are all
  // strings.  Non-string elements are skipped.  Returns an empty vector for
  // non-list values.
  std::vector<std::string> AsStringList() const;

  // Factory helpers
  static BpValue String(const std::string& s);
  static BpValue List(const std::vector<std::string>& v);
};

// A single module definition, e.g. cc_binary { name: "foo", ... }.
struct BpModule {
  std::string type;  // "cc_binary", "cc_library", etc.
  std::string name;
  std::map<std::string, BpValue> properties;
};

// The result of parsing a single Android.bp file.
struct BpFile {
  std::vector<BpModule> modules;
  std::map<std::string, BpValue> variables;
};

// A recursive-descent parser for Android.bp (Blueprint) files.
//
// Usage:
//   BpParser parser;
//   BpFile result;
//   if (parser.ParseFile("/path/to/Android.bp", &result)) {
//     // use result.modules
//   } else {
//     LOG(ERROR) << parser.GetError();
//   }
class BpParser {
 public:
  BpParser();
  ~BpParser();

  // Parse a file from disk.  Returns true on success and fills |result|.
  // On failure returns false and GetError() returns a description.
  bool ParseFile(const std::string& path, BpFile* result);

  // Parse a source string directly.  Useful for testing.  The |path| is
  // only used as a base directory for glob expansion.
  bool ParseSource(const std::string& source, const std::string& path,
                    BpFile* result);

  // Get the most recent error message (includes line numbers).
  const std::string& GetError() const;

  // Expand a glob pattern relative to base_dir_.  Supports:
  //   *.c           - all .c files in the base directory
  //   src/**/*.c    - all .c files under src/ recursively
  std::vector<std::string> ExpandGlob(const std::string& pattern);

 private:
  // ---- Tokenizer ----
  enum TokenType {
    TOK_EOF,
    TOK_IDENT,     // foo, cc_binary, true
    TOK_STRING,    // "hello"
    TOK_INT,       // 42
    TOK_LBRACE,    // {
    TOK_RBRACE,    // }
    TOK_LBRACKET,  // [
    TOK_RBRACKET,  // ]
    TOK_COLON,      // :
    TOK_COMMA,     // ,
    TOK_PLUS,      // +
    TOK_ASSIGN,    // =
  };

  struct Token {
    TokenType type;
    std::string value;
    int line = 0;
  };

  bool Tokenize(const std::string& source, std::vector<Token>* tokens);

  // ---- Parser (recursive descent) ----

  // Top-level: a sequence of variable assignments and module definitions.
  bool ParseTopLevel(BpFile* result);

  // Parse a module definition: <type> { <properties> }
  bool ParseModule(BpModule* module);

  // Parse a single property inside a module: <key>: <value>
  bool ParseProperty(std::map<std::string, BpValue>* props);

  // Parse a value, including + operator expressions.
  bool ParseValue(BpValue* value);

  // Parse a single primary value (literal, variable reference, list, map).
  bool ParsePrimary(BpValue* value);

  // Parse a string literal token.
  bool ParseStringLiteral(BpValue* value);

  // Parse a list literal: [v1, v2, ...]
  bool ParseList(BpValue* value);

  // Parse a map literal: { k1: v1, k2: v2, ... }
  bool ParseMap(BpValue* value);

  // ---- Token helpers ----
  bool Peek(TokenType type) const;
  bool Accept(TokenType type);
  bool Expect(TokenType type, const std::string& what);
  const Token& Current() const;
  void Advance();
  bool AtEnd() const;

  // ---- Variable resolution ----
  BpValue ResolveVariable(const std::string& name) const;

  // ---- Glob helpers ----
  // Walk |dir| recursively, collecting entries that match |pattern|.
  void GlobWalk(const std::string& dir, const std::string& pattern,
                std::vector<std::string>* out);

  // Match a single path component against a glob segment (e.g. "*.c" vs
  // "foo.c").  Supports * and ? wildcards.
  static bool GlobMatch(const std::string& text, const std::string& glob);

  // ---- Error handling ----
  [[nodiscard]] bool Error(const std::string& msg);
  [[nodiscard]] bool ErrorAt(const Token& tok, const std::string& msg);

  // ---- State ----
  std::string error_;
  size_t pos_ = 0;
  std::vector<Token> tokens_;
  std::map<std::string, BpValue>* variables_ = nullptr;
  std::string base_dir_;  // directory of the file being parsed (for globs)
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_BPPARSER_H_
