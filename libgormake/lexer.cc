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

#ifdef DEBUG_GORMAKE
#include <iostream>
#endif
#include "line.h"
#include "token.h"
#include "lexer.h"
#include "OS.h"
#include "table.h"

namespace gormake {

Lexer::Lexer(const char* name) {
  file_.reset(nullptr);
  if (OS::FileExists(name) == true) {
    file_.reset(OS::OpenFileReadOnly(name));
  }
}

Lexer::Lexer(std::string& name) {
  file_.reset(nullptr);
  if (OS::FileExists(name.c_str()) == true) {
    file_.reset(OS::OpenFileReadOnly(name.c_str()));
  }
}

Lexer::~Lexer() {
  if (file_.get() != nullptr) {
    file_->Close();
  }
}

bool Lexer::IsOpen() const {
  if (file_.get() == nullptr) {
    return false;
  }
  return file_->IsOpen();
}

const std::string& Lexer::GetFilePath() const {
  return file_->GetFilePath();
}

bool Lexer::IsBOL() const {
  return file_->IsBOL();
}

bool Lexer::IsEOL() const {
  return file_->IsEOL();
}

bool Lexer::IsBOF() const {
  return file_->IsBOF();
}

bool Lexer::IsEOF() const {
  return file_->IsEOF();
}

bool Lexer::NextLine() {
  if (file_->GetLineNumber() == file_->NextLine()) {
    return false;
  }
  return true;
}

bool Lexer::PrevLine() {
  if (file_->GetLineNumber() == file_->PrevLine()) {
    return false;
  }
  return true;
}

Token::TokTyp Lexer::GetNextToken() {
  // EOF check
  if (file_->IsEOF() == true) {
    return Token::TOK_EOF;
  }

  // Update the line no. when it's at BOL
  if (file_->IsBOL() == true) {
    tok_.line_ = file_->GetLineNumber();
    tok_.offset_ = 0;
  }

  // Update the line offset and start position of the string
  tok_.offset_ = file_->GetLineOffset();
  tok_.strPos_ = file_->GetPosition();

  // Read a byte
  file_->ReadByte(&strBuf_[0]);

  // Set to default 1 byte length
  tok_.strLen_ = BYTE_1_STR;

  // Treat it as a single byte keyword
  tok_.tokenType_ = MatchCharToken(&strBuf_[0]);

  // If it's a valid single byte keyword
  if (tok_.tokenType_ != Token::TOK_INVALID) {
    // Handle for 2 bytes keyword
    switch (tok_.tokenType_) {
      // Comment, move to next line
      case Token::TOK_COMMENT:
        tok_.strLen_ = file_->GetLineLength();  // +1(#)-1(\n)
        file_->ReadAt(strBuf_, tok_.strLen_, tok_.strPos_);
        file_->AdvancePos(tok_.strLen_ - 1);  // Move to '\n'
        return tok_.tokenType_;

      // :=
      case Token::TOK_COLON:
        strBuf_[1] = file_->SnoopCurr();
        if (strBuf_[1] == '=') {
          tok_.tokenType_ = Token::TOK_COLEQ;
          tok_.strLen_ = BYTE_2_STR;
          file_->AdvancePos();
        }
        return tok_.tokenType_;

      // +=, or just a '+' string
      case Token::TOK_PLUS:
        strBuf_[1] = file_->SnoopCurr();
        if (strBuf_[1] == '=') {
          tok_.tokenType_ = Token::TOK_PLUSEQ;
          tok_.strLen_ = BYTE_2_STR;
          file_->AdvancePos();
        } else {
          tok_.tokenType_ = Token::TOK_ID;
        }
        return tok_.tokenType_;

      // ?=
      case Token::TOK_QMARK:
        strBuf_[1] = file_->SnoopCurr();
        if (strBuf_[1] == '=') {
          tok_.tokenType_ = Token::TOK_QMEQ;
          tok_.strLen_ = BYTE_2_STR;
          file_->AdvancePos();
        }
        return tok_.tokenType_;

      // $(...), or $..., variable, otherwise it a '$' string
      case Token::TOK_DOLLAR:
        strBuf_[1] = file_->SnoopCurr();
        if (strBuf_[1] != '(') {
          // Locate next SPACE, TAB, or NL
          tok_.strLen_ = file_->LocateEitherByte(' ', '\t', '\n') + 1;
          // Just a '$' string, not a variable
          if (tok_.strLen_ == 0) {
            tok_.tokenType_ = Token::TOK_ID;
            return tok_.tokenType_;
          }
        } else {
          // Locate the ')' character
          tok_.strLen_ = file_->LocateByte(')') + 2;  // '$' and ')'
          if ((tok_.strLen_ - 1) >= file_->GetLineLength()) {
#ifdef DEBUG_GORMAKE
            std::cerr << "Invalid format\n";
#endif
            return Token::TOK_INVALID;
          }
        }
        // Override the token data and return
        file_->ReadAt(strBuf_, tok_.strLen_, tok_.strPos_);
        file_->AdvancePos(tok_.strLen_ - 1);  // Move to next token
        tok_.tokenType_ = Token::TOK_VAR;
        return tok_.tokenType_;

      // Default
      default:
        return tok_.tokenType_;
    }  // switch (tok_.tokenType_)
  }  // if (tok_.tokenType_ != Token::TOK_INVALID)

  // Handle keyword phrase
  tok_.tokenType_ = MatchPhraseToken();
  if (tok_.tokenType_ != Token::TOK_INVALID) {
    return tok_.tokenType_;
  }

  // Handle varying length ID token
  tok_.tokenType_ = Token::TOK_ID;
  file_->AdvancePos(tok_.strLen_ - 1);  // Deducted the first byte
  return tok_.tokenType_;
}

const std::string* Lexer::GetTokenString() {
  return new std::string(strBuf_, tok_.strLen_);
}

const std::string* Lexer::GetTokenVarString() {
  if ((strBuf_[0] == '$')
      && (strBuf_[1] == '(')) {
    return new std::string(&strBuf_[2], (tok_.strLen_ - 3));
  }
  return new std::string(&strBuf_[1], tok_.strLen_ - 1);
}

const std::string* Lexer::GetLineString() {
  int64_t loff = file_->GetLineOffset();
  int64_t off = file_->GetPosition() - loff;
  int64_t len = loff + file_->GetLineLength() - 1;

  if (len >= LN_BUF) {
    return &EMPTY_STR;
  }
  if (file_->ReadAt(lnBuf_, len, off) != len) {
    return &EMPTY_STR;
  }
  lnBuf_[len] = '\0';
  return new std::string(lnBuf_);
}

Token::TokTyp Lexer::GetTokenType() const {
  return tok_.tokenType_;
}

int64_t Lexer::GetTokenLineNo() const {
  return tok_.line_;
}

int64_t Lexer::GetTokenLineOffset() const {
  return tok_.offset_;
}

int64_t Lexer::GetTokenStrPos() const {
  return tok_.strPos_;
}

int64_t Lexer::GetTokenStrLen() const {
  return tok_.strLen_;
}

Line::LnTyp Lexer::GetLineType() const {
  return ln_.lineType_;
}

int64_t Lexer::GetLineNo() const {
  return ln_.line_;
}

int64_t Lexer::GetLineStrPos() const {
  return ln_.strPos_;
}

int64_t Lexer::GetLineStrLen() const {
  return ln_.strLen_;
}

Line::LnTyp Lexer::DetermineLineType() {
  int64_t loff = file_->GetLineOffset();
  char* p = reinterpret_cast<char*>(file_->GetFileMem());

  // Initial state
  bool onlyTabAndNl = true;
  bool equalSign = false;
  bool firstTab = false;
  ln_.line_ = file_->GetLineNumber();
  ln_.lineType_ = Line::LT_EMPTY;
  ln_.strPos_ = file_->GetPosition() - loff;
  ln_.strLen_ = loff + file_->GetLineLength() - 1;

  // Iterate each character to determine the line type
  for (int64_t i = 0; i < ln_.strLen_; ++i) {
    // Get a character
    char c = *(p + ln_.strPos_ + i);

    // If it's the 1st char
    if (i == 0) {
      switch (c) {
        case '#':
          ln_.lineType_ = Line::LT_COMMENT;
          return ln_.lineType_;
        case '\t':
          firstTab = true;
          ln_.lineType_ = Line::LT_RULE;
          break;
      }
    }

    // Detect for an empty line
    if ((c != ' ') && (c != '\t') && (c != '#')) {
      onlyTabAndNl = false;
    }

    // Detect for '=' sign
    if (c == '=') {
      equalSign = true;
      if (firstTab == true) {
        // Has to check context for a further determination
        ln_.lineType_ = Line::LT_AMB_TABVAR;
      } else {
        ln_.lineType_ = Line::LT_VARIABLE;
      }
      return ln_.lineType_;
    }

    // Detect for '#' sign
    if (c == '#') {
      if (onlyTabAndNl == true) {
        ln_.lineType_ = Line::LT_COMMENT;
        return ln_.lineType_;
      }
      return ln_.lineType_;
    }

    // Detect for ':' sign
    if (c == ':') {
      if ((firstTab == false) && (*(p + ln_.strPos_ + i + 1) != '=')) {
        ln_.lineType_ = Line::LT_TARGET;
        return ln_.lineType_;
      }
    }
  }  // for (int64_t i = 0; i < ln_.strLen_; ++i)

  // Empty line
  if (onlyTabAndNl == true) {
    ln_.lineType_ = Line::LT_EMPTY;
    return ln_.lineType_;
  }

  // Return the line type
  return ln_.lineType_;
}

Token::TokTyp Lexer::MatchCharToken(const char* c) const {
  for (int32_t i = 0; i < NrKeyCharToTokTbl; ++i) {
    if (KeyCharToTokTbl[i].c == *c) {
      return KeyCharToTokTbl[i].tok;
    }
  }
  return Token::TOK_INVALID;
}

Token::TokTyp Lexer::MatchPhraseToken() {
  int64_t base = file_->GetPosition() - BYTE_1_STR;
  size_t len = BYTE_1_STR;
  int64_t restLen = file_->GetLength() - base + BYTE_1_STR;
  int64_t i;

  // Read into buffer and detect the delimiter
  // Determine the string length first
  for (i = BYTE_1_STR; i < restLen; ++i, ++len) {
    // Overflow guard
    if (i >= SZ_BUF) {
#ifdef DEBUG_GORMAKE
      std::cerr << "String buffer overflowed\n";
#endif
      return Token::TOK_INVALID;
    }

    // Snoop a byte
    strBuf_[i] = file_->SnoopAt(base + i);
    // Check if it's a keyword
    if (MatchCharToken(&strBuf_[i]) != Token::TOK_INVALID) {
      // If not a keyword, continue
      break;
    }
  }
  strBuf_[i] = '\0';  // Terminate the string
  tok_.strLen_ = len;  // Update the string length

  // Match the keyword phrase table
  for (i = 0; i < NrKeyPhraseToTokTbl; ++i) {
    // Iterate each element, make sure the lengths are identical first
    if (KeyPhraseToTokTbl[i].strLen == len) {
      // Compare the content
      if (!memcmp(KeyPhraseToTokTbl[i].str, strBuf_, len)) {
        // Found a match, return it to the caller
        return KeyPhraseToTokTbl[i].tok;
      }
    }
  }

  // Don't match anything
  return Token::TOK_INVALID;
}

}  // namespace gormake
