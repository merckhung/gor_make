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
  Line::LnTyp ln_typ;
  ReceiptAst* r_ast;
  ExprAst* e_ast;

  while (lxr_->IsEOF() == false) {
    // Determine the line type
    ln_typ = lxr_->DetermineLineType();

    // Go to next line if it's empty or a comment
    if ((ln_typ == Line::LT_EMPTY) || (ln_typ == Line::LT_COMMENT)) {
      lxr_->NextLine();
      continue;
    }

    // Dispatch to line handlers
    switch (ln_typ) {
      // Construct a Receipt AST
      case Line::LT_TARGET:
        r_ast = ConstructReceipt();
        if (r_ast == nullptr) {
          break;
        }
        receipt_asts_.push_back(r_ast);
        continue;
      // Construct a Variable Assignment AST
      case Line::LT_VARIABLE:
        e_ast = ConstructVariableAssignment();
        if (e_ast == nullptr) {
          return Parser::PSR_WARNING;
        }
        expr_asts_.push_back(e_ast);
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
  return expr_asts_;
}

const std::vector<ReceiptAst*>& Parser::GetReceiptAst() const {
  return receipt_asts_;
}

ReceiptAst* Parser::ConstructReceipt() {
  Token::TokTyp tok_typ;
  Line::LnTyp ln_typ = lxr_->DetermineLineType();
  std::vector<std::unique_ptr<ExprAst>*>* names = new std::vector<std::unique_ptr<ExprAst>*>();
  std::vector<std::unique_ptr<ExprAst>*>* prereqs = new std::vector<std::unique_ptr<ExprAst>*>();
  std::vector<std::unique_ptr<std::vector< std::unique_ptr<ExprAst>*>>*>* rules
    = new std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>();
  std::vector<std::unique_ptr<ExprAst>*>* sub_rule = new std::vector<std::unique_ptr<ExprAst>*>();
  bool cross_colon = false;
  bool cross_leading_tabs = false;

  while (true) {
    // Retrieve a token
    tok_typ = lxr_->GetNextToken();

    // If it's a NEWLINE, then reset everything
    if ((tok_typ == Token::TOK_NEWLINE)
        || (tok_typ == Token::TOK_EOF)) {
      // If it's a RULE type, must push back first (next rule)
      if (ln_typ == Line::LT_RULE) {
        rules->push_back(new std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>(sub_rule));
        // EOF handle
        if (tok_typ == Token::TOK_EOF) {
          break;
        }
        // Allocate a new storage
        sub_rule = new std::vector<std::unique_ptr<ExprAst>*>();
      }

      // EOF Handle
      if (tok_typ == Token::TOK_EOF) {
        break;
      }

      // Update the line type
      ln_typ = lxr_->DetermineLineType();

      // If the new line is a target, then we should stop here
      if (ln_typ == Line::LT_TARGET) {
        break;
      }

      // Reset the state
      cross_leading_tabs = false;
      continue;
    }

    // ':' is the separator of the target line
    if ((tok_typ == Token::TOK_COLON)
        && (ln_typ == Line::LT_TARGET)) {
      cross_colon = true;
      continue;
    }

    // In ConstructReceipt(), we handle only TARGET, RULE,
    // COMMENT, and EMPTY lines
    switch (ln_typ) {
      case Line::LT_TARGET:
        if (cross_colon == false) {
          // Target names
          if (tok_typ == Token::TOK_ID) {
            names->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
            continue;
          }
          if (tok_typ == Token::TOK_VAR) {
            names->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
            continue;
          }
        } else {  // cross_colon == true
          // Target prerequsites
          if (tok_typ == Token::TOK_ID) {
            prereqs->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
            continue;
          }
          if (tok_typ == Token::TOK_VAR) {
            prereqs->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
            continue;
          }
        }
        // Ignore TAB, SPACE, and other tokens
        continue;

      case Line::LT_RULE:
        // Ignore leading tabs & spaces
        if (cross_leading_tabs == false) {
          if ((tok_typ != Token::TOK_TAB)
              && (tok_typ != Token::TOK_SPACE)) {
            cross_leading_tabs = true;
          } else {
            // Ignore this tab or space char.
            continue;
          }
        }
        // cross_first_tab == true
        if (tok_typ == Token::TOK_VAR) {
          sub_rule->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
        } else {
          sub_rule->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
        }
        continue;

      case Line::LT_COMMENT:
      case Line::LT_EMPTY:
        // Go to next line and update the line type
        lxr_->NextLine();
        ln_typ = lxr_->DetermineLineType();
        continue;

      default:
        break;
    }
  }

  // Construct an AST
  return new ComplexReceiptAst(names, prereqs, rules);
}

ExprAst* Parser::ConstructVariableAssignment() {
  Token::TokTyp tok_typ;
  VariableAssignExprAst::VarAttr attr = VariableAssignExprAst::VAR_NORMAL;
  std::string *name = nullptr;
  std::vector<std::unique_ptr<ExprAst>*>* values = new std::vector<std::unique_ptr<ExprAst>*>();
  bool cross_equal = false;
  bool cross_leading_tabs = false;

  while (lxr_->IsEOF() == false) {
    // Retrieve a token
    tok_typ = lxr_->GetNextToken();

    // If it's a NEWLINE, then it's done
    if (tok_typ == Token::TOK_NEWLINE) {
      break;
    }

    // '=', ':=',, '+=' or '?=' token is the separator
    if ((tok_typ == Token::TOK_EQUAL)
        || (tok_typ == Token::TOK_QMEQ)
        || (tok_typ == Token::TOK_PLUSEQ)
        || (tok_typ == Token::TOK_COLEQ)) {
      cross_equal = true;
      // Override the variable attribute, if it's needed
      switch (tok_typ) {
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
    if (cross_equal == true) {
      // Ignore leading tabs & spaces
      if (cross_leading_tabs == false) {
        if ((tok_typ != Token::TOK_TAB)
            && (tok_typ != Token::TOK_SPACE)) {
          cross_leading_tabs = true;
        } else {
          // Ignore this tab or space char.
          continue;
        }
      }
      // Absorb all ID, TAB, and SPACE tokens
      if ((tok_typ == Token::TOK_ID)
          || (tok_typ == Token::TOK_TAB)
          || (tok_typ == Token::TOK_SPACE)) {
          values->push_back(new std::unique_ptr<ExprAst>(new StringExprAst(lxr_->GetTokenString())));
          continue;
      }
      if (tok_typ == Token::TOK_VAR) {
          values->push_back(new std::unique_ptr<ExprAst>(new VariableExprAst(lxr_->GetTokenVarString())));
          continue;
      }
      // It it's followed by a comment
      if (tok_typ == Token::TOK_COMMENT) {
        lxr_->NextLine();
        break;
      }
      // Syntax error (Internal parser error)
      return nullptr;
    }

    // Variable name portion, before the equal sign
    if (tok_typ == Token::TOK_ID) {
      name = new std::string(*lxr_->GetTokenString());
      continue;
    }

    // Ignore SPACE and TAB
    if ((tok_typ == Token::TOK_SPACE)
        || (tok_typ == Token::TOK_TAB)) {
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
