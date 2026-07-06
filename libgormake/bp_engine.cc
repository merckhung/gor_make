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

#include "bp_engine.h"
#include "build_engine_base.h"

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
static std::string ReplaceExt(const std::string& path, const std::string& new_ext) {
  size_t dot = path.find_last_of('.');
  std::string base = (dot == std::string::npos) ? path : path.substr(0, dot);
  return base + new_ext;
}

// BpEngine implementation ------------------------------------------------

BpEngine::BpEngine() {
  // Detect available compilers
  if (buildutil::FileExists("/usr/bin/gcc")) { cc_ = "gcc"; cxx_ = "g++"; }
  else if (buildutil::FileExists("/usr/bin/clang")) { cc_ = "clang"; cxx_ = "clang++"; }
  ar_ = "ar";

  // common_cflags_ = {"-Wall", "-Werror=no-unused-parameter"};
  // Note: -Werror=no-unused-parameter is not standard; use -Wno-unused-parameter instead
  common_cflags_ = {"-Wall"};
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
  std::string bp_path = opts.bp_file_path;
  if (!buildutil::FileExists(bp_path)) {
    // Check if it's a directory
    struct stat st;
    if (stat(bp_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      // It's a directory, look for Android.bp inside it
      std::string dir_path = bp_path;
      if (!dir_path.empty() && dir_path.back() == '/') dir_path.pop_back();
      if (buildutil::FileExists(dir_path + "/Android.bp")) {
        bp_path = dir_path + "/Android.bp";
      } else {
        // Walk the directory for Android.bp files
        ParseBpDirectory(dir_path);
        bp_path = "";  // Already parsed
      }
    } else if (buildutil::FileExists("Android.bp")) {
      bp_path = "Android.bp";
    } else {
      fprintf(stderr, "gor_make: *** No Android.bp file found.\n");
      return 1;
    }
  }

  if (!bp_path.empty()) {
    if (!ParseBpFiles(bp_path)) {
      fprintf(stderr, "gor_make: *** Failed to parse Android.bp: %s\n",
              bp_path.c_str());
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
  if (opts.json_output) {
    OutputJson();
    return 0;
  }

  // Determine goals
  std::vector<std::string> goals = opts.goals;
  if (goals.empty()) {
    // Build all cc_binary modules by default
    for (const auto& [name, mod] : modules_) {
      if (mod->is_binary) {
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
      if (!opts.keep_going) break;
    }
  }

  if (result == 0 && !goals.empty()) {
    fprintf(stderr, "Build completed successfully.\n");
  }

  return result;
}

bool BpEngine::ParseBpFiles(const std::string& root_path) {
  if (!ParseSingleBp(root_path)) {
    return false;
  }

  // Parse Android.bp files in subdirectories
  fs::path root_dir = fs::path(root_path).parent_path();
  if (root_dir.empty()) root_dir = ".";
  std::string root_abs = fs::canonical(root_dir).string();

  for (auto& entry : fs::recursive_directory_iterator(root_dir)) {
    if (entry.path().filename() == "Android.bp") {
      std::string path = entry.path().string();
      // Skip the root file (already parsed)
      std::string entry_abs = fs::canonical(entry.path()).string();
      if (entry_abs == root_abs) continue;
      // Skip common output/build directories
      std::string entry_str = entry.path().string();
      if (entry_str.find("/out/") != std::string::npos) continue;
      if (entry_str.find("/bazel-") != std::string::npos) continue;
      if (entry_str.find("/.git/") != std::string::npos) continue;
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

void BpEngine::ParseBpDirectory(const std::string& dir_path) {
  // Walk the directory tree and parse all Android.bp files
  for (auto& entry : fs::recursive_directory_iterator(dir_path)) {
    if (entry.path().filename() == "Android.bp") {
      std::string entry_str = entry.path().string();
      // Skip common output/build directories
      if (entry_str.find("/out/") != std::string::npos) continue;
      if (entry_str.find("/bazel-") != std::string::npos) continue;
      if (entry_str.find("/.git/") != std::string::npos) continue;
      try {
        ParseSingleBp(entry_str);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "gor_make: [warning] error parsing %s: %s\n",
                     entry_str.c_str(), e.what());
      } catch (...) {
        std::fprintf(stderr, "gor_make: [warning] unknown error parsing %s\n",
                     entry_str.c_str());
      }
    }
  }
}

bool BpEngine::ParseSingleBp(const std::string& path) {
  // Avoid parsing the same file twice
  std::string abs_path = fs::canonical(path).string();
  if (parsed_files_.count(abs_path) > 0) return true;
  parsed_files_.insert(abs_path);

  BpParser parser;
  BpFile result;
  if (!parser.ParseFile(path, &result)) {
    fprintf(stderr, "gor_make: Parse error in %s: %s\n",
            path.c_str(), parser.GetError().c_str());
    return false;
  }

  std::string src_dir = fs::path(path).parent_path().string();
  if (src_dir.empty()) src_dir = ".";

  for (const auto& bp_mod : result.modules) {
    auto mod = ConvertModule(bp_mod, src_dir);
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

  bp_files_.push_back(std::move(result));
  return true;
}

std::unique_ptr<BpBuildModule> BpEngine::ConvertModule(
    const BpModule& bp_module, const std::string& src_dir) {
  auto mod = std::make_unique<BpBuildModule>();
  mod->type = bp_module.type;
  mod->src_dir = src_dir;

  // Get name
  auto name_it = bp_module.properties.find("name");
  if (name_it == bp_module.properties.end() || !name_it->second.IsString()) {
    // Silently skip non-buildable module types that don't have names
    // (package, license, license_kind, etc.)
    return nullptr;
  }
  mod->name = name_it->second.AsString();

  // Set module type flags based on type
  const std::string& t = bp_module.type;
  if (t == "cc_binary" || t == "cc_test_binary") {
    mod->is_binary = true;
    if (t == "cc_test_binary") mod->is_test = true;
  } else if (t == "cc_library" || t == "cc_library_shared") {
    mod->is_shared = true;
  } else if (t == "cc_library_static") {
    mod->is_static = true;
  } else if (t == "cc_test") {
    mod->is_test = true;
    mod->is_binary = true;
  } else if (t == "cc_benchmark") {
    mod->is_binary = true;
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
  auto it = bp_module.properties.find("srcs");
  if (it != bp_module.properties.end()) {
    mod->srcs = ResolveSrcs(it->second, src_dir);
  }

  it = bp_module.properties.find("shared_libs");
  if (it != bp_module.properties.end()) {
    mod->shared_libs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("static_libs");
  if (it != bp_module.properties.end()) {
    mod->static_libs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("whole_static_libs");
  if (it != bp_module.properties.end()) {
    mod->whole_static_libs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("header_libs");
  if (it != bp_module.properties.end()) {
    mod->header_libs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("cflags");
  if (it != bp_module.properties.end()) {
    mod->cflags = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("cppflags");
  if (it != bp_module.properties.end()) {
    mod->cppflags = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("ldflags");
  if (it != bp_module.properties.end()) {
    mod->ldflags = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("include_dirs");
  if (it != bp_module.properties.end()) {
    mod->include_dirs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("local_include_dirs");
  if (it != bp_module.properties.end()) {
    mod->local_include_dirs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("export_include_dirs");
  if (it != bp_module.properties.end()) {
    mod->export_include_dirs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("system_shared_libs");
  if (it != bp_module.properties.end()) {
    mod->system_shared_libs = GetStringList(it->second, src_dir);
  }

  it = bp_module.properties.find("stl");
  if (it != bp_module.properties.end() && it->second.IsString()) {
    mod->stl = it->second.AsString();
  }

  // Genrule properties
  it = bp_module.properties.find("cmd");
  if (it != bp_module.properties.end() && it->second.IsString()) {
    mod->gen_cmd = it->second.AsString();
  }

  it = bp_module.properties.find("out");
  if (it != bp_module.properties.end()) {
    mod->gen_out = GetStringList(it->second, src_dir);
  }

  return mod;
}

std::vector<std::string> BpEngine::GetStringList(const BpValue& val,
                                                  const std::string& src_dir) {
  std::vector<std::string> result;
  if (val.IsString()) {
    result.push_back(val.AsString());
  } else if (val.IsList()) {
    for (const auto& item : val.list_val) {
      if (item.IsString()) {
        result.push_back(item.AsString());
      }
    }
  }
  return result;
}

std::vector<std::string> BpEngine::ResolveSrcs(const BpValue& val,
                                                const std::string& src_dir) {
  std::vector<std::string> result;
  if (val.IsString()) {
    // Single source file
    std::string s = val.AsString();
    if (s.find('*') != std::string::npos) {
      // Glob pattern
      BpParser parser;
      auto expanded = parser.ExpandGlob(s);
      for (const auto& e : expanded) {
        result.push_back(src_dir + "/" + e);
      }
    } else {
      result.push_back(src_dir + "/" + s);
    }
  } else if (val.IsList()) {
    for (const auto& item : val.list_val) {
      if (item.IsString()) {
        std::string s = item.AsString();
        if (s.find('*') != std::string::npos) {
          BpParser parser;
          auto expanded = parser.ExpandGlob(s);
          for (const auto& e : expanded) {
            result.push_back(src_dir + "/" + e);
          }
        } else {
          result.push_back(src_dir + "/" + s);
        }
      }
    }
  }
  return result;
}

void BpEngine::ApplyDefaults() {
  // Build a map of defaults name -> BpModule*
  std::map<std::string, const BpModule*> defaults_map;
  for (const auto& bp_file : bp_files_) {
    for (const auto& mod : bp_file.modules) {
      if (mod.type == "cc_defaults") {
        defaults_map[mod.name] = &mod;
      }
    }
  }

  // Apply defaults to each build module
  for (const auto& [name, mod] : modules_) {
    // Find the original BpModule to get its defaults list
    for (const auto& bp_file : bp_files_) {
      for (const auto& bp_mod : bp_file.modules) {
        if (bp_mod.name == name) {
          auto defaults_it = bp_mod.properties.find("defaults");
          if (defaults_it != bp_mod.properties.end() && defaults_it->second.IsList()) {
            for (const auto& d : defaults_it->second.list_val) {
              if (d.IsString()) {
                auto dit = defaults_map.find(d.AsString());
                if (dit != defaults_map.end()) {
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
  auto apply_list = [&](const char* prop, std::vector<std::string>* target) {
    if (!target->empty()) return;  // Already set by module
    auto it = defaults.properties.find(prop);
    if (it != defaults.properties.end()) {
      *target = GetStringList(it->second, module->src_dir);
    }
  };

  apply_list("cflags", &module->cflags);
  apply_list("cppflags", &module->cppflags);
  apply_list("ldflags", &module->ldflags);
  apply_list("include_dirs", &module->include_dirs);
  apply_list("local_include_dirs", &module->local_include_dirs);
  apply_list("export_include_dirs", &module->export_include_dirs);
  apply_list("system_shared_libs", &module->system_shared_libs);
  
  // For srcs, shared_libs, static_libs — always prepend defaults
  // (these are additive in Blueprint semantics)
  auto prepend_list = [&](const char* prop, std::vector<std::string>* target) {
    auto it = defaults.properties.find(prop);
    if (it != defaults.properties.end()) {
      auto defaults_list = GetStringList(it->second, module->src_dir);
      std::vector<std::string> combined = defaults_list;
      combined.insert(combined.end(), target->begin(), target->end());
      *target = combined;
    }
  };
  prepend_list("srcs", &module->srcs);
  prepend_list("shared_libs", &module->shared_libs);
  prepend_list("static_libs", &module->static_libs);
  prepend_list("whole_static_libs", &module->whole_static_libs);
  prepend_list("header_libs", &module->header_libs);
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
    std::string lib_path = "lib" + name + ".so";
    if (stat(("/usr/lib/" + lib_path).c_str(), &st) == 0 ||
        stat(("/usr/lib/x86_64-linux-gnu/" + lib_path).c_str(), &st) == 0 ||
        stat(("/lib/x86_64-linux-gnu/" + lib_path).c_str(), &st) == 0) {
      visited.insert(name);
      return true;
    }
    fprintf(stderr, "gor_make: *** No module named '%s'.\n", name.c_str());
    return false;
  }

  building.insert(name);

  // Build static library dependencies first
  for (const auto& dep : mod->static_libs) {
    if (!BuildModule(dep, visited, building)) {
      building.erase(name);
      if (!opts_->keep_going) return false;
    }
  }

  // Build whole static library dependencies
  for (const auto& dep : mod->whole_static_libs) {
    if (!BuildModule(dep, visited, building)) {
      building.erase(name);
      if (!opts_->keep_going) return false;
    }
  }

  // Build shared library dependencies first
  for (const auto& dep : mod->shared_libs) {
    if (!BuildModule(dep, visited, building)) {
      building.erase(name);
      if (!opts_->keep_going) return false;
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
  std::string obj_dir = opts_->build_dir + "/obj/" + module->name;
  if (!opts_->dry_run) buildutil::MkdirP(obj_dir);

  // Collect include directories
  std::vector<std::string> all_include_dirs = module->include_dirs;
  for (const auto& d : module->local_include_dirs) {
    all_include_dirs.push_back(d);
  }

  // Add include dirs from dependency modules (export_include_dirs)
  for (const auto& dep : module->static_libs) {
    BpBuildModule* dep_mod = FindModule(dep);
    if (dep_mod) {
      for (const auto& d : dep_mod->export_include_dirs) {
        all_include_dirs.push_back(dep_mod->src_dir + "/" + d);
      }
    }
  }
  for (const auto& dep : module->shared_libs) {
    BpBuildModule* dep_mod = FindModule(dep);
    if (dep_mod) {
      for (const auto& d : dep_mod->export_include_dirs) {
        all_include_dirs.push_back(dep_mod->src_dir + "/" + d);
      }
    }
  }

  // Compile each source file
  module->object_files.clear();

  for (const auto& src : module->srcs) {
    std::string src_path = src;
    if (src_path[0] != '/' && src_path.substr(0, 2) != "./") {
      src_path = module->src_dir + "/" + src;
    }

    if (!buildutil::FileExists(src_path)) {
      fprintf(stderr, "gor_make: *** Source file not found: %s\n", src_path.c_str());
      return false;
    }

    std::string obj_path = GetObjectPath(*module, src);

    // Check if recompilation is needed
    bool need_compile = opts_->dry_run ? false : true;
    if (!need_compile) {
      // In dry-run mode, still show commands
    } else {
      need_compile = !buildutil::FileExists(obj_path);
      if (!need_compile) {
        need_compile = GetMtime(src_path) > GetMtime(obj_path);
      }
    }

    if (need_compile || opts_->dry_run) {
      // Choose compiler based on file type
      std::string compiler = buildutil::IsCppSource(src_path) ? cxx_ : cc_;

      // Build command
      std::string cmd = compiler + " -MMD -MP -c";

      // Add cflags
      for (const auto& f : common_cflags_) {
        cmd += " " + f;
      }
      for (const auto& f : module->cflags) {
        cmd += " " + f;
      }
      if (buildutil::IsCppSource(src_path)) {
        for (const auto& f : module->cppflags) {
          cmd += " " + f;
        }
      }

      // Add include directories
      for (const auto& d : all_include_dirs) {
        cmd += " -I" + d;
      }

      // Add source and output
      cmd += " -o " + obj_path + " " + src_path;

      if (!ExecuteCmd(cmd, module->is_test && opts_->silent)) {
        fprintf(stderr, "gor_make: *** Compilation failed for %s\n", src_path.c_str());
        return false;
      }
    }

    module->object_files.push_back(obj_path);
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
  if (module->object_files.empty()) return true;

  std::string out_path = GetOutputPath(*module);
  if (!opts_->dry_run) buildutil::MkdirP(buildutil::DirName(out_path));

  if (module->is_static) {
    // Create static library with ar
    std::string cmd = ar_ + " rcs " + out_path;
    for (const auto& obj : module->object_files) {
      cmd += " " + obj;
    }

    // Check if relink needed
    bool need_link = !buildutil::FileExists(out_path);
    if (!need_link && !opts_->dry_run) {
      for (const auto& obj : module->object_files) {
        if (GetMtime(obj) > GetMtime(out_path)) {
          need_link = true;
          break;
        }
      }
    }

    if (need_link || opts_->dry_run) {
      if (!ExecuteCmd(cmd, opts_->silent)) {
        fprintf(stderr, "gor_make: *** Archiving failed for %s\n", out_path.c_str());
        return false;
      }
    }
  } else {
    // Link binary or shared library
    std::string linker = module->is_binary ? cxx_ : cxx_;
    std::string cmd = linker;

    // Add ldflags
    for (const auto& f : module->ldflags) {
      cmd += " " + f;
    }

    // Add object files
    for (const auto& obj : module->object_files) {
      cmd += " " + obj;
    }

    // Add static libraries from dependencies
    for (const auto& dep : module->static_libs) {
      BpBuildModule* dep_mod = FindModule(dep);
      if (dep_mod) {
        cmd += " " + GetOutputPath(*dep_mod);
      }
    }

    // Add whole static libraries
    for (const auto& dep : module->whole_static_libs) {
      BpBuildModule* dep_mod = FindModule(dep);
      if (dep_mod) {
        cmd += " -Wl,--whole-archive " + GetOutputPath(*dep_mod) + " -Wl,--no-whole-archive";
      }
    }

    // Add shared library flags
    for (const auto& dep : module->shared_libs) {
      BpBuildModule* dep_mod = FindModule(dep);
      if (dep_mod) {
        // Link against the shared library
        std::string dep_out = GetOutputPath(*dep_mod);
        std::string dep_dir = buildutil::DirName(dep_out);
        cmd += " -L" + dep_dir + " -l" + StripLibPrefix(dep_mod->name);
      } else {
        // System library
        cmd += " -l" + StripLibPrefix(dep);
      }
    }

    // Add output path
    if (module->is_shared) {
      cmd += " -shared -o " + out_path;
    } else {
      cmd += " -o " + out_path;
    }

    // Check if relink needed
    bool need_link = !buildutil::FileExists(out_path);
    if (!need_link && !opts_->dry_run) {
      for (const auto& obj : module->object_files) {
        if (GetMtime(obj) > GetMtime(out_path)) {
          need_link = true;
          break;
        }
      }
      // Also check dependency outputs
      if (!need_link) {
        for (const auto& dep : module->static_libs) {
          BpBuildModule* dep_mod = FindModule(dep);
          if (dep_mod) {
            if (GetMtime(GetOutputPath(*dep_mod)) > GetMtime(out_path)) {
              need_link = true;
              break;
            }
          }
        }
      }
    }

    if (need_link || opts_->dry_run) {
      if (!ExecuteCmd(cmd, opts_->silent)) {
        fprintf(stderr, "gor_make: *** Linking failed for %s\n", out_path.c_str());
        return false;
      }
    }
  }

  module->output_file = out_path;
  return true;
}

bool BpEngine::BuildGenrule(BpBuildModule* module) {
  if (module->gen_cmd.empty()) return true;

  std::string out_dir = opts_->build_dir + "/gen/" + module->name;
  if (!opts_->dry_run) buildutil::MkdirP(out_dir);

  // Simplified genrule: replace $(in) and $(out)
  std::string cmd = module->gen_cmd;
  // Replace $(in) with source files
  std::string in_files = Join(module->srcs, " ");
  size_t pos = cmd.find("$(in)");
  while (pos != std::string::npos) {
    cmd.replace(pos, 5, in_files);
    pos = cmd.find("$(in)");
  }
  // Replace $(out) with output files
  std::vector<std::string> out_paths;
  for (const auto& out : module->gen_out) {
    out_paths.push_back(out_dir + "/" + out);
  }
  std::string out_files = Join(out_paths, " ");
  pos = cmd.find("$(out)");
  while (pos != std::string::npos) {
    cmd.replace(pos, 6, out_files);
    pos = cmd.find("$(out)");
  }

  if (!ExecuteCmd(cmd, opts_->silent)) {
    fprintf(stderr, "gor_make: *** Genrule failed for %s\n", module->name.c_str());
    return false;
  }

  module->object_files = out_paths;
  module->output_file = out_paths.empty() ? "" : out_paths[0];
  return true;
}

void BpEngine::Clean() {
  if (buildutil::FileExists(opts_->build_dir)) {
    fs::remove_all(opts_->build_dir);
    printf("Cleaned %s\n", opts_->build_dir.c_str());
  }
}

std::string BpEngine::GetOutputPath(const BpBuildModule& module) const {
  std::string file_name = module.name;
  // Only add "lib" prefix if the name doesn't already start with "lib"
  bool needs_lib_prefix = !(file_name.size() >= 3 &&
                          file_name.substr(0, 3) == "lib");
  std::string lib_prefix = needs_lib_prefix ? "lib" : "";

  if (module.is_static) {
    return opts_->build_dir + "/lib/" + lib_prefix + file_name + ".a";
  }
  if (module.is_shared) {
    return opts_->build_dir + "/lib/" + lib_prefix + file_name + ".so";
  }
  // Binary
  return opts_->build_dir + "/bin/" + file_name;
}

std::string BpEngine::GetObjectPath(const BpBuildModule& module,
                                     const std::string& src_file) const {
  std::string base_name = buildutil::BaseName(src_file);
  return opts_->build_dir + "/obj/" + module.name + "/" +
         ReplaceExt(base_name, ".o");
}

bool BpEngine::NeedsRecompile(const std::string& obj_file,
                              const std::string& src_file,
                              const std::vector<std::string>& headers) const {
  if (buildutil::NeedsRecompile(obj_file, src_file)) return true;
  long obj_mtime = GetMtime(obj_file);
  for (const auto& h : headers) {
    if (GetMtime(h) > obj_mtime) return true;
  }
  return false;
}

bool BpEngine::ExecuteCmd(const std::string& cmd, bool silent) {
  if (!opts_->dry_run || !opts_->silent) {
    if (!silent || opts_->dry_run) {
      printf("%s\n", cmd.c_str());
      fflush(stdout);
    }
  }

  if (opts_->dry_run) {
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
    printf("      \"src_dir\": \"%s\",\n", JsonEscape(mod->src_dir).c_str());
    printf("      \"is_binary\": %s,\n", mod->is_binary ? "true" : "false");
    printf("      \"is_static\": %s,\n", mod->is_static ? "true" : "false");
    printf("      \"is_shared\": %s,\n", mod->is_shared ? "true" : "false");
    printf("      \"is_test\": %s,\n", mod->is_test ? "true" : "false");

    printf("      \"srcs\": ");
    OutputJsonArray(stdout, "      ", mod->srcs);
    printf(",\n");

    printf("      \"shared_libs\": ");
    OutputJsonArray(stdout, "      ", mod->shared_libs);
    printf(",\n");

    printf("      \"static_libs\": ");
    OutputJsonArray(stdout, "      ", mod->static_libs);
    printf(",\n");

    printf("      \"whole_static_libs\": ");
    OutputJsonArray(stdout, "      ", mod->whole_static_libs);
    printf(",\n");

    printf("      \"header_libs\": ");
    OutputJsonArray(stdout, "      ", mod->header_libs);
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
    OutputJsonArray(stdout, "      ", mod->include_dirs);
    printf(",\n");

    printf("      \"local_include_dirs\": ");
    OutputJsonArray(stdout, "      ", mod->local_include_dirs);
    printf(",\n");

    printf("      \"export_include_dirs\": ");
    OutputJsonArray(stdout, "      ", mod->export_include_dirs);
    printf("\n");

    printf("    }");
  }

  printf("\n  ]\n");
  printf("}\n");
}

}  // namespace gormake
