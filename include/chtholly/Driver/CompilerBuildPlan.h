#pragma once

#include "chtholly/Basic/LanguageVersion.h"
#include "chtholly/Driver/TargetConfig.h"
#include "chtholly/Compiler/CFFIIdentity.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace chtholly {

struct CompilerInvocation;

struct CompilerBuildPlanPackage {
  std::string name;
  LanguageVersion language_version = DefaultLanguageVersion;
  std::string manifest_path;
  std::string root_directory;
  std::string entry_path;
  std::string interop_bundle_path;
  std::string interop_bundle_digest;
  std::string artifact_archive_path;
  std::string artifact_archive_digest;
  std::vector<std::string> module_roots;
  std::vector<std::string> resolved_features;
  std::vector<std::size_t> dependencies;
  std::vector<std::string> native_library_paths;
  std::vector<std::string> native_link_libraries;
  std::string cffi_receipt_path;
  std::string cffi_receipt_digest;
  std::optional<compiler::CFFIReceiptIdentity> cffi_identity;
  bool cffi_required = false;
  std::uint32_t component_abi = 0;
  std::string component_identity;
  std::vector<std::string> component_exports;

  [[nodiscard]] bool isComponent() const {
    return component_abi != 0;
  }
};

struct CompilerBuildPlan {
  std::vector<CompilerBuildPlanPackage> packages;
  std::size_t root_package = 0;
  std::string package_name;
  std::string project_manifest_path;
  std::string project_root;
  std::string workspace_manifest_path;
  std::string workspace_root;
  std::string lockfile_path;
  std::string resource_dir;
  std::string runtime_library_path;
  std::string runtime_link_manifest_path;
  std::vector<std::string> runtime_link_libraries;
  std::vector<std::pair<std::string, std::string>> runtime_symbol_mappings;
  TargetConfig target;
};

std::optional<CompilerBuildPlan>
resolveNextBuildPlan(const CompilerInvocation &invocation, std::string &error);

bool verifyOrUpdateNextLockfile(const CompilerBuildPlan &plan, bool locked,
                                std::string &error);

} // namespace chtholly
