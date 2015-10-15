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

#include "WrFile.h"

namespace UnixFile {

WrFile::WrFile()
  : fd_(-1),
    auto_close_(true) {
}

WrFile::WrFile(int fd)
  : fd_(fd),
    auto_close_(true) {
}

WrFile::WrFile(int fd, const std::string& path)
  : fd_(fd),
  file_path_(path),
  auto_close_(true) {
}

WrFile::~WrFile() {
  if (auto_close_ && fd_ >= 0) {
    Close();
  }
}

void WrFile::DisableAutoClose() {
  auto_close_ = false;
}

bool WrFile::Open(const std::string& path, int flags) {
  return Open(path, flags, 0640);
}

bool WrFile::Open(const std::string& path, int flags, mode_t mode) {
  fd_ = open(path.c_str(), flags, mode);
  if (fd_ < 0) {
    return false;
  }
  file_path_ = path;
  return true;
}

bool WrFile::Close() {
  if (close(fd_) != 0) {
    return false;
  }

  fd_ = -1;
  file_path_ = "";
  return true;
}

bool WrFile::Flush() {
  return (fsync(fd_) == 0) ? true : false;
}

int64_t WrFile::Read(char* buf, int64_t byte_count, int64_t offset) const {
  return pread(fd_, buf, byte_count, offset);
}

bool WrFile::SetLength(int64_t new_length) {
  return (ftruncate(fd_, new_length) == 0) ? true : false;
}

int64_t WrFile::GetLength() const {
  struct stat s;
  int rc = fstat(fd_, &s);
  return (rc < 0) ? rc : s.st_size;
}

int64_t WrFile::Write(const char* buf, int64_t byte_count, int64_t offset) {
  return pwrite(fd_, buf, byte_count, offset);
}

bool WrFile::IsOpened() const {
  return fd_ >= 0;
}

std::string WrFile::GetPath() const {
  return file_path_;
}

bool WrFile::ReadFully(void* buffer, int64_t byte_count) {
  char* ptr = static_cast<char*>(buffer);
  while (byte_count > 0) {
    ssize_t bytes_read = read(fd_, ptr, byte_count);
    if (bytes_read <= 0) {
      return false;
    }
    byte_count -= bytes_read;
    ptr += bytes_read;
  }
  return true;
}

bool WrFile::WriteFully(const void* buffer, int64_t byte_count) {
  const char* ptr = static_cast<const char*>(buffer);
  while (byte_count > 0) {
    ssize_t bytes_read = write(fd_, ptr, byte_count);
    if (bytes_read < 0) {
      return false;
    }
    byte_count -= bytes_read;
    ptr += bytes_read;
  }
  return true;
}

}  // namespace UnixFile
