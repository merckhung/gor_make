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

#include <string.h>
#include "RdFile.h"

namespace UnixFile {

RdFile::RdFile()
  : fMem_(MAP_FAILED),
    fd_(-1),
    fileLen_(-1),
    currPos_(-1),
    currLine_(-1),
    byteRead_(0),
    nrLine_(-1),
    writable_(false) {
}

RdFile::RdFile(int fd)
  : fMem_(MAP_FAILED),
    fd_(fd),
    fileLen_(-1),
    currPos_(-1),
    currLine_(-1),
    byteRead_(0),
    nrLine_(-1),
    writable_(false) {
    RestFlowForOpening();
}

RdFile::RdFile(int fd, const std::string& name)
  : fMem_(nullptr),
    fd_(fd),
    fileLen_(-1),
    currPos_(-1),
    currLine_(-1),
    byteRead_(0),
    nrLine_(-1),
    writable_(false),
    filePath_(name) {
  RestFlowForOpening();
}

RdFile::~RdFile() {
  if (IsOpen() == true) {
    Close();
  }
}

bool RdFile::Open(const std::string& name, int flags) {
  // Open the file
  fd_ = open(name.c_str(), flags);
  if (fd_ < 0) {
    return false;
  }

  // Store the name
  filePath_ = name;
  return RestFlowForOpening();
}

bool RdFile::Open(const std::string& name, int flags, mode_t mode) {
  // Open the file
  fd_ = open(name.c_str(), flags, mode);
  if (fd_ < 0) {
    return false;
  }

  // Store the name
  filePath_ = name;
  return RestFlowForOpening();
}

bool RdFile::Close() {
  // Unmap and then close the file
  if (MemoryUnmapFile() == false) {
    return false;
  }
  if (close(fd_) != 0) {
    return false;
  }

  // Reset the state
  fd_ = -1;
  fMem_ = MAP_FAILED;
  fileLen_ = -1;
  currPos_ = -1;
  currLine_ = -1;
  byteRead_ = 0;
  nrLine_ = -1;
  writable_ = false;
  filePath_ = "";

  // Return
  return true;
}

bool RdFile::IsOpen() const {
  return (fd_ >= 0);
}

const std::string& RdFile::GetFilePath() const {
  return filePath_;
}

bool RdFile::SetWritable(bool en) {
  // Unmap the file first
  if (MemoryUnmapFile() == false) {
    return false;
  }

  if (MemoryMapFile(en) == false) {
    Close();
    return false;
  }

  // Update the state
  writable_ = en;

  // Return
  return true;
}

bool RdFile::SetLength(int64_t newLen) {
  // DO NOT allow newLen <= 0,
  // use WrFile class instead
  if (newLen <= 0) {
    return false;
  }

  // Unmap the file first
  if (MemoryUnmapFile() == false) {
    return false;
  }

#ifdef __APPLE__
  if (ftruncate(fd_, static_cast<int>(newLen))) {
#else
  if (ftruncate64(fd_, static_cast<int>(newLen))) {
#endif
    return false;
  }

  if (MemoryMapFile(writable_) == false) {
    Close();
    return false;
  }

  // Update the cache of the length
  // Reset the current position
  if (currPos_ > GetLength()) {
    currPos_ = GetLength();

    // Reset line no.
    currLine_ = -1;
    CalibrateCurrLine();

    // Reset the number of line
    nrLine_ = -1;
    GetNrLine();
  }

  // Update the state
  byteRead_ = 0;
  fileLen_ = newLen;

  // Return
  return true;
}

int64_t RdFile::GetLength() {
  struct stat sb;
  int rc;

  // If it's been cached
  if (fileLen_ > -1) {
    return fileLen_;
  }

  // Read the file length from the file system
  rc = fstat(fd_, &sb);
  if (rc) {
    // Error happens
    return rc;
  }

  // Cache it
  fileLen_ = sb.st_size;
  return fileLen_;
}

int64_t RdFile::ReadAt(char* buf, int64_t len, int64_t offset) {
  if (len > (fileLen_ - offset)) {
    len = fileLen_ - offset;
  }

  memcpy(buf, (reinterpret_cast<char*>(fMem_) + offset), len);
  return len;
}

int64_t RdFile::WriteAt(const char* buf, int64_t len, int64_t offset) {
  if (len > (fileLen_ - offset)) {
    len = fileLen_ - offset;
  }

  memcpy((reinterpret_cast<char*>(fMem_) + offset), buf, len);
  return len;
}

inline char RdFile::_SnoopAt(int64_t offset) const {
  return *(reinterpret_cast<char*>(fMem_) + offset);
}

char RdFile::SnoopAt(int64_t offset) const {
  return _SnoopAt(offset);
}

int RdFile::Flush() const {
  return (fsync(fd_) == 0) ? true : false;
}

char RdFile::ReadByte() {
  char c;

  // Ensure that it's still in the range
  if (currPos_ >= fileLen_) {
    byteRead_ = 0;
    return '\0';
  }

  // Update the state
  byteRead_ = 1;

  // Update the line number
  c = *(reinterpret_cast<char*>(fMem_) + currPos_++);
  if (c == '\n') {
    currLine_++;
  }

  // Read a byte
  return c;
}

void RdFile::ReadByte(char *buf) {
  *buf = ReadByte();
}

int64_t RdFile::ReadBytes(char* buf, int64_t len) {
  // Ensure that it's still in the range
  if ((currPos_ + len) >= fileLen_) {
    len = fileLen_ - currPos_;
  }

  // Read some bytes
  memcpy(buf, reinterpret_cast<char*>(fMem_) + currPos_, len);

  // Update the line number
  for (int64_t i = 0; i < len; ++i) {
    if (*(reinterpret_cast<char*>(fMem_) + currPos_ + i) == '\n') {
      currLine_++;
    }
  }

  // Update the state
  byteRead_ = len;
  currPos_ += len;
  return len;
}

char RdFile::SnoopCurr() const {
  return _SnoopAt(currPos_);
}

char RdFile::SnoopNext() const {
  return _SnoopAt(currPos_ + 1);
}

int64_t RdFile::AdvancePos() {
  return AdvancePos(1);
}

int64_t RdFile::AdvancePos(int64_t off) {
  if ((currPos_ + off) >= fileLen_) {
    currPos_ = fileLen_ - 1;
    return currPos_;
  }
  currPos_ += off;
  return currPos_;
}

int64_t RdFile::LocateByte(char ch) const {
  int64_t len;
  char* p = reinterpret_cast<char*>(fMem_);

  for (len = currPos_; len < fileLen_; len++) {
    if (*(p + len) == ch) {
      return (len - currPos_);
    }
  }

  return (fileLen_ - currPos_ - 1);
}

int64_t RdFile::LocateEitherByte(char c1, char c2) const {
  int64_t len;
  char* p = reinterpret_cast<char*>(fMem_);

  for (len = currPos_; len < fileLen_; len++) {
    if ((*(p + len) == c1) || (*(p + len) == c2)) {
      return (len - currPos_);
    }
  }

  return (fileLen_ - currPos_ - 1);
}

int64_t RdFile::LocateEitherByte(char c1, char c2, char c3) const {
  int64_t len;
  char* p = reinterpret_cast<char*>(fMem_);

  for (len = currPos_; len < fileLen_; len++) {
    if ((*(p + len) == c1)
        || (*(p + len) == c2)
        || (*(p + len) == c3)) {
      return (len - currPos_);
    }
  }

  return (fileLen_ - currPos_ - 1);
}

int64_t RdFile::RollBack() {
  if (byteRead_ <= 0) {
    return 0;
  }
  int64_t tmp = byteRead_;
  currPos_ -= byteRead_;
  byteRead_ = 0;
  return tmp;
}

int64_t RdFile::GetPosition() const {
  return currPos_;
}

int64_t RdFile::SetPosition(int64_t pos) {
  if ((pos < 0) || (pos >= fileLen_)) {
    return -1;
  }
  currPos_ = pos;
  byteRead_ = 0;
  CalibrateCurrLine();
  return pos;
}

int64_t RdFile::ResetPosition() {
  return SetPosition(0);
}

bool RdFile::IsBOF() const {
  return (currPos_ == 0) ? true : false;
}

bool RdFile::IsEOF() const {
  return ((currPos_ + 1) >= fileLen_) ? true : false;
}

int64_t RdFile::ReadLine(char* buf, int64_t len) {
  int64_t off;

  // Ensure it's in the range
  if ((currPos_ + len) >= fileLen_) {
    len = fileLen_ - currPos_;
  }

  // Look for a new line character
  for (off = 0; off < len; ++off) {
    if (_SnoopAt(currPos_ + off) == '\n') {
      break;
    }
  }

  // Copy it but omit the NL character
  // Then, move to BOL of next line
  memcpy(buf, (reinterpret_cast<char*>(fMem_) + currPos_), off);
  buf[off] = '\0';

  // If didn't see a NL, then copy it all
  // It's assumed to be at the last line (EOF)
  if (off == len) {
    // Update only the currPos, it's at last line
    currPos_ += off;
    byteRead_ = off;
    return off;
  }

  // Update the state
  currPos_ += (off + 1);
  currLine_++;
  byteRead_ = off;
  return off;
}

int64_t RdFile::ReadLines(char* buf, int64_t len, int nr) {
  int64_t off;
  int cnt = 0;

  // Ensure it's in the range
  if ((currPos_ + len) >= fileLen_) {
    len = fileLen_ - currPos_;
  }

  // Look for some new line characters
  for (off = 0; off < len; ++off) {
    if (_SnoopAt(currPos_ + off) == '\n') {
      if (++cnt == nr) {
        break;
      }
    }
  }

  // Copy it but omit the NL character
  // Then, move to BOL of next line
  memcpy(buf, (reinterpret_cast<char*>(fMem_) + currPos_), off);
  buf[off] = '\0';

  // If not enough NLs, then copy it all
  // It's assumed to be close to the last line (EOF)
  if (cnt != nr) {
    // Update only the currPos, it's at last line
    currPos_ += off;
    currLine_+= cnt;
    byteRead_ = off;
    return off;
  }

  // Update the state
  currPos_ += (off + 1);
  currLine_+= cnt;
  byteRead_ = off;
  return off;
}

int64_t RdFile::GetLineLength() const {
  return LocateByte('\n') + 1;
}

int64_t RdFile::CalibrateCurrLine() {
  char *p = reinterpret_cast<char*>(fMem_);
  int64_t curr = FIRST_LINE_IDX;

  // Compute the number of lines from the beginning
  // and exclude the current position itself (< currPos_)
  for (int i = 0; i < currPos_; ++i) {
    if (*(p + i) == '\n') {
      curr++;
    }
  }

  currLine_ = curr;
  return currLine_;
}

int64_t RdFile::GetNrLine() {
  char *p = reinterpret_cast<char*>(fMem_);

  // Response by the cache
  if (nrLine_ > -1) {
    return nrLine_;
  }

  // Calculate the number of lines
  nrLine_ = FIRST_LINE_IDX;
  for (int i = 0; i < fileLen_; ++i) {
    if (*(p + i) == '\n') {
      nrLine_++;
    }
  }

  return nrLine_;
}

int64_t RdFile::NextLine() {
  int64_t i;

  // Look to having 1 NL
  // Intent to move to BOL of next line
  for (i = currPos_; i < fileLen_; ++i) {
    if (_SnoopAt(i) == '\n') {
      break;
    }
  }

  // If it reaches the EOF, move the cursor to EOF
  if (i == fileLen_) {
    currPos_ = fileLen_ - 1;
    return currLine_;
  }

  // Update the state
  currPos_ = i + 1;
  byteRead_ = 0;
  return ++currLine_;
}

int64_t RdFile::PrevLine() {
  int cnt = 0, expect = 2;
  int64_t i;

  // If it's happened to be on a NL
  if (_SnoopAt(currPos_) == '\n') {
    expect = 3;
  }

  // Look to having 2 or 3 NLs
  // Intent to move to BOL of previous line
  for (i = currPos_; i >= 0; --i) {
    if (_SnoopAt(i) == '\n') {
      cnt++;
    }
    if (cnt == expect) {
      break;
    }
  }

  // if cnt == 0 or == 1,
  // then move the cursor to BOF
  if (cnt >= 0 && cnt <= 1) {
    currPos_ = 0;
    currLine_ = FIRST_LINE_IDX;
  } else {
    currPos_ = (i + 1);  // BOL
    currLine_--;
  }

  byteRead_ = 0;
  return currLine_;
}

int64_t RdFile::MoveToLine(int64_t line) {
  currPos_ = GetPositionByLineOffset(line, 0);
  currLine_ = line;
  byteRead_ = 0;
  return currPos_;
}

int64_t RdFile::MoveToLineOffset(int64_t line, int64_t off) {
  int64_t pos = GetPositionByLineOffset(line, off);

  // Check if it's valid
  if (pos < 0) {
    return -1;
  }

  // Update the state
  currPos_ = pos;
  currLine_ = line;
  byteRead_ = 0;
  return currPos_;
}

int64_t RdFile::GetLineNumber() {
  return currLine_;
}

int64_t RdFile::GetLineOffset() {
  int64_t cnt = 0;
  if (currPos_ == 0) {
    return 0;
  }
  for (int64_t i = currPos_; i > 0; --i, ++cnt) {
    if (_SnoopAt(i - 1) == '\n') {
      break;
    }
  }
  return cnt;
}

int64_t RdFile::GetPositionByLineOffset(int64_t line, int64_t off) {
  int64_t ln, ioff, pos;

  // Check if the inputs are invalid, error
  if ((line < FIRST_LINE_IDX) || (off < 0)) {
    return -1;
  }

  for (pos = 0, ln = FIRST_LINE_IDX, ioff = 0; pos < fileLen_; ++pos) {
    // Check the exit condition
    if ((ln == line) && (ioff == off)) {
      // Find the valid offset
      return pos;
    } else if (ln > line) {
      // No such offset
      return -1;
    }

    // Count the line no. and offset
    if (_SnoopAt(pos) == '\n') {
      ln++;
      ioff = 0;
    } else {
      ioff++;
    }
  }

  return -1;
}

bool RdFile::IsBOL() const {
  if (currPos_ == 0) {
    return true;
  } else if ((currPos_ > 0) && (_SnoopAt(currPos_ - 1) == '\n')) {
    return true;
  }
  return false;
}

bool RdFile::IsEOL() const {
  if (currPos_ >= (fileLen_ - 1)) {
    return true;
  } else if (_SnoopAt(currPos_ + 1) == '\n') {
    return true;
  }
  return false;
}

void* RdFile::GetFileMem() const {
  return fMem_;
}

bool RdFile::RestFlowForOpening() {
  // Check the length of the opened file
  // Don't allow length == 0, because the new
  // file isn't able to be mmapped, use WrFile
  // class instead (Slower Read & Write)
  if (GetLength() <= 0) {
    goto ErrExit;
  }

  // Memory map the file
  if (MemoryMapFile(writable_) == false) {
    goto ErrExit;
  }

  // Change the state
  currPos_ = 0;
  currLine_ = FIRST_LINE_IDX;

  // Return
  return true;

 ErrExit:
  Close();
  return false;
}

bool RdFile::MemoryMapFile(bool wr) {
  int prot = (wr == true) ? (PROT_READ | PROT_WRITE) : (PROT_READ);
  int flag = (wr == true) ? MAP_SHARED : MAP_PRIVATE;

  // Map the file into memory
  fMem_ = mmap(nullptr, fileLen_, prot, flag, fd_, 0);
  if (fMem_ == MAP_FAILED )
    return false;
  return true;
}

bool RdFile::MemoryUnmapFile() {
  int rc = 0;
  if (fMem_ != MAP_FAILED) {
    rc = munmap(fMem_, fileLen_);
  }
  return (rc) ? false : true;
}

}  // namespace UnixFile
