// Included inside CompilationSession::compile; this fragment aliases the
// existing session Impl and preserves phase/state ordering.
  for (const auto id : order) {
    if (cancelled()) {
      error = "compiler compilation cancelled";
      return false;
    }
    auto &data = *impl_->units[id.index]->impl_;
    const auto module_name = std::string(impl_->units[id.index]->moduleName());
    const auto source_fingerprint = fingerprintSource(
        impl_->package_name, module_name, data.kind, data.source.text());
    const auto *previous_module =
        previous_manifest ? previous_manifest->findModule(module_name)
                          : nullptr;
    const auto unit_emission_role = emission_role(id);
    std::vector<UnitInvalidation> invalidations;
    if (!previous_module) {
      invalidations.push_back({UnitInvalidationReason::ModuleAdded});
    } else {
      if (!configuration_matches)
        invalidations.push_back(
            {UnitInvalidationReason::CompilationConfigurationChanged});
      if (previous_module->source_fingerprint != source_fingerprint)
        invalidations.push_back({UnitInvalidationReason::SourceChanged});
      if (previous_module->unit_kind != data.kind)
        invalidations.push_back({UnitInvalidationReason::SourceKindChanged});
      if (previous_module->emission_role != unit_emission_role)
        invalidations.push_back(
            {UnitInvalidationReason::ObjectEmissionRoleChanged});
      if (previous_module->source_fingerprint == source_fingerprint) {
        for (const auto &observation : previous_module->observations) {
          // Local lifecycle witnesses belong to this source unit. Its source
          // and imported dependencies are already checked; it cannot depend
          // on its own not-yet-published interface during cache replay.
          if (observation.provider.package_name == impl_->package_name &&
              observation.provider.module_name == module_name)
            continue;
          const PublicInterface *provider = nullptr;
          const auto provider_name =
              impl_->values->internIdentifier(observation.provider.module_name);
          if (observation.provider.package_name == impl_->package_name) {
            const auto provider_id = modules.find(provider_name.index);
            if (provider_id != modules.end())
              provider =
                  impl_->units[provider_id->second.index]->publicInterface();
          } else {
            const auto package_name = impl_->values->internIdentifier(
                observation.provider.package_name);
            provider = impl_->public_interfaces->tryGet(
                impl_->public_interfaces->findByModule(package_name,
                                                       provider_name));
          }
          if (!provider) {
            invalidations.push_back(
                {UnitInvalidationReason::ImportedModuleRemoved,
                 observation.provider});
            continue;
          }
          if (observation.kind == DependencyObservationKind::ModulePresence)
            continue;
          if (observation.kind == DependencyObservationKind::ExportSet) {
            if (provider->fingerprint() != observation.expected_fingerprint)
              invalidations.push_back({UnitInvalidationReason::ExportSetChanged,
                                       observation.provider});
            continue;
          }
          if (observation.kind == DependencyObservationKind::NominalBinding ||
              observation.kind ==
                  DependencyObservationKind::RelocationClosure) {
            const auto binding_name =
                impl_->values->internIdentifier(observation.binding_name);
            const auto binding_id = provider->findNominalType(binding_name);
            const auto *binding = binding_id.hasValue()
                                      ? &provider->nominalType(binding_id)
                                      : nullptr;
            const auto *entity = binding
                                     ? impl_->public_interfaces->tryGetEntity(
                                           binding->canonical_entity)
                                     : nullptr;
            const auto canonical_matches =
                entity &&
                impl_->values->identifier(entity->package_name) ==
                    observation.canonical_provider.package_name &&
                impl_->values->identifier(entity->module_name) ==
                    observation.canonical_provider.module_name &&
                impl_->values->identifier(entity->name) ==
                    observation.canonical_name;
            StableFingerprint current_fingerprint;
            if (canonical_matches &&
                observation.kind == DependencyObservationKind::NominalBinding)
              current_fingerprint = entity->fingerprint;
            if (canonical_matches &&
                observation.kind ==
                    DependencyObservationKind::RelocationClosure) {
              const auto artifact = std::ranges::find_if(
                  provider->nominalArtifacts(), [&](const auto &candidate) {
                    return candidate.entity.canonical_package ==
                               observation.canonical_provider.package_name &&
                           candidate.entity.canonical_module ==
                               observation.canonical_provider.module_name &&
                           candidate.entity.canonical_name ==
                               observation.canonical_name;
                  });
              if (artifact != provider->nominalArtifacts().end())
                current_fingerprint = fingerprintRelocationClosure(*artifact);
            }
            if (current_fingerprint != observation.expected_fingerprint)
              invalidations.push_back(
                  {observation.kind ==
                           DependencyObservationKind::RelocationClosure
                       ? UnitInvalidationReason::RelocationClosureChanged
                       : UnitInvalidationReason::NominalBindingChanged,
                   observation.provider, observation.binding_name});
            continue;
          }
          const auto binding_name =
              impl_->values->internIdentifier(observation.binding_name);
          PublicBindingId binding_id;
          for (std::uint32_t index = 0; index < provider->bindingCount();
               ++index) {
            const auto candidate_id = PublicBindingId(index);
            const auto &candidate = provider->function(candidate_id);
            if (candidate.name != binding_name)
              continue;
            const auto *candidate_entity =
                impl_->public_interfaces->tryGetEntity(
                    candidate.canonical_entity);
            if (candidate_entity &&
                impl_->values->identifier(candidate_entity->package_name) ==
                    observation.canonical_provider.package_name &&
                impl_->values->identifier(candidate_entity->module_name) ==
                    observation.canonical_provider.module_name &&
                impl_->values->identifier(candidate_entity->name) ==
                    observation.canonical_name) {
              binding_id = candidate_id;
              break;
            }
          }
          const auto *binding =
              binding_id.hasValue() ? &provider->function(binding_id) : nullptr;
          const auto *entity = binding ? impl_->public_interfaces->tryGetEntity(
                                             binding->canonical_entity)
                                       : nullptr;
          if (!entity ||
              impl_->values->identifier(entity->package_name) !=
                  observation.canonical_provider.package_name ||
              impl_->values->identifier(entity->module_name) !=
                  observation.canonical_provider.module_name ||
              impl_->values->identifier(entity->name) !=
                  observation.canonical_name ||
              entity->fingerprint != observation.expected_fingerprint) {
            invalidations.push_back(
                {observation.kind ==
                         DependencyObservationKind::LifecycleCallable
                     ? UnitInvalidationReason::LifecycleCallableChanged
                     : UnitInvalidationReason::EntityBindingChanged,
                 observation.provider, observation.binding_name});
            continue;
          }
          if (observation.kind ==
              DependencyObservationKind::LifecycleCallable) {
            const PackageModuleArtifact *provider_artifact = nullptr;
            if (observation.provider.package_name == impl_->package_name) {
              const auto found = std::ranges::find(
                  module_artifacts, observation.provider.module_name,
                  [](const PackageModuleArtifact &module) {
                    return module.moduleName();
                  });
              if (found != module_artifacts.end())
                provider_artifact = &*found;
              else if (observation.provider.module_name == module_name)
                provider_artifact = previous_module;
            } else {
              const auto dependency = std::ranges::find(
                  dependency_manifests, observation.provider.package_name,
                  [](const CompilerPackageArtifactManifest *manifest) {
                    return manifest->packageName();
                  });
              if (dependency != dependency_manifests.end())
                provider_artifact =
                    (*dependency)->findModule(observation.provider.module_name);
            }
            const auto has_witness = [&](const auto *manifest) {
              return manifest &&
                     std::ranges::any_of(
                         manifest->modules(), [&](const auto &module) {
                           return std::ranges::any_of(
                               module.nominal_semantic_witnesses,
                               [&](const NominalArtifactReference &reference) {
                                 return reference.result_fingerprint ==
                                        observation.witness_fingerprint;
                               });
                         });
            };
            auto witness_available =
                provider_artifact &&
                std::ranges::any_of(
                    provider_artifact->nominal_semantic_witnesses,
                    [&](const NominalArtifactReference &reference) {
                      return reference.result_fingerprint ==
                             observation.witness_fingerprint;
                    });
            if (!witness_available)
              witness_available =
                  std::ranges::any_of(dependency_manifests, has_witness);
            if (!witness_available)
              witness_available = std::ranges::any_of(
                  request.template_dependency_closure, has_witness);
            if (!witness_available)
              invalidations.push_back(
                  {UnitInvalidationReason::LifecycleCallableChanged,
                   observation.provider, observation.binding_name});
          }
        }
      }
    }

    if (previous_module && invalidations.empty() &&
        required_transient_outputs.contains(id.index))
      invalidations.push_back({UnitInvalidationReason::RequestedOutput});

    if (request.mode == CompilationRequest::Mode::Build && previous_module &&
        invalidations.empty()) {
      auto loaded =
          preloaded_objects[id.index] ? std::move(*preloaded_objects[id.index])
          : request.load_object
              ? request.load_object(previous_module->object_fingerprint,
                                    previous_module->specific_fingerprint)
              : ObjectArtifactLoadResult{};
      if (loaded.status == ObjectArtifactLoadStatus::Error) {
        error = loaded.error.empty()
                    ? "failed to load an incremental object artifact"
                    : std::move(loaded.error);
        return false;
      }
      if (loaded.status == ObjectArtifactLoadStatus::Missing) {
        invalidations.push_back(
            {UnitInvalidationReason::ObjectArtifactMissing});
      } else if (loaded.status == ObjectArtifactLoadStatus::Corrupt ||
                 loaded.bytes.empty() ||
                 fingerprintObject(impl_->target_triple, loaded.bytes,
                                   previous_module->specific_fingerprint) !=
                     previous_module->object_fingerprint) {
        invalidations.push_back(
            {UnitInvalidationReason::ObjectArtifactCorrupt});
      } else {
        data.object_bytes = std::move(loaded.bytes);
      }
    }

    if (previous_module && invalidations.empty() &&
        !request.component_exports.empty())
      invalidations.push_back({UnitInvalidationReason::RequestedOutput});
    if (previous_module && invalidations.empty()) {
      data.public_interface = impl_->public_interfaces->registerArtifact(
          data.id, previous_module->public_interface, error);
      if (!data.public_interface.hasValue() ||
          !impl_->public_interfaces->verify(error))
        return false;
      data.reused = true;
      data.completed = true;
      decisions.push_back({module_name, UnitCompilationAction::Reuse, {}});
      module_artifacts.push_back(*previous_module);
      check_module_artifacts.push_back(
          {.source_fingerprint = previous_module->source_fingerprint,
           .unit_kind = previous_module->unit_kind,
           .public_interface = previous_module->public_interface,
           .module_dependencies = previous_module->module_dependencies,
           .required_foreign_symbols =
               previous_module->required_foreign_symbols,
           .observations = previous_module->observations,
           .specific_fingerprint = previous_module->specific_fingerprint});
      continue;
    }

    std::vector<ImportIR> imports;
    imports.reserve(data.imports.size());
    for (const auto &dependency : data.imports) {
      auto interface_id = dependency.public_interface;
      if (dependency.local_unit.hasValue())
        interface_id =
            impl_->units[dependency.local_unit.index]->impl_->public_interface;
      if (!interface_id.hasValue()) {
        error = "dependency did not publish a public interface";
        return false;
      }
      imports.push_back({interface_id, dependency.is_export, dependency.alias});
    }
    try {
      if (data.kind == CompilationUnitKind::ForeignBinding) {
        data.sem_ir.emplace(buildCFDLSemIR(
            *data.cfdl_syntax, data.arena, *impl_->values, data.diagnostics,
            *impl_->public_interfaces, *impl_->interop_registry, data.id,
            impl_->package_name, data.module_name, impl_->target_triple,
            imports, impl_->language_contract.source));
      } else {
        data.sem_ir.emplace(buildSemIR(
            *data.parse_tree, data.arena, *impl_->values, data.diagnostics,
            *impl_->public_interfaces, *impl_->interop_registry, data.id,
            data.module_name, imports,
            fingerprintConcreteSemanticOptions(impl_->resolved_features),
            request.load_specialization,
            llvm::Triple(llvm::Triple::normalize(impl_->target_triple))
                    .isArch32Bit()
                ? 32U
                : 64U,
            llvm::Triple::normalize(impl_->target_triple),
            impl_->compiler_intrinsics, impl_->language_contract.source));
      }
    } catch (const std::exception &exception) {
      error = "SemIR construction failed for module '" + module_name +
              "': " + exception.what();
      return false;
    }
    if (data.diagnostics.hasError()) {
      error = format_diagnostics();
      return false;
    }
    try {
      data.public_interface = registerPublicInterface(
          *data.sem_ir, *impl_->public_interfaces, *impl_->interop_registry,
          impl_->package_name_id, error, &data.native_definition_exports);
    } catch (const std::exception &exception) {
      error = "public interface registration failed for module '" +
              module_name + "': " + exception.what();
      return false;
    }
    if (!data.public_interface.hasValue() ||
        !impl_->public_interfaces->verify(error)) {
      return false;
    }
    const auto *source_interface =
        impl_->public_interfaces->tryGet(data.public_interface);
    for (std::uint32_t function_index = 0;
         source_interface && function_index < data.sem_ir->functionCount();
         ++function_index) {
      const auto function_id = FunctionId(function_index);
      auto function = data.sem_ir->function(function_id);
      if ((function.flags & SemFunctionAsync) == 0)
        continue;
      auto binding_name = data.sem_ir->name(function.name).text;
      if ((function.flags & SemFunctionPublic) == 0 ||
          impl_->values->identifier(binding_name) == "main")
        binding_name = impl_->values->internIdentifier(
            "$async$" + std::string(impl_->values->identifier(binding_name)));
      function.flags |= SemFunctionCoroutineScaffold;
      data.sem_ir->setFunction(function_id, function);
      bool bound = false;
      std::string bind_error;
      for (const auto binding : source_interface->findFunctions(binding_name)) {
        if (data.sem_ir->bindCoroutineConstructorEntity(
                function_id,
                source_interface->function(binding).canonical_entity,
                bind_error)) {
          bound = true;
          break;
        }
      }
      if (!bound) {
        error = bind_error.empty()
                    ? "source async function has no constructor entity"
                    : std::move(bind_error);
        return false;
      }
    }
    auto public_artifact =
        impl_->public_interfaces->buildArtifact(data.public_interface, error);
    if (!error.empty())
      return false;
    if (!request.component_identity.empty()) {
      const auto module_prefix =
          std::string(public_artifact.moduleName()) + "::";
      for (const auto &requested : request.component_exports) {
        if (!requested.starts_with(module_prefix))
          continue;
        const auto source_name = requested.substr(module_prefix.size());
        const auto candidates = public_artifact.findFunctions(source_name);
        if (candidates.size() != 1) {
          error = candidates.empty()
                      ? "component export '" + requested + "' was not found"
                      : "component export '" + requested +
                            "' names an overloaded function";
          return false;
        }
        const auto &function = *candidates.front();
        if (function.member_kind != PublicFunctionArtifact::MemberKind::None ||
            function.member_owner || function.generic_parameter_count != 0 ||
            function.generic_template ||
            function.execution_kind != PublicFunctionExecutionKind::Immediate ||
            function.declaration_kind !=
                PublicCallableDeclarationKind::Definition ||
            function.is_unsafe || function.error_type) {
          error = "component export '" + requested +
                  "' must be a concrete synchronous safe free definition";
          return false;
        }
        ComponentExportArtifact artifact;
        artifact.canonical_name = requested;
        artifact.result = componentValueKind(function.return_type, true)
                              .value_or(ComponentValueKind::Count);
        for (const auto &parameter : function.parameters) {
          const auto kind = componentValueKind(parameter, false);
          if (!kind) {
            error = "component export '" + requested +
                    "' has a parameter outside ABI epoch 1";
            return false;
          }
          artifact.parameters.push_back(*kind);
        }
        if (artifact.result == ComponentValueKind::Count) {
          error = "component export '" + requested +
                  "' has a result outside ABI epoch 1";
          return false;
        }
        artifact.export_id =
            componentExportId(request.component_identity, requested);
        artifact.signature_digest =
            componentSignatureDigest(artifact.parameters, artifact.result);
        FunctionRefId target;
        for (std::uint32_t index = 0; index < data.sem_ir->functionRefCount();
             ++index) {
          const auto reference = FunctionRefId(index);
          const auto &candidate = data.sem_ir->functionRef(reference);
          if (!candidate.local_function.hasValue())
            continue;
          const auto &local = data.sem_ir->function(candidate.local_function);
          if ((local.flags & SemFunctionPublic) != 0 &&
              !local.semantic_owner.hasValue() &&
              data.sem_ir->identifier(data.sem_ir->name(local.name).text) ==
                  source_name) {
            if (target.hasValue()) {
              error = "component export '" + requested +
                      "' has ambiguous local definitions";
              return false;
            }
            target = reference;
          }
        }
        if (!target.hasValue()) {
          error = "component export '" + requested +
                  "' has no local lowering target";
          return false;
        }
        data.component_exports.push_back({std::move(artifact), target.index});
        data.native_definition_exports.functions.push_back(
            data.sem_ir->functionRef(target).local_function.index);
      }
      data.native_definition_exports.canonicalize();
    }
    if (request.mode == CompilationRequest::Mode::Build) {
      std::optional<BuiltNominalArtifacts> nominal_artifacts;
      try {
        nominal_artifacts = buildNominalArtifacts(
            *data.sem_ir, public_artifact, *impl_->public_interfaces,
            fingerprintConcreteSemanticOptions(impl_->resolved_features),
            impl_->target_triple, resolve_nominal_semantic_options,
            resolve_nominal_semantic_witness, error);
      } catch (const std::exception &exception) {
        error = "nominal artifact construction failed for module '" +
                module_name + "': " + exception.what();
        return false;
      }
      if (!nominal_artifacts) {
        if (error.empty())
          error = "failed to build nominal artifacts";
        return false;
      }
      for (const auto &[type, witness_index] :
           nominal_artifacts->type_witness_bindings) {
        if (witness_index >=
                nominal_artifacts->nominal_semantic_witnesses.size() ||
            !data.sem_ir->bindNominalSemanticWitness(
                type,
                nominal_artifacts->nominal_semantic_witnesses[witness_index],
                error))
          return false;
      }
      data.nominal_type_specifics = std::move(nominal_artifacts->specifics);
      data.nominal_semantic_witnesses =
          std::move(nominal_artifacts->nominal_semantic_witnesses);
      available_nominal_semantic_witnesses.insert(
          available_nominal_semantic_witnesses.end(),
          data.nominal_semantic_witnesses.begin(),
          data.nominal_semantic_witnesses.end());
      data.nominal_type_layouts = std::move(nominal_artifacts->layouts);
      try {
        data.low_ir.emplace(
            lowerToLowIR(*data.sem_ir, data.arena,
                         llvm::Triple::normalize(impl_->target_triple),
                         data.nominal_type_layouts,
                         nominal_artifacts->type_layout_bindings));
      } catch (const std::exception &exception) {
        error = "LowIR construction failed for module '" + module_name +
                "': " + exception.what();
        return false;
      }
      if (!data.low_ir->verify(error))
        return false;
      try {
        data.llvm_module.emplace(lowerToLLVM(
            *data.low_ir, impl_->package_name,
            impl_->values->identifier(data.module_name), impl_->target_triple,
            error, unit_emission_role, data.native_definition_exports.functions,
            impl_->runtime_symbol_mappings, request.debug_info,
            data.component_exports, request.optimization));
      } catch (const std::exception &exception) {
        error = "LLVM construction failed for module '" + module_name +
                "': " + exception.what();
        return false;
      }
      if (!error.empty())
        return false;
      data.object_bytes = data.llvm_module->emitObject(error);
      if (!error.empty() || data.object_bytes.empty()) {
        if (error.empty())
          error = "compiler compilation emitted an empty native object";
        return false;
      }
    }
    data.completed = true;

    auto observations = collect_observations(data);
    if (!observations) {
      error = "cannot build an artifact from an invalid import observation";
      return false;
    }
    const auto specific_fingerprint =
        data.sem_ir->specificDependencyFingerprint();
    std::vector<ModuleIdentity> module_dependencies;
    module_dependencies.reserve(data.imports.size());
    for (const auto &dependency : data.imports)
      module_dependencies.push_back(dependency.provider);
    std::ranges::sort(module_dependencies);
    module_dependencies.erase(
        std::unique(module_dependencies.begin(), module_dependencies.end()),
        module_dependencies.end());
    std::vector<ForeignSymbolRequirement> required_foreign_symbols;
    for (std::uint32_t function_index = 0;
         function_index < data.sem_ir->functionCount(); ++function_index) {
      const auto function = FunctionId(function_index);
      if (data.sem_ir->functionDeclaration(function).kind !=
          SemCallableDeclarationKind::Foreign)
        continue;
      const auto &declaration = data.sem_ir->functionDeclaration(function);
      if (!declaration.foreign_signature ||
          !declaration.external_symbol.hasValue()) {
        error = "foreign declaration omitted its structured link requirement";
        return false;
      }
      required_foreign_symbols.push_back(
          {.logical_name = std::string(data.sem_ir->identifier(
               data.sem_ir->name(data.sem_ir->function(function).name).text)),
           .external_symbol = std::string(
               data.sem_ir->identifier(declaration.external_symbol)),
           .calling_convention =
               declaration.foreign_signature->calling_convention,
           .signature_fingerprint =
               foreignAbiSignatureFingerprint(*declaration.foreign_signature)});
    }
    std::ranges::sort(required_foreign_symbols);
    required_foreign_symbols.erase(std::unique(required_foreign_symbols.begin(),
                                               required_foreign_symbols.end()),
                                   required_foreign_symbols.end());
    if (request.mode == CompilationRequest::Mode::Build) {
      module_artifacts.push_back(
          {.source_fingerprint = source_fingerprint,
           .unit_kind = data.kind,
           .emission_role = unit_emission_role,
           .public_interface = public_artifact,
           .module_dependencies = module_dependencies,
           .required_foreign_symbols = required_foreign_symbols,
           .observations = *observations,
           .specializations = std::vector<ConcreteSpecializationReference>(
               data.sem_ir->specializationReferences().begin(),
               data.sem_ir->specializationReferences().end()),
           .nominal_type_specifics =
               [&] {
                 std::vector<NominalArtifactReference> references;
                 for (const auto &artifact : data.nominal_type_specifics)
                   references.push_back({artifact.request_fingerprint,
                                         artifact.result_fingerprint});
                 return references;
               }(),
           .nominal_semantic_witnesses =
               [&] {
                 std::vector<NominalArtifactReference> references;
                 for (const auto &artifact : data.nominal_semantic_witnesses)
                   references.push_back({artifact.request_fingerprint,
                                         artifact.result_fingerprint});
                 return references;
               }(),
           .nominal_type_layouts =
               [&] {
                 std::vector<NominalArtifactReference> references;
                 for (const auto &artifact : data.nominal_type_layouts)
                   references.push_back({artifact.request_fingerprint,
                                         artifact.result_fingerprint});
                 return references;
               }(),
           .specific_fingerprint = specific_fingerprint,
           .object_fingerprint = fingerprintObject(
               impl_->target_triple, data.object_bytes, specific_fingerprint)});
    }
    check_module_artifacts.push_back(
        {.source_fingerprint = source_fingerprint,
         .unit_kind = data.kind,
         .public_interface = std::move(public_artifact),
         .module_dependencies = std::move(module_dependencies),
         .required_foreign_symbols = std::move(required_foreign_symbols),
         .observations = std::move(*observations),
         .specific_fingerprint = specific_fingerprint});
    decisions.push_back({module_name, UnitCompilationAction::Rebuild,
                         std::move(invalidations)});
  }
