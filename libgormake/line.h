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

#ifndef GORMAKE_LIBGORMAKE_LINE_H_
#define GORMAKE_LIBGORMAKE_LINE_H_

#include <stdint.h>

namespace gormake {

struct Line {
  // Constant defintions
  enum LnTyp {
    LT_INVALID = 0,
    LT_EMPTY,
    LT_TARGET,
    LT_RULE,
    LT_VARIABLE,
    LT_DIRECTIVE,
    LT_COMMENT,
    LT_AMB_TABVAR,
  };

  // Property of a line
  LnTyp lineType_;
  int64_t line_;
  int64_t strPos_;
  int64_t strLen_;
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_LINE_H_
