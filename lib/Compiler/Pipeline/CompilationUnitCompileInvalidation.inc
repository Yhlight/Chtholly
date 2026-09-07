// Included inside CompilationSession::compile; this fragment aliases the
// existing session Impl and preserves phase/state ordering.
  if (const auto diagnostics = format_diagnostics(); !diagnostics.empty()) {
    std::vector<UnitCompilationDecision> failed_decisions;
    const auto configuration_matches =
        !previous_manifest ||
        previous_manifest->configurationFingerprint() ==
            fingerprintCompilationConfiguration(
                impl_->package_name, impl_->target_triple,
                impl_->compile_toolchain_fingerprint, impl_->resolved_features,
                impl_->provenance, impl_->language_contract,
                impl_->cffi_identity);
    for (const auto &unit : impl_->units) {
      const auto module_name = std::string(unit->moduleName());
      const auto source_fingerprint =
          fingerprintSource(impl_->package_name, module_name, unit->impl_->kind,
                            unit->impl_->source.text());
      const auto *previous_module =
          previous_manifest ? previous_manifest->findModule(module_name)
                            : nullptr;
      std::vector<UnitInvalidation> invalidations;
      if (!previous_module) {
        invalidations.push_back({UnitInvalidationReason::ModuleAdded});
      } else {
        if (!configuration_matches)
          invalidations.push_back(
              {UnitInvalidationReason::CompilationConfigurationChanged});
        if (previous_module->source_fingerprint != source_fingerprint) {
          invalidations.push_back({UnitInvalidationReason::SourceChanged});
          if (previous_module->unit_kind != unit->kind())
            invalidations.push_back(
                {UnitInvalidationReason::SourceKindChanged});
        } else {
          if (required_transient_outputs.contains(unit->checkIRId().index))
            invalidations.push_back({UnitInvalidationReason::RequestedOutput});
          for (const auto &observation : previous_module->observations) {
            bool provider_exists = false;
            if (observation.provider.package_name == impl_->package_name) {
              const auto provider = impl_->values->internIdentifier(
                  observation.provider.module_name);
              provider_exists = modules.contains(provider.index);
            } else {
              const auto dependency = std::ranges::find(
                  dependency_manifests, observation.provider.package_name,
                  [](const CompilerPackageArtifactManifest *manifest) {
                    return manifest->packageName();
                  });
              provider_exists =
                  dependency != dependency_manifests.end() &&
                  (*dependency)->findModule(observation.provider.module_name);
            }
            if (!provider_exists)
              invalidations.push_back(
                  {UnitInvalidationReason::ImportedModuleRemoved,
                   observation.provider, observation.binding_name});
          }
        }
      }
      failed_decisions.push_back({module_name,
                                  invalidations.empty()
                                      ? UnitCompilationAction::Reuse
                                      : UnitCompilationAction::Rebuild,
                                  std::move(invalidations)});
    }
    if (previous_manifest) {
      for (const auto &previous_module : previous_manifest->modules()) {
        const auto module = impl_->values->internIdentifier(
            previous_module.public_interface.moduleName());
        if (!modules.contains(module.index))
          failed_decisions.push_back(
              {std::string(previous_module.moduleName()),
               UnitCompilationAction::Removed,
               {{UnitInvalidationReason::ModuleRemoved}}});
      }
    }
    impl_->compilation_plan.emplace(std::move(failed_decisions));
    std::string plan_error;
    if (!impl_->compilation_plan->verify(plan_error)) {
      error = plan_error;
      return false;
    }
    error = diagnostics;
    return false;
  }

  std::vector<std::uint32_t> remaining(impl_->units.size());
  std::vector<std::vector<CheckIRId>> dependents(impl_->units.size());
  for (const auto &unit : impl_->units) {
    for (const auto &dependency : unit->impl_->imports) {
      if (!dependency.local_unit.hasValue())
        continue;
      ++remaining[unit->checkIRId().index];
      dependents[dependency.local_unit.index].push_back(unit->checkIRId());
    }
  }
  std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>>
      ready;
  for (std::uint32_t id = 0; id < remaining.size(); ++id) {
    if (remaining[id] == 0)
      ready.push(id);
  }
  std::vector<CheckIRId> order;
  while (!ready.empty()) {
    const auto id = ready.top();
    ready.pop();
    order.emplace_back(id);
    for (const auto dependent : dependents[id]) {
      if (--remaining[dependent.index] == 0)
        ready.push(dependent.index);
    }
  }
  if (order.size() != impl_->units.size()) {
    for (std::uint32_t id = 0; id < remaining.size(); ++id) {
      if (remaining[id] != 0)
        impl_->units[id]->impl_->diagnostics.emit(DiagnosticKind::ImportCycle,
                                                  0);
    }
    error = format_diagnostics();
    return false;
  }

