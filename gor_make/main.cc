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

#include <stdint.h>
#include <iostream>
#include "gormake.h"
#include "lexer.h"
#include "parser.h"
#include "intrp.h"

static void usage() {
std::cerr << \
"Usage: make [options] [target] ... \n" \
"Options:\n" \
"  -b, -m                      Ignored for compatibility.\n" \
"  -B, --always-make           Unconditionally make all targets.\n" \
"  -C DIRECTORY, --directory=DIRECTORY\n" \
"                              Change to DIRECTORY before doing anything.\n" \
"  -d                          Print lots of debugging information.\n" \
"  --debug[=FLAGS]             Print various types of debugging information.\n" \
"  -e, --environment-overrides\n" \
"                              Environment variables override makefiles.\n" \
"  -f FILE, --file=FILE, --makefile=FILE\n" \
"                              Read FILE as a makefile.\n" \
"  -h, --help                  Print this message and exit.\n" \
"  -i, --ignore-errors         Ignore errors from commands.\n" \
"  -I DIRECTORY, --include-dir=DIRECTORY\n" \
"                              Search DIRECTORY for included makefiles.\n" \
"  -j [N], --jobs[=N]          Allow N jobs at once; infinite jobs with no arg.\n" \
"  -k, --keep-going            Keep going when some targets can't be made.\n" \
"  -l [N], --load-average[=N], --max-load[=N]\n" \
"                              Don't start multiple jobs unless load is below N.\n" \
"  -L, --check-symlink-times   Use the latest mtime between symlinks and target.\n" \
"  -n, --just-print, --dry-run, --recon\n" \
"                              Don't actually run any commands; just print them.\n" \
"  -o FILE, --old-file=FILE, --assume-old=FILE\n" \
"                              Consider FILE to be very old and don't remake it.\n" \
"  -p, --print-data-base       Print make's internal database.\n" \
"  -q, --question              Run no commands; exit status says if up to date.\n" \
"  -r, --no-builtin-rules      Disable the built-in implicit rules.\n" \
"  -R, --no-builtin-variables  Disable the built-in variable settings.\n" \
"  -s, --silent, --quiet       Don't echo commands.\n" \
"  -S, --no-keep-going, --stop\n" \
"                              Turns off -k.\n" \
"  -t, --touch                 Touch targets instead of remaking them.\n" \
"  -v, --version               Print the version number of make and exit.\n" \
"  -w, --print-directory       Print the current directory.\n" \
"  --no-print-directory        Turn off -w, even if it was turned on implicitly.\n" \
"  -W FILE, --what-if=FILE, --new-file=FILE, --assume-new=FILE\n" \
"                              Consider FILE to be infinitely new.\n" \
"  --warn-undefined-variables  Warn when an undefined variable is referenced.\n" \
"  -N OPTION, --NeXT-option=OPTION\n" \
"                              Turn on value of NeXT OPTION.\n" \
"\n" \
"  This program built for i386-apple-darwin11.3.0\n" \
"  Report bugs to " GM_AUTHOR_NAME "\n";
}

static void version() {
std::cerr << \
GM_FULLNAME " " GM_REVISION "\n" \
GM_COPYRIGHT_TEXT "\n" \
"\n" \
"This program built for i386-apple-darwin11.3.0\n";
}

int main(int argc, char** argv) {
  std::string defName = "Makefile";
  std::string* filename = nullptr;
  std::unique_ptr<gormake::Lexer> lxr;
  std::unique_ptr<gormake::Parser> psr;
  std::unique_ptr<gormake::Intrp> intrp;
  int ret;

  // Handle input arguments
  if (argc == 1) {
    filename = &defName;
  } else if (argc) {
    filename = new std::string(argv[1]);
  }

  // Create a lexer
  lxr.reset(new gormake::Lexer(*filename));
  if (lxr->IsOpen() == false) {
    std::cerr << "Failed to open makefile: " << *filename << "\n";
    return -1;
  }

  // Create a parser
  psr.reset(new gormake::Parser(lxr.get()));
  if (psr->IsReady() == false) {
    std::cerr << "Failed to create a parser for makefile: " << *filename << "\n";
    return -1;
  }

  // Do parsing
  gormake::Parser::PsrState rst;
  do {
    rst = psr->ParseToAst();
    if (rst == gormake::Parser::PSR_ERROR) {
      std::cerr << "Syntax error in makefile: " << *filename << "\n";
      return -1;
    }
  } while (rst != gormake::Parser::PSR_DONE);
  if (rst == gormake::Parser::PSR_ERROR) {
    std::cerr << "Syntax error in makefile: " << *filename << "\n";
    return -1;
  }

  // Create a interpreter
  intrp.reset(new gormake::Intrp());
  intrp->SetupAsts(&(psr->GetExprAsts()), &(psr->GetReceiptAst()));

  // Execute the target
  ret = intrp->ExecuteMakefile(argc - 1, argv + 1);
  if (ret != 0) {
    std::cerr << "Error " << ret << " \n";
  }

  // Return
  return ret;
  version();
  usage();
}
