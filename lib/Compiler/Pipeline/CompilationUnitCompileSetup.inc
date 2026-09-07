// Included inside CompilationSession::compile; this fragment aliases the
// existing session Impl and preserves phase/state ordering.
                                 const CompilationRequest &request) {
  error.clear();
  if (impl_->compilation_started) {
    error = "compilation session can only be compiled once";
    return false;
  }
  impl_->compilation_started = true;
  impl_->component_identity = request.component_identity;
  if (request.component_identity.empty() != request.component_exports.empty()) {
    error = "component compilation requires both identity and exports";
    return false;
  }
  if (!impl_->language_contract.isSupported()) {
    error = "compilation session has an unsupported language contract";
    return false;
  }
  if ((!impl_->compiler_intrinsics.empty() &&
       impl_->provenance.kind !=
           PackageProvenanceKind::ToolchainStandardLibrary) ||
      std::ranges::any_of(impl_->compiler_intrinsics, [](const auto &binding) {
        return binding.module_name.empty() || binding.entity_name.empty() ||
               binding.role == CompilerIntrinsicRole::None ||
               binding.role >= CompilerIntrinsicRole::Count;
      })) {
    error = "compilation session has invalid compiler intrinsic authority";
    return false;
  }
  const auto cancelled = [&] {
    return request.is_cancelled && request.is_cancelled();
  };
  if (cancelled()) {
    error = "compiler compilation cancelled";
    return false;
  }
  std::unordered_set<std::uint32_t> required_transient_outputs;
  for (const auto id : request.required_transient_outputs) {
    if (!id.hasValue() || id.index >= impl_->units.size() ||
        !required_transient_outputs.insert(id.index).second) {
      error = "compilation request has an invalid transient output unit";
      return false;
    }
  }
  if (request.unit_emission_roles.size() > impl_->units.size() ||
      std::ranges::any_of(request.unit_emission_roles, [](const auto role) {
        return role >= ModuleEmissionRole::Count;
      })) {
    error = "compilation request has invalid unit emission roles";
    return false;
  }
  const auto emission_role = [&](CheckIRId id) {
    return id.index < request.unit_emission_roles.size()
               ? request.unit_emission_roles[id.index]
               : ModuleEmissionRole::Library;
  };
  for (const auto &unit : impl_->units) {
    if (unit->kind() == CompilationUnitKind::ForeignBinding &&
        emission_role(unit->checkIRId()) != ModuleEmissionRole::Library) {
      error = "CFDL binding modules can only use library emission";
      return false;
    }
  }
  const auto *previous_manifest = request.previous_manifest;
  if (previous_manifest && !previous_manifest->verify(error))
    return false;

  std::vector<const CompilerPackageArtifactManifest *> dependency_manifests =
      request.dependency_manifests;
  std::ranges::sort(dependency_manifests, {},
                    [](const CompilerPackageArtifactManifest *manifest) {
                      return manifest ? manifest->packageName()
                                      : std::string_view{};
                    });
  std::vector<PackageManifestDependency> direct_dependencies;
  std::vector<PackageCheckDependency> direct_check_dependencies;
  std::vector<const PublicInterfaceArtifact *> artifact_closure;
  direct_dependencies.reserve(dependency_manifests.size());
  for (std::size_t index = 0; index < dependency_manifests.size(); ++index) {
    const auto *dependency = dependency_manifests[index];
    if (!dependency || !dependency->verify(error) ||
        dependency->targetTriple() != impl_->target_triple ||
        dependency->compileToolchainFingerprint() !=
            impl_->compile_toolchain_fingerprint ||
        !impl_->language_contract.isDependencyCompatibleWith(
            dependency->languageContract()) ||
        dependency->packageName() == impl_->package_name ||
        (index != 0 && dependency_manifests[index - 1]->packageName() ==
                           dependency->packageName())) {
      if (error.empty())
        error = "compilation request has an invalid direct dependency manifest";
      return false;
    }
    direct_dependencies.push_back(
        {.package_name = std::string(dependency->packageName()),
         .manifest_fingerprint = dependency->fingerprint()});
    if (request.dependency_check_artifacts.empty()) {
      direct_check_dependencies.push_back(
          {.package_name = std::string(dependency->packageName()),
           .artifact_fingerprint = dependency->fingerprint()});
      for (const auto &module : dependency->modules())
        artifact_closure.push_back(&module.public_interface);
    }
  }
  std::vector<const CompilerPackageCheckArtifact *> dependency_checks =
      request.dependency_check_artifacts;
  std::ranges::sort(
      dependency_checks, {}, [](const CompilerPackageCheckArtifact *artifact) {
        return artifact ? artifact->packageName() : std::string_view{};
      });
  direct_check_dependencies.reserve(dependency_checks.size());
  for (std::size_t index = 0; index < dependency_checks.size(); ++index) {
    const auto *dependency = dependency_checks[index];
    if (!dependency || !dependency->verify(error) ||
        dependency->targetTriple() != impl_->target_triple ||
        dependency->compileToolchainFingerprint() !=
            impl_->compile_toolchain_fingerprint ||
        !impl_->language_contract.isDependencyCompatibleWith(
            dependency->languageContract()) ||
        dependency->packageName() == impl_->package_name ||
        (index != 0 && dependency_checks[index - 1]->packageName() ==
                           dependency->packageName())) {
      if (error.empty())
        error = "compilation request has an invalid direct check dependency";
      return false;
    }
    direct_check_dependencies.push_back(
        {.package_name = std::string(dependency->packageName()),
         .artifact_fingerprint = dependency->fingerprint()});
    for (const auto &module : dependency->modules())
      artifact_closure.push_back(&module.public_interface);
  }
  for (const auto *dependency : request.template_dependency_closure) {
    if (!dependency || !dependency->verify(error) ||
        dependency->targetTriple() != impl_->target_triple ||
        dependency->compileToolchainFingerprint() !=
            impl_->compile_toolchain_fingerprint ||
        !impl_->language_contract.isDependencyCompatibleWith(
            dependency->languageContract()))
      return false;
    for (const auto &module : dependency->modules())
      artifact_closure.push_back(&module.public_interface);
  }
  for (const auto *dependency : request.template_dependency_check_closure) {
    if (!dependency || !dependency->verify(error) ||
        dependency->targetTriple() != impl_->target_triple ||
        dependency->compileToolchainFingerprint() !=
            impl_->compile_toolchain_fingerprint ||
        !impl_->language_contract.isDependencyCompatibleWith(
            dependency->languageContract()))
      return false;
    for (const auto &module : dependency->modules())
      artifact_closure.push_back(&module.public_interface);
  }
  std::ranges::sort(artifact_closure, [](const auto *lhs, const auto *rhs) {
    return std::tuple(lhs->packageName(), lhs->moduleName()) <
           std::tuple(rhs->packageName(), rhs->moduleName());
  });
  for (std::size_t index = 1; index < artifact_closure.size(); ++index) {
    const auto *previous = artifact_closure[index - 1];
    const auto *current = artifact_closure[index];
    if (previous->packageName() == current->packageName() &&
        previous->moduleName() == current->moduleName() &&
        previous->fingerprint() != current->fingerprint()) {
      error = "compilation request has conflicting public interface artifacts";
      return false;
    }
  }
  artifact_closure.erase(
      std::unique(artifact_closure.begin(), artifact_closure.end(),
                  [](const auto *lhs, const auto *rhs) {
                    return lhs->packageName() == rhs->packageName() &&
                           lhs->moduleName() == rhs->moduleName();
                  }),
      artifact_closure.end());
  if (!impl_->public_interfaces->registerArtifactClosure(artifact_closure,
                                                         error))
    return false;
  const auto format_diagnostics = [&]() {
    std::string result;
    for (const auto &unit : impl_->units) {
      if (unit->impl_->diagnostics.hasError())
        result += unit->impl_->diagnostics.format(unit->impl_->source);
    }
    return result;
  };

