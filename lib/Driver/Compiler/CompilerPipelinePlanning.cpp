#include "CompilerPipelineInternal.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"

#include <algorithm>
#include <sstream>
#include <filesystem>

namespace chtholly {

namespace {

void appendCanonicalField(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace

compiler::PackageProvenance CompilerPipelinePlanningService::packageProvenance(
    const CompilerPackagePlan &package) {
  return {.kind = package.is_standard_library
                      ? compiler::PackageProvenanceKind::ToolchainStandardLibrary
                      : compiler::PackageProvenanceKind::Workspace,
          .contract_fingerprint = package.package_contract_fingerprint};
}

LanguageContract CompilerPipelinePlanningService::packageLanguageContract(
    const CompilerPackagePlan &package) {
  auto contract = CurrentLanguageContract;
  contract.source = package.language_version;
  return contract;
}

compiler::CompilationUnitKind
CompilerPipelinePlanningService::compilationUnitKindForPath(
    std::string_view path) {
  return std::filesystem::path(path).extension() == ".cfdl"
             ? compiler::CompilationUnitKind::ForeignBinding
             : compiler::CompilationUnitKind::ChthollySource;
}

std::vector<std::string>
CompilerPipelinePlanningService::sourceSnapshotPaths(
    const CompilerDriverPlan &plan) {
  std::vector<std::string> paths;
  for (const auto &package : plan.packages)
    paths.insert(paths.end(), package.sources.begin(), package.sources.end());
  return paths;
}

std::size_t CompilerPipelinePlanningService::maximumPackageQueryParallelism(
    const CompilerDriverPlan &plan) {
  const auto count = plan.packages.size();
  std::vector<std::vector<bool>> reaches(count,
                                         std::vector<bool>(count, false));
  for (std::size_t package = 0; package < count; ++package) {
    std::vector<std::size_t> pending(
        plan.packages[package].dependencies.begin(),
        plan.packages[package].dependencies.end());
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
      const auto dependency = pending[cursor];
      if (dependency >= count || reaches[package][dependency])
        continue;
      reaches[package][dependency] = true;
      pending.insert(pending.end(),
                     plan.packages[dependency].dependencies.begin(),
                     plan.packages[dependency].dependencies.end());
    }
  }
  std::vector<std::size_t> matched_right(count, count);
  std::size_t matching = 0;
  for (std::size_t left = 0; left < count; ++left) {
    std::vector<bool> visited(count, false);
    const auto augment = [&](const auto &self, std::size_t candidate) -> bool {
      for (std::size_t right = 0; right < count; ++right) {
        if (!reaches[candidate][right] || visited[right])
          continue;
        visited[right] = true;
        if (matched_right[right] == count || self(self, matched_right[right])) {
          matched_right[right] = candidate;
          return true;
        }
      }
      return false;
    };
    if (augment(augment, left))
      ++matching;
  }
  return std::max<std::size_t>(1, count - matching);
}

compiler::StableFingerprint CompilerPipelineFingerprintService::resolution(
    const CompilerDriverPlan &plan) {
  std::ostringstream canonical;
  canonical << "chtholly.next.resolved-build-control.v4\n"
            << plan.packages.size() << '\n' << plan.root_package << '\n';
  for (const auto &package : plan.packages) {
    appendCanonicalField(canonical, package.package_name);
    appendCanonicalField(canonical, package.interop_bundle_path);
    appendCanonicalField(canonical, package.interop_bundle_digest);
    appendCanonicalField(canonical, package.artifact_archive_path);
    appendCanonicalField(canonical, package.artifact_archive_digest);
    appendCanonicalField(canonical, package.cffi_receipt_path);
    appendCanonicalField(canonical, package.cffi_receipt_digest);
    appendCanonicalField(canonical, package.language_version.str());
    appendCanonicalField(canonical, package.source_entry);
    appendCanonicalField(canonical, package.root_source);
    canonical << (package.include_entry ? "include-entry\n" : "exclude-entry\n")
              << (package.is_root ? "root\n" : "dependency\n")
              << package.module_roots.size() << '\n';
    for (const auto &root : package.module_roots)
      appendCanonicalField(canonical, normalizeCompilerInputPath(root));
    canonical << package.sources.size() << '\n';
    for (const auto &source : package.sources)
      appendCanonicalField(canonical, source);
    canonical << package.resolved_features.size() << '\n';
    for (const auto &feature : package.resolved_features)
      appendCanonicalField(canonical, feature);
    canonical << package.dependencies.size() << '\n';
    for (const auto dependency : package.dependencies)
      appendCanonicalField(canonical, plan.packages[dependency].package_name);
    appendCanonicalField(canonical, package.package_contract_fingerprint.hex());
    canonical << (package.is_standard_library ? "toolchain-stdlib\n"
                                              : "workspace\n");
    canonical << package.expected_modules.size() << '\n';
    for (const auto &module : package.expected_modules)
      appendCanonicalField(canonical, module);
  }
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

compiler::StableFingerprint CompilerPipelineFingerprintService::compileToolchain(
    const CompilerInvocation &invocation, const CompilerDriverPlan &plan) {
  std::ostringstream canonical;
  canonical << "chtholly.next.compile-toolchain.v2\n";
  appendCanonicalField(canonical, compilerVersion());
  appendCanonicalField(canonical, "chtholly.next.language-semantics.v4");
  appendCanonicalField(canonical, "chtholly.next.codegen.v3");
  appendCanonicalField(canonical, plan.build.target.info.triple);
  appendCanonicalField(canonical, abiVersionSpelling(invocation.abi_version));
  appendCanonicalField(canonical,
                       optimizationLevelSpelling(invocation.optimization));
  appendCanonicalField(canonical, debugInfoKindSpelling(invocation.debug_info));
  appendCanonicalField(canonical, plan.build.target.sysroot_path);
  appendCanonicalField(canonical, plan.build.target.object_extension);
  for (const auto &mapping : plan.build.runtime_symbol_mappings) {
    appendCanonicalField(canonical, mapping.first);
    appendCanonicalField(canonical, mapping.second);
  }
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

compiler::StableFingerprint CompilerPipelineFingerprintService::linkToolchain(
    const CompilerInvocation &invocation, const CompilerDriverPlan &plan,
    const compiler::StableFingerprint &compile_fingerprint) {
  std::ostringstream canonical;
  canonical << "chtholly.next.link-toolchain.v1\n";
  appendCanonicalField(canonical, compile_fingerprint.hex());
  appendCanonicalField(canonical, plan.build.target.linker_path);
  appendCanonicalField(canonical, plan.build.resource_dir);
  appendCanonicalField(canonical, plan.build.runtime_library_path);
  appendCanonicalField(canonical, plan.build.runtime_link_manifest_path);
  for (const auto &path : invocation.library_search_paths)
    appendCanonicalField(canonical, path);
  for (const auto &library : invocation.link_libraries)
    appendCanonicalField(canonical, library);
  for (const auto &library : plan.build.runtime_link_libraries)
    appendCanonicalField(canonical, library);
  for (const auto &mapping : plan.build.runtime_symbol_mappings) {
    appendCanonicalField(canonical, mapping.first);
    appendCanonicalField(canonical, mapping.second);
  }
  for (const auto &package : plan.packages) {
    appendCanonicalField(canonical, package.package_name);
    for (const auto &path : package.native_library_paths)
      appendCanonicalField(canonical, path);
    for (const auto &library : package.native_link_libraries)
      appendCanonicalField(canonical, library);
  }
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

CompilerBuildControlInputs CompilerPipelinePlanningService::buildControlInputs(
    const CompilerInvocation &invocation, const CompilerDriverPlan &plan) {
  CompilerBuildControlInputs inputs;
  const auto add_required = [&](const std::string &path) {
    if (!path.empty())
      inputs.required_files.push_back(path);
  };
  add_required(plan.build.workspace_manifest_path);
  for (const auto &package : plan.build.packages)
    add_required(package.manifest_path);
  for (const auto &package : plan.build.packages)
    add_required(package.interop_bundle_path);
  for (const auto &package : plan.build.packages)
    add_required(package.artifact_archive_path);
  for (const auto &package : plan.build.packages)
    add_required(package.cffi_receipt_path);
  if (!invocation.disable_lockfile && !plan.build.lockfile_path.empty())
    inputs.optional_files.push_back(plan.build.lockfile_path);
  add_required(plan.build.runtime_link_manifest_path);
  add_required(plan.standard_library_manifest_path);
  std::ranges::sort(inputs.required_files);
  inputs.required_files.erase(
      std::unique(inputs.required_files.begin(), inputs.required_files.end()),
      inputs.required_files.end());
  std::ranges::sort(inputs.optional_files);
  inputs.optional_files.erase(
      std::unique(inputs.optional_files.begin(), inputs.optional_files.end()),
      inputs.optional_files.end());
  inputs.resolution_fingerprint = CompilerPipelineFingerprintService::resolution(plan);
  inputs.compile_toolchain_fingerprint =
      CompilerPipelineFingerprintService::compileToolchain(invocation, plan);
  inputs.link_toolchain_fingerprint = CompilerPipelineFingerprintService::linkToolchain(
      invocation, plan, inputs.compile_toolchain_fingerprint);
  return inputs;
}

std::string CompilerPipelinePlanningService::generatedInteropBundlePath(
    const CompilerDriverPlan &plan, const CompilerPackagePlan &package) {
  std::ostringstream canonical;
  canonical << "chtholly.next.generated-interop.v1\n"
            << package.package_name << '\n'
            << package.package_contract_fingerprint.hex() << '\n'
            << plan.build.target.info.triple << '\n'
            << CurrentSemanticArtifactEpoch << '\n';
  const auto fingerprint =
      compiler::StableFingerprint::fromCanonicalBytes(canonical.str()).hex();
  return (std::filesystem::path(plan.store_root) / "interop" /
          fingerprint.substr(0, 2) / (fingerprint + ".interop"))
      .string();
}

std::vector<std::size_t>
CompilerPipelinePlanningService::templateDependencyClosure(
    std::size_t package_index, const CompilerDriverPlan &plan) {
  std::vector<bool> direct(plan.packages.size());
  std::vector<bool> visited(plan.packages.size());
  std::vector<std::size_t> pending;
  for (const auto dependency : plan.packages[package_index].dependencies) {
    direct[dependency] = true;
    pending.push_back(dependency);
  }
  std::vector<std::size_t> result;
  for (std::size_t index = 0; index < pending.size(); ++index) {
    const auto dependency = pending[index];
    if (visited[dependency])
      continue;
    visited[dependency] = true;
    if (!direct[dependency])
      result.push_back(dependency);
    for (const auto child : plan.packages[dependency].dependencies)
      pending.push_back(child);
  }
  std::ranges::sort(result);
  return result;
}

} // namespace chtholly
