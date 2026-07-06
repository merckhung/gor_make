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

#include "buildenginebase.h"

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

bool NeedsRecompile(const std::string& objFile,
                    const std::string& srcFile) {
  if (!FileExists(objFile)) return true;
  struct stat objStat, srcStat;
  if (stat(objFile.c_str(), &objStat) != 0) return true;
  if (stat(srcFile.c_str(), &srcStat) != 0) return true;
  if (srcStat.st_mtime > objStat.st_mtime) return true;
  // Check .d dependency file for header changes
  return CheckDepFile(objFile);
}

bool CheckDepFile(const std::string& objFile) {
  std::string depFile = objFile;
  // gcc -MMD generates .d file by replacing .o extension with .d
  size_t dotPos = depFile.rfind('.');
  if (dotPos != std::string::npos) {
    depFile = depFile.substr(0, dotPos) + ".d";
  } else {
    depFile += ".d";
  }
  std::ifstream f(depFile);
  if (!f.is_open()) return false;  // No .d file, can't check deps

  // Get .o file mtime
  struct stat objStat;
  if (stat(objFile.c_str(), &objStat) != 0) return false;

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
        std::string depPath = deps.substr(start, end - start);
        // Check if this dependency is newer than the .o file
        struct stat depStat;
        if (stat(depPath.c_str(), &depStat) == 0) {
          if (depStat.st_mtime > objStat.st_mtime) return true;
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
