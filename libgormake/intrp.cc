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

#include <vector>
#include "ast.h"
#include "parser.h"
#include "intrp.h"

#include <iostream>

namespace gormake {

Intrp::Intrp()
  : expr_asts_(nullptr),
    receipt_asts_(nullptr) {
}

Intrp::~Intrp() {
}

bool Intrp::SetupAsts(const std::vector<ExprAst*>* expr_asts,
                      const std::vector<ReceiptAst*>* receipt_asts) {
  // Setup the pointers
  expr_asts_ = expr_asts;
  receipt_asts_ = receipt_asts;
  return true;
}

int Intrp::ExecuteMakefile(int argc, char** argv) {
  bool def_tgt = false;
  // int ret = 0;
  std::string *file = nullptr;
  std::string cmd;

  if ((expr_asts_ == nullptr) || (receipt_asts_ == nullptr) || (argc < 1) || (argv == nullptr)) {
    return -1;
  }

  // Determine that whether to take default target
  if (argc < 2) {
    def_tgt = true;
  } else {
    file = new std::string(argv[1]);
  }

  // Iterate
  for (std::vector<ReceiptAst*>::const_iterator it = receipt_asts_->begin();
       it != receipt_asts_->end();
       ++it) {
    // Static cast to ComplexReceiptAst
    if ((*it)->GetType() != ReceiptAst::RECEIPTAST_COMPLEX) {
      continue;
    }
    ComplexReceiptAst* cr_ast = static_cast<ComplexReceiptAst *>(*it);

    // Target names
    for (std::vector<std::unique_ptr<ExprAst>*>::const_iterator j = cr_ast->GetNames()->begin();
         j != cr_ast->GetNames()->end();
         ++j) {
      ExprAst* b_ast = static_cast<ExprAst*>((*j)->get());
      if (def_tgt == true) {
        break;
      } else if (*b_ast->GetName() == *file) {
        break;
      }
    }

    // Rules
    for (std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>::const_iterator z
            = cr_ast->GetRules()->begin();
         z != cr_ast->GetRules()->end();
         ++z) {
      // Iterate sub-rules
      for (std::vector<std::unique_ptr<ExprAst>*>::const_iterator u = (*z)->get()->begin();
           u != (*z)->get()->end();
           ++u) {
        // Static cast to either StringExprAst or VariableExprAst
        StringExprAst* s_ast = static_cast<StringExprAst*>((*u)->get());
        VariableExprAst* v_ast = static_cast<VariableExprAst*>((*u)->get());

        if (s_ast->GetType() == ExprAst::EXPRAST_STRING) {
          cmd.append(*s_ast->GetName());
        } else if (v_ast->GetType() == ExprAst::EXPRAST_VAR) {
          cmd.append(*s_ast->GetName());
        }
      }

      // Execute commands
      // ret = execvp(cmd.c_str(), nullptr);
      std::cout << "Execute command \"" << cmd << "\"\n";
      system(cmd.c_str());
      cmd.clear();
    }

    // Only do the 1st one as the default target or not
    if (def_tgt == true) {
      break;
    }
  }

  return 0;
}

}  // namespace gormake
