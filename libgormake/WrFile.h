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

#ifndef GORMAKE_LIBGORMAKE_WRFILE_H_
#define GORMAKE_LIBGORMAKE_WRFILE_H_

#include "macros.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string>

namespace UnixFile {

class WrFile {
 public:
  WrFile();
  explicit WrFile(int fd);
  explicit WrFile(int fd, const std::string& path);

  virtual ~WrFile();

  bool Open(const std::string& file_path, int flags);
  bool Open(const std::string& file_path, int flags, mode_t mode);

  bool Close();
  int64_t Read(char* buf, int64_t byte_count, int64_t offset) const;
  bool SetLength(int64_t new_length);
  int64_t GetLength() const;
  int64_t Write(const char* buf, int64_t byte_count, int64_t offset);
  bool Flush();

  int Fd() const;
  bool IsOpened() const;
  std::string GetPath() const;
  void DisableAutoClose();
  bool ReadFully(void* buffer, int64_t byte_count);
  bool WriteFully(const void* buffer, int64_t byte_count);

 private:
  int fd_;
  std::string file_path_;
  bool auto_close_;

  DISALLOW_COPY_AND_ASSIGN(WrFile);
};

}  // namespace UnixFile

#endif  // GORMAKE_LIBGORMAKE_WRFILE_H_
