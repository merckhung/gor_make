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

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "bpengine.h"
#include "bpparser.h"
#include "cmakescanner.h"
#include "engine.h"
#include "gnscanner.h"
#include "mkscanner.h"
#include "sconscanner.h"

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

// Counter for pass/fail across all tests.
static int g_pass = 0;
static int g_fail = 0;

// Write |content| to |path|.  Returns true on success.
static bool WriteFile(const std::string& path, const std::string& content) {
  std::ofstream ofs(path);
  if (!ofs) {
    return false;
  }
  ofs << content;
  return ofs.good();
}

// Create a temporary directory using mkdtemp.  Returns the path (with trailing
// slash) on success, or an empty string on failure.
static std::string MakeTempDir() {
  char tmpl[] = "/tmp/gormake_test_XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (dir == nullptr) {
    std::cerr << "mkdtemp failed: " << strerror(errno) << "\n";
    return "";
  }
  return std::string(dir) + "/";
}

// Remove a directory tree recursively (best effort).
static void RemoveDir(const std::string& path) {
  std::string cmd = "rm -rf '" + path + "'";
  // Intentionally not checking the return value; this is best-effort cleanup.
  int rc = system(cmd.c_str());
  (void)rc;
}

// Simple JSON validator: checks that the output captured from a scanner's
// OutputJson() call is non-empty and has balanced braces/brackets.
// Returns true if the output looks like valid JSON.
static bool ValidateJson(const std::string& json) {
  if (json.empty()) return false;

  int braces = 0;
  int brackets = 0;
  bool inString = false;
  bool escape = false;

  for (size_t i = 0; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      inString = !inString;
      continue;
    }
    if (inString) continue;

    if (c == '{') braces++;
    else if (c == '}') braces--;
    else if (c == '[') brackets++;
    else if (c == ']') brackets--;
  }

  // Braces/brackets must be balanced and we must have exited any string.
  return braces == 0 && brackets == 0 && !inString;
}

// Capture stdout produced by |fn| (e.g., OutputJson).
static std::string CaptureStdout(void (*fn)(void*), void* ctx) {
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  if (saved < 0) return "";

  char tmpl[] = "/tmp/gormake_capture_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    close(saved);
    return "";
  }

  dup2(fd, STDOUT_FILENO);
  close(fd);

  fn(ctx);

  fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);

  std::ifstream ifs(tmpl);
  std::string content((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
  unlink(tmpl);
  return content;
}

// Wrappers for CaptureStdout to call OutputJson on each scanner type.
static void CallBpOutputJson(void* ctx) {
  static_cast<gormake::BpEngine*>(ctx)->OutputJson();
}
static void CallMkOutputJson(void* ctx) {
  static_cast<gormake::MkScanner*>(ctx)->OutputJson();
}
static void CallGnOutputJson(void* ctx) {
  static_cast<gormake::GnScanner*>(ctx)->OutputJson();
}
static void CallCmakeOutputJson(void* ctx) {
  static_cast<gormake::CmakeScanner*>(ctx)->OutputJson();
}
static void CallSconOutputJson(void* ctx) {
  static_cast<gormake::SconScanner*>(ctx)->OutputJson();
}
static void CallEngineOutputJson(void* ctx) {
  static_cast<gormake::Engine*>(ctx)->OutputJson();
}

// Helper to report a test result.
static void ReportResult(const std::string& name, bool ok) {
  if (ok) {
    std::cout << "[  PASS  ] " << name << "\n";
    g_pass++;
  } else {
    std::cout << "[  FAIL  ] " << name << "\n";
    g_fail++;
  }
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

// test_bp: Create Android.bp with cc_binary + cc_library_static, scan,
// verify 2 modules.
static void TestBp() {
  std::string tmpdir = MakeTempDir();
  if (tmpdir.empty()) {
    ReportResult("test_bp", false);
    return;
  }

  std::string bpPath = tmpdir + "Android.bp";
  std::string content =
      "cc_binary {\n"
      "    name: \"hello\",\n"
      "    srcs: [\"main.c\"],\n"
      "}\n"
      "\n"
      "cc_library_static {\n"
      "    name: \"libutil\",\n"
      "    srcs: [\"util.c\"],\n"
      "}\n";

  if (!WriteFile(bpPath, content)) {
    ReportResult("test_bp", false);
    RemoveDir(tmpdir);
    return;
  }

  gormake::BpParser parser;
  gormake::BpFile result;
  bool ok = parser.ParseFile(bpPath, &result);

  bool pass = ok && result.modules.size() == 2;
  if (pass) {
    // Verify module types.
    bool foundBinary = false;
    bool foundStatic = false;
    for (const auto& m : result.modules) {
      if (m.type == "cc_binary" && m.name == "hello") foundBinary = true;
      if (m.type == "cc_library_static" && m.name == "libutil")
        foundStatic = true;
    }
    pass = foundBinary && foundStatic;
  }

  ReportResult("test_bp", pass);

  // Test JSON output through the BpEngine (which uses BpParser internally).
  if (pass) {
    gormake::BpBuildOptions opts;
    opts.bpFilePath = bpPath;
    opts.jsonOutput = true;
    gormake::BpEngine engine;
    engine.Run(opts);
    // Capture and validate JSON
    gormake::BpEngine engine2;
    engine2.Run(opts);
    std::string json =
        CaptureStdout(CallBpOutputJson, &engine2);
    bool jsonOk = ValidateJson(json);
    ReportResult("test_bp_json", jsonOk);
  }

  RemoveDir(tmpdir);
}

// test_mk: Create Android.mk with BUILD_STATIC_LIBRARY + BUILD_EXECUTABLE,
// scan, verify 2 modules.
static void TestMk() {
  std::string tmpdir = MakeTempDir();
  if (tmpdir.empty()) {
    ReportResult("test_mk", false);
    return;
  }

  std::string mkPath = tmpdir + "Android.mk";
  std::string content =
      "LOCAL_PATH := $(call my-dir)\n"
      "\n"
      "include $(CLEAR_VARS)\n"
      "LOCAL_MODULE := libmath\n"
      "LOCAL_SRC_FILES := src/math.c\n"
      "include $(BUILD_STATIC_LIBRARY)\n"
      "\n"
      "include $(CLEAR_VARS)\n"
      "LOCAL_MODULE := calculator\n"
      "LOCAL_SRC_FILES := src/main.c\n"
      "LOCAL_STATIC_LIBRARIES := libmath\n"
      "include $(BUILD_EXECUTABLE)\n";

  if (!WriteFile(mkPath, content)) {
    ReportResult("test_mk", false);
    RemoveDir(tmpdir);
    return;
  }

  gormake::MkScanner scanner;
  bool ok = scanner.ScanFile(mkPath);
  const auto& modules = scanner.GetModules();

  bool pass = ok && modules.size() == 2;
  if (pass) {
    bool foundStatic = false;
    bool foundExec = false;
    for (const auto& m : modules) {
      if (m.name == "libmath" && m.buildType == "BUILD_STATIC_LIBRARY")
        foundStatic = true;
      if (m.name == "calculator" && m.buildType == "BUILD_EXECUTABLE")
        foundExec = true;
    }
    pass = foundStatic && foundExec;
  }

  ReportResult("test_mk", pass);

  if (pass) {
    std::string json = CaptureStdout(CallMkOutputJson, &scanner);
    bool jsonOk = ValidateJson(json);
    ReportResult("test_mk_json", jsonOk);
  }

  RemoveDir(tmpdir);
}

// test_gn: Create BUILD.gn with static_library + executable, scan,
// verify targets.
static void TestGn() {
  std::string tmpdir = MakeTempDir();
  if (tmpdir.empty()) {
    ReportResult("test_gn", false);
    return;
  }

  std::string gnPath = tmpdir + "BUILD.gn";
  std::string content =
      "static_library(\"libmath\") {\n"
      "  sources = [\"src/math.cc\"]\n"
      "}\n"
      "\n"
      "executable(\"calculator\") {\n"
      "  sources = [\"src/main.cc\"]\n"
      "  deps = [\":libmath\"]\n"
      "  cflags = [\"-Wall\", \"-O2\"]\n"
      "}\n";

  if (!WriteFile(gnPath, content)) {
    ReportResult("test_gn", false);
    RemoveDir(tmpdir);
    return;
  }

  gormake::GnScanner scanner;
  bool ok = scanner.ScanFile(gnPath);
  const auto& targets = scanner.GetTargets();

  bool pass = ok && targets.size() == 2;
  if (pass) {
    bool foundStatic = false;
    bool foundExec = false;
    for (const auto& t : targets) {
      if (t.name == "libmath" && t.type == "static_library") foundStatic = true;
      if (t.name == "calculator" && t.type == "executable") foundExec = true;
    }
    pass = foundStatic && foundExec;
  }

  ReportResult("test_gn", pass);

  if (pass) {
    std::string json = CaptureStdout(CallGnOutputJson, &scanner);
    bool jsonOk = ValidateJson(json);
    ReportResult("test_gn_json", jsonOk);
  }

  RemoveDir(tmpdir);
}

// test_cmake: Create CMakeLists.txt with add_library + add_executable, scan,
// verify targets.
static void TestCmake() {
  std::string tmpdir = MakeTempDir();
  if (tmpdir.empty()) {
    ReportResult("test_cmake", false);
    return;
  }

  std::string cmakePath = tmpdir + "CMakeLists.txt";
  std::string content =
      "cmake_minimum_required(VERSION 3.10)\n"
      "project(Calculator C CXX)\n"
      "\n"
      "add_library(math STATIC src/math.c)\n"
      "target_include_directories(math PUBLIC src)\n"
      "\n"
      "add_executable(calculator src/main.c)\n"
      "target_link_libraries(calculator PRIVATE math)\n";

  if (!WriteFile(cmakePath, content)) {
    ReportResult("test_cmake", false);
    RemoveDir(tmpdir);
    return;
  }

  gormake::CmakeScanner scanner;
  bool ok = scanner.ScanFile(cmakePath);
  const auto& targets = scanner.GetTargets();

  bool pass = ok && targets.size() == 2;
  if (pass) {
    bool foundLib = false;
    bool foundExec = false;
    for (const auto& t : targets) {
      if (t.name == "math" && t.type == "static_library") foundLib = true;
      if (t.name == "calculator" && t.type == "executable") foundExec = true;
    }
    pass = foundLib && foundExec;
  }

  ReportResult("test_cmake", pass);

  if (pass) {
    std::string json = CaptureStdout(CallCmakeOutputJson, &scanner);
    bool jsonOk = ValidateJson(json);
    ReportResult("test_cmake_json", jsonOk);
  }

  RemoveDir(tmpdir);
}

// test_scons: Create SConstruct with Library + Program, scan,
// verify targets.
static void TestScons() {
  std::string tmpdir = MakeTempDir();
  if (tmpdir.empty()) {
    ReportResult("test_scons", false);
    return;
  }

  std::string sconPath = tmpdir + "SConstruct";
  std::string content =
      "# Simple SCons build file\n"
      "env = Environment()\n"
      "env.Append(CPPPATH=['src'])\n"
      "env.Append(CCFLAGS=['-Wall', '-O2'])\n"
      "\n"
      "# Build static library\n"
      "env.StaticLibrary('math', ['src/math.cc'])\n"
      "\n"
      "# Build executable\n"
      "env.Program('calculator', ['src/main.cc'], LIBS=['math'])\n";

  if (!WriteFile(sconPath, content)) {
    ReportResult("test_scons", false);
    RemoveDir(tmpdir);
    return;
  }

  gormake::SconScanner scanner;
  bool ok = scanner.ScanFile(sconPath);
  const auto& targets = scanner.GetTargets();

  bool pass = ok && targets.size() == 2;
  if (pass) {
    bool foundLib = false;
    bool foundExec = false;
    for (const auto& t : targets) {
      if (t.name == "math" && t.type == "library") foundLib = true;
      if (t.name == "calculator" && t.type == "program") foundExec = true;
    }
    pass = foundLib && foundExec;
  }

  ReportResult("test_scons", pass);

  if (pass) {
    std::string json = CaptureStdout(CallSconOutputJson, &scanner);
    bool jsonOk = ValidateJson(json);
    ReportResult("test_scons_json", jsonOk);
  }

  RemoveDir(tmpdir);
}

// test_makefile: Create Makefile with a simple rule, run engine,
// verify target.
static void TestMakefile() {
  std::string tmpdir = MakeTempDir();
  if (tmpdir.empty()) {
    ReportResult("test_makefile", false);
    return;
  }

  // Create a Makefile that builds a simple target using touch.
  std::string mkPath = tmpdir + "Makefile";
  std::string builtFile = tmpdir + "built.txt";
  std::string content =
      "all: mytarget\n"
      "\n"
      "mytarget:\n"
      "\ttouch " + builtFile + "\n";

  if (!WriteFile(mkPath, content)) {
    ReportResult("test_makefile", false);
    RemoveDir(tmpdir);
    return;
  }

  gormake::MakeOptions opts;
  opts.makefilePath = mkPath;
  opts.directory = tmpdir;
  opts.goals.push_back("mytarget");

  gormake::Engine engine;
  int ret = engine.Run(opts);

  bool pass = (ret == 0);

  // Check that the built file exists.
  if (pass) {
    struct stat st;
    pass = (stat(builtFile.c_str(), &st) == 0);
  }

  ReportResult("test_makefile", pass);

  if (pass) {
    std::string json = CaptureStdout(CallEngineOutputJson, &engine);
    bool jsonOk = ValidateJson(json);
    ReportResult("test_makefile_json", jsonOk);
  }

  RemoveDir(tmpdir);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  std::cout << "========================================\n";
  std::cout << "  GORMAKE Scanner Test Suite\n";
  std::cout << "========================================\n\n";

  TestBp();
  TestMk();
  TestGn();
  TestCmake();
  TestScons();
  TestMakefile();

  std::cout << "\n========================================\n";
  std::cout << "  Results: " << g_pass << " passed, " << g_fail
            << " failed\n";
  std::cout << "========================================\n";

  return g_fail > 0 ? 1 : 0;
}
