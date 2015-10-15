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

#ifndef GORMAKE_LIBGORMAKE_OS_H_
#define GORMAKE_LIBGORMAKE_OS_H_

namespace UnixFile {
class RdFile;
class WrFile;
}  // namespace UnixFile

namespace gormake {

typedef ::UnixFile::RdFile RdFile;
typedef ::UnixFile::WrFile WrFile;

class OS {
 public:
  // Open an existing file with read only access
  static RdFile* OpenFileReadOnly(const char* name);

  // Open an existing file with read/write access
  static RdFile* OpenFileReadWrite(const char* name);

  // Create an empty file with read/write access
  static WrFile* CreateEmptyFile(const char* name);

  // Open a file with the specified flags
  static RdFile* OpenFileWithFlags(const char* name, int flags);

  // Check if a file exists
  static bool FileExists(const char* name);

  // Check if a directory exists
  static bool DirectoryExists(const char* name);
};

}  // namespace gormake

#endif  // GORMAKE_LIBGORMAKE_OS_H_
