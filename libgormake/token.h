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

#ifndef GORMAKE_LIBGORMAKE_TOKEN_H_
#define GORMAKE_LIBGORMAKE_TOKEN_H_

#include <stdint.h>

namespace gormake {

struct Token {
  // Constant defintions
  enum TokTyp {
    TOK_INVALID = 0,
    TOK_ID,
    TOK_NEWLINE,
    TOK_RETURN,
    TOK_COMMENT,
    TOK_DOLLAR,
    TOK_SPACE,
    TOK_TAB,
    TOK_COLON,
    TOK_QMARK,
    TOK_EQUAL,
    TOK_COLEQ,
    TOK_QMEQ,
    TOK_PLUSEQ,
    TOK_PLUS,
    TOK_AT,
    TOK_LESSER,
    TOK_GREATER,
    TOK_VAR,
    TOK_PRECENT,
    TOK_CARET,
    TOK_STAR,
    TOK_EOF,

    TOK_VPATH,
    TOK_VPATH_CAPS,
    TOK_INCLUDE,
    TOK_IF,
    TOK_IFDEF,
    TOK_IFNDEF,
    TOK_ENDEF,
    TOK_IFEQ,
    TOK_IFNEQ,
    TOK_ELSE,
    TOK_ENDIF,

    TOK_LOAD,
    TOK_DEF_GOAL,
    TOK_DEF,
    TOK_DEL_ON_ERR,
    TOK_EXP_ALL_VAR,
    TOK_FEATURES,
    TOK_IGNORE,
    TOK_INC_DIRS,
    TOK_INTERMED,
    TOK_LIBPATTERNS,
    TOK_LOADED,
    TOK_LOW_RES_TIME,
    TOK_NOTPARALLEL,
    TOK_ONESHELL,
    TOK_PHONY,
    TOK_POSIX,
    TOK_PRECIOUS,
    TOK_RECIPE_PREFIX,
    TOK_SECONDARY,
    TOK_SECONDEXPANSION,
    TOK_SHELLFLAGS,
    TOK_SLIENT,
    TOK_SUFFIXES,
    TOK_VARIABLES,
  };

  // Property of a token
  TokTyp tokenType_;
  int64_t line_;
  int64_t offset_;
  int64_t strPos_;
  int64_t strLen_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_TOKEN_H_
