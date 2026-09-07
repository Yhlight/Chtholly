#include "CompilerPipelineInternal.h"

#include "chtholly/Driver/CompilerArtifactLoadExecutor.h"
#include "chtholly/Driver/CompilerArtifactLoadMetrics.h"
#include "chtholly/Driver/CompilerArtifactStore.h"
#include "chtholly/Driver/CompilerSourceSnapshot.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <ranges>

namespace chtholly {

PackageQueryResult CompilerPipelineExecutionService::packageQuery(
    std::size_t package_index, const CompilerPackageQueryContext &context) {
  const auto &plan = context.plan;
  const auto &invocation = context.invocation;
  const auto &previous_manifests = context.previous_manifests;
  const auto completed = context.completed;
  const auto &lease = context.lease;
  const auto &snapshot = context.snapshot;
  const auto &compile_toolchain_fingerprint =
      context.compile_toolchain_fingerprint;
  auto &artifact_loads = context.artifact_loads;
  auto *artifact_load_metrics = context.artifact_load_metrics;
  PackageQueryResult result;
  const auto &package = plan.packages[package_index];
  result.session = std::make_unique<compiler::CompilationSession>(
      plan.build.target.info.triple, package.package_name,
      package.resolved_features, compile_toolchain_fingerprint,
      CompilerPipelinePlanningService::packageProvenance(package), CompilerPipelinePlanningService::packageLanguageContract(package),
      package.compiler_intrinsics, package.cffi_identity);
  if (!result.session->setRuntimeSymbolMappings(
          plan.build.runtime_symbol_mappings, result.error))
    return result;
  std::vector<std::size_t> interop_packages;
  interop_packages.push_back(package_index);
  interop_packages.insert(interop_packages.end(), package.dependencies.begin(),
                          package.dependencies.end());
  const auto template_dependencies =
      CompilerPipelinePlanningService::templateDependencyClosure(package_index, plan);
  interop_packages.insert(interop_packages.end(), template_dependencies.begin(),
                          template_dependencies.end());
  std::ranges::sort(interop_packages);
  interop_packages.erase(
      std::unique(interop_packages.begin(), interop_packages.end()),
      interop_packages.end());
  for (const auto interop_package : interop_packages) {
    const auto &provider = plan.packages[interop_package];
    auto interop_bundle_path = provider.interop_bundle_path;
    auto interop_bundle_digest = provider.interop_bundle_digest;
    if (interop_package != package_index &&
        interop_package < completed.size() &&
        !completed[interop_package].interop_bundle_path.empty()) {
      interop_bundle_path = completed[interop_package].interop_bundle_path;
      interop_bundle_digest = completed[interop_package].interop_bundle_digest;
    }
    if (interop_bundle_path.empty() &&
        std::ranges::any_of(provider.sources, [](const auto &path) {
          return pathForFileSystem(path).extension() == ".cfdl";
        })) {
      const auto candidate = CompilerPipelinePlanningService::generatedInteropBundlePath(plan, provider);
      std::error_code file_error;
      if (std::filesystem::is_regular_file(pathForFileSystem(candidate),
                                           file_error) &&
          !file_error) {
        interop_bundle_path = candidate;
        const auto digest = sha256File(candidate);
        if (!digest) {
          result.error = "failed to fingerprint cached Interop bundle: '" +
                         candidate + "'";
          return result;
        }
        interop_bundle_digest = *digest;
      }
    }
    if (interop_bundle_path.empty())
      continue;
    const auto digest = sha256File(interop_bundle_path);
    if (!digest || *digest != interop_bundle_digest) {
      result.error = "compiler Interop bundle changed during session setup: '" +
                     interop_bundle_path + "'";
      return result;
    }
    if (!result.session->loadInteropBundle(interop_bundle_path,
                                           provider.package_name, result.error))
      return result;
  }
  for (const auto &source_path : package.sources) {
    const auto *source = snapshot.find(source_path);
    if (!source) {
      result.error =
          "compiler source snapshot omitted planned input '" + source_path + "'";
      return result;
    }
    const auto id = result.session->addUnit(
        snapshot.sourceInput(*source), CompilerPipelinePlanningService::compilationUnitKindForPath(source_path));
    if (source_path == package.root_source)
      result.root_id = id;
  }
  if (package.is_root && !result.root_id.hasValue()) {
    result.error = "compiler build did not identify its requested root module";
    return result;
  }

  compiler::CompilationRequest request;
  request.component_identity = package.component_identity;
  request.component_exports = package.component_exports;
  request.debug_info = invocation.debug_info == DebugInfoKind::LineTablesOnly
                           ? compiler::DebugInfoMode::LineTablesOnly
                       : invocation.debug_info == DebugInfoKind::Full
                           ? compiler::DebugInfoMode::Full
                           : compiler::DebugInfoMode::None;
  switch (invocation.optimization) {
  case OptimizationLevel::O0:
    request.optimization = compiler::LLVMOptimizationLevel::O0;
    break;
  case OptimizationLevel::O1:
    request.optimization = compiler::LLVMOptimizationLevel::O1;
    break;
  case OptimizationLevel::O2:
    request.optimization = compiler::LLVMOptimizationLevel::O2;
    break;
  case OptimizationLevel::O3:
    request.optimization = compiler::LLVMOptimizationLevel::O3;
    break;
  case OptimizationLevel::Os:
    request.optimization = compiler::LLVMOptimizationLevel::Os;
    break;
  case OptimizationLevel::Oz:
    request.optimization = compiler::LLVMOptimizationLevel::Oz;
    break;
  }
  request.unit_emission_roles.assign(result.session->unitCount(),
                                     compiler::ModuleEmissionRole::Library);
  if (package.component_abi == compiler::ComponentAbiEpoch)
    std::ranges::fill(request.unit_emission_roles,
                      compiler::ModuleEmissionRole::ComponentLibrary);
  if (package.is_root &&
      (invocation.action == DriverAction::EmitLLVM ||
       invocation.action == DriverAction::EmitObject ||
       invocation.action == DriverAction::EmitExecutable) &&
      package.component_abi == 0)
    request.unit_emission_roles[result.root_id.index] =
        compiler::ModuleEmissionRole::ExecutableEntry;
  if (const auto previous = previous_manifests.find(package.package_name);
      previous != previous_manifests.end())
    request.previous_manifest = &previous->second;
  for (const auto dependency : package.dependencies) {
    if (dependency >= completed.size() || !completed[dependency].session) {
      result.error = "compiler package query ran before a dependency completed";
      return result;
    }
    request.dependency_manifests.push_back(
        &completed[dependency].session->packageManifest());
    request.dependency_check_artifacts.push_back(
        &completed[dependency].session->packageCheckArtifact());
  }
  for (const auto dependency : template_dependencies) {
    if (dependency >= completed.size() || !completed[dependency].session) {
      result.error =
          "compiler package query omitted a template dependency artifact";
      return result;
    }
    request.template_dependency_closure.push_back(
        &completed[dependency].session->packageManifest());
    request.template_dependency_check_closure.push_back(
        &completed[dependency].session->packageCheckArtifact());
  }
  if (package.is_root && (invocation.action == DriverAction::EmitLLVM ||
                          !invocation.compiler_sem_ir_output_path.empty() ||
                          !invocation.compiler_low_ir_output_path.empty()))
    request.required_transient_outputs.push_back(result.root_id);
  request.load_object = [&](const auto &fingerprint,
                            const auto &specific_fingerprint) {
    return lease.loadObject(
        fingerprint, specific_fingerprint, plan.build.target.info.triple,
        plan.build.target.object_extension, artifact_load_metrics);
  };
  request.load_objects = [&](auto requests) {
    return artifact_loads.loadObjects(requests, request.load_object);
  };
  request.load_specialization = [&](const auto &request_fingerprint) {
    return artifact_loads.loadSpecialization(
        request_fingerprint, [&](const auto &fingerprint) {
          return lease.loadSpecialization(fingerprint, artifact_load_metrics);
        });
  };
  request.load_nominal_semantic_witness = [&](const auto &request_fingerprint,
                                              std::string &load_error)
      -> std::optional<compiler::NominalSemanticWitnessArtifact> {
    load_error.clear();
    const auto load_from_dependency = [&](std::size_t dependency)
        -> std::optional<compiler::NominalSemanticWitnessArtifact> {
      if (dependency >= completed.size() || !completed[dependency].session)
        return std::nullopt;
      const auto &session = *completed[dependency].session;
      for (std::size_t unit_index = 0; unit_index < session.unitCount();
           ++unit_index)
        for (const auto &witness :
             session
                 .unit(compiler::CheckIRId(static_cast<std::uint32_t>(unit_index)))
                 .nominalSemanticWitnessArtifacts())
          if (witness.request_fingerprint == request_fingerprint)
            return witness;
      return std::nullopt;
    };
    for (const auto dependency : package.dependencies)
      if (auto witness = load_from_dependency(dependency))
        return witness;
    for (const auto dependency : template_dependencies)
      if (auto witness = load_from_dependency(dependency))
        return witness;
    return lease.loadNominalSemanticWitness(request_fingerprint, load_error);
  };
  request.load_nominal_semantic_witnesses = [&](auto fingerprints) {
    return artifact_loads.loadNominalSemanticWitnesses(
        fingerprints, request.load_nominal_semantic_witness);
  };
  if (!result.session->compile(result.error, request))
    return result;
  if (package.is_standard_library) {
    std::vector<std::string> actual_modules;
    actual_modules.reserve(result.session->unitCount());
    for (std::uint32_t index = 0; index < result.session->unitCount(); ++index)
      actual_modules.push_back(std::string(
          result.session->unit(compiler::CheckIRId(index)).moduleName()));
    std::ranges::sort(actual_modules);
    if (actual_modules != package.expected_modules) {
      result.error =
          "compiler standard-library source declarations disagree with manifest";
      return result;
    }
  }
  const auto has_cfdl_source =
      std::ranges::any_of(package.sources, [](const auto &path) {
        return pathForFileSystem(path).extension() == ".cfdl";
      });
  if (has_cfdl_source) {
    result.interop_bundle_path = CompilerPipelinePlanningService::generatedInteropBundlePath(plan, package);
    std::error_code file_error;
    std::filesystem::create_directories(
        pathForFileSystem(result.interop_bundle_path).parent_path(),
        file_error);
    if (file_error) {
      result.error = "failed to create generated Interop directory: " +
                     file_error.message();
      return result;
    }
    if (!result.session->exportInteropBundle(result.interop_bundle_path,
                                             result.error))
      return result;
    const auto digest = sha256File(result.interop_bundle_path);
    if (!digest) {
      result.error = "failed to fingerprint generated Interop bundle";
      return result;
    }
    result.interop_bundle_digest = *digest;
  }
  return result;
}

} // namespace chtholly
