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

#ifndef GORMAKE_LIBGORMAKE_BUILD_ENGINE_BASE_H_
#define GORMAKE_LIBGORMAKE_BUILD_ENGINE_BASE_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gormake {

// Shared utility functions for build engines.
// All scanners use these to avoid code duplication.
namespace buildutil {

// Check if a file exists.
bool FileExists(const std::string& path);

// Create directories recursively (like mkdir -p).
bool MkdirP(const std::string& path);

// Get the basename of a file (without extension).
std::string BaseName(const std::string& path);

// Get the directory name of a path.
std::string DirName(const std::string& path);

// Check if a source file is C++ (.cc, .cpp, .cxx, .C).
bool IsCppSource(const std::string& src);

// Check if a source file is C (.c).
bool IsCSource(const std::string& src);

// Check if an object file needs recompilation (mtime-based).
// Also checks .d dependency file for header changes.
bool NeedsRecompile(const std::string& obj_file, const std::string& src_file);

// Check if any header listed in a .d dependency file is newer than the .o.
bool CheckDepFile(const std::string& obj_file);

// Execute a shell command, printing it first. Returns true on success.
bool ExecuteCmd(const std::string& cmd);

// Get the appropriate compiler for a source file.
std::string GetCompiler(const std::string& src);

}  // namespace buildutil

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_BUILD_ENGINE_BASE_H_
