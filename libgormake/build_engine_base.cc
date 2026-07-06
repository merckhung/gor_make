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

#include "build_engine_base.h"

#include <cctype>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace gormake {
namespace buildutil {

bool FileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool MkdirP(const std::string& path) {
  std::string cmd = "mkdir -p " + path;
  return system(cmd.c_str()) == 0;
}

std::string BaseName(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base =
      (slash == std::string::npos) ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  return (dot == std::string::npos) ? base : base.substr(0, dot);
}

std::string DirName(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

bool IsCppSource(const std::string& src) {
  size_t dot = src.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = src.substr(dot);
  return ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C";
}

bool IsCSource(const std::string& src) {
  size_t dot = src.find_last_of('.');
  if (dot == std::string::npos) return false;
  return src.substr(dot) == ".c";
}

std::string GetCompiler(const std::string& src) {
  return IsCppSource(src) ? "g++" : "gcc";
}

bool NeedsRecompile(const std::string& obj_file,
                    const std::string& src_file) {
  if (!FileExists(obj_file)) return true;
  struct stat obj_stat, src_stat;
  if (stat(obj_file.c_str(), &obj_stat) != 0) return true;
  if (stat(src_file.c_str(), &src_stat) != 0) return true;
  if (src_stat.st_mtime > obj_stat.st_mtime) return true;
  // Check .d dependency file for header changes
  return CheckDepFile(obj_file);
}

bool CheckDepFile(const std::string& obj_file) {
  std::string dep_file = obj_file;
  // gcc -MMD generates .d file by replacing .o extension with .d
  size_t dot_pos = dep_file.rfind('.');
  if (dot_pos != std::string::npos) {
    dep_file = dep_file.substr(0, dot_pos) + ".d";
  } else {
    dep_file += ".d";
  }
  std::ifstream f(dep_file);
  if (!f.is_open()) return false;  // No .d file, can't check deps

  // Get .o file mtime
  struct stat obj_stat;
  if (stat(obj_file.c_str(), &obj_stat) != 0) return false;

  // Parse .d file: format is "output: dep1 dep2 dep3..."
  std::string line;
  while (std::getline(f, line)) {
    // Find the colon separator
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;

    // Parse dependency files after the colon
    std::string deps = line.substr(colon + 1);
    // Split by whitespace
    size_t start = 0;
    while (start < deps.size()) {
      while (start < deps.size() && isspace(static_cast<unsigned char>(deps[start])))
        start++;
      if (start >= deps.size()) break;
      size_t end = start;
      while (end < deps.size() && !isspace(static_cast<unsigned char>(deps[end])))
        end++;
      // Ignore line continuations
      if (end > start && deps[end-1] != '\\') {
        std::string dep_path = deps.substr(start, end - start);
        // Check if this dependency is newer than the .o file
        struct stat dep_stat;
        if (stat(dep_path.c_str(), &dep_stat) == 0) {
          if (dep_stat.st_mtime > obj_stat.st_mtime) return true;
        }
      }
      start = end;
    }
  }
  return false;
}

bool ExecuteCmd(const std::string& cmd) {
  std::printf("  %s\n", cmd.c_str());
  return system(cmd.c_str()) == 0;
}

}  // namespace buildutil
}  // namespace gormake
