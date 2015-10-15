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

#include <memory>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "OS.h"
#include "RdFile.h"
#include "WrFile.h"

namespace gormake {

RdFile* OS::OpenFileReadOnly(const char* name) {
  return OpenFileWithFlags(name, O_RDONLY);
}

RdFile* OS::OpenFileReadWrite(const char* name) {
  return OpenFileWithFlags(name, O_RDWR);
}

WrFile* OS::CreateEmptyFile(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  std::unique_ptr<WrFile> file(new WrFile);
  if (!file->Open(name, O_RDWR | O_CREAT | O_TRUNC, 0666)) {
    return nullptr;
  }
  return file.release();
}

RdFile* OS::OpenFileWithFlags(const char* name, int flags) {
  if (name == nullptr) {
    return nullptr;
  }
  std::unique_ptr<RdFile> file(new RdFile);
  if (!file->Open(name, flags, 0666)) {
    return nullptr;
  }
  return file.release();
}

bool OS::FileExists(const char* name) {
  struct stat st;
  if (stat(name, &st) == 0) {
    return S_ISREG(st.st_mode);
  }
  return false;
}

bool OS::DirectoryExists(const char* name) {
  struct stat st;
  if (stat(name, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  return false;
}

}  // namespace gormake
