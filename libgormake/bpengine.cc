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

#include "bpengine.h"
#include "buildenginebase.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gormake {

namespace fs = std::filesystem;

// Helper: get file modification time
static long GetMtime(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return st.st_mtime;
}

// Helper: split string by whitespace
static std::vector<std::string> SplitWords(const std::string& s) {
  std::vector<std::string> result;
  std::string current;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      if (!current.empty()) { result.push_back(current); current.clear(); }
    } else {
      current += c;
    }
  }
  if (!current.empty()) result.push_back(current);
  return result;
}

// Helper: join strings with separator
static std::string Join(const std::vector<std::string>& v, const std::string& sep) {
  std::string result;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i > 0) result += sep;
    result += v[i];
  }
  return result;
}

// Helper: replace extension
static std::string ReplaceExt(const std::string& path, const std::string& newExt) {
  size_t dot = path.find_last_of('.');
  std::string base = (dot == std::string::npos) ? path : path.substr(0, dot);
  return base + newExt;
}

// Helper: check if a file is an assembly source (.S, .s)
static bool IsAsmSource(const std::string& path) {
  if (path.size() > 2 && path.substr(path.size() - 2) == ".S") return true;
  if (path.size() > 2 && path.substr(path.size() - 2) == ".s") return true;
  return false;
}

// BpEngine implementation ------------------------------------------------

BpEngine::BpEngine() {
  // Detect available compilers
  if (buildutil::FileExists("/usr/bin/gcc")) { cc_ = "gcc"; cxx_ = "g++"; }
  else if (buildutil::FileExists("/usr/bin/clang")) { cc_ = "clang"; cxx_ = "clang++"; }
  ar_ = "ar";

  // commonCflags_ = {"-Wall", "-Werror=no-unused-parameter"};
  // Note: -Werror=no-unused-parameter is not standard; use -Wno-unused-parameter instead
  commonCflags_ = {"-Wall"};
}

BpEngine::~BpEngine() {
}

int BpEngine::Run(const BpBuildOptions& opts) {
  opts_ = &opts;

  // Handle clean
  if (opts.clean) {
    Clean();
    return 0;
  }

  // Parse Android.bp files
  std::string bpPath = opts.bpFilePath;
  if (!buildutil::FileExists(bpPath)) {
    // Check if it's a directory
    struct stat st;
    if (stat(bpPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      // It's a directory, look for Android.bp inside it
      std::string dirPath = bpPath;
      if (!dirPath.empty() && dirPath.back() == '/') dirPath.pop_back();
      if (buildutil::FileExists(dirPath + "/Android.bp")) {
        bpPath = dirPath + "/Android.bp";
      } else {
        // Walk the directory for Android.bp files
        ParseBpDirectory(dirPath);
        bpPath = "";  // Already parsed
      }
    } else if (buildutil::FileExists("Android.bp")) {
      bpPath = "Android.bp";
    } else {
      fprintf(stderr, "gor_make: *** No Android.bp file found.\n");
      return 1;
    }
  }

  if (!bpPath.empty()) {
    if (!ParseBpFiles(bpPath)) {
      fprintf(stderr, "gor_make: *** Failed to parse Android.bp: %s\n",
              bpPath.c_str());
      return 1;
    }
  }

  if (modules_.empty()) {
    fprintf(stderr, "gor_make: *** No modules found in Android.bp.\n");
    return 1;
  }

  // Apply defaults
  ApplyDefaults();

  // JSON output mode: print module relationships and exit
  if (opts.jsonOutput) {
    OutputJson();
    return 0;
  }

  // Determine goals
  std::vector<std::string> goals = opts.goals;
  if (goals.empty()) {
    // Build all cc_binary modules by default
    for (const auto& [name, mod] : modules_) {
      if (mod->isBinary) {
        goals.push_back(name);
      }
    }
    if (goals.empty()) {
      // If no binaries, build all modules
      for (const auto& [name, mod] : modules_) {
        goals.push_back(name);
      }
    }
  }

  // Build each goal
  int result = 0;
  for (const auto& goal : goals) {
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> building;
    if (!BuildModule(goal, visited, building)) {
      result = 1;
      if (!opts.keepGoing) break;
    }
  }

  if (result == 0 && !goals.empty()) {
    fprintf(stderr, "Build completed successfully.\n");
  }

  return result;
}

bool BpEngine::ParseBpFiles(const std::string& rootPath) {
  if (!ParseSingleBp(rootPath)) {
    return false;
  }

  // Parse Android.bp files in subdirectories
  fs::path rootDir = fs::path(rootPath).parent_path();
  if (rootDir.empty()) rootDir = ".";
  std::string rootAbs = fs::canonical(rootDir).string();

  for (auto& entry : fs::recursive_directory_iterator(rootDir)) {
    if (entry.path().filename() == "Android.bp") {
      std::string path = entry.path().string();
      // Skip the root file (already parsed)
      std::string entryAbs = fs::canonical(entry.path()).string();
      if (entryAbs == rootAbs) continue;
      // Skip common output/build directories
      std::string entryStr = entry.path().string();
      if (entryStr.find("/out/") != std::string::npos) continue;
      if (entryStr.find("/bazel-") != std::string::npos) continue;
      if (entryStr.find("/.git/") != std::string::npos) continue;
      try {
        ParseSingleBp(path);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "gor_make: [warning] error parsing %s: %s\n",
                     path.c_str(), e.what());
      } catch (...) {
        std::fprintf(stderr, "gor_make: [warning] unknown error parsing %s\n",
                     path.c_str());
      }
    }
  }

  return true;
}

void BpEngine::ParseBpDirectory(const std::string& dirPath) {
  // Walk the directory tree and parse all Android.bp files
  for (auto& entry : fs::recursive_directory_iterator(dirPath)) {
    if (entry.path().filename() == "Android.bp") {
      std::string entryStr = entry.path().string();
      // Skip common output/build directories
      if (entryStr.find("/out/") != std::string::npos) continue;
      if (entryStr.find("/bazel-") != std::string::npos) continue;
      if (entryStr.find("/.git/") != std::string::npos) continue;
      try {
        ParseSingleBp(entryStr);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "gor_make: [warning] error parsing %s: %s\n",
                     entryStr.c_str(), e.what());
      } catch (...) {
        std::fprintf(stderr, "gor_make: [warning] unknown error parsing %s\n",
                     entryStr.c_str());
      }
    }
  }
}

bool BpEngine::ParseSingleBp(const std::string& path) {
  // Avoid parsing the same file twice
  std::string absPath = fs::canonical(path).string();
  if (parsedFiles_.count(absPath) > 0) return true;
  parsedFiles_.insert(absPath);

  BpParser parser;
  BpFile result;
  if (!parser.ParseFile(path, &result)) {
    fprintf(stderr, "gor_make: Parse error in %s: %s\n",
            path.c_str(), parser.GetError().c_str());
    return false;
  }

  std::string srcDir = fs::path(path).parent_path().string();
  if (srcDir.empty()) srcDir = ".";

  for (const auto& bpMod : result.modules) {
    auto mod = ConvertModule(bpMod, srcDir);
    if (mod) {
      const std::string& name = mod->name;
      if (modules_.count(name) > 0) {
        fprintf(stderr, "gor_make: Warning: duplicate module name '%s'\n",
                name.c_str());
        continue;
      }
      modules_[name] = std::move(mod);
    }
  }

  bpFiles_.push_back(std::move(result));
  return true;
}

std::unique_ptr<BpBuildModule> BpEngine::ConvertModule(
    const BpModule& bpModule, const std::string& srcDir) {
  auto mod = std::make_unique<BpBuildModule>();
  mod->type = bpModule.type;
  mod->srcDir = srcDir;

  // Get name
  auto nameIt = bpModule.properties.find("name");
  if (nameIt == bpModule.properties.end() || !nameIt->second.IsString()) {
    // Silently skip non-buildable module types that don't have names
    // (package, license, license_kind, etc.)
    return nullptr;
  }
  mod->name = nameIt->second.AsString();

  // Set module type flags based on type
  const std::string& t = bpModule.type;
  if (t == "cc_binary" || t == "cc_test_binary") {
    mod->isBinary = true;
    if (t == "cc_test_binary") mod->isTest = true;
  } else if (t == "cc_library" || t == "cc_library_shared") {
    mod->isShared = true;
  } else if (t == "cc_library_static") {
    mod->isStatic = true;
  } else if (t == "cc_test") {
    mod->isTest = true;
    mod->isBinary = true;
  } else if (t == "cc_benchmark") {
    mod->isBinary = true;
  } else if (t == "genrule") {
    // Handle genrule
  } else if (t == "cc_defaults" || t == "filegroup" ||
             t == "package" || t == "license" || t == "license_kind" ||
             t == "android_app" || t == "android_test" ||
             t == "java_library" || t == "java_test" ||
             t == "prebuilt_etc" || t == "prebuilt_build_header" ||
             t == "cc_prebuilt_library_shared" ||
             t == "cc_prebuilt_library_static" ||
             t == "cc_prebuilt_binary" ||
             t == "prebuilt_font" || t == "sh_binary" ||
             t == "sh_test" || t == "python_test" ||
             t == "python_binary_host" || t == "java_binary" ||
             t == "java_library_host" || t == "java_test_host" ||
             t == "cc_library_host" || t == "cc_binary_host" ||
             t == "cc_test_host" || t == "cc_benchmark_host" ||
             t == "soong_config_module_type" || t == "soong_config_string_variable" ||
             t == "soong_config_bool_variable") {
    // These are handled separately or not build targets for our engine
    return nullptr;
  }
  // For any other unknown type, keep the module but don't set flags

  // Extract properties
  auto it = bpModule.properties.find("srcs");
  if (it != bpModule.properties.end()) {
    mod->srcs = ResolveSrcs(it->second, srcDir);
  }

  it = bpModule.properties.find("shared_libs");
  if (it != bpModule.properties.end()) {
    mod->sharedLibs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("static_libs");
  if (it != bpModule.properties.end()) {
    mod->staticLibs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("whole_static_libs");
  if (it != bpModule.properties.end()) {
    mod->wholeStaticLibs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("header_libs");
  if (it != bpModule.properties.end()) {
    mod->headerLibs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("cflags");
  if (it != bpModule.properties.end()) {
    mod->cflags = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("cppflags");
  if (it != bpModule.properties.end()) {
    mod->cppflags = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("ldflags");
  if (it != bpModule.properties.end()) {
    mod->ldflags = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("include_dirs");
  if (it != bpModule.properties.end()) {
    mod->includeDirs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("local_include_dirs");
  if (it != bpModule.properties.end()) {
    mod->localIncludeDirs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("export_include_dirs");
  if (it != bpModule.properties.end()) {
    mod->exportIncludeDirs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("system_shared_libs");
  if (it != bpModule.properties.end()) {
    mod->systemSharedLibs = GetStringList(it->second, srcDir);
  }

  it = bpModule.properties.find("stl");
  if (it != bpModule.properties.end() && it->second.IsString()) {
    mod->stl = it->second.AsString();
  }

  // Genrule properties
  it = bpModule.properties.find("cmd");
  if (it != bpModule.properties.end() && it->second.IsString()) {
    mod->genCmd = it->second.AsString();
  }

  it = bpModule.properties.find("out");
  if (it != bpModule.properties.end()) {
    mod->genOut = GetStringList(it->second, srcDir);
  }

  return mod;
}

std::vector<std::string> BpEngine::GetStringList(const BpValue& val,
                                                  const std::string& srcDir) {
  std::vector<std::string> result;
  if (val.IsString()) {
    result.push_back(val.AsString());
  } else if (val.IsList()) {
    for (const auto& item : val.listVal) {
      if (item.IsString()) {
        result.push_back(item.AsString());
      }
    }
  }
  return result;
}

std::vector<std::string> BpEngine::ResolveSrcs(const BpValue& val,
                                                const std::string& srcDir) {
  std::vector<std::string> result;
  if (val.IsString()) {
    // Single source file
    std::string s = val.AsString();
    if (s.find('*') != std::string::npos) {
      // Glob pattern
      BpParser parser;
      auto expanded = parser.ExpandGlob(s);
      for (const auto& e : expanded) {
        result.push_back(srcDir + "/" + e);
      }
    } else {
      result.push_back(srcDir + "/" + s);
    }
  } else if (val.IsList()) {
    for (const auto& item : val.listVal) {
      if (item.IsString()) {
        std::string s = item.AsString();
        if (s.find('*') != std::string::npos) {
          BpParser parser;
          auto expanded = parser.ExpandGlob(s);
          for (const auto& e : expanded) {
            result.push_back(srcDir + "/" + e);
          }
        } else {
          result.push_back(srcDir + "/" + s);
        }
      }
    }
  }
  return result;
}

void BpEngine::ApplyDefaults() {
  // Build a map of defaults name -> BpModule*
  std::map<std::string, const BpModule*> defaultsMap;
  for (const auto& bpFile : bpFiles_) {
    for (const auto& mod : bpFile.modules) {
      if (mod.type == "cc_defaults") {
        defaultsMap[mod.name] = &mod;
      }
    }
  }

  // Apply defaults to each build module
  for (const auto& [name, mod] : modules_) {
    // Find the original BpModule to get its defaults list
    for (const auto& bpFile : bpFiles_) {
      for (const auto& bpMod : bpFile.modules) {
        if (bpMod.name == name) {
          auto defaultsIt = bpMod.properties.find("defaults");
          if (defaultsIt != bpMod.properties.end() && defaultsIt->second.IsList()) {
            for (const auto& d : defaultsIt->second.listVal) {
              if (d.IsString()) {
                auto dit = defaultsMap.find(d.AsString());
                if (dit != defaultsMap.end()) {
                  ApplyDefaultsToModule(*dit->second, mod.get());
                }
              }
            }
          }
        }
      }
    }
  }
}

void BpEngine::ApplyDefaultsToModule(const BpModule& defaults,
                                      BpBuildModule* module) {
  // Apply defaults properties, but only for properties that aren't already set
  // in the module (module-specific values take precedence).
  auto applyList = [&](const char* prop, std::vector<std::string>* target) {
    if (!target->empty()) return;  // Already set by module
    auto it = defaults.properties.find(prop);
    if (it != defaults.properties.end()) {
      *target = GetStringList(it->second, module->srcDir);
    }
  };

  applyList("cflags", &module->cflags);
  applyList("cppflags", &module->cppflags);
  applyList("ldflags", &module->ldflags);
  applyList("include_dirs", &module->includeDirs);
  applyList("local_include_dirs", &module->localIncludeDirs);
  applyList("export_include_dirs", &module->exportIncludeDirs);
  applyList("system_shared_libs", &module->systemSharedLibs);
  
  // For srcs, shared_libs, static_libs — always prepend defaults
  // (these are additive in Blueprint semantics)
  auto prependList = [&](const char* prop, std::vector<std::string>* target) {
    auto it = defaults.properties.find(prop);
    if (it != defaults.properties.end()) {
      auto defaultsList = GetStringList(it->second, module->srcDir);
      std::vector<std::string> combined = defaultsList;
      combined.insert(combined.end(), target->begin(), target->end());
      *target = combined;
    }
  };
  prependList("srcs", &module->srcs);
  prependList("shared_libs", &module->sharedLibs);
  prependList("static_libs", &module->staticLibs);
  prependList("whole_static_libs", &module->wholeStaticLibs);
  prependList("header_libs", &module->headerLibs);
}

BpBuildModule* BpEngine::FindModule(const std::string& name) {
  auto it = modules_.find(name);
  if (it != modules_.end()) {
    return it->second.get();
  }
  return nullptr;
}

bool BpEngine::BuildModule(const std::string& name,
                            std::unordered_set<std::string>& visited,
                            std::unordered_set<std::string>& building) {
  // Cycle detection
  if (building.count(name) > 0) {
    fprintf(stderr, "gor_make: Circular dependency detected for '%s'.\n",
            name.c_str());
    return false;
  }

  if (visited.count(name) > 0) return true;

  BpBuildModule* mod = FindModule(name);
  if (!mod) {
    // Might be a system library
    struct stat st;
    std::string libPath = "lib" + name + ".so";
    if (stat(("/usr/lib/" + libPath).c_str(), &st) == 0 ||
        stat(("/usr/lib/x86_64-linux-gnu/" + libPath).c_str(), &st) == 0 ||
        stat(("/lib/x86_64-linux-gnu/" + libPath).c_str(), &st) == 0) {
      visited.insert(name);
      return true;
    }
    fprintf(stderr, "gor_make: *** No module named '%s'.\n", name.c_str());
    return false;
  }

  building.insert(name);

  // Build static library dependencies first
  for (const auto& dep : mod->staticLibs) {
    if (!BuildModule(dep, visited, building)) {
      building.erase(name);
      if (!opts_->keepGoing) return false;
    }
  }

  // Build whole static library dependencies
  for (const auto& dep : mod->wholeStaticLibs) {
    if (!BuildModule(dep, visited, building)) {
      building.erase(name);
      if (!opts_->keepGoing) return false;
    }
  }

  // Build shared library dependencies first
  for (const auto& dep : mod->sharedLibs) {
    if (!BuildModule(dep, visited, building)) {
      building.erase(name);
      if (!opts_->keepGoing) return false;
    }
  }

  building.erase(name);
  visited.insert(name);

  // Compile the module
  if (mod->type == "genrule") {
    if (!BuildGenrule(mod)) {
      return false;
    }
  } else if (!mod->srcs.empty()) {
    if (!CompileModule(mod)) {
      return false;
    }
    if (!LinkModule(mod)) {
      return false;
    }
  }

  return true;
}

bool BpEngine::CompileModule(BpBuildModule* module) {
  // Create output directory
  std::string objDir = opts_->buildDir + "/obj/" + module->name;
  if (!opts_->dryRun) buildutil::MkdirP(objDir);

  // Collect include directories
  std::vector<std::string> allIncludeDirs = module->includeDirs;
  for (const auto& d : module->localIncludeDirs) {
    allIncludeDirs.push_back(d);
  }

  // Add include dirs from dependency modules (export_include_dirs)
  for (const auto& dep : module->staticLibs) {
    BpBuildModule* depMod = FindModule(dep);
    if (depMod) {
      for (const auto& d : depMod->exportIncludeDirs) {
        allIncludeDirs.push_back(depMod->srcDir + "/" + d);
      }
    }
  }
  for (const auto& dep : module->sharedLibs) {
    BpBuildModule* depMod = FindModule(dep);
    if (depMod) {
      for (const auto& d : depMod->exportIncludeDirs) {
        allIncludeDirs.push_back(depMod->srcDir + "/" + d);
      }
    }
  }

  // Compile each source file
  module->objectFiles.clear();

  for (const auto& src : module->srcs) {
    std::string srcPath = src;
    if (srcPath[0] != '/' && srcPath.substr(0, 2) != "./") {
      srcPath = module->srcDir + "/" + src;
    }

    if (!buildutil::FileExists(srcPath)) {
      fprintf(stderr, "gor_make: *** Source file not found: %s\n", srcPath.c_str());
      return false;
    }

    std::string objPath = GetObjectPath(*module, src);

    // Check if recompilation is needed
    bool needCompile = opts_->dryRun ? false : true;
    if (!needCompile) {
      // In dry-run mode, still show commands
    } else {
      needCompile = !buildutil::FileExists(objPath);
      if (!needCompile) {
        needCompile = GetMtime(srcPath) > GetMtime(objPath);
      }
    }

    if (needCompile || opts_->dryRun) {
      // Choose compiler based on file type
      std::string compiler = buildutil::IsCppSource(srcPath) ? cxx_ : cc_;

      // Build command
      std::string cmd = compiler + " -c";

      // Add cflags
      for (const auto& f : commonCflags_) {
        cmd += " " + f;
      }
      for (const auto& f : module->cflags) {
        cmd += " " + f;
      }
      if (buildutil::IsCppSource(srcPath)) {
        for (const auto& f : module->cppflags) {
          cmd += " " + f;
        }
      }

      // Add include directories
      for (const auto& d : allIncludeDirs) {
        cmd += " -I" + d;
      }

      // Add source and output
      cmd += " -o " + objPath + " " + srcPath;

      if (!ExecuteCmd(cmd, module->isTest && opts_->silent)) {
        fprintf(stderr, "gor_make: *** Compilation failed for %s\n", srcPath.c_str());
        return false;
      }
    }

    module->objectFiles.push_back(objPath);
  }

  return true;
}

// Helper: strip "lib" prefix from library name
static std::string StripLibPrefix(const std::string& name) {
  if (name.size() > 3 && name.substr(0, 3) == "lib") {
    return name.substr(3);
  }
  return name;
}

bool BpEngine::LinkModule(BpBuildModule* module) {
  if (module->objectFiles.empty()) return true;

  std::string outPath = GetOutputPath(*module);
  if (!opts_->dryRun) buildutil::MkdirP(buildutil::DirName(outPath));

  if (module->isStatic) {
    // Create static library with ar
    std::string cmd = ar_ + " rcs " + outPath;
    for (const auto& obj : module->objectFiles) {
      cmd += " " + obj;
    }

    // Check if relink needed
    bool needLink = !buildutil::FileExists(outPath);
    if (!needLink && !opts_->dryRun) {
      for (const auto& obj : module->objectFiles) {
        if (GetMtime(obj) > GetMtime(outPath)) {
          needLink = true;
          break;
        }
      }
    }

    if (needLink || opts_->dryRun) {
      if (!ExecuteCmd(cmd, opts_->silent)) {
        fprintf(stderr, "gor_make: *** Archiving failed for %s\n", outPath.c_str());
        return false;
      }
    }
  } else {
    // Link binary or shared library
    std::string linker = module->isBinary ? cxx_ : cxx_;
    std::string cmd = linker;

    // Add ldflags
    for (const auto& f : module->ldflags) {
      cmd += " " + f;
    }

    // Add object files
    for (const auto& obj : module->objectFiles) {
      cmd += " " + obj;
    }

    // Add static libraries from dependencies
    for (const auto& dep : module->staticLibs) {
      BpBuildModule* depMod = FindModule(dep);
      if (depMod) {
        cmd += " " + GetOutputPath(*depMod);
      }
    }

    // Add whole static libraries
    for (const auto& dep : module->wholeStaticLibs) {
      BpBuildModule* depMod = FindModule(dep);
      if (depMod) {
        cmd += " -Wl,--whole-archive " + GetOutputPath(*depMod) + " -Wl,--no-whole-archive";
      }
    }

    // Add shared library flags
    for (const auto& dep : module->sharedLibs) {
      BpBuildModule* depMod = FindModule(dep);
      if (depMod) {
        // Link against the shared library
        std::string depOut = GetOutputPath(*depMod);
        std::string depDir = buildutil::DirName(depOut);
        cmd += " -L" + depDir + " -l" + StripLibPrefix(depMod->name);
      } else {
        // System library
        cmd += " -l" + StripLibPrefix(dep);
      }
    }

    // Add output path
    if (module->isShared) {
      cmd += " -shared -o " + outPath;
    } else {
      cmd += " -o " + outPath;
    }

    // Check if relink needed
    bool needLink = !buildutil::FileExists(outPath);
    if (!needLink && !opts_->dryRun) {
      for (const auto& obj : module->objectFiles) {
        if (GetMtime(obj) > GetMtime(outPath)) {
          needLink = true;
          break;
        }
      }
      // Also check dependency outputs
      if (!needLink) {
        for (const auto& dep : module->staticLibs) {
          BpBuildModule* depMod = FindModule(dep);
          if (depMod) {
            if (GetMtime(GetOutputPath(*depMod)) > GetMtime(outPath)) {
              needLink = true;
              break;
            }
          }
        }
      }
    }

    if (needLink || opts_->dryRun) {
      if (!ExecuteCmd(cmd, opts_->silent)) {
        fprintf(stderr, "gor_make: *** Linking failed for %s\n", outPath.c_str());
        return false;
      }
    }
  }

  module->outputFile = outPath;
  return true;
}

bool BpEngine::BuildGenrule(BpBuildModule* module) {
  if (module->genCmd.empty()) return true;

  std::string outDir = opts_->buildDir + "/gen/" + module->name;
  if (!opts_->dryRun) buildutil::MkdirP(outDir);

  // Simplified genrule: replace $(in) and $(out)
  std::string cmd = module->genCmd;
  // Replace $(in) with source files
  std::string inFiles = Join(module->srcs, " ");
  size_t pos = cmd.find("$(in)");
  while (pos != std::string::npos) {
    cmd.replace(pos, 5, inFiles);
    pos = cmd.find("$(in)");
  }
  // Replace $(out) with output files
  std::vector<std::string> outPaths;
  for (const auto& out : module->genOut) {
    outPaths.push_back(outDir + "/" + out);
  }
  std::string outFiles = Join(outPaths, " ");
  pos = cmd.find("$(out)");
  while (pos != std::string::npos) {
    cmd.replace(pos, 6, outFiles);
    pos = cmd.find("$(out)");
  }

  if (!ExecuteCmd(cmd, opts_->silent)) {
    fprintf(stderr, "gor_make: *** Genrule failed for %s\n", module->name.c_str());
    return false;
  }

  module->objectFiles = outPaths;
  module->outputFile = outPaths.empty() ? "" : outPaths[0];
  return true;
}

void BpEngine::Clean() {
  if (buildutil::FileExists(opts_->buildDir)) {
    fs::remove_all(opts_->buildDir);
    printf("Cleaned %s\n", opts_->buildDir.c_str());
  }
}

std::string BpEngine::GetOutputPath(const BpBuildModule& module) const {
  std::string fileName = module.name;
  // Only add "lib" prefix if the name doesn't already start with "lib"
  bool needsLibPrefix = !(fileName.size() >= 3 &&
                          fileName.substr(0, 3) == "lib");
  std::string libPrefix = needsLibPrefix ? "lib" : "";

  if (module.isStatic) {
    return opts_->buildDir + "/lib/" + libPrefix + fileName + ".a";
  }
  if (module.isShared) {
    return opts_->buildDir + "/lib/" + libPrefix + fileName + ".so";
  }
  // Binary
  return opts_->buildDir + "/bin/" + fileName;
}

std::string BpEngine::GetObjectPath(const BpBuildModule& module,
                                     const std::string& srcFile) const {
  std::string baseName = buildutil::BaseName(srcFile);
  return opts_->buildDir + "/obj/" + module.name + "/" +
         ReplaceExt(baseName, ".o");
}

bool BpEngine::NeedsRecompile(const std::string& objFile,
                              const std::string& srcFile,
                              const std::vector<std::string>& headers) const {
  if (buildutil::NeedsRecompile(objFile, srcFile)) return true;
  long objMtime = GetMtime(objFile);
  for (const auto& h : headers) {
    if (GetMtime(h) > objMtime) return true;
  }
  return false;
}

bool BpEngine::ExecuteCmd(const std::string& cmd, bool silent) {
  if (!opts_->dryRun || !opts_->silent) {
    if (!silent || opts_->dryRun) {
      printf("%s\n", cmd.c_str());
      fflush(stdout);
    }
  }

  if (opts_->dryRun) {
    return true;  // Don't actually execute
  }

  return buildutil::ExecuteCmd(cmd);
}

// Helper: escape a string for JSON output
static std::string JsonEscape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

// Helper: output a JSON string array
static void OutputJsonArray(FILE* f, const char* indent,
                            const std::vector<std::string>& arr) {
  fprintf(f, "[");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) fprintf(f, ", ");
    fprintf(f, "\"%s\"", JsonEscape(arr[i]).c_str());
  }
  fprintf(f, "]");
}

void BpEngine::OutputJson() const {
  printf("{\n");
  printf("  \"format\": \"android.bp\",\n");
  printf("  \"module_count\": %zu,\n", modules_.size());
  printf("  \"modules\": [\n");

  bool first = true;
  for (const auto& [name, mod] : modules_) {
    if (!first) printf(",\n");
    first = false;

    printf("    {\n");
    printf("      \"name\": \"%s\",\n", JsonEscape(mod->name).c_str());
    printf("      \"type\": \"%s\",\n", JsonEscape(mod->type).c_str());
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(mod->srcDir).c_str());
    printf("      \"is_binary\": %s,\n", mod->isBinary ? "true" : "false");
    printf("      \"is_static\": %s,\n", mod->isStatic ? "true" : "false");
    printf("      \"is_shared\": %s,\n", mod->isShared ? "true" : "false");
    printf("      \"is_test\": %s,\n", mod->isTest ? "true" : "false");

    printf("      \"srcs\": ");
    OutputJsonArray(stdout, "      ", mod->srcs);
    printf(",\n");

    printf("      \"shared_libs\": ");
    OutputJsonArray(stdout, "      ", mod->sharedLibs);
    printf(",\n");

    printf("      \"static_libs\": ");
    OutputJsonArray(stdout, "      ", mod->staticLibs);
    printf(",\n");

    printf("      \"whole_static_libs\": ");
    OutputJsonArray(stdout, "      ", mod->wholeStaticLibs);
    printf(",\n");

    printf("      \"header_libs\": ");
    OutputJsonArray(stdout, "      ", mod->headerLibs);
    printf(",\n");

    printf("      \"cflags\": ");
    OutputJsonArray(stdout, "      ", mod->cflags);
    printf(",\n");

    printf("      \"cppflags\": ");
    OutputJsonArray(stdout, "      ", mod->cppflags);
    printf(",\n");

    printf("      \"ldflags\": ");
    OutputJsonArray(stdout, "      ", mod->ldflags);
    printf(",\n");

    printf("      \"include_dirs\": ");
    OutputJsonArray(stdout, "      ", mod->includeDirs);
    printf(",\n");

    printf("      \"local_include_dirs\": ");
    OutputJsonArray(stdout, "      ", mod->localIncludeDirs);
    printf(",\n");

    printf("      \"export_include_dirs\": ");
    OutputJsonArray(stdout, "      ", mod->exportIncludeDirs);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

}  // namespace gormake
