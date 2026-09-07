#include "chtholly/Driver/CompilerPipeline.h"

#include "chtholly/Driver/ArtifactStore.h"
#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/NativeLinker.h"
#include "chtholly/Driver/CompilerArtifactLoadExecutor.h"
#include "chtholly/Driver/CompilerArtifactStore.h"
#include "chtholly/Driver/CompilerBuildControlSnapshot.h"
#include "chtholly/Driver/CompilerBuildPlan.h"
#include "chtholly/Driver/CompilerDaemon.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"
#include "chtholly/Driver/CompilerSourceSnapshot.h"
#include "chtholly/Driver/CompilerStandardLibrary.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Driver/ResourceLocator.h"
#include "chtholly/Driver/TargetConfig.h"

#include "CompilerPipelineInternal.h"
#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/LLVM.h"
#include "chtholly/Compiler/Lexer.h"
#include "chtholly/Compiler/PackageQueryGraph.h"
#include "chtholly/Compiler/Parser.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace chtholly {
namespace {
#include "CompilerPipelineSupportInternal.h"

} // namespace

struct CompilerPreparedRequest::Impl {
  CompilerInvocation invocation;
  CompilerCompilerEnvironment environment;
  CompilerDriverPlan plan;
  std::shared_ptr<const CompilerRequestSnapshot> snapshot;
};

struct CompilerPreparedRequestAccess {
  static const CompilerPreparedRequest::Impl &
  get(const CompilerPreparedRequest &value) {
    return *value.impl_;
  }
};

CompilerPreparedRequest::CompilerPreparedRequest(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CompilerPreparedRequest::~CompilerPreparedRequest() = default;

const CompilerRequestSnapshot &CompilerPreparedRequest::snapshot() const {
  return *impl_->snapshot;
}

std::shared_ptr<const CompilerPreparedRequest>
prepareNextCompilerRequest(const CompilerInvocation &invocation,
                           const CompilerCompilerEnvironment &environment,
                           std::string &error) {
  if (!validateCompilerInvocation(invocation, error))
    return {};
  if (!environment.input_files) {
    error = "compiler compiler environment has no input file system";
    return {};
  }
  const auto &file_system = *environment.input_files;
  auto stable_plan = resolveStableNextDriverPlan(
      invocation, file_system,
      environment.update_lockfile && !invocation.suppress_lockfile_update,
      error);
  if (!stable_plan)
    return {};
  auto snapshot_paths = sourceSnapshotPaths(stable_plan->plan);
  auto source_snapshot =
      CompilerSourceSnapshot::capture(file_system, snapshot_paths, error);
  if (!source_snapshot)
    return {};
  auto request_snapshot = std::make_shared<const CompilerRequestSnapshot>(
      std::move(stable_plan->controls), std::move(*source_snapshot));
  if (!verifyStandardLibrarySnapshotIdentity(stable_plan->plan, file_system,
                                             error) ||
      !verifyRequestSnapshotBarrier(invocation, stable_plan->plan,
                                    *request_snapshot, file_system, error))
    return {};
  auto impl = std::make_unique<CompilerPreparedRequest::Impl>();
  impl->invocation = invocation;
  impl->environment = environment;
  impl->plan = std::move(stable_plan->plan);
  impl->snapshot = std::move(request_snapshot);
  return std::shared_ptr<const CompilerPreparedRequest>(
      new CompilerPreparedRequest(std::move(impl)));
}

namespace {
struct CheckPackageResult {
  std::shared_ptr<const compiler::CompilationSession> session;
  std::shared_ptr<compiler::CompilationSession> failed_session;
  compiler::CompilationOutcome outcome = compiler::CompilationOutcome::Failed;
  std::string error;
  bool cache_hit = false;
  bool cache_queried = false;
};

std::string checkCacheKey(const CompilerPackagePlan &package,
                          const CompilerDriverPlan &plan,
                          const CompilerRequestSnapshot &snapshot,
                          std::span<const CheckPackageResult> completed) {
  std::ostringstream canonical;
  canonical << "chtholly.next.daemon-package-query.v2\n";
  appendCanonicalField(canonical, package.package_name);
  appendCanonicalField(canonical, package.language_version.str());
  appendCanonicalField(canonical, plan.build.target.info.triple);
  appendCanonicalField(canonical,
                       snapshot.controls().compileToolchainFingerprint().hex());
  appendCanonicalField(canonical, package.package_contract_fingerprint.hex());
  canonical << (package.is_standard_library ? "toolchain-stdlib\n"
                                            : "workspace\n");
  for (const auto &feature : package.resolved_features)
    appendCanonicalField(canonical, feature);
  for (const auto &path : package.sources) {
    appendCanonicalField(canonical, path);
    const auto *source = snapshot.sources().find(path);
    appendCanonicalField(canonical,
                         source ? source->fingerprint().hex() : std::string{});
  }
  for (const auto dependency : package.dependencies) {
    const auto &result = completed[dependency];
    appendCanonicalField(canonical, plan.packages[dependency].package_name);
    appendCanonicalField(
        canonical,
        result.session
            ? result.session->packageCheckArtifact().fingerprint().hex()
            : std::string{});
  }
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str()).hex();
}

void appendSessionDiagnostics(const compiler::CompilationSession &session,
                              std::vector<CompilerSourceDiagnostic> &output) {
  for (std::uint32_t index = 0; index < session.unitCount(); ++index) {
    const auto &unit = session.unit(compiler::CheckIRId(index));
    for (const auto &diagnostic : unit.diagnostics()) {
      output.push_back(
          {.path = std::string(unit.sourcePath()),
           .level = compiler::diagnosticLevel(diagnostic.kind),
           .code = std::string(compiler::diagnosticCode(diagnostic.kind)),
           .message = compiler::diagnosticMessage(diagnostic),
           .offset = diagnostic.offset,
           .length = diagnostic.length,
           .location = unit.source().lineColumn(diagnostic.offset),
           .related = [&] {
             std::vector<CompilerSourceDiagnostic::Related> related;
             related.reserve(diagnostic.notes.size());
             for (const auto &note : diagnostic.notes) {
               const auto path = note.path.empty()
                                     ? std::string(unit.sourcePath())
                                     : note.path;
               compiler::LineColumn location;
               bool location_available = false;
               if (note.path.empty()) {
                 location = unit.source().lineColumn(note.offset);
                 location_available = true;
               } else {
                 // Imported source may still be part of this package session.
                 for (std::uint32_t source_index = 0;
                      source_index < session.unitCount(); ++source_index) {
                   const auto &candidate =
                       session.unit(compiler::CheckIRId(source_index));
                   if (candidate.sourcePath() != path)
                     continue;
                   location = candidate.source().lineColumn(note.offset);
                   location_available = true;
                   break;
                 }
               }
               related.push_back(
                   {.level = note.level,
                    .code = note.code.empty()
                                ? std::string(compiler::diagnosticCode(note.kind))
                                : note.code,
                    .message = note.message.empty()
                                   ? compiler::diagnosticMessage(
                                         compiler::Diagnostic{
                                             note.kind, note.offset, note.length,
                                             compiler::TokenKind::Invalid,
                                             compiler::TokenKind::Invalid, {}})
                                   : note.message,
                    .path = path,
                    .offset = note.offset,
                    .length = note.length,
                    .location = location,
                    .location_available = location_available});
             }
             return related;
           }()});
    }
  }
}
} // namespace

CompilerCheckExecutionResult
executeNextCheckRequest(const CompilerPreparedRequest &request,
                        CompilerPackageQueryCache &cache,
                        const CompilerCancellationToken &cancellation) {
  const auto &prepared = CompilerPreparedRequestAccess::get(request);
  CompilerCheckExecutionResult output;
  output.snapshot = prepared.snapshot;
  if (cancellation.isCancelled()) {
    output.status = CompilerDaemonRequestStatus::Cancelled;
    return output;
  }
  if (!verifyRequestSnapshotBarrier(
          prepared.invocation, prepared.plan, *prepared.snapshot,
          *prepared.environment.input_files, output.error)) {
    output.status = CompilerDaemonRequestStatus::Failed;
    return output;
  }

  std::vector<compiler::PackageQueryNode> nodes;
  nodes.reserve(prepared.plan.packages.size());
  for (const auto &package : prepared.plan.packages) {
    std::vector<compiler::PackageQueryId> dependencies;
    for (const auto dependency : package.dependencies)
      dependencies.emplace_back(static_cast<std::uint32_t>(dependency));
    nodes.emplace_back(package.package_name, std::move(dependencies));
  }
  compiler::CompilerPackageQueryGraph graph(std::move(nodes));
  if (!graph.initialize(output.error))
    return output;
  std::vector<CheckPackageResult> results(prepared.plan.packages.size());
  if (results.empty()) {
    output.error = "compiler package query graph is empty";
    return output;
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::size_t completed_count = 0;
  bool stop = false;
  const auto worker_count = std::min(
      results.size(), std::max<std::size_t>(1, prepared.invocation.jobs));
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      while (true) {
        std::size_t index = 0;
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, [&] {
            return stop || cancellation.isCancelled() ||
                   completed_count == results.size() || graph.hasReadyQuery();
          });
          if (stop || cancellation.isCancelled() ||
              completed_count == results.size())
            return;
          index = graph.takeReadyQuery()->index;
        }

        CheckPackageResult result;
        const auto &package = prepared.plan.packages[index];
        const auto key =
            checkCacheKey(package, prepared.plan, *prepared.snapshot, results);
        result.cache_queried = true;
        if (auto cached = cache.lookup(key)) {
          result.session = std::move(cached);
          result.outcome = compiler::CompilationOutcome::Success;
          result.cache_hit = true;
        } else if (!cancellation.isCancelled()) {
          auto session = std::make_shared<compiler::CompilationSession>(
              prepared.plan.build.target.info.triple, package.package_name,
              package.resolved_features,
              prepared.snapshot->controls().compileToolchainFingerprint(),
              packageProvenance(package), packageLanguageContract(package));
          for (const auto &source_path : package.sources) {
            const auto *source = prepared.snapshot->sources().find(source_path);
            if (!source) {
              result.error = "compiler request snapshot omitted planned input '" +
                             source_path + "'";
              break;
            }
            (void)session->addUnit(
                prepared.snapshot->sources().sourceInput(*source),
                compilationUnitKindForPath(source_path));
          }
          if (result.error.empty()) {
            compiler::CompilationRequest compilation_request;
            compilation_request.mode = compiler::CompilationRequest::Mode::Check;
            compilation_request.component_identity = package.component_identity;
            compilation_request.component_exports = package.component_exports;
            compilation_request.is_cancelled = [&cancellation] {
              return cancellation.isCancelled();
            };
            for (const auto dependency : package.dependencies)
              compilation_request.dependency_check_artifacts.push_back(
                  &results[dependency].session->packageCheckArtifact());
            for (const auto dependency :
                 templateDependencyClosure(index, prepared.plan))
              compilation_request.template_dependency_check_closure.push_back(
                  &results[dependency].session->packageCheckArtifact());
            result.outcome =
                session->compileRequest(result.error, compilation_request);
          }
          if (result.outcome == compiler::CompilationOutcome::Success) {
            result.session = session;
            if (!cancellation.isCancelled())
              cache.insert(key, result.session);
          } else {
            result.failed_session = std::move(session);
          }
        } else {
          result.outcome = compiler::CompilationOutcome::Cancelled;
        }

        {
          std::lock_guard lock(mutex);
          results[index] = std::move(result);
          ++completed_count;
          if (results[index].outcome != compiler::CompilationOutcome::Success) {
            std::string transition_error;
            (void)graph.markFailed(
                compiler::PackageQueryId(static_cast<std::uint32_t>(index)),
                transition_error);
            stop = true;
          } else {
            std::string transition_error;
            if (!graph.markSucceeded(
                    compiler::PackageQueryId(static_cast<std::uint32_t>(index)),
                    transition_error)) {
              output.error = std::move(transition_error);
              stop = true;
            }
          }
        }
        changed.notify_all();
      }
    });
  }
  changed.notify_all();
  for (auto &worker : workers)
    worker.join();

  if (cancellation.isCancelled()) {
    output.status = CompilerDaemonRequestStatus::Cancelled;
    return output;
  }
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (results[index].cache_queried) {
      output.cache_hits += results[index].cache_hit ? 1 : 0;
      output.cache_misses += results[index].cache_hit ? 0 : 1;
    }
    if (results[index].session)
      output.package_sessions.push_back(results[index].session);
    if (results[index].failed_session) {
      output.package_sessions.push_back(results[index].failed_session);
      appendSessionDiagnostics(*results[index].failed_session,
                               output.diagnostics);
    }
    if (!results[index].error.empty() && output.error.empty())
      output.error = "package '" + prepared.plan.packages[index].package_name +
                     "': " + results[index].error;
  }
  output.symbol_index =
      CompilerWorkspaceSymbolIndex::build(output.package_sessions);
  if (stop || completed_count != results.size()) {
    output.status = CompilerDaemonRequestStatus::Failed;
    return output;
  }
  output.status = CompilerDaemonRequestStatus::Succeeded;
  return output;
}

int runCompilerPipeline(
    const CompilerInvocation &invocation,
    const CompilerCompilerEnvironment &environment, std::string &error,
    std::vector<WorkspaceArtifactResult::InvalidationExplanation>
        *invalidation_explanations,
    std::vector<CompilerSourceDiagnostic> *diagnostics) {
  if (invalidation_explanations)
    invalidation_explanations->clear();
  if (diagnostics)
    diagnostics->clear();
  auto prepared = prepareNextCompilerRequest(invocation, environment, error);
  if (!prepared)
    return 1;
  auto &prepared_impl = *prepared->impl_;
  const auto &file_system = *prepared_impl.environment.input_files;
  auto *plan = &prepared_impl.plan;
  const auto &request_snapshot = *prepared_impl.snapshot;

  CompilerArtifactStore artifact_store(plan->store_root);
  auto lease = artifact_store.acquireLease(
      plan->session_key, plan->build.target.info.triple,
      plan->packages[plan->root_package].package_name, error);
  if (!lease)
    return 1;
  const auto &previous_manifests = lease->previousManifests();

  const auto artifact_worker_count =
      invocation.jobs <= 1 ? 0 : std::min<std::size_t>(4, invocation.jobs);
  std::shared_ptr<CompilerArtifactLoadMetrics> artifact_load_metrics;
  if (!invocation.compiler_artifact_load_metrics_output_path.empty()) {
    artifact_load_metrics = std::make_shared<CompilerArtifactLoadMetrics>(
        invocation.jobs, artifact_worker_count,
        std::max<std::size_t>(1, artifact_worker_count * 4));
    artifact_load_metrics->recordArchiveInstallSummary(
        plan->archive_install_attempts, plan->archive_install_closure_hits,
        plan->archive_install_fresh_installs, plan->archive_install_bytes);
  }
  auto artifact_loads = std::make_unique<CompilerArtifactLoadExecutor>(
      artifact_worker_count, std::function<bool()>{}, artifact_load_metrics);

  std::vector<PackageQueryResult> query_results;
  if (!executePackageQueryGraph(
          invocation, *plan, previous_manifests, *lease,
          request_snapshot.sources(),
          request_snapshot.controls().compileToolchainFingerprint(),
          *artifact_loads, artifact_load_metrics.get(), query_results, error)) {
    const auto compile_error = error;
    if (diagnostics) {
      for (const auto &result : query_results)
        if (result.session)
          appendSessionDiagnostics(*result.session, *diagnostics);
    }
    artifact_loads->cancelPending();
    artifact_loads->drain();
    const auto failure_metrics =
        artifact_load_metrics ? artifact_load_metrics->json() : std::string{};
    artifact_loads.reset();
    std::string report_error;
    if (!writeArtifactLoadMetrics(invocation, failure_metrics, report_error))
      error = compile_error +
              "; failed to write artifact-load metrics: " + report_error;
    else
      error = compile_error;
    return 1;
  }
  artifact_loads->drain();
  const auto artifact_load_metrics_json =
      artifact_load_metrics ? artifact_load_metrics->json() : std::string{};
  artifact_loads.reset();
  if (!writeArtifactLoadMetrics(invocation, artifact_load_metrics_json, error))
    return 1;
  if (invalidation_explanations) {
    for (std::size_t index = 0; index < query_results.size(); ++index)
      appendInvalidationExplanations(
          plan->packages[index].package_name,
          query_results[index].session->compilationPlan(),
          *invalidation_explanations);
  }

  for (std::size_t package_index = 0; package_index < query_results.size();
       ++package_index) {
    const auto &session = *query_results[package_index].session;
    for (std::uint32_t index = 0; index < session.unitCount(); ++index) {
      const auto &unit = session.unit(compiler::CheckIRId(index));
      const auto is_root_entry =
          package_index == plan->root_package &&
          index == query_results[package_index].root_id.index;
      if (unit.hasEntryPoint() && !is_root_entry) {
        error = "only the requested compiler root module may define main";
        return 1;
      }
      if (plan->packages[package_index].component_abi != 0 &&
          unit.hasEntryPoint()) {
        error = "component packages cannot define main";
        return 1;
      }
      if (invocation.action == DriverAction::EmitExecutable && is_root_entry &&
          plan->packages[package_index].component_abi == 0 &&
          !unit.hasEntryPoint()) {
        error = "the requested compiler root module has no main function";
        return 1;
      }
    }
  }

  for (std::size_t package_index = 0; package_index < query_results.size();
       ++package_index) {
    const auto &package = plan->packages[package_index];
    if (!package.is_standard_library)
      continue;
    const auto &manifest =
        query_results[package_index].session->packageManifest();
    std::map<std::string, std::string> mapped_runtime_symbols;
    for (const auto &mapping : plan->build.runtime_symbol_mappings)
      mapped_runtime_symbols.emplace(mapping.first, mapping.second);
    for (const auto &module_name : package.expected_modules) {
      const auto *artifact = manifest.findModule(module_name);
      if (!artifact) {
        error = "compiler standard-library manifest omitted expected reachable "
                "module '" +
                module_name + "'";
        return 1;
      }
      std::vector<compiler::ModuleIdentity> expected_dependencies;
      for (const auto &dependency :
           package.expected_module_dependencies.at(module_name))
        expected_dependencies.push_back(
            {.package_name = package.package_name, .module_name = dependency});
      std::ranges::sort(expected_dependencies);
      std::vector<std::string> logical_symbols;
      for (const auto &symbol : artifact->required_foreign_symbols)
        logical_symbols.push_back(symbol.logical_name);
      std::ranges::sort(logical_symbols);
      if (artifact->module_dependencies != expected_dependencies ||
          logical_symbols != package.expected_runtime_symbols.at(module_name)) {
        error = "compiler standard-library module '" + module_name +
                "' does not match its manifest link closure";
        return 1;
      }
      for (const auto &symbol : artifact->required_foreign_symbols) {
        const auto mapping = mapped_runtime_symbols.find(symbol.logical_name);
        if (mapping == mapped_runtime_symbols.end() ||
            mapping->second != symbol.external_symbol) {
          error = "compiler standard-library module '" + module_name +
                  "' requires runtime symbol '" + symbol.logical_name +
                  "' without an exact manifest mapping";
          return 1;
        }
      }
    }
  }

  if (!writeAnalysisMetrics(invocation, query_results, error))
    return 1;

  if (invocation.workflow == DriverWorkflow::Check) {
    auto &root_session = *query_results[plan->root_package].session;
    const auto &root =
        root_session.unit(query_results[plan->root_package].root_id);
    if (!writeNextDumps(invocation, root_session, root, error))
      return 1;
    std::cout << "checked\t" << plan->build.package_name << '\n';
    return 0;
  }

  std::vector<std::string> object_paths;
  std::map<std::string, std::string> object_paths_by_module;
  std::map<std::string, const compiler::PackageModuleArtifact *>
      artifacts_by_module;
  std::vector<CompilerPublishedObject> published_objects;
  std::vector<CompilerPublishedSpecialization> published_specializations;
  std::vector<CompilerPublishedNominalTypeSpecific> published_nominal_specifics;
  std::vector<CompilerPublishedNominalSemanticWitness>
      published_nominal_semantic_witnesses;
  std::vector<CompilerPublishedNominalTypeLayout> published_nominal_layouts;
  std::vector<const compiler::CompilerPackageArtifactManifest *> published_manifests;
  published_manifests.reserve(query_results.size());
  for (const auto &query : query_results)
    published_manifests.push_back(&query.session->packageManifest());
  CompilerArtifactPublicationState publication_state{
      *plan, artifact_store, query_results, object_paths, object_paths_by_module,
      artifacts_by_module, published_objects, published_specializations,
      published_nominal_specifics, published_nominal_semantic_witnesses,
      published_nominal_layouts};
  if (!CompilerArtifactPublicationService::collect(publication_state, error))
    return 1;

  auto &root_session = *query_results[plan->root_package].session;
  if (!verifyRequestSnapshotBarrier(invocation, *plan, request_snapshot,
                                    file_system, error))
    return 1;
  if (!artifact_store.publish(
          *lease, root_session.packageManifest(), published_manifests,
          published_objects, published_specializations,
          published_nominal_specifics, published_nominal_semantic_witnesses,
          published_nominal_layouts, error))
    return 1;
  CompilerGarbageCollectionReport gc_report;
  std::string gc_error;
  if (!artifact_store.collectGarbage(false, std::chrono::hours(24), gc_report,
                                     gc_error))
    std::cerr << "chthollyc: warning: " << gc_error << '\n';
  if (artifact_load_metrics) {
    artifact_load_metrics->recordArtifactStoreReport(gc_report);
    std::string metrics_error;
    if (!writeArtifactLoadMetrics(invocation, artifact_load_metrics->json(),
                                  metrics_error))
      std::cerr << "chthollyc: warning: failed to update artifact-load metrics: "
                << metrics_error << '\n';
  }
  lease.reset();

  const auto &root =
      root_session.unit(query_results[plan->root_package].root_id);
  if (!writeNextDumps(invocation, root_session, root, error))
    return 1;
  if (invocation.action == DriverAction::EmitLLVM) {
    return writeNextOutput(plan->output_path, root.printLLVM(), error) ? 0 : 1;
  }
  if (invocation.action == DriverAction::EmitObject) {
    const auto object = root.emitObject(error);
    return error.empty() && writeNextOutput(plan->output_path, object, error)
               ? 0
               : 1;
  }

  const auto resources = locateCompilerResources(invocation, error);
  if (!resources)
    return 1;
  auto library_search_paths = invocation.library_search_paths;
  auto libraries = invocation.link_libraries;
  for (const auto &package : plan->packages) {
    for (const auto &path : package.native_library_paths)
      appendUniqueLinkValue(library_search_paths, path);
    for (const auto &library : package.native_link_libraries)
      appendUniqueLinkValue(libraries, library);
  }
  appendUniqueLinkValue(libraries, resources->runtime_library_path);
  for (const auto &library : resources->runtime_link_libraries)
    appendUniqueLinkValue(libraries, library);
  // Container intrinsics are emitted only for builds that use the generic
  // bridge. Keep the bridge archive outside the legacy runtime manifest, but
  // make it available to the linker whenever the resource distribution ships
  // it. This preserves runtime-v1 symbol closure while allowing generated
  // container calls to resolve their C ABI entry points.
  if (!resources->runtime_library_path.empty()) {
#if defined(_WIN32)
    constexpr std::string_view container_archive =
        "chtholly_next_container_v1.lib";
#else
    constexpr std::string_view container_archive =
        "libchtholly_next_container_v1.a";
#endif
    const auto candidate =
        std::filesystem::path(resources->runtime_library_path).parent_path() /
        container_archive;
    std::error_code container_error;
    if (std::filesystem::is_regular_file(candidate, container_error))
      appendUniqueLinkValue(libraries, candidate.string());
  }
  appendHostedRuntimeSystemLibraries(libraries, plan->build.target);
  const auto component = plan->packages[plan->root_package].component_abi ==
                         compiler::ComponentAbiEpoch;
  const auto &root_unit =
      root_session.unit(query_results[plan->root_package].root_id);
  if (!CompilerLinkClosureService::collect(
          *plan, plan->root_package, root_unit, object_paths_by_module,
          artifacts_by_module, component,
          [](std::string_view symbol) {
            return isHostedAsyncRuntimeSymbol(symbol);
          },
          object_paths, error))
    return 1;
  std::string descriptor_object_path;
  std::string component_contract_bytes;
  if (component) {
    auto contract = root_session.componentContract(error);
    if (!contract)
      return 1;
    component_contract_bytes = contract->encode(error);
    auto descriptor = compiler::emitComponentDescriptorObject(
        *contract, plan->build.target.info.triple, HostedRuntimeAbiVersion,
        error);
    if (!error.empty() || descriptor.empty())
      return 1;
    descriptor_object_path = temporaryObjectPathForExecutable(
        plan->output_path + ".descriptor", plan->build.target);
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::filesystem::path(descriptor_object_path).parent_path(),
        directory_error);
    if (directory_error) {
      error = "failed to create component descriptor directory: " +
              directory_error.message();
      return 1;
    }
    if (!writeTextFile(descriptor_object_path, descriptor, error))
      return 1;
    object_paths.push_back(descriptor_object_path);
  }
  const auto linked =
      component ? linkNativeSharedLibrary(
                      object_paths, plan->output_path, library_search_paths,
                      libraries, plan->build.target, error,
                      std::array<std::string, 1>{"chtholly_component_query_v1"})
                : linkNativeExecutable(object_paths, plan->output_path,
                                       library_search_paths, libraries,
                                       plan->build.target, error);
  if (!descriptor_object_path.empty()) {
    std::error_code remove_error;
    removeFile(descriptor_object_path, remove_error);
  }
  if (!linked)
    return 1;
  if (component && !writeTextFile(plan->output_path + ".chcomponent",
                                  component_contract_bytes, error))
    return 1;
  if (invocation.workflow == DriverWorkflow::Build) {
    std::cout << "built\t" << plan->build.package_name << '\t'
              << (component ? "component" : "executable") << '\t'
              << plan->output_path << "\ttarget\t"
              << plan->build.target.info.triple << '\n';
  }
  if (invocation.workflow == DriverWorkflow::Run) {
    if (!plan->build.target.is_host_compatible) {
      error = "run cannot execute target '" + plan->build.target.info.triple +
              "' on this host";
      return 1;
    }
    const auto exit_code = runProcessPassthrough(
        plan->output_path, invocation.program_arguments, error);
    return exit_code ? *exit_code : 1;
  }
  return 0;
}

int runCompilerPipeline(
    const CompilerInvocation &invocation, std::string &error,
    std::vector<WorkspaceArtifactResult::InvalidationExplanation>
        *invalidation_explanations,
    std::vector<CompilerSourceDiagnostic> *diagnostics) {
  CompilerCompilerEnvironment environment{.input_files =
                                          makeCompilerRealInputFileSystem()};
  return runCompilerPipeline(invocation, environment, error,
                                 invalidation_explanations, diagnostics);
}

} // namespace chtholly
