// Included inside CompilationSession::compile; this fragment aliases the
// existing session Impl and preserves phase/state ordering.
  std::vector<UnitCompilationDecision> decisions;
  std::vector<PackageModuleArtifact> module_artifacts;
  std::vector<PackageModuleCheckArtifact> check_module_artifacts;
  decisions.reserve(
      impl_->units.size() +
      (previous_manifest ? previous_manifest->modules().size() : 0));
  module_artifacts.reserve(impl_->units.size());
  check_module_artifacts.reserve(impl_->units.size());
  struct NominalSemanticWitnessCandidate {
    std::string package_name;
    StableFingerprint request_fingerprint;
  };
  std::vector<NominalSemanticWitnessCandidate> semantic_witness_candidates;
  std::unordered_map<std::string, StableFingerprint> nominal_semantic_options;
  nominal_semantic_options.emplace(
      impl_->package_name,
      fingerprintConcreteSemanticOptions(impl_->resolved_features));
  const auto add_nominal_manifest = [&](const CompilerPackageArtifactManifest *m) {
    if (!m)
      return;
    nominal_semantic_options.insert_or_assign(
        std::string(m->packageName()),
        fingerprintConcreteSemanticOptions(m->resolvedFeatures()));
    for (const auto &module : m->modules())
      for (const auto &reference : module.nominal_semantic_witnesses)
        semantic_witness_candidates.push_back(
            {std::string(m->packageName()), reference.request_fingerprint});
  };
  add_nominal_manifest(previous_manifest);
  for (const auto *manifest : dependency_manifests)
    add_nominal_manifest(manifest);
  for (const auto *manifest : request.template_dependency_closure)
    add_nominal_manifest(manifest);
  std::ranges::sort(semantic_witness_candidates, {}, [](const auto &candidate) {
    return std::tuple(candidate.package_name,
                      candidate.request_fingerprint.hex());
  });
  semantic_witness_candidates.erase(
      std::unique(semantic_witness_candidates.begin(),
                  semantic_witness_candidates.end(),
                  [](const auto &lhs, const auto &rhs) {
                    return lhs.package_name == rhs.package_name &&
                           lhs.request_fingerprint == rhs.request_fingerprint;
                  }),
      semantic_witness_candidates.end());
  std::vector<NominalSemanticWitnessLoadResult> preloaded_witnesses;
  if (request.load_nominal_semantic_witnesses &&
      !semantic_witness_candidates.empty()) {
    std::vector<StableFingerprint> fingerprints;
    fingerprints.reserve(semantic_witness_candidates.size());
    for (const auto &candidate : semantic_witness_candidates)
      fingerprints.push_back(candidate.request_fingerprint);
    preloaded_witnesses = request.load_nominal_semantic_witnesses(fingerprints);
    if (preloaded_witnesses.size() != fingerprints.size()) {
      error = "nominal semantic witness batch loader returned the wrong "
              "result count";
      return false;
    }
  }
  std::vector<NominalSemanticWitnessArtifact>
      available_nominal_semantic_witnesses;
  std::unordered_set<std::string> attempted_nominal_semantic_witnesses;
  const auto resolve_nominal_semantic_options =
      [&](const PublicEntityReferenceArtifact &entity) {
        const auto found =
            nominal_semantic_options.find(entity.canonical_package);
        return found == nominal_semantic_options.end()
                   ? fingerprintConcreteSemanticOptions(
                         impl_->resolved_features)
                   : found->second;
      };
  const auto resolve_nominal_semantic_witness =
      [&](const NominalTypeSpecificArtifact &specific,
          std::string &nested_error)
      -> std::optional<NominalSemanticWitnessArtifact> {
    const auto matches = [&](const NominalSemanticWitnessArtifact &witness) {
      return witness.nominal_template == specific.template_entity &&
             witness.arguments == specific.arguments &&
             witness.semantic_options_fingerprint ==
                 specific.semantic_options_fingerprint &&
             witness.structural_specific_fingerprint ==
                 specific.structural_fingerprint;
    };
    if (const auto found =
            std::ranges::find_if(available_nominal_semantic_witnesses, matches);
        found != available_nominal_semantic_witnesses.end())
      return *found;
    for (std::size_t candidate_index = 0;
         candidate_index < semantic_witness_candidates.size();
         ++candidate_index) {
      const auto &candidate = semantic_witness_candidates[candidate_index];
      if (candidate.package_name !=
              specific.template_entity.canonical_package ||
          !attempted_nominal_semantic_witnesses
               .insert(candidate.request_fingerprint.hex())
               .second)
        continue;
      std::optional<NominalSemanticWitnessArtifact> loaded;
      if (!preloaded_witnesses.empty()) {
        nested_error = preloaded_witnesses[candidate_index].error;
        loaded = preloaded_witnesses[candidate_index].artifact;
      } else if (request.load_nominal_semantic_witness) {
        loaded = request.load_nominal_semantic_witness(
            candidate.request_fingerprint, nested_error);
      }
      if (!loaded) {
        if (!nested_error.empty())
          return std::nullopt;
        continue;
      }
      available_nominal_semantic_witnesses.push_back(std::move(*loaded));
      if (matches(available_nominal_semantic_witnesses.back()))
        return available_nominal_semantic_witnesses.back();
    }
    return std::nullopt;
  };
  const auto configuration_matches =
      !previous_manifest ||
      previous_manifest->configurationFingerprint() ==
          fingerprintCompilationConfiguration(
              impl_->package_name, impl_->target_triple,
              impl_->compile_toolchain_fingerprint, impl_->resolved_features,
              impl_->provenance, impl_->language_contract,
              impl_->cffi_identity);

  std::vector<std::optional<ObjectArtifactLoadResult>> preloaded_objects(
      impl_->units.size());
  if (request.mode == CompilationRequest::Mode::Build && request.load_objects &&
      previous_manifest) {
    std::vector<ObjectArtifactLoadRequest> object_requests;
    std::vector<CheckIRId> object_units;
    for (const auto id : order) {
      const auto *previous_module =
          previous_manifest->findModule(impl_->units[id.index]->moduleName());
      if (!previous_module)
        continue;
      object_requests.push_back({previous_module->object_fingerprint,
                                 previous_module->specific_fingerprint});
      object_units.push_back(id);
    }
    auto loaded = request.load_objects(object_requests);
    if (loaded.size() != object_requests.size()) {
      error = "object artifact batch loader returned the wrong result count";
      return false;
    }
    for (std::size_t index = 0; index < loaded.size(); ++index)
      preloaded_objects[object_units[index].index] = std::move(loaded[index]);
  }

  const auto collect_observations = [&](const CompilationUnit::Impl &data) {
    std::vector<DependencyObservation> observations;
    observations.reserve(data.imports.size());
    for (const auto &dependency : data.imports) {
      const PublicInterface *provider_interface = nullptr;
      if (dependency.local_unit.hasValue())
        provider_interface =
            impl_->units[dependency.local_unit.index]->publicInterface();
      else
        provider_interface =
            impl_->public_interfaces->tryGet(dependency.public_interface);
      if (!provider_interface)
        return std::optional<std::vector<DependencyObservation>>{};
      observations.push_back(
          {DependencyObservationKind::ModulePresence,
           dependency.provider,
           {},
           fingerprintModuleIdentity(dependency.provider.package_name,
                                     dependency.provider.module_name)});
      if (dependency.is_export)
        observations.push_back({DependencyObservationKind::ExportSet,
                                dependency.provider,
                                {},
                                provider_interface->fingerprint()});
    }
    for (std::uint32_t index = 0; index < data.sem_ir->importIRs().instCount();
         ++index) {
      const auto import_ir_inst = ImportIRInstId(index);
      const auto *import_inst =
          data.sem_ir->importIRs().tryGetInst(import_ir_inst);
      const auto *provider =
          import_inst
              ? data.sem_ir->importIRs().tryGetInterface(import_inst->ir)
              : nullptr;
      const auto *binding =
          import_inst ? data.sem_ir->importIRs().tryGetFunction(*import_inst)
                      : nullptr;
      std::string canonical_error;
      const auto canonical = data.sem_ir->importIRs().canonicalEntity(
          import_ir_inst, canonical_error);
      const auto *entity = data.sem_ir->importIRs().tryGetEntity(canonical);
      if (!provider || !binding || !entity)
        return std::optional<std::vector<DependencyObservation>>{};
      observations.push_back(
          {DependencyObservationKind::EntityBinding,
           {std::string(impl_->values->identifier(provider->packageName())),
            std::string(impl_->values->identifier(provider->moduleName()))},
           std::string(impl_->values->identifier(binding->name)),
           entity->fingerprint,
           {},
           {},
           {},
           {std::string(impl_->values->identifier(entity->package_name)),
            std::string(impl_->values->identifier(entity->module_name))},
           std::string(impl_->values->identifier(entity->name))});
      if (entity->generic.hasValue()) {
        const ModuleIdentity provider_id{
            std::string(impl_->values->identifier(provider->packageName())),
            std::string(impl_->values->identifier(provider->moduleName()))};
        const auto already_observed = std::ranges::any_of(
            observations, [&](const DependencyObservation &observation) {
              return observation.kind == DependencyObservationKind::ExportSet &&
                     observation.provider == provider_id;
            });
        if (!already_observed)
          observations.push_back({DependencyObservationKind::ExportSet,
                                  provider_id,
                                  {},
                                  provider->fingerprint()});
      }
    }
    for (std::uint32_t index = 0; index < data.sem_ir->nominalTypeCount();
         ++index) {
      const auto &nominal = data.sem_ir->nominalType(NominalTypeId(index));
      if (!nominal.canonical_entity.hasValue())
        continue;
      const auto *entity =
          data.sem_ir->importIRs().tryGetEntity(nominal.canonical_entity);
      if (!entity || entity->kind != PublicEntityKind::NominalType)
        return std::optional<std::vector<DependencyObservation>>{};
      const auto entity_package =
          impl_->values->identifier(entity->package_name);
      const auto entity_module = impl_->values->identifier(entity->module_name);
      if (entity_package == impl_->package_name &&
          entity_module == impl_->values->identifier(data.module_name))
        continue;

      const PublicInterface *visible_provider = nullptr;
      std::string visible_name;
      for (const auto &dependency : data.imports) {
        const auto *interface =
            impl_->public_interfaces->tryGet(dependency.public_interface);
        if (!interface)
          continue;
        for (std::uint32_t binding_index = 0;
             binding_index < interface->nominalTypeBindingCount();
             ++binding_index) {
          const auto &binding =
              interface->nominalType(PublicBindingId(binding_index));
          if (binding.canonical_entity != nominal.canonical_entity)
            continue;
          visible_provider = interface;
          visible_name = std::string(impl_->values->identifier(binding.name));
          break;
        }
        if (visible_provider)
          break;
      }
      if (!visible_provider)
        continue;

      const ModuleIdentity provider{std::string(impl_->values->identifier(
                                        visible_provider->packageName())),
                                    std::string(impl_->values->identifier(
                                        visible_provider->moduleName()))};
      const ModuleIdentity canonical_provider{
          std::string(impl_->values->identifier(entity->package_name)),
          std::string(impl_->values->identifier(entity->module_name))};
      const auto canonical_name =
          std::string(impl_->values->identifier(entity->name));
      DependencyObservation nominal_observation{
          .kind = DependencyObservationKind::NominalBinding,
          .provider = provider,
          .binding_name = visible_name,
          .expected_fingerprint = entity->fingerprint,
          .canonical_provider = canonical_provider,
          .canonical_name = canonical_name};
      if (std::ranges::find(observations, nominal_observation) ==
          observations.end())
        observations.push_back(std::move(nominal_observation));

      if (entity->nominal_kind != NominalKind::ForeignResource)
        continue;
      const auto artifact = std::ranges::find_if(
          visible_provider->nominalArtifacts(), [&](const auto &candidate) {
            return candidate.entity.canonical_package ==
                       canonical_provider.package_name &&
                   candidate.entity.canonical_module ==
                       canonical_provider.module_name &&
                   candidate.entity.canonical_name == canonical_name;
          });
      if (artifact == visible_provider->nominalArtifacts().end())
        return std::optional<std::vector<DependencyObservation>>{};
      DependencyObservation closure_observation{
          .kind = DependencyObservationKind::RelocationClosure,
          .provider = provider,
          .binding_name = visible_name,
          .expected_fingerprint = fingerprintRelocationClosure(*artifact),
          .canonical_provider = canonical_provider,
          .canonical_name = canonical_name};
      if (std::ranges::find(observations, closure_observation) ==
          observations.end())
        observations.push_back(std::move(closure_observation));
    }
    if (data.low_ir) {
      for (std::uint32_t index = 0; index < data.low_ir->instCount(); ++index) {
        const auto &instruction = data.low_ir->inst(LowInstId(index));
        LifecycleObservationRole role = LifecycleObservationRole::Count;
        TypeId type;
        FunctionRefId target;
        if (instruction.kind == LowInstKind::LifecycleCopy) {
          role = LifecycleObservationRole::Copy;
          type = TypeId(instruction.type);
          target = FunctionRefId(instruction.arg0);
        } else if (instruction.kind == LowInstKind::LifecycleDestroy) {
          role = LifecycleObservationRole::Drop;
          target = FunctionRefId(instruction.arg0);
          type = data.low_ir->place(LowPlaceId(instruction.arg1)).type;
        } else if (instruction.kind == LowInstKind::LifecycleDestroyValue) {
          role = LifecycleObservationRole::Drop;
          target = FunctionRefId(instruction.arg0);
          type = TypeId(data.low_ir->inst(LowInstId(instruction.arg1)).type);
        } else {
          continue;
        }
        const auto *witness = data.sem_ir->nominalSemanticWitness(type);
        if (!witness)
          return std::optional<std::vector<DependencyObservation>>{};
        const auto &reference = data.sem_ir->functionRef(target);
        const PublicEntity *entity = nullptr;
        if (reference.local_function.hasValue()) {
          const auto &contract =
              data.sem_ir->functionSemanticContract(reference.local_function);
          if (contract.domain != CallableSemanticDomain::Lifecycle ||
              !contract.owner.hasValue())
            return std::optional<std::vector<DependencyObservation>>{};
          const auto &owner = data.sem_ir->nominalType(contract.owner);
          const auto hidden =
              "$lifecycle$" +
              std::string(
                  data.sem_ir->identifier(data.sem_ir->name(owner.name).text)) +
              "$" + (role == LifecycleObservationRole::Copy ? "copy" : "drop");
          const auto *interface =
              impl_->public_interfaces->tryGet(data.public_interface);
          const auto id =
              interface ? interface->findFunction(
                              impl_->values->internIdentifier(hidden))
                        : PublicBindingId::invalid();
          entity = id.hasValue() ? impl_->public_interfaces->tryGetEntity(
                                       interface->function(id).canonical_entity)
                                 : nullptr;
        } else {
          entity =
              impl_->public_interfaces->tryGetEntity(reference.public_entity);
        }
        if (!entity)
          return std::optional<std::vector<DependencyObservation>>{};
        ModuleIdentity observed_provider{
            std::string(impl_->values->identifier(entity->package_name)),
            std::string(impl_->values->identifier(entity->module_name))};
        auto observed_binding =
            std::string(impl_->values->identifier(entity->name));
        if (!reference.local_function.hasValue()) {
          bool found_visible_binding = false;
          for (const auto &dependency : data.imports) {
            const auto *interface =
                impl_->public_interfaces->tryGet(dependency.public_interface);
            if (!interface)
              continue;
            for (std::uint32_t binding_index = 0;
                 binding_index < interface->bindingCount(); ++binding_index) {
              const auto &binding =
                  interface->function(PublicBindingId(binding_index));
              if (binding.canonical_entity != reference.public_entity)
                continue;
              observed_provider = {std::string(impl_->values->identifier(
                                       interface->packageName())),
                                   std::string(impl_->values->identifier(
                                       interface->moduleName()))};
              observed_binding =
                  std::string(impl_->values->identifier(binding.name));
              found_visible_binding = true;
              break;
            }
            if (found_visible_binding)
              break;
          }
        }
        std::string closure_bytes = "chtholly.next.lifecycle-closure.v1";
        for (const auto &fingerprint :
             witness->transitive_specific_fingerprints)
          closure_bytes += fingerprint.hex();
        DependencyObservation lifecycle_observation{
            .kind = DependencyObservationKind::LifecycleCallable,
            .provider = std::move(observed_provider),
            .binding_name = std::move(observed_binding),
            .expected_fingerprint = entity->fingerprint,
            .lifecycle_role = role,
            .witness_fingerprint = witness->result_fingerprint,
            .specific_closure_fingerprint =
                StableFingerprint::fromCanonicalBytes(closure_bytes),
            .canonical_provider =
                {std::string(impl_->values->identifier(entity->package_name)),
                 std::string(impl_->values->identifier(entity->module_name))},
            .canonical_name =
                std::string(impl_->values->identifier(entity->name))};
        if (std::ranges::find(observations, lifecycle_observation) ==
            observations.end())
          observations.push_back(std::move(lifecycle_observation));
      }
    }
    return std::optional(std::move(observations));
  };

