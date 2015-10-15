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

#ifndef GORMAKE_LIBGORMAKE_TABLE_H_
#define GORMAKE_LIBGORMAKE_TABLE_H_

#include <string>
#include <string.h>

namespace gormake {

static const std::string EMPTY_STR = "";

static const Lexer::KeyCharToTok KeyCharToTokTbl[] = {
  { ' ',  Token::TOK_SPACE    },
  { '#',  Token::TOK_COMMENT  },
  { '\t', Token::TOK_TAB      },
  { '\n', Token::TOK_NEWLINE  },
  { '\r', Token::TOK_RETURN   },
  { '$',  Token::TOK_DOLLAR   },
  { ':',  Token::TOK_COLON    },
  { '=',  Token::TOK_EQUAL    },
  { '?',  Token::TOK_QMARK    },
  { '+',  Token::TOK_PLUS     },
  { '@',  Token::TOK_AT       },
  { '<',  Token::TOK_LESSER   },
  { '>',  Token::TOK_GREATER  },
  { '%',  Token::TOK_PRECENT  },
  { '^',  Token::TOK_CARET    },
  { '*',  Token::TOK_STAR     },
};
static const int32_t NrKeyCharToTokTbl = ARRAY_SIZE(KeyCharToTokTbl);

#define X(STR, TOK) {(const int8_t*)STR, strlen(STR), TOK}
const Lexer::KeyPhraseToTok KeyPhraseToTokTbl[] = {
  X("vpath",                Token::TOK_VPATH),
  X("VPATH",                Token::TOK_VPATH_CAPS),
  X("include",              Token::TOK_INCLUDE),
  X("if",                   Token::TOK_IF),
  X("ifdef",                Token::TOK_IFDEF),
  X("ifndef",               Token::TOK_IFNDEF),
  X("endef",                Token::TOK_ENDEF),
  X("ifeq",                 Token::TOK_IFEQ),
  X("ifneq",                Token::TOK_IFNEQ),
  X("else",                 Token::TOK_ELSE),
  X("endif",                Token::TOK_ENDIF),
  X("load",                 Token::TOK_LOAD),
  X(".DEFAULT_GOAL",        Token::TOK_DEF_GOAL),
  X(".DEFAULT",             Token::TOK_DEF),
  X(".DELETE_ON_ERROR",     Token::TOK_DEL_ON_ERR),
  X(".EXPORT_ALL_VARIABLE", Token::TOK_EXP_ALL_VAR),
  X(".FEATURES",            Token::TOK_FEATURES),
  X(".IGNORE",              Token::TOK_IGNORE),
  X(".INCLUDE_DIRS",        Token::TOK_INC_DIRS),
  X(".INTERMEDIATE",        Token::TOK_INTERMED),
  X(".LIBPATTERNS",         Token::TOK_LIBPATTERNS),
  X(".LOADED",              Token::TOK_LOADED),
  X(".LOW_RESOLUTION_TIME", Token::TOK_LOW_RES_TIME),
  X(".NOTPARALLEL",         Token::TOK_NOTPARALLEL),
  X(".ONESHELL",            Token::TOK_ONESHELL),
  X(".PHONY",               Token::TOK_PHONY),
  X(".POSIX",               Token::TOK_POSIX),
  X(".PRECIOUS",            Token::TOK_PRECIOUS),
  X(".RECIPEPREFIX",        Token::TOK_RECIPE_PREFIX),
  X(".SECONDARY",           Token::TOK_SECONDARY),
  X(".SECONDEXPANSION",     Token::TOK_SECONDEXPANSION),
  X(".SHELLFLAGS",          Token::TOK_SHELLFLAGS),
  X(".SILENT",              Token::TOK_SLIENT),
  X(".SUFFIXES",            Token::TOK_SUFFIXES),
  X(".VARIABLES",           Token::TOK_VARIABLES),
};
static const int32_t NrKeyPhraseToTokTbl = ARRAY_SIZE(KeyPhraseToTokTbl);
#undef X

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_TABLE_H_
