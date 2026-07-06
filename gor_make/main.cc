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

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "engine.h"
#include "bpengine.h"
#include "cmakescanner.h"
#include "gnscanner.h"
#include "mkscanner.h"
#include "sconscanner.h"
#include "gormake.h"

static void usage() {
  std::cerr <<
"Usage: make [options] [target] ... \n"
"Options:\n"
"  -b, -m                      Ignored for compatibility.\n"
"  -B, --always-make           Unconditionally make all targets.\n"
"  -C DIRECTORY, --directory=DIRECTORY\n"
"                              Change to DIRECTORY before doing anything.\n"
"  -d                          Print lots of debugging information.\n"
"  --debug[=FLAGS]             Print various types of debugging information.\n"
"  -e, --environment-overrides\n"
"                              Environment variables override makefiles.\n"
"  -f FILE, --file=FILE, --makefile=FILE\n"
"                              Read FILE as a makefile.\n"
"  -h, --help                  Print this message and exit.\n"
"  -i, --ignore-errors         Ignore errors from commands.\n"
"  -I DIRECTORY, --include-dir=DIRECTORY\n"
"                              Search DIRECTORY for included makefiles.\n"
"  -j [N], --jobs[=N]          Allow N jobs at once; infinite jobs with no arg.\n"
"  -k, --keep-going            Keep going when some targets can't be made.\n"
"  -l [N], --load-average[=N], --max-load[=N]\n"
"                              Don't start multiple jobs unless load is below N.\n"
"  -L, --check-symlink-times   Use the latest mtime between symlinks and target.\n"
"  -n, --just-print, --dry-run, --recon\n"
"                              Don't actually run any commands; just print them.\n"
"  -o FILE, --old-file=FILE, --assume-old=FILE\n"
"                              Consider FILE to be very old and don't remake it.\n"
"  -p, --print-data-base       Print make's internal database.\n"
"  -q, --question              Run no commands; exit status says if up to date.\n"
"  -r, --no-builtin-rules      Disable the built-in implicit rules.\n"
"  -R, --no-builtin-variables  Disable the built-in variable settings.\n"
"  -s, --silent, --quiet       Don't echo commands.\n"
"  -S, --no-keep-going, --stop\n"
"                              Turns off -k.\n"
"  -t, --touch                 Touch targets instead of remaking them.\n"
"  -v, --version               Print the version number of make and exit.\n"
"  -w, --print-directory       Print the current directory.\n"
"  --no-print-directory        Turn off -w, even if it was turned on implicitly.\n"
"  -W FILE, --what-if=FILE, --new-file=FILE, --assume-new=FILE\n"
"                              Consider FILE to be infinitely new.\n"
"  --warn-undefined-variables  Warn when an undefined variable is referenced.\n"
"  --bp                        Build using Android.bp (Blueprint) files.\n"
"  --bp-file=FILE              Use FILE as the Android.bp file.\n"
"  --clean                     Remove all build outputs.\n"
"  -v, --verbose               Show all commands (not just recipe lines).\n"
"\n"
"  Report bugs to " GM_AUTHOR_NAME "\n";
}

static void version() {
  std::cerr <<
  GM_FULLNAME " " GM_REVISION "\n"
  GM_COPYRIGHT_TEXT "\n"
  "\n"
  "This program built for " GM_FULLNAME "\n";
}

// Check if a string is a VAR=value assignment.
static bool IsVarAssignment(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '=') return true;
    if (s[i] == ':') {
      if (i + 1 < s.size() && s[i+1] == '=') return true;
      return false;
    }
    if (s[i] == ' ' || s[i] == '\t') return false;
  }
  return false;
}

int main(int argc, char** argv) {
  gormake::MakeOptions opts;
  gormake::BpBuildOptions bpOpts;
  bool bpMode = false;
  bool mkMode = false;
  bool mkJsonOutput = false;
  std::string mkFile;
  std::string mkDir;
  bool gnMode = false;
  bool gnJsonOutput = false;
  std::string gnFile;
  std::string gnDir;
  bool cmakeMode = false;
  bool cmakeJsonOutput = false;
  std::string cmakeFile;
  std::string cmakeDir;
  bool sconsMode = false;
  bool sconsJsonOutput = false;
  std::string sconsFile;
  std::string sconsDir;
  bool dryRun = false;
  int jobs = 1;


  // Parse arguments
  int i = 1;
  while (i < argc) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (arg == "-v" || arg == "--version") {
      version();
      return 0;
    } else if (arg == "-n" || arg == "--dry-run" || arg == "--just-print" ||
               arg == "--recon") {
      opts.dryRun = true;
      bpOpts.dryRun = true;
      dryRun = true;
    } else if (arg == "-s" || arg == "--silent" || arg == "--quiet") {
      opts.silent = true;
    } else if (arg == "-k" || arg == "--keep-going") {
      opts.keepGoing = true;
    } else if (arg == "-i" || arg == "--ignore-errors") {
      opts.ignoreErrors = true;
    } else if (arg == "-B" || arg == "--always-make") {
      opts.alwaysMake = true;
    } else if (arg == "-w" || arg == "--print-directory") {
      opts.printDir = true;
    } else if (arg == "-f" || arg == "--file" || arg == "--makefile") {
      if (i + 1 < argc) {
        opts.makefilePath = argv[++i];
      }
    } else if (arg.substr(0, 9) == "--file=") {
      opts.makefilePath = arg.substr(9);
    } else if (arg.substr(0, 11) == "--makefile=") {
      opts.makefilePath = arg.substr(11);
    } else if (arg == "-C" || arg == "--directory") {
      if (i + 1 < argc) {
        opts.directory = argv[++i];
      }
    } else if (arg.substr(0, 12) == "--directory=") {
      opts.directory = arg.substr(12);
    } else if (arg == "-j" || arg == "--jobs") {
      opts.jobs = 0;  // unlimited
    } else if (arg.substr(0, 3) == "-j") {
      opts.jobs = atoi(arg.substr(3).c_str());
    } else if (arg.substr(0, 7) == "--jobs=") {
      opts.jobs = atoi(arg.substr(7).c_str());
    } else if (arg == "-e" || arg == "--environment-overrides") {
      // Environment overrides makefile (simplified: already imported env)
    } else if (arg == "-r" || arg == "--no-builtin-rules") {
      // No-op for now
    } else if (arg == "-R" || arg == "--no-builtin-variables") {
      // No-op for now
    } else if (arg == "-b" || arg == "-m") {
      // Ignored for compatibility
    } else if (arg == "-S" || arg == "--no-keep-going" || arg == "--stop") {
      opts.keepGoing = false;
    } else if (arg == "--no-print-directory") {
      opts.printDir = false;
    } else if (arg == "--bp") {
      bpMode = true;
    } else if (arg.substr(0, 10) == "--bp-file=") {
      bpMode = true;
      bpOpts.bpFilePath = arg.substr(10);
    } else if (arg == "--json") {
      bpOpts.jsonOutput = true;
      opts.jsonOutput = true;
      mkJsonOutput = true;
      gnJsonOutput = true;
      cmakeJsonOutput = true;
      sconsJsonOutput = true;
    } else if (arg == "--mk") {
      mkMode = true;
    } else if (arg.substr(0, 9) == "--mk-file") {
      mkMode = true;
      if (arg.size() > 9 && arg[9] == '=') {
        mkFile = arg.substr(10);
      } else if (i + 1 < argc) {
        mkFile = argv[++i];
      }
    } else if (arg.substr(0, 8) == "--mk-dir") {
      mkMode = true;
      if (arg.size() > 8 && arg[8] == '=') {
        mkDir = arg.substr(9);
      } else if (i + 1 < argc) {
        mkDir = argv[++i];
      }
    } else if (arg == "--gn") {
      gnMode = true;
    } else if (arg.substr(0, 9) == "--gn-file") {
      gnMode = true;
      if (arg.size() > 9 && arg[9] == '=') {
        gnFile = arg.substr(10);
      } else if (i + 1 < argc) {
        gnFile = argv[++i];
      }
    } else if (arg.substr(0, 8) == "--gn-dir") {
      gnMode = true;
      if (arg.size() > 8 && arg[8] == '=') {
        gnDir = arg.substr(9);
      } else if (i + 1 < argc) {
        gnDir = argv[++i];
      }
    } else if (arg == "--cmake") {
      cmakeMode = true;
    } else if (arg.substr(0, 12) == "--cmake-file") {
      cmakeMode = true;
      if (arg.size() > 12 && arg[12] == '=') {
        cmakeFile = arg.substr(13);
      } else if (i + 1 < argc) {
        cmakeFile = argv[++i];
      }
    } else if (arg.substr(0, 11) == "--cmake-dir") {
      cmakeMode = true;
      if (arg.size() > 11 && arg[11] == '=') {
        cmakeDir = arg.substr(12);
      } else if (i + 1 < argc) {
        cmakeDir = argv[++i];
      }
    } else if (arg == "--scons") {
      sconsMode = true;
    } else if (arg.substr(0, 12) == "--scons-file") {
      sconsMode = true;
      if (arg.size() > 12 && arg[12] == '=') {
        sconsFile = arg.substr(13);
      } else if (i + 1 < argc) {
        sconsFile = argv[++i];
      }
    } else if (arg.substr(0, 11) == "--scons-dir") {
      sconsMode = true;
      if (arg.size() > 11 && arg[11] == '=') {
        sconsDir = arg.substr(12);
      } else if (i + 1 < argc) {
        sconsDir = argv[++i];
      }
    } else if (arg == "--clean") {
      if (bpMode) {
        bpOpts.clean = true;
      } else {
        // Will be handled by make engine
      }
    } else if (arg == "-v" || arg == "--verbose") {
      opts.silent = false;  // verbose is opposite of silent
      bpOpts.verbose = true;
    } else if (arg[0] == '-' && arg.size() > 1) {
      // Unknown option, skip
    } else if (IsVarAssignment(arg)) {
      opts.cmdLineVars.push_back(arg);
    } else {
      // Target
      opts.goals.push_back(arg);
      bpOpts.goals.push_back(arg);
    }
    i++;
  }

  if (mkMode) {
    gormake::MkScanner scanner;
    scanner.SetDryRun(dryRun);
    scanner.SetJobs(jobs);
    if (!mkFile.empty()) {
      scanner.ScanFile(mkFile);
    } else if (!mkDir.empty()) {
      scanner.ScanDirectory(mkDir);
    } else if (std::ifstream("Android.mk").good()) {
      scanner.ScanFile("Android.mk");
    } else {
      std::cerr << "gor_make: No Android.mk file found.\n";
      return 1;
    }
    if (mkJsonOutput) {
      scanner.OutputJson();
    } else {
      return scanner.BuildAll();
    }
    return 0;
  }

  if (gnMode) {
    gormake::GnScanner scanner;
    scanner.SetDryRun(dryRun);
    scanner.SetJobs(jobs);
    if (!gnFile.empty()) {
      scanner.ScanFile(gnFile);
    } else if (!gnDir.empty()) {
      scanner.ScanDirectory(gnDir);
    } else if (std::ifstream("BUILD.gn").good()) {
      scanner.ScanFile("BUILD.gn");
    } else {
      std::cerr << "gor_make: No BUILD.gn file found.\n";
      return 1;
    }
    if (gnJsonOutput) {
      scanner.OutputJson();
    } else {
      return scanner.BuildAll();
    }
    return 0;
  }

  if (cmakeMode) {
    gormake::CmakeScanner scanner;
    scanner.SetDryRun(dryRun);
    scanner.SetJobs(jobs);
    if (!cmakeFile.empty()) {
      scanner.ScanFile(cmakeFile);
    } else if (!cmakeDir.empty()) {
      scanner.ScanDirectory(cmakeDir);
    } else if (std::ifstream("CMakeLists.txt").good()) {
      scanner.ScanFile("CMakeLists.txt");
    } else {
      std::cerr << "gor_make: No CMakeLists.txt file found.\n";
      return 1;
    }
    if (cmakeJsonOutput) {
      scanner.OutputJson();
    } else {
      return scanner.BuildAll();
    }
    return 0;
  }

  if (sconsMode) {
    gormake::SconScanner scanner;
    scanner.SetDryRun(dryRun);
    scanner.SetJobs(jobs);
    if (!sconsFile.empty()) {
      scanner.ScanFile(sconsFile);
    } else if (!sconsDir.empty()) {
      scanner.ScanDirectory(sconsDir);
    } else if (std::ifstream("SConstruct").good()) {
      scanner.ScanFile("SConstruct");
    } else if (std::ifstream("SConscript").good()) {
      scanner.ScanFile("SConscript");
    } else {
      std::cerr << "gor_make: No SConstruct/SConscript file found.\n";
      return 1;
    }
    if (sconsJsonOutput) {
      scanner.OutputJson();
    } else {
      return scanner.BuildAll();
    }
    return 0;
  }

  if (bpMode) {
    // Check if Android.bp exists
    if (bpOpts.bpFilePath == "Android.bp" && !std::ifstream("Android.bp").good()) {
      std::cerr << "gor_make: No Android.bp file found.\n";
      return 1;
    }
    gormake::BpEngine bpEngine;
    int ret = bpEngine.Run(bpOpts);
    if (ret != 0) {
      std::cerr << "gor_make: *** Build failed.\n";
    }
    return ret;
  }

  gormake::Engine engine;
  int ret = engine.Run(opts);

  if (ret != 0) {
    std::cerr << "gor_make: *** [" << ret << "] Error\n";
  }

  return ret;
}
