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

#include "parser.h"

namespace gormake {

Parser::Parser(Lexer* lxr)
  : state_(PSR_INVALID) {
  lxr_.reset(lxr);
  if (lxr != nullptr) {
    if (lxr_->IsOpen() == true) {
      state_ = PSR_READY;
    }
  }
}

Parser::~Parser() {
}

Parser::PsrState Parser::ParseToAst() {
  Line::LnTyp lnTyp;
  ReceiptAst* rAst;
  ExprAst* eAst;

  while (lxr_->IsEOF() == false) {
    // Determine the line type
    lnTyp = lxr_->DetermineLineType();

    // Go to next line if it's empty or a comment
    if ((lnTyp == Line::LT_EMPTY) || (lnTyp == Line::LT_COMMENT)) {
      lxr_->NextLine();
      continue;
    }

    // Dispatch to line handlers
    switch (lnTyp) {
      // Construct a Receipt AST
      case Line::LT_TARGET:
        rAst = ConstructReceipt();
        if (rAst == nullptr) {
          break;
        }
        receiptAsts_.push_back(rAst);
        continue;
      // Construct a Variable Assignment AST
      case Line::LT_VARIABLE:
        eAst = ConstructVariableAssignment();
        if (eAst == nullptr) {
          return Parser::PSR_WARNING;
        }
        exprAsts_.push_back(eAst);
        continue;

      case Line::LT_AMB_TABVAR:
      case Line::LT_DIRECTIVE:
        lxr_->NextLine();
        continue;

      case Line::LT_RULE:
        break;

      default:
        break;
    }
    // Syntax error, return false
    return Parser::PSR_ERROR;
  }  // while (lxr_->IsEOF() == false)

  // Finished well, return true
  return Parser::PSR_DONE;
}

bool Parser::IsReady() const {
  if (state_ == Parser::PSR_READY) {
    return true;
  }
  return false;
}

const std::string& Parser::GetFilePath() const {
  return lxr_->GetFilePath();
}

const std::string* Parser::GetLineString() const {
  return lxr_->GetLineString();
}

int64_t Parser::GetLineNumber() const {
  return lxr_->GetTokenLineNo();
}

int64_t Parser::GetTokenLineOffset() const {
  return lxr_->GetTokenLineOffset();
}

int64_t Parser::GetTokenStrLen() const {
  return lxr_->GetTokenStrLen();
}

const std::vector<ExprAst*>& Parser::GetExprAsts() const {
  return exprAsts_;
}

const std::vector<ReceiptAst*>& Parser::GetReceiptAst() const {
  return receiptAsts_;
}

ReceiptAst* Parser::ConstructReceipt() {
  Token::TokTyp tokTyp;
  Line::LnTyp lnTyp = lxr_->DetermineLineType();
  std::vector<std::unique_ptr<ExprAst>*>* names = new std::vector<std::unique_ptr<ExprAst>*>();
  std::vector<std::unique_ptr<ExprAst>*>* prereqs = new std::vector<std::unique_ptr<ExprAst>*>();
  std::vector<std::unique_ptr<std::vector< std::unique_ptr<ExprAst>*>>*>* rules
    = new std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>();
  std::vector<std::unique_ptr<ExprAst>*>* subRule = new std::vector<std::unique_ptr<ExprAst>*>();
  bool crossColon = false;
  bool crossLeadingTabs = false;

  while (true) {
    // Retrieve a token
    tokTyp = lxr_->GetNextToken();

    // If it's a NEWLINE, then reset everything
    if ((tokTyp == Token::TOK_NEWLINE)
        || (tokTyp == Token::TOK_EOF)) {
      // If it's a RULE type, must push back first (next rule)
      if (lnTyp == Line::LT_RULE) {
        rules->push_back(new std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>(subRule));
        // EOF handle
        if (tokTyp == Token::TOK_EOF) {
          break;
        }
        // Allocate a new storage
        subRule = new std::vector<std::unique_ptr<ExprAst>*>();
      }

      // EOF Handle
      if (tokTyp == Token::TOK_EOF) {
        break;
      }

      // Update the line type
      lnTyp = lxr_->DetermineLineType();

      // If the new line is a target, then we should stop here
      if (lnTyp == Line::LT_TARGET) {
        break;
      }

      // Reset the state
      crossLeadingTabs = false;
      continue;
    }

    // ':' is the separator of the target line
    if ((tokTyp == Token::TOK_COLON)
        && (lnTyp == Line::LT_TARGET)) {
      crossColon = true;
      continue;
    }

    // In ConstructReceipt(), we handle only TARGET, RULE,
    // COMMENT, and EMPTY lines
    switch (lnTyp) {
      case Line::LT_TARGET:
        if (crossColon == false) {
          // Target names
          if (tokTyp == Token::TOK_ID) {
            names->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
            continue;
          }
          if (tokTyp == Token::TOK_VAR) {
            names->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
            continue;
          }
        } else {  // crossColon == true
          // Target prerequsites
          if (tokTyp == Token::TOK_ID) {
            prereqs->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
            continue;
          }
          if (tokTyp == Token::TOK_VAR) {
            prereqs->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
            continue;
          }
        }
        // Ignore TAB, SPACE, and other tokens
        continue;

      case Line::LT_RULE:
        // Ignore leading tabs & spaces
        if (crossLeadingTabs == false) {
          if ((tokTyp != Token::TOK_TAB)
              && (tokTyp != Token::TOK_SPACE)) {
            crossLeadingTabs = true;
          } else {
            // Ignore this tab or space char.
            continue;
          }
        }
        // crossFirstTab == true
        if (tokTyp == Token::TOK_VAR) {
          subRule->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
        } else {
          subRule->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
        }
        continue;

      case Line::LT_COMMENT:
      case Line::LT_EMPTY:
        // Go to next line and update the line type
        lxr_->NextLine();
        lnTyp = lxr_->DetermineLineType();
        continue;

      default:
        break;
    }
  }

  // Construct an AST
  return new ComplexReceiptAst(names, prereqs, rules);
}

ExprAst* Parser::ConstructVariableAssignment() {
  Token::TokTyp tokTyp;
  VariableAssignExprAst::VarAttr attr = VariableAssignExprAst::VAR_NORMAL;
  std::string *name = nullptr;
  std::vector<std::unique_ptr<ExprAst>*>* values = new std::vector<std::unique_ptr<ExprAst>*>();
  bool crossEqual = false;
  bool crossLeadingTabs = false;

  while (lxr_->IsEOF() == false) {
    // Retrieve a token
    tokTyp = lxr_->GetNextToken();

    // If it's a NEWLINE, then it's done
    if (tokTyp == Token::TOK_NEWLINE) {
      break;
    }

    // '=', ':=',, '+=' or '?=' token is the separator
    if ((tokTyp == Token::TOK_EQUAL)
        || (tokTyp == Token::TOK_QMEQ)
        || (tokTyp == Token::TOK_PLUSEQ)
        || (tokTyp == Token::TOK_COLEQ)) {
      crossEqual = true;
      // Override the variable attribute, if it's needed
      switch (tokTyp) {
        case Token::TOK_QMEQ:
          attr = VariableAssignExprAst::VAR_QMARK;
          break;
        case Token::TOK_COLEQ:
          attr = VariableAssignExprAst::VAR_COLON;
          break;
        case Token::TOK_PLUSEQ:
          attr = VariableAssignExprAst::VAR_PLUS;
          break;
        default:
          break;
      }
      continue;
    }

    // Assignment portion, behind the equal sign
    if (crossEqual == true) {
      // Ignore leading tabs & spaces
      if (crossLeadingTabs == false) {
        if ((tokTyp != Token::TOK_TAB)
            && (tokTyp != Token::TOK_SPACE)) {
          crossLeadingTabs = true;
        } else {
          // Ignore this tab or space char.
          continue;
        }
      }
      // Absorb all ID, TAB, and SPACE tokens
      if ((tokTyp == Token::TOK_ID)
          || (tokTyp == Token::TOK_TAB)
          || (tokTyp == Token::TOK_SPACE)) {
          values->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
          continue;
      }
      if (tokTyp == Token::TOK_VAR) {
          values->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
          continue;
      }
      // It it's followed by a comment
      if (tokTyp == Token::TOK_COMMENT) {
        lxr_->NextLine();
        break;
      }
      // Syntax error (Internal parser error)
      return nullptr;
    }

    // Variable name portion, before the equal sign
    if (tokTyp == Token::TOK_ID) {
      name = new std::string(*lxr_->GetTokenString());
      continue;
    }

    // Ignore SPACE and TAB
    if ((tokTyp == Token::TOK_SPACE)
        || (tokTyp == Token::TOK_TAB)) {
      continue;
    }

    // Syntax error if it's not a TOK_ID
    lxr_->NextLine();
    return nullptr;
  }

  // Construct an AST
  return new VariableAssignExprAst(name, attr, values);
}

}  // namespace gormake
