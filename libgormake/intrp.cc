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
  : exprAsts_(nullptr),
    receiptAsts_(nullptr) {
}

Intrp::~Intrp() {
}

bool Intrp::SetupAsts(const std::vector<ExprAst*>* exprAsts,
                      const std::vector<ReceiptAst*>* receiptAsts) {
  // Setup the pointers
  exprAsts_ = exprAsts;
  receiptAsts_ = receiptAsts;
  return true;
}

int Intrp::ExecuteMakefile(int argc, char** argv) {
  bool defTgt = false;
  // int ret = 0;
  std::string *file = nullptr;
  std::string cmd;

  if ((exprAsts_ == nullptr) || (receiptAsts_ == nullptr) || (argc < 1) || (argv == nullptr)) {
    return -1;
  }

  // Determine that whether to take default target
  if (argc < 2) {
    defTgt = true;
  } else {
    file = new std::string(argv[1]);
  }

  // Iterate
  for (std::vector<ReceiptAst*>::const_iterator it = receiptAsts_->begin();
       it != receiptAsts_->end();
       ++it) {
    // Static cast to ComplexReceiptAst
    if ((*it)->GetType() != ReceiptAst::RECEIPTAST_COMPLEX) {
      continue;
    }
    ComplexReceiptAst* crAst = static_cast<ComplexReceiptAst *>(*it);

    // Target names
    for (std::vector<std::unique_ptr<ExprAst>*>::const_iterator j = crAst->GetNames()->begin();
         j != crAst->GetNames()->end();
         ++j) {
      ExprAst* bAst = static_cast<ExprAst*>((*j)->get());
      if (defTgt == true) {
        break;
      } else if (*bAst->GetName() == *file) {
        break;
      }
    }

    // Rules
    for (std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>::const_iterator z
            = crAst->GetRules()->begin();
         z != crAst->GetRules()->end();
         ++z) {
      // Iterate sub-rules
      for (std::vector<std::unique_ptr<ExprAst>*>::const_iterator u = (*z)->get()->begin();
           u != (*z)->get()->end();
           ++u) {
        // Static cast to either StringExprAst or VariableExprAst
        StringExprAst* sAst = static_cast<StringExprAst*>((*u)->get());
        VariableExprAst* vAst = static_cast<VariableExprAst*>((*u)->get());

        if (sAst->GetType() == ExprAst::EXPRAST_STRING) {
          cmd.append(*sAst->GetName());
        } else if (vAst->GetType() == ExprAst::EXPRAST_VAR) {
          cmd.append(*sAst->GetName());
        }
      }

      // Execute commands
      // ret = execvp(cmd.c_str(), nullptr);
      std::cout << "Execute command \"" << cmd << "\"\n";
      system(cmd.c_str());
      cmd.clear();
    }

    // Only do the 1st one as the default target or not
    if (defTgt == true) {
      break;
    }
  }

  return 0;
}

}  // namespace gormake
