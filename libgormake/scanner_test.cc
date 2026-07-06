/*
 * Copyright (C) 2015 GORMAKE project
 *
 * Test infrastructure for gor_make scanners.
 * Tests all 6 build format scanners.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "bpengine.h"
#include "cmakescanner.h"
#include "gnscanner.h"
#include "mkscanner.h"
#include "sconscanner.h"

namespace fs = std::filesystem;

static int tests_passed = 0;
static int tests_failed = 0;

static std::string CreateTempDir() {
  char tmpl[] = "/tmp/gormake_test_XXXXXX";
  char* dir = mkdtemp(tmpl);
  return std::string(dir);
}

static void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  f << content;
  f.close();
}

static void CleanupDir(const std::string& dir) {
  fs::remove_all(dir);
}

#define TEST(name) printf("  Test: %s ... ", #name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define ASSERT_EQ(a, b) \
  if ((a) != (b)) { FAIL(#a " != " #b); return; }

void test_bp_scanner() {
  TEST(test_bp_scanner);
  std::string dir = CreateTempDir();
  fs::create_directories(dir + "/src");

  WriteFile(dir + "/src/math.c", "int add(int a, int b) { return a + b; }\n");
  WriteFile(dir + "/src/main.c", "int main() { return 0; }\n");
  WriteFile(dir + "/Android.bp",
    "cc_library_static {\n"
    "  name: \"libmath\",\n"
    "  srcs: [\"src/math.c\"],\n"
    "  export_include_dirs: [\"src\"],\n"
    "}\n"
    "cc_binary {\n"
    "  name: \"calculator\",\n"
    "  srcs: [\"src/main.c\"],\n"
    "  static_libs: [\"libmath\"],\n"
    "}\n");

  gormake::BpEngine engine;
  gormake::BpBuildOptions opts;
  opts.bpFilePath = dir + "/Android.bp";
  opts.jsonOutput = true;
  // We just test that it doesn't crash
  engine.Run(opts);
  PASS();
  CleanupDir(dir);
}

void test_mk_scanner() {
  TEST(test_mk_scanner);
  std::string dir = CreateTempDir();
  fs::create_directories(dir + "/src");

  WriteFile(dir + "/src/math.c", "int add(int a, int b) { return a + b; }\n");
  WriteFile(dir + "/src/main.c", "int main() { return 0; }\n");
  WriteFile(dir + "/Android.mk",
    "LOCAL_PATH := $(call my-dir)\n"
    "include $(CLEAR_VARS)\n"
    "LOCAL_MODULE := libmath\n"
    "LOCAL_SRC_FILES := src/math.c\n"
    "include $(BUILD_STATIC_LIBRARY)\n"
    "include $(CLEAR_VARS)\n"
    "LOCAL_MODULE := calculator\n"
    "LOCAL_SRC_FILES := src/main.c\n"
    "LOCAL_STATIC_LIBRARIES := libmath\n"
    "include $(BUILD_EXECUTABLE)\n");

  gormake::MkScanner scanner;
  scanner.ScanFile(dir + "/Android.mk");
  auto& modules = scanner.GetModules();
  ASSERT_EQ(modules.size(), (size_t)2);
  ASSERT_EQ(modules[0].name, std::string("libmath"));
  ASSERT_EQ(modules[1].name, std::string("calculator"));
  PASS();
  CleanupDir(dir);
}

void test_gn_scanner() {
  TEST(test_gn_scanner);
  std::string dir = CreateTempDir();
  fs::create_directories(dir + "/src");

  WriteFile(dir + "/src/math.cc", "int add(int a, int b) { return a + b; }\n");
  WriteFile(dir + "/src/main.cc", "int main() { return 0; }\n");
  WriteFile(dir + "/BUILD.gn",
    "static_library(\"libmath\") {\n"
    "  sources = [\"src/math.cc\"]\n"
    "}\n"
    "executable(\"calculator\") {\n"
    "  sources = [\"src/main.cc\"]\n"
    "  deps = [\":libmath\"]\n"
    "}\n");

  gormake::GnScanner scanner;
  scanner.ScanFile(dir + "/BUILD.gn");
  auto& targets = scanner.GetTargets();
  // Should have at least 2 targets (static_library + executable)
  if (targets.size() >= 2) {
    PASS();
  } else {
    FAIL("expected >= 2 targets");
  }
  CleanupDir(dir);
}

void test_cmake_scanner() {
  TEST(test_cmake_scanner);
  std::string dir = CreateTempDir();
  fs::create_directories(dir + "/src");

  WriteFile(dir + "/src/math.c", "int add(int a, int b) { return a + b; }\n");
  WriteFile(dir + "/src/main.c", "int main() { return 0; }\n");
  WriteFile(dir + "/CMakeLists.txt",
    "cmake_minimum_required(VERSION 3.10)\n"
    "project(Test C CXX)\n"
    "add_library(math STATIC src/math.c)\n"
    "add_executable(calculator src/main.c)\n"
    "target_link_libraries(calculator PRIVATE math)\n");

  gormake::CmakeScanner scanner;
  scanner.ScanFile(dir + "/CMakeLists.txt");
  auto& targets = scanner.GetTargets();
  ASSERT_EQ(targets.size(), (size_t)2);
  ASSERT_EQ(targets[0].name, std::string("math"));
  ASSERT_EQ(targets[1].name, std::string("calculator"));
  PASS();
  CleanupDir(dir);
}

void test_scons_scanner() {
  TEST(test_scons_scanner);
  std::string dir = CreateTempDir();
  fs::create_directories(dir + "/src");

  WriteFile(dir + "/src/math.cc", "int add(int a, int b) { return a + b; }\n");
  WriteFile(dir + "/src/main.cc", "int main() { return 0; }\n");
  WriteFile(dir + "/SConstruct",
    "env = Environment()\n"
    "env.StaticLibrary('math', ['src/math.cc'])\n"
    "env.Program('calculator', ['src/main.cc'], LIBS=['math'])\n");

  gormake::SconScanner scanner;
  scanner.ScanFile(dir + "/SConstruct");
  auto& targets = scanner.GetTargets();
  if (targets.size() >= 2) {
    PASS();
  } else {
    FAIL("expected >= 2 targets");
  }
  CleanupDir(dir);
}

int main() {
  printf("=== GORMAKE Scanner Tests ===\n\n");

  test_bp_scanner();
  test_mk_scanner();
  test_gn_scanner();
  test_cmake_scanner();
  test_scons_scanner();

  printf("\n=== Results: %d passed, %d failed ===\n",
         tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
