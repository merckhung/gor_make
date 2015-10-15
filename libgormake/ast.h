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

#ifndef GORMAKE_LIBGORMAKE_AST_H_
#define GORMAKE_LIBGORMAKE_AST_H_

#include <memory>
#include <vector>

namespace gormake {

class ExprAst {
 public:
  enum ExprAstType {
    EXPRAST_BASE = 0,
    EXPRAST_STRING,
    EXPRAST_VAR,
    EXPRAST_VARASSIGN,
  };

 public:
  explicit ExprAst(const std::string* name)
    : type_(EXPRAST_BASE) {
    name_.reset(name);
  }
  explicit ExprAst(const std::string* name, ExprAstType type) {
    name_.reset(name);
    type_ = type;
  }
  virtual ~ExprAst() {
  }

  const std::string* GetName() const {
    return name_.get();
  }

  ExprAstType GetType() const {
    return type_;
  }

  // virtual llvm::Value* Codegen() = 0;

 private:
  std::unique_ptr<const std::string> name_;
  ExprAstType type_;
};

class StringExprAst : public ExprAst {
 public:
  explicit StringExprAst(const std::string* name)
    : ExprAst(name, EXPRAST_STRING) {
  }
};

class VariableExprAst : public ExprAst {
 public:
  explicit VariableExprAst(const std::string* name)
    : ExprAst(name, EXPRAST_VAR) {
  }
};

class VariableAssignExprAst : public ExprAst {
 public:
  enum VarAttr {
    VAR_NORMAL = 0,
    VAR_COLON,
    VAR_QMARK,
    VAR_PLUS,
  };

 public:
  explicit VariableAssignExprAst(const std::string* name,
                                 const std::vector<std::unique_ptr<ExprAst>*>* values)
    : ExprAst(name, EXPRAST_VARASSIGN),
      attr_(VAR_NORMAL) {
    values_.reset(values);
  }
  explicit VariableAssignExprAst(const std::string* name,
                                 VarAttr attr,
                                 const std::vector<std::unique_ptr<ExprAst>*>* values)
    : ExprAst(name, EXPRAST_VARASSIGN),
      attr_(attr) {
    values_.reset(values);
  }

  const std::vector<std::unique_ptr<ExprAst>*>* GetValues() const {
    return values_.get();
  }

  VarAttr GetAttr() const {
    return attr_;
  }

 private:
  std::unique_ptr<const std::vector<std::unique_ptr<ExprAst>*>> values_;
  VarAttr attr_;
};

class ReceiptAst {
 public:
  enum ReceiptAstType {
    RECEIPTAST_BASE = 0,
    RECEIPTAST_SIMPLE,
    RECEIPTAST_COMPLEX,
  };

 public:
  explicit ReceiptAst()
    : type_(RECEIPTAST_BASE) {
  }
  explicit ReceiptAst(ReceiptAstType type) {
    type_ = type;
  }
  virtual ~ReceiptAst() {
  }

  // virtual llvm::Value* Codegen() = 0;

  ReceiptAstType GetType() const {
    return type_;
  }

 private:
  ReceiptAstType type_;
};

class SimpleReceiptAst : public ReceiptAst {
 public:
  SimpleReceiptAst(const std::string* name,
                   const std::string* prereq,
                   const std::string* rule)
    : ReceiptAst(RECEIPTAST_SIMPLE),
      name_(name),
      prereq_(prereq),
      rule_(rule) {
  }

  const std::string* name_;
  const std::string* prereq_;
  const std::string* rule_;
};

class ComplexReceiptAst : public ReceiptAst {
 public:
  ComplexReceiptAst(const std::vector<std::unique_ptr<ExprAst>*>* names,
                    const std::vector<std::unique_ptr<ExprAst>*>* prereqs,
                    const std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>* rules)
    : ReceiptAst(RECEIPTAST_COMPLEX) {
    names_.reset(names);
    prereqs_.reset(prereqs);
    rules_.reset(rules);
  }

  const std::vector<std::unique_ptr<ExprAst>*>* GetNames() const {
    return names_.get();
  }

  const std::vector<std::unique_ptr<ExprAst>*>* GetPrereqs() const {
    return prereqs_.get();
  }

  const std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>* GetRules() const {
    return rules_.get();
  }

 private:
  std::unique_ptr<const std::vector<std::unique_ptr<ExprAst>*>> names_;
  std::unique_ptr<const std::vector<std::unique_ptr<ExprAst>*>> prereqs_;
  std::unique_ptr<const std::vector<std::unique_ptr<std::vector<std::unique_ptr<ExprAst>*>>*>> rules_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_AST_H_
