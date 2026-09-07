#pragma once

#include "chtholly/Basic/AbiVersion.h"
#include "chtholly/Driver/CompilerOptions.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

enum class DriverWorkflow { Compile, Check, Build, Run, New, Init, Doctor };

enum class DriverAction {
  EmitLLVM,
  EmitObject,
  EmitExecutable,
  Check,
  Scaffold,
  Doctor,
  Help,
  Version,
};

struct PackageFeatureSelection {
  std::string package_name;
  std::string feature_name;
};

struct CompilerInvocation {
  std::string executable_path;
  std::string resource_dir;
  DriverWorkflow workflow = DriverWorkflow::Compile;
  DriverAction action = DriverAction::EmitLLVM;
  std::string input_path;
  std::string scaffold_path;
  std::string scaffold_name;
  std::string output_path;
  std::string project_path;
  std::string manifest_path;
  std::string workspace_path;
  std::string package_name;
  std::vector<std::string> feature_selections;
  std::vector<PackageFeatureSelection> package_feature_selections;
  std::vector<std::string> default_feature_disabled_packages;
  std::string out_dir;
  std::string compiler_tokens_output_path;
  std::string compiler_parse_tree_output_path;
  std::string compiler_sem_ir_output_path;
  std::string compiler_low_ir_output_path;
  std::string compiler_foreign_protocols_output_path;
  std::string compiler_metrics_output_path;
  std::string compiler_analysis_metrics_output_path;
  std::string compiler_artifact_load_metrics_output_path;
  std::string cache_dir;
  std::string target_triple;
  std::string sysroot_path;
  std::string linker_path;
  std::string cffi_config_path;
  std::vector<std::string> library_search_paths;
  std::vector<std::string> link_libraries;
  std::vector<std::string> program_arguments;
  bool locked = false;
  bool disable_lockfile = false;
  bool enable_default_features = true;
  bool suppress_lockfile_update = false;
  bool abi_version_specified = false;
  OptimizationLevel optimization = OptimizationLevel::O0;
  DebugInfoKind debug_info = DebugInfoKind::None;
  bool explain_invalidation = false;
  AbiVersion abi_version = DefaultChthollyAbiVersion;
  bool jobs_specified = false;
  bool scaffold_library = false;
  std::size_t jobs = 1;
};

bool parseCompilerInvocation(int argc, char **argv,
                             CompilerInvocation &invocation,
                             std::string &error);
std::string_view driverActionName(DriverAction action);
bool validateCompilerInvocation(const CompilerInvocation &invocation,
                                    std::string &error);
std::string compilerUsage(std::string_view program_name);
std::string compilerVersion();

} // namespace chtholly
