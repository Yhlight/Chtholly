#pragma once

// Private planning, snapshot, output, and package-query support for the
// compiler pipeline. Included inside the pipeline's anonymous namespace so
// the public Driver API remains unchanged.

bool isHostedAsyncRuntimeSymbol(std::string_view symbol) {
  static constexpr std::string_view Symbols[] = {
      "chtholly_compiler_hosted_async_v1_completion_arm",
      "chtholly_compiler_hosted_async_v1_completion_detach",
      "chtholly_compiler_hosted_async_v1_completion_poll",
      "chtholly_compiler_hosted_async_v1_completion_wait",
      "chtholly_compiler_hosted_async_v1_scheduler_resume",
      "chtholly_compiler_hosted_async_v1_scope_request_cancel",
      "chtholly_compiler_hosted_async_v1_task_is_cancelled",
      "chtholly_compiler_hosted_async_v1_task_request_cancel",
      "chtholly_compiler_hosted_async_v1_subscription_cancel",
      "chtholly_compiler_hosted_async_v1_subscription_cancel_async",
      "chtholly_compiler_hosted_async_v1_subscription_register",
      "chtholly_compiler_hosted_async_v1_subscription_unregister"};
  return std::ranges::find(Symbols, symbol) != std::end(Symbols);
}

std::string moduleIdentityKey(std::string_view package,
                              std::string_view module) {
  return std::string(package) + "\n" + std::string(module);
}

compiler::PackageProvenance packageProvenance(const CompilerPackagePlan &package) {
  return CompilerPipelinePlanningService::packageProvenance(package);
}

LanguageContract packageLanguageContract(const CompilerPackagePlan &package) {
  return CompilerPipelinePlanningService::packageLanguageContract(package);
}

compiler::CompilationUnitKind compilationUnitKindForPath(std::string_view path) {
  return CompilerPipelinePlanningService::compilationUnitKindForPath(path);
}

std::optional<CompilerSourceInventory> inspectNextSourceInventory(
    std::span<const std::string> source_paths, LanguageVersion language_version,
    const CompilerInputFileSystem &file_system, std::string &error) {
  return CompilerPipelinePlanningService::inspectSourceInventory(
      source_paths, language_version, file_system, error);
}

std::string safePathComponent(std::string text) {
  for (auto &character : text) {
    if (!std::isalnum(static_cast<unsigned char>(character)) &&
        character != '-' && character != '_' && character != '.')
      character = '_';
  }
  return text;
}

void appendCanonicalField(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string uniqueTemporaryPath(const std::string &path) {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return path + ".tmp." + std::to_string(stamp) + "." +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool atomicWriteNextFile(const std::string &path, const std::string &bytes,
                         std::string &error) {
  std::error_code file_error;
  const auto parent = pathForFileSystem(path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, file_error);
  if (file_error) {
    error = "failed to create compiler output directory: " + file_error.message();
    return false;
  }
  const auto temporary = uniqueTemporaryPath(path);
  if (!writeTextFile(temporary, bytes, error))
    return false;
  if (!replaceFile(temporary, path, file_error)) {
    error = "failed to publish compiler output: " + file_error.message();
    removeFile(temporary, file_error);
    return false;
  }
  return true;
}

bool writeNextOutput(const std::string &path, const std::string &contents,
                     std::string &error) {
  if (path == "-") {
    std::cout << contents;
    return true;
  }
  return atomicWriteNextFile(path, contents, error);
}

std::optional<std::vector<std::string>>
collectNextSources(std::span<const std::string> module_roots,
                   std::string_view entry_path, bool include_entry,
                   const CompilerInputFileSystem &file_system, std::string &error) {
  std::set<std::string> sources;
  const auto normalized_entry =
      entry_path.empty() ? std::string{} : normalizeCompilerInputPath(entry_path);
  if (include_entry && !normalized_entry.empty())
    sources.insert(normalized_entry);
  for (const auto &root_text : module_roots) {
    std::vector<std::string> root_sources;
    if (!file_system.enumerateSources(root_text, root_sources, error))
      return std::nullopt;
    for (auto &source : root_sources) {
      if (include_entry || source != normalized_entry)
        sources.insert(std::move(source));
    }
  }
  return std::vector<std::string>(sources.begin(), sources.end());
}

std::optional<CompilerDriverPlan>
resolveNextDriverPlan(const CompilerInvocation &invocation,
                      const CompilerInputFileSystem &file_system,
                      std::string &error) {
  auto build = resolveNextBuildPlan(invocation, error);
  if (!build)
    return std::nullopt;
  if (build->packages.empty() ||
      build->root_package >= build->packages.size()) {
    error = "compiler build plan has no selected root package";
    return std::nullopt;
  }
  const auto &root = build->packages[build->root_package];
  if (root.isComponent() &&
      (!build->target.info.triple.starts_with("x86_64") ||
       (build->target.info.triple.find("windows") == std::string::npos &&
        build->target.info.triple.find("linux") == std::string::npos))) {
    error = "component ABI epoch 1 supports only Windows x64 and Linux x64";
    return std::nullopt;
  }
  if (root.isComponent() && invocation.workflow == DriverWorkflow::Run) {
    error = "component packages support check and build, not run";
    return std::nullopt;
  }
  if (root.entry_path.empty() && !root.isComponent() &&
      invocation.workflow != DriverWorkflow::Check) {
    error = "the selected compiler package requires build.entry";
    return std::nullopt;
  }
  auto root_source = normalizeCompilerInputPath(root.entry_path);
  if (!root_source.empty() &&
      pathForFileSystem(root_source).extension() != ".cns") {
    error = "compiler build.entry must name a .cns source; CFDL modules are "
            "library-only providers";
    return std::nullopt;
  }

  std::filesystem::path cache_base;
  if (!invocation.cache_dir.empty()) {
    cache_base = invocation.cache_dir;
  } else if (!build->workspace_root.empty()) {
    cache_base =
        std::filesystem::path(build->workspace_root) / ".chtholly" / "cache";
  } else if (!build->project_root.empty()) {
    cache_base =
        std::filesystem::path(build->project_root) / ".chtholly" / "cache";
  } else {
    auto parent = std::filesystem::path(invocation.output_path).parent_path();
    if (parent.empty())
      parent = ".";
    cache_base = parent / ".chtholly" / "cache";
  }
  // Keep compiler-owned caches isolated by every persisted semantic contract
  // that can make an old reference or component unsafe to replay.  This is a
  // path namespace only: artifact bytes and their wire formats remain
  // unchanged, and older namespaces are never migrated implicitly.
  const auto compiler_cache_namespace =
      std::string("next-v47-sem") +
      std::to_string(CurrentSemanticArtifactEpoch) + "-sl" +
      std::to_string(CurrentStandardLibraryEpoch) + "-api" +
      std::to_string(CompilerStandardLibraryApiEpoch) + "-sf" +
      std::to_string(CompilerStandardLibraryFormatVersion) + "-cc" +
      std::to_string(CompilerCompilerContractEpoch) + "-sc" +
      std::to_string(compiler::ConcreteSpecializationComponentFormat) + "-" +
      safePathComponent(build->target.info.triple);
  const auto compiler_root = cache_base / compiler_cache_namespace;
  ArtifactStore package_store((cache_base / "packages").string());
  std::uint64_t archive_install_attempts = 0;
  std::uint64_t archive_install_closure_hits = 0;
  std::uint64_t archive_install_fresh_installs = 0;
  std::uint64_t archive_install_bytes = 0;

  std::vector<CompilerPackagePlan> packages;
  packages.reserve(build->packages.size());
  for (std::size_t index = 0; index < build->packages.size(); ++index) {
    const auto &package = build->packages[index];
    const bool is_root = index == build->root_package;
    if (package.name == "std") {
      error =
          "compiler reserves package name 'std' for the toolchain standard library";
      return std::nullopt;
    }
    auto source_entry = is_root ? root_source : package.entry_path;
    auto sources = collectNextSources(package.module_roots, source_entry,
                                      is_root, file_system, error);
    if (!sources)
      return std::nullopt;
    if (is_root && root_source.empty()) {
      const auto first_source =
          std::ranges::find_if(*sources, [](const std::string &source) {
            return pathForFileSystem(source).extension() == ".cns";
          });
      if (first_source == sources->end()) {
        error = "the selected library package has no .cns source to check";
        return std::nullopt;
      }
      root_source = *first_source;
      source_entry = root_source;
    }
    if (is_root && !std::ranges::binary_search(*sources, root_source)) {
      error = "compiler build entry is not part of the selected root package";
      return std::nullopt;
    }
    auto interop_bundle_path = package.interop_bundle_path;
    auto interop_bundle_digest = package.interop_bundle_digest;
    if (!package.artifact_archive_path.empty()) {
      ArtifactStoreInstallObservation observation;
      auto installed = package_store.install(package.artifact_archive_path,
                                             error, &observation);
      if (!installed)
        return std::nullopt;
      ++archive_install_attempts;
      archive_install_bytes += observation.archive_bytes;
      if (observation.closure_hit)
        ++archive_install_closure_hits;
      else
        ++archive_install_fresh_installs;
      if (installed->archive_sha256 != package.artifact_archive_digest) {
        error = "compiler dependency archive changed during installation: '" +
                package.artifact_archive_path + "'";
        return std::nullopt;
      }
      const auto locator = renderArtifactStoreLocator(
          {installed->artifact_identity, installed->closure_digest});
      if (installed->root_manifest_relative_path.empty()) {
        error = "compiler dependency archive has no root manifest";
        return std::nullopt;
      }
      auto root_manifest = package_store.filePath(
          locator, installed->root_manifest_relative_path, error);
      if (!root_manifest)
        return std::nullopt;
      auto root_text = readTextFile(*root_manifest, error);
      if (!root_text)
        return std::nullopt;
      auto artifact_manifest =
          parsePackageArtifactManifest(*root_text, *root_manifest, error);
      if (!artifact_manifest)
        return std::nullopt;
      if (artifact_manifest->package_name != package.name) {
        error = "compiler dependency archive package identity mismatch for '" +
                package.name + "'";
        return std::nullopt;
      }
      if (artifact_manifest->interop_bundle) {
        const auto root_directory =
            std::filesystem::path(installed->root_manifest_relative_path)
                .parent_path();
        auto sidecar = package_store.filePath(
            locator,
            (root_directory /
             std::filesystem::path(
                 artifact_manifest->interop_bundle->relative_path))
                .generic_string(),
            error);
        if (!sidecar)
          return std::nullopt;
        interop_bundle_path = *sidecar;
        interop_bundle_digest = artifact_manifest->interop_bundle->sha256;
      }
    }
    packages.push_back(
        {.package_name = package.name,
         .language_version = package.language_version,
         .sources = std::move(*sources),
         .module_roots = package.module_roots,
         .source_entry = source_entry,
         .root_source = is_root ? root_source : std::string{},
         .interop_bundle_path = std::move(interop_bundle_path),
         .interop_bundle_digest = std::move(interop_bundle_digest),
         .artifact_archive_path = package.artifact_archive_path,
         .artifact_archive_digest = package.artifact_archive_digest,
         .resolved_features = package.resolved_features,
         .dependencies = package.dependencies,
         .native_library_paths = package.native_library_paths,
         .native_link_libraries = package.native_link_libraries,
         .cffi_receipt_path = package.cffi_receipt_path,
         .cffi_receipt_digest = package.cffi_receipt_digest,
         .cffi_identity = package.cffi_identity,
         .cffi_required = package.cffi_required,
         .component_abi = package.component_abi,
         .component_identity = package.component_identity,
         .component_exports = package.component_exports,
         .include_entry = is_root,
         .is_root = is_root});
  }
  std::size_t root_package = build->root_package;

  bool needs_standard_library = false;
  std::vector<bool> package_uses_standard_library(packages.size());
  std::vector<CompilerSourceInventory> package_inventories;
  package_inventories.reserve(packages.size());
  for (std::size_t index = 0; index < packages.size(); ++index) {
    auto inventory = inspectNextSourceInventory(
        packages[index].sources, packages[index].language_version, file_system,
        error);
    if (!inventory)
      return std::nullopt;
    if (inventory->uses_candidate_async &&
        packages[index].language_version < FrozenV11LanguageVersion) {
      error = "Chtholly " + packages[index].language_version.str() +
              " package '" + packages[index].package_name +
              "' uses async syntax introduced in 1.1";
      return std::nullopt;
    }
    for (const auto &module : inventory->declared_modules) {
      if (module == "std" || module.starts_with("std::")) {
        error = "compiler package '" + packages[index].package_name +
                "' cannot declare reserved standard-library module '" + module +
                "'";
        return std::nullopt;
      }
    }
    package_uses_standard_library[index] = inventory->imports_standard_library;
    needs_standard_library =
        needs_standard_library || inventory->imports_standard_library;
    std::string package_contract =
        "chtholly.next.workspace-package-contract.v2\n" +
        packages[index].package_name + "\n" +
        packages[index].component_identity + "\n" +
        std::to_string(packages[index].component_abi) + "\n";
    for (const auto &entry : packages[index].component_exports)
      package_contract += entry + "\n";
    packages[index].package_contract_fingerprint =
        compiler::StableFingerprint::fromCanonicalBytes(package_contract);
    package_inventories.push_back(std::move(*inventory));
  }

  std::string standard_library_manifest_path;
  if (needs_standard_library) {
    auto standard_library = CompilerStandardLibraryManifest::load(
        build->resource_dir, file_system, error);
    if (!standard_library)
      return std::nullopt;
    CompilerPackagePlan standard_package;
    standard_package.package_name =
        std::string(standard_library->packageName());
    standard_package.language_version = LatestLanguageVersion;
    standard_package.module_roots = {std::string(standard_library->rootPath())};
    standard_package.package_contract_fingerprint =
        standard_library->distributionFingerprint();
    standard_package.is_standard_library = true;
    std::set<std::string> reachable_modules;
    std::vector<std::string> pending_modules;
    for (std::size_t index = 0; index < packages.size(); ++index) {
      for (const auto &imported : package_inventories[index].imported_modules) {
        if (imported != "std" && !imported.starts_with("std::"))
          continue;
        if (imported == "std") {
          error = "compiler import of bare standard-library package 'std' is not "
                  "a module; import a named std module";
          return std::nullopt;
        }
        const auto module =
            std::ranges::find(standard_library->modules(), imported,
                              &CompilerStandardLibraryModule::module_name);
        if (module == standard_library->modules().end()) {
          error = "compiler package '" + packages[index].package_name +
                  "' imports unknown standard-library module '" + imported +
                  "'";
          return std::nullopt;
        }
        if (packages[index].language_version <
            module->minimum_language_version) {
          error = "Chtholly " + packages[index].language_version.str() +
                  " package '" + packages[index].package_name +
                  "' cannot import standard-library module '" + imported +
                  "' introduced in " + module->minimum_language_version.str();
          return std::nullopt;
        }
        if (reachable_modules.insert(imported).second)
          pending_modules.push_back(imported);
      }
    }
    for (std::size_t index = 0; index < pending_modules.size(); ++index) {
      const auto module =
          std::ranges::find(standard_library->modules(), pending_modules[index],
                            &CompilerStandardLibraryModule::module_name);
      for (const auto &dependency : module->imports)
        if (reachable_modules.insert(dependency).second)
          pending_modules.push_back(dependency);
    }
    for (const auto &module : standard_library->modules()) {
      if (!reachable_modules.contains(module.module_name))
        continue;
      standard_package.sources.push_back(module.source_path);
      standard_package.expected_modules.push_back(module.module_name);
      standard_package.expected_module_dependencies.emplace(module.module_name,
                                                            module.imports);
      standard_package.expected_runtime_symbols.emplace(module.module_name,
                                                        module.runtime_symbols);
      standard_package.compiler_intrinsics.insert(
          standard_package.compiler_intrinsics.end(),
          module.compiler_intrinsics.begin(), module.compiler_intrinsics.end());
    }
    standard_library_manifest_path =
        std::string(standard_library->manifestPath());
    for (auto &package : packages)
      for (auto &dependency : package.dependencies)
        ++dependency;
    for (std::size_t index = 0; index < packages.size(); ++index) {
      if (package_uses_standard_library[index])
        packages[index].dependencies.push_back(0);
      std::ranges::sort(packages[index].dependencies);
      packages[index].dependencies.erase(
          std::unique(packages[index].dependencies.begin(),
                      packages[index].dependencies.end()),
          packages[index].dependencies.end());
    }
    packages.insert(packages.begin(), std::move(standard_package));
    ++root_package;
  }

  std::filesystem::path identity_path;
  if (!build->workspace_manifest_path.empty())
    identity_path = build->workspace_manifest_path;
  else if (!build->project_manifest_path.empty())
    identity_path = build->project_manifest_path;
  else
    identity_path = root_source;
  std::ostringstream identity;
  identity << "chtholly.next.driver-session.v8\n"
           << normalizeCompilerInputPath(identity_path.string()) << '\n'
           << build->package_name << '\n'
           << build->target.info.triple << '\n'
           << CurrentSemanticArtifactEpoch << '\n'
           << CurrentStandardLibraryEpoch << '\n';
  for (const auto &package : build->packages) {
    identity << package.name << '\n';
    if (package.interop_bundle_path.empty()) {
      identity << "no-interop\n";
    } else {
      identity << package.interop_bundle_path << '\n'
               << package.interop_bundle_digest << '\n';
    }
    identity << package.artifact_archive_path << '\n'
             << package.artifact_archive_digest << '\n'
             << package.cffi_receipt_path << '\n'
             << package.cffi_receipt_digest << '\n'
             << package.component_abi << '\n'
             << package.component_identity << '\n';
    for (const auto &component_export : package.component_exports)
      identity << component_export << '\n';
  }
  const auto session_key = sha256Hex(identity.str());

  std::filesystem::path output_path = invocation.output_path;
  if (invocation.workflow != DriverWorkflow::Compile) {
    std::filesystem::path out_dir;
    if (!invocation.out_dir.empty()) {
      out_dir = invocation.out_dir;
    } else {
      const auto &output_root = build->workspace_root.empty()
                                    ? build->project_root
                                    : build->workspace_root;
      out_dir = std::filesystem::path(output_root) / ".chtholly" / "build" /
                safePathComponent(build->target.info.triple) / "next";
    }
    auto filename = build->package_name.empty() ? "app" : build->package_name;
    if (root.isComponent()) {
#ifdef _WIN32
      filename += ".dll";
#else
      filename = "lib" + filename + ".so";
#endif
    } else {
#ifdef _WIN32
      filename += ".exe";
#endif
    }
    output_path = out_dir / filename;
  }

  return CompilerDriverPlan{
      .build = std::move(*build),
      .packages = std::move(packages),
      .root_package = root_package,
      .output_path = output_path.string(),
      .session_key = session_key,
      .store_root = compiler_root.string(),
      .standard_library_manifest_path =
          std::move(standard_library_manifest_path),
      .archive_install_attempts = archive_install_attempts,
      .archive_install_closure_hits = archive_install_closure_hits,
      .archive_install_fresh_installs = archive_install_fresh_installs,
      .archive_install_bytes = archive_install_bytes};
}

std::vector<std::string> sourceSnapshotPaths(const CompilerDriverPlan &plan) {
  return CompilerPipelinePlanningService::sourceSnapshotPaths(plan);
}

compiler::StableFingerprint resolutionFingerprint(const CompilerDriverPlan &plan) {
  return CompilerPipelineFingerprintService::resolution(plan);
}

compiler::StableFingerprint
compileToolchainFingerprint(const CompilerInvocation &invocation,
                            const CompilerDriverPlan &plan) {
  return CompilerPipelineFingerprintService::compileToolchain(invocation, plan);
}

compiler::StableFingerprint
linkToolchainFingerprint(const CompilerInvocation &invocation,
                         const CompilerDriverPlan &plan,
                         const compiler::StableFingerprint &compile_fingerprint) {
  return CompilerPipelineFingerprintService::linkToolchain(
      invocation, plan, compile_fingerprint);
}

CompilerBuildControlInputs buildControlInputs(const CompilerInvocation &invocation,
                                          const CompilerDriverPlan &plan) {
  return CompilerPipelinePlanningService::buildControlInputs(invocation, plan);
}

struct StableNextDriverPlan {
  CompilerDriverPlan plan;
  CompilerBuildControlSnapshot controls;
};

std::optional<StableNextDriverPlan>
resolveStableNextDriverPlan(const CompilerInvocation &invocation,
                            const CompilerInputFileSystem &file_system,
                            bool update_lockfile, std::string &error) {
  CompilerInvocation resolution_invocation = invocation;
  resolution_invocation.suppress_lockfile_update = true;
  auto provisional =
      resolveNextDriverPlan(resolution_invocation, file_system, error);
  if (!provisional)
    return std::nullopt;
  if (update_lockfile && !invocation.disable_lockfile &&
      !verifyOrUpdateNextLockfile(provisional->build, invocation.locked, error))
    return std::nullopt;

  auto inputs = buildControlInputs(invocation, *provisional);
  auto controls = CompilerBuildControlSnapshot::capture(file_system, inputs, error);
  if (!controls)
    return std::nullopt;

  auto final_plan =
      resolveNextDriverPlan(resolution_invocation, file_system, error);
  if (!final_plan)
    return std::nullopt;
  auto final_inputs = buildControlInputs(invocation, *final_plan);
  if (!controls->verifyCurrentInputs(file_system, final_inputs, error))
    return std::nullopt;
  if (update_lockfile && !invocation.disable_lockfile &&
      !verifyOrUpdateNextLockfile(final_plan->build, true, error)) {
    error =
        "compiler lockfile resolution did not converge after one update: " + error;
    return std::nullopt;
  }
  return StableNextDriverPlan{.plan = std::move(*final_plan),
                              .controls = std::move(*controls)};
}

bool verifySourceSnapshotBarrier(const CompilerDriverPlan &plan,
                                 const CompilerSourceSnapshot &snapshot,
                                 const CompilerInputFileSystem &file_system,
                                 std::string &error) {
  std::vector<std::string> current_paths;
  for (const auto &package : plan.packages) {
    if (package.is_standard_library) {
      current_paths.insert(current_paths.end(), package.sources.begin(),
                           package.sources.end());
      continue;
    }
    std::string scan_error;
    auto sources =
        collectNextSources(package.module_roots, package.source_entry,
                           package.include_entry, file_system, scan_error);
    if (!sources) {
      error = "compiler source snapshot conflict while refreshing package '" +
              package.package_name + "': " + scan_error;
      return false;
    }
    if (*sources != package.sources) {
      error = "compiler source snapshot conflict: the source inventory for package "
              "'" +
              package.package_name + "' changed; retry the build";
      return false;
    }
    current_paths.insert(current_paths.end(), sources->begin(), sources->end());
  }
  return snapshot.verifyCurrentSources(file_system, current_paths, error);
}

bool verifyRequestSnapshotBarrier(const CompilerInvocation &invocation,
                                  const CompilerDriverPlan &plan,
                                  const CompilerRequestSnapshot &snapshot,
                                  const CompilerInputFileSystem &file_system,
                                  std::string &error) {
  auto controls = buildControlInputs(invocation, plan);
  return snapshot.controls().verifyCurrentInputs(file_system,
                                                 std::move(controls), error) &&
         verifySourceSnapshotBarrier(plan, snapshot.sources(), file_system,
                                     error);
}

bool verifyStandardLibrarySnapshotIdentity(
    const CompilerDriverPlan &plan, const CompilerInputFileSystem &file_system,
    std::string &error) {
  if (plan.standard_library_manifest_path.empty())
    return true;
  const auto package = std::ranges::find_if(
      plan.packages, &CompilerPackagePlan::is_standard_library);
  if (package == plan.packages.end()) {
    error = "compiler standard-library plan omitted its package identity";
    return false;
  }
  auto current = CompilerStandardLibraryManifest::load(plan.build.resource_dir,
                                                   file_system, error);
  if (!current) {
    error = "compiler standard-library snapshot conflict: " + error;
    return false;
  }
  if (current->distributionFingerprint() !=
      package->package_contract_fingerprint) {
    error = "compiler standard-library snapshot conflict: the distribution "
            "changed while preparing the request; retry the build";
    return false;
  }
  return true;
}

bool writeNextDumps(const CompilerInvocation &invocation,
                    const compiler::CompilationSession &session,
                    const compiler::CompilationUnit &unit, std::string &error) {
  CompilerPipelineOutputState state{
      invocation,
      [](const std::string &path, const std::string &contents,
         std::string &output_error) {
        return writeNextOutput(path, contents, output_error);
      }};
  return CompilerPipelineOutputService::writeDumps(state, session, unit, error);
}

bool writeArtifactLoadMetrics(const CompilerInvocation &invocation,
                              std::string_view metrics, std::string &error) {
  CompilerPipelineOutputState state{
      invocation,
      [](const std::string &path, const std::string &contents,
         std::string &output_error) {
        return writeNextOutput(path, contents, output_error);
      }};
  return CompilerPipelineOutputService::writeArtifactLoadMetrics(state, metrics,
                                                                   error);
}

void appendInvalidationExplanations(
    std::string_view package_name,
    const compiler::IncrementalCompilationPlan &compilation_plan,
    std::vector<WorkspaceArtifactResult::InvalidationExplanation> &output) {
  CompilerPipelineDiagnosticsService::appendInvalidationExplanations(
      package_name, compilation_plan, output);
}

bool writeAnalysisMetrics(const CompilerInvocation &invocation,
                          std::span<const PackageQueryResult> results,
                          std::string &error) {
  CompilerPipelineOutputState state{
      invocation,
      [](const std::string &path, const std::string &contents,
         std::string &output_error) {
        return writeNextOutput(path, contents, output_error);
      }};
  return CompilerPipelineOutputService::writeAnalysisMetrics(state, results,
                                                               error);
}

std::string generatedInteropBundlePath(const CompilerDriverPlan &plan,
                                       const CompilerPackagePlan &package) {
  return CompilerPipelinePlanningService::generatedInteropBundlePath(plan,
                                                                       package);
}

std::vector<std::size_t> templateDependencyClosure(std::size_t package_index,
                                                   const CompilerDriverPlan &plan) {
  return CompilerPipelinePlanningService::templateDependencyClosure(
      package_index, plan);
}

std::size_t maximumPackageQueryParallelism(const CompilerDriverPlan &plan) {
  return CompilerPipelinePlanningService::maximumPackageQueryParallelism(plan);
}

bool executePackageQueryGraph(
    const CompilerInvocation &invocation, const CompilerDriverPlan &plan,
    const std::map<std::string, compiler::CompilerPackageArtifactManifest>
        &previous_manifests,
    const CompilerArtifactLease &lease, const CompilerSourceSnapshot &snapshot,
    const compiler::StableFingerprint &compile_toolchain_fingerprint,
    CompilerArtifactLoadExecutor &artifact_loads,
    CompilerArtifactLoadMetrics *artifact_load_metrics,
    std::vector<PackageQueryResult> &results, std::string &error) {
  CompilerPackageQueryExecutionState state{
      plan,
      invocation.jobs,
      [&]() { return maximumPackageQueryParallelism(plan); },
      [&](std::size_t index, std::span<const PackageQueryResult> completed) {
        const CompilerPackageQueryContext context{
            plan, invocation, previous_manifests, completed, lease, snapshot,
            compile_toolchain_fingerprint, artifact_loads,
            artifact_load_metrics};
        return CompilerPipelineExecutionService::packageQuery(index, context);
      },
      artifact_load_metrics};
  return CompilerPipelineExecutionService::packageQueryGraph(state, results,
                                                              error);
}
