#pragma once

#include "chtholly/Basic/LanguageVersion.h"
#include "chtholly/Compiler/CFFIIdentity.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/IncrementalDependencies.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Driver/CompilerBuildPlan.h"
#include "chtholly/Driver/CompilerBuildControlSnapshot.h"
#include "chtholly/Driver/CompilerArtifactStore.h"
#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/WorkspaceArtifactTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chtholly {

class CompilerArtifactLoadMetrics;
class CompilerArtifactLoadExecutor;
class CompilerArtifactLease;
class CompilerSourceSnapshot;
class CompilerInputFileSystem;

struct CompilerPackagePlan {
  std::string package_name;
  LanguageVersion language_version = DefaultLanguageVersion;
  std::vector<std::string> sources;
  std::vector<std::string> module_roots;
  std::string source_entry;
  std::string root_source;
  std::string interop_bundle_path;
  std::string interop_bundle_digest;
  std::string artifact_archive_path;
  std::string artifact_archive_digest;
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
  compiler::StableFingerprint package_contract_fingerprint;
  std::vector<std::string> expected_modules;
  std::map<std::string, std::vector<std::string>> expected_module_dependencies;
  std::map<std::string, std::vector<std::string>> expected_runtime_symbols;
  std::vector<compiler::CompilerIntrinsicBinding> compiler_intrinsics;
  bool is_standard_library = false;
  bool include_entry = false;
  bool is_root = false;
};

struct CompilerDriverPlan {
  CompilerBuildPlan build;
  std::vector<CompilerPackagePlan> packages;
  std::size_t root_package = 0;
  std::string output_path;
  std::string session_key;
  std::string store_root;
  std::string standard_library_manifest_path;
  std::uint64_t archive_install_attempts = 0;
  std::uint64_t archive_install_closure_hits = 0;
  std::uint64_t archive_install_fresh_installs = 0;
  std::uint64_t archive_install_bytes = 0;
};

struct CompilerSourceInventory {
  bool imports_standard_library = false;
  bool uses_candidate_async = false;
  std::vector<std::string> imported_modules;
  std::vector<std::string> declared_modules;
};

struct PackageQueryResult {
  std::unique_ptr<compiler::CompilationSession> session;
  compiler::CheckIRId root_id;
  std::string interop_bundle_path;
  std::string interop_bundle_digest;
  std::string error;
};

struct CompilerPackageQueryContext {
  const CompilerDriverPlan &plan;
  const CompilerInvocation &invocation;
  const std::map<std::string, compiler::CompilerPackageArtifactManifest>
      &previous_manifests;
  std::span<const PackageQueryResult> completed;
  const CompilerArtifactLease &lease;
  const CompilerSourceSnapshot &snapshot;
  const compiler::StableFingerprint &compile_toolchain_fingerprint;
  CompilerArtifactLoadExecutor &artifact_loads;
  CompilerArtifactLoadMetrics *artifact_load_metrics = nullptr;
};

struct CompilerPackageQueryExecutionState {
  const CompilerDriverPlan &plan;
  std::size_t jobs = 1;
  std::function<std::size_t()> maximum_parallelism;
  std::function<PackageQueryResult(
      std::size_t, std::span<const PackageQueryResult>)>
      execute_query;
  CompilerArtifactLoadMetrics *artifact_load_metrics = nullptr;
};

struct CompilerPipelineExecutionService {
  [[nodiscard]] static PackageQueryResult packageQuery(
      std::size_t package_index, const CompilerPackageQueryContext &context);
  [[nodiscard]] static bool packageQueryGraph(
      CompilerPackageQueryExecutionState &state,
      std::vector<PackageQueryResult> &results, std::string &error);
};

struct CompilerPipelinePlanningService {
  [[nodiscard]] static compiler::PackageProvenance
  packageProvenance(const CompilerPackagePlan &package);
  [[nodiscard]] static LanguageContract
  packageLanguageContract(const CompilerPackagePlan &package);
  [[nodiscard]] static compiler::CompilationUnitKind
  compilationUnitKindForPath(std::string_view path);
  [[nodiscard]] static std::vector<std::string>
  sourceSnapshotPaths(const CompilerDriverPlan &plan);
  [[nodiscard]] static std::size_t
  maximumPackageQueryParallelism(const CompilerDriverPlan &plan);
  [[nodiscard]] static CompilerBuildControlInputs buildControlInputs(
      const CompilerInvocation &invocation, const CompilerDriverPlan &plan);
  [[nodiscard]] static std::string generatedInteropBundlePath(
      const CompilerDriverPlan &plan, const CompilerPackagePlan &package);
  [[nodiscard]] static std::vector<std::size_t> templateDependencyClosure(
      std::size_t package_index, const CompilerDriverPlan &plan);
  [[nodiscard]] static std::optional<CompilerSourceInventory>
  inspectSourceInventory(std::span<const std::string> source_paths,
                         LanguageVersion language_version,
                         const CompilerInputFileSystem &file_system,
                         std::string &error);
};

struct CompilerPipelineFingerprintService {
  [[nodiscard]] static compiler::StableFingerprint resolution(
      const CompilerDriverPlan &plan);
  [[nodiscard]] static compiler::StableFingerprint compileToolchain(
      const CompilerInvocation &invocation, const CompilerDriverPlan &plan);
  [[nodiscard]] static compiler::StableFingerprint linkToolchain(
      const CompilerInvocation &invocation, const CompilerDriverPlan &plan,
      const compiler::StableFingerprint &compile_fingerprint);
};

struct CompilerPipelineDiagnosticsService {
  static void appendInvalidationExplanations(
      std::string_view package_name,
      const compiler::IncrementalCompilationPlan &compilation_plan,
      std::vector<WorkspaceArtifactResult::InvalidationExplanation> &output);
  [[nodiscard]] static std::string analysisMetricsJson(
      std::span<const PackageQueryResult> results);
};

struct CompilerPipelineOutputState {
  const CompilerInvocation &invocation;
  std::function<bool(const std::string &, const std::string &, std::string &)>
      write_output;
};

struct CompilerPipelineOutputService {
  [[nodiscard]] static bool writeDumps(
      CompilerPipelineOutputState &state,
      const compiler::CompilationSession &session,
      const compiler::CompilationUnit &unit, std::string &error);
  [[nodiscard]] static bool writeArtifactLoadMetrics(
      CompilerPipelineOutputState &state, std::string_view metrics,
      std::string &error);
  [[nodiscard]] static bool writeAnalysisMetrics(
      CompilerPipelineOutputState &state,
      std::span<const PackageQueryResult> results, std::string &error);
};

struct CompilerLinkClosureService {
  [[nodiscard]] static bool collect(
      const CompilerDriverPlan &plan, std::size_t root_package,
      const compiler::CompilationUnit &root_unit,
      const std::map<std::string, std::string> &object_paths_by_module,
      const std::map<std::string, const compiler::PackageModuleArtifact *>
          &artifacts_by_module,
      bool component,
      const std::function<bool(std::string_view)> &is_hosted_async_symbol,
      std::vector<std::string> &object_paths, std::string &error);
};

struct CompilerArtifactPublicationState {
  const CompilerDriverPlan &plan;
  CompilerArtifactStore &artifact_store;
  std::span<const PackageQueryResult> query_results;
  std::vector<std::string> &object_paths;
  std::map<std::string, std::string> &object_paths_by_module;
  std::map<std::string, const compiler::PackageModuleArtifact *>
      &artifacts_by_module;
  std::vector<CompilerPublishedObject> &published_objects;
  std::vector<CompilerPublishedSpecialization> &published_specializations;
  std::vector<CompilerPublishedNominalTypeSpecific> &published_nominal_specifics;
  std::vector<CompilerPublishedNominalSemanticWitness>
      &published_nominal_semantic_witnesses;
  std::vector<CompilerPublishedNominalTypeLayout> &published_nominal_layouts;
};

struct CompilerArtifactPublicationService {
  [[nodiscard]] static bool collect(CompilerArtifactPublicationState &state,
                                    std::string &error);
};

} // namespace chtholly
