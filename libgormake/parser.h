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

#ifndef GORMAKE_LIBGORMAKE_PARSER_H_
#define GORMAKE_LIBGORMAKE_PARSER_H_

#include "lexer.h"
#include "ast.h"

namespace gormake {

class Parser {
 public:
  enum PsrState {
    PSR_INVALID = 0,
    PSR_READY,
    PSR_WARNING,
    PSR_ERROR,
    PSR_DONE,
  };

 public:
  explicit Parser(Lexer* lxr);
  virtual ~Parser();

  PsrState ParseToAst();
  bool IsReady() const;
  const std::string& GetFilePath() const;
  const std::string* GetLineString() const;
  int64_t GetLineNumber() const;
  int64_t GetTokenLineOffset() const;
  int64_t GetTokenStrLen() const;

  const std::vector<ExprAst*>& GetExprAsts() const;
  const std::vector<ReceiptAst*>& GetReceiptAst() const;

 private:
  ReceiptAst* ConstructReceipt();
  ExprAst* ConstructVariableAssignment();

  std::unique_ptr<Lexer> lxr_;
  std::vector<ExprAst*> exprAsts_;
  std::vector<ReceiptAst*> receiptAsts_;
  PsrState state_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_PARSER_H_

