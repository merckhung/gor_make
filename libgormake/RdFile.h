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

#ifndef GORMAKE_LIBGORMAKE_RDFILE_H_
#define GORMAKE_LIBGORMAKE_RDFILE_H_

#include "macros.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string>

namespace UnixFile {

class RdFile {
 public:
  // Constructors & destructor
  RdFile();
  explicit RdFile(int fd);
  explicit RdFile(int fd, const std::string& name);

  virtual ~RdFile();

  // General operation
  bool Open(const std::string& name, int flags);
  bool Open(const std::string& name, int flags, mode_t mode);
  bool Close();
  bool IsOpen() const;
  const std::string& GetFilePath() const;
  bool SetWritable(bool en);

  // Random access
  bool SetLength(int64_t newLen);
  int64_t GetLength();
  int64_t ReadAt(char* buf, int64_t len, int64_t offset);
  int64_t WriteAt(const char* buf, int64_t len, int64_t offset);
  char SnoopAt(int64_t offset) const;
  int Flush() const;

  // Sequential access
  char ReadByte();
  void ReadByte(char *buf);
  char SnoopCurr() const;
  char SnoopNext() const;
  int64_t AdvancePos();
  int64_t AdvancePos(int64_t off);
  int64_t LocateByte(char ch) const;
  int64_t LocateEitherByte(char c1, char c2) const;
  int64_t LocateEitherByte(char c1, char c2, char c3) const;
  int64_t ReadBytes(char* buf, int64_t len);
  int64_t RollBack();
  int64_t GetPosition() const;
  int64_t SetPosition(int64_t pos);
  int64_t ResetPosition();
  bool IsBOF() const;
  bool IsEOF() const;

  // Line operation
  int64_t ReadLine(char* buf, int64_t len);
  int64_t ReadLines(char* buf, int64_t len, int nrLine);
  int64_t GetLineLength() const;
  int64_t CalibrateCurrLine();
  int64_t GetNrLine();
  int64_t NextLine();
  int64_t PrevLine();
  int64_t MoveToLine(int64_t line);
  int64_t MoveToLineOffset(int64_t line, int64_t off);
  int64_t GetLineNumber();
  int64_t GetLineOffset();
  int64_t GetPositionByLineOffset(int64_t line, int64_t off);
  bool IsBOL() const;
  bool IsEOL() const;

  void* GetFileMem() const;

  // Static constants
  static const int64_t FIRST_LINE_IDX = 0;

 private:
  bool RestFlowForOpening();
  bool MemoryMapFile(bool wr);
  bool MemoryUnmapFile();
  inline char _SnoopAt(int64_t offset) const;

  void *fMem_;
  int fd_;
  int64_t fileLen_;
  int64_t currPos_;
  int64_t currLine_;
  int64_t byteRead_;
  int64_t nrLine_;
  bool writable_;
  std::string filePath_;

  DISALLOW_COPY_AND_ASSIGN(RdFile);
};

}  // namespace UnixFile

#endif  // GORMAKE_LIBGORMAKE_RDFILE_H_
