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

#ifndef GORMAKE_LIBGORMAKE_LEXER_H_
#define GORMAKE_LIBGORMAKE_LEXER_H_

#include <memory>

#include <string>
#include "token.h"
#include "line.h"
#include "RdFile.h"
#include "macros.h"

namespace gormake {

class Lexer {
 public:
  explicit Lexer(const char* name);
  explicit Lexer(std::string& name);
  virtual ~Lexer();

  bool IsOpen() const;
  const std::string& GetFilePath() const;
  bool IsBOL() const;
  bool IsEOL() const;
  bool IsBOF() const;
  bool IsEOF() const;
  bool NextLine();
  bool PrevLine();

  Token::TokTyp GetNextToken();
  const std::string* GetTokenString();
  const std::string* GetTokenVarString();
  const std::string* GetLineString();

  Token::TokTyp GetTokenType() const;
  int64_t GetTokenLineNo() const;
  int64_t GetTokenLineOffset() const;
  int64_t GetTokenStrPos() const;
  int64_t GetTokenStrLen() const;

  Line::LnTyp GetLineType() const;
  int64_t GetLineNo() const;
  int64_t GetLineStrPos() const;
  int64_t GetLineStrLen() const;
  Line::LnTyp DetermineLineType();

  typedef struct {
    const int8_t        c;
    const Token::TokTyp tok;
  } KeyCharToTok;

  typedef struct {
    const int8_t        *str;
    const size_t        strLen;
    const Token::TokTyp tok;
  } KeyPhraseToTok;

  static const int64_t BYTE_1_STR = 1;
  static const int64_t BYTE_2_STR = 2;
  static const int64_t SZ_BUF = 256;
  static const int64_t LN_BUF = 1024;

 private:
  Token::TokTyp MatchCharToken(const char* c) const;
  Token::TokTyp MatchPhraseToken();

  std::unique_ptr<UnixFile::RdFile> file_;
  Token tok_;
  Line ln_;
  char strBuf_[SZ_BUF];
  char lnBuf_[LN_BUF];

  DISALLOW_COPY_AND_ASSIGN(Lexer);
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_LEXER_H_
