// Included inside CompilationSession::compile; this fragment aliases the
// existing session Impl and preserves phase/state ordering.
  if (previous_manifest) {
    for (const auto &previous_module : previous_manifest->modules()) {
      const auto name = impl_->values->internIdentifier(
          previous_module.public_interface.moduleName());
      if (!modules.contains(name.index))
        decisions.push_back({std::string(previous_module.moduleName()),
                             UnitCompilationAction::Removed,
                             {{UnitInvalidationReason::ModuleRemoved}}});
    }
  }

  impl_->compilation_plan.emplace(std::move(decisions));
  if (!impl_->compilation_plan->verify(error))
    return false;
  if (!impl_->interop_registry->verify(error))
    return false;
  impl_->package_check_artifact.emplace(
      impl_->package_name, impl_->target_triple,
      impl_->compile_toolchain_fingerprint, impl_->resolved_features,
      std::move(direct_check_dependencies), std::move(check_module_artifacts),
      impl_->provenance, impl_->language_contract, impl_->cffi_identity);
  if (!impl_->package_check_artifact->verify(error))
    return false;
  if (!request.component_identity.empty()) {
    auto contract = componentContract(error);
    if (!contract || contract->identity != request.component_identity ||
        contract->exports.size() != request.component_exports.size()) {
      if (error.empty())
        error = "component export set is incomplete or duplicated";
      return false;
    }
  }
  if (request.mode == CompilationRequest::Mode::Check)
    return true;
  impl_->package_manifest.emplace(
      impl_->package_name, impl_->target_triple,
      impl_->compile_toolchain_fingerprint, impl_->resolved_features,
      std::move(direct_dependencies), std::move(module_artifacts),
      impl_->provenance, impl_->language_contract, impl_->cffi_identity);
  if (!impl_->package_manifest->verifyDependencies(dependency_manifests, error))
    return false;
  return true;
