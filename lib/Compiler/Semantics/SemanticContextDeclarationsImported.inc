// Included by SemanticContextDeclarations.cpp; this fragment aliases the
// single SemanticContext and preserves declaration-phase ordering.
SemanticContext::concretePublicType(CanonicalTypeId type_id,
                                    GenericId symbolic_generic) {
  const auto &canonical = values_.generics().type(type_id);
  switch (canonical.kind) {
  case CanonicalTypeKind::Void:
    return PublicTypeKind::Void;
  case CanonicalTypeKind::Never:
    return PublicTypeKind::Never;
  case CanonicalTypeKind::Bool:
    return PublicTypeKind::Bool;
  case CanonicalTypeKind::Char:
    return PublicTypeKind::Char;
  case CanonicalTypeKind::Integer:
    return PublicType::integer(canonical.arg0, canonical.arg1 != 0);
  case CanonicalTypeKind::Float:
    return PublicType::floating(canonical.arg0);
  case CanonicalTypeKind::String:
    return PublicTypeKind::String;
  case CanonicalTypeKind::Array: {
    auto element =
        concretePublicType(CanonicalTypeId(canonical.arg0), symbolic_generic);
    if (!element || canonical.arg1 == 0)
      return std::nullopt;
    return PublicType(std::move(*element), canonical.arg1);
  }
  case CanonicalTypeKind::Tuple: {
    std::vector<PublicType> elements;
    for (const auto element_type : canonical.elements) {
      auto element = concretePublicType(element_type, symbolic_generic);
      if (!element)
        return std::nullopt;
      elements.push_back(std::move(*element));
    }
    return PublicType::tuple(std::move(elements));
  }
  case CanonicalTypeKind::Slice: {
    if (canonical.elements.size() != 1)
      return std::nullopt;
    auto element =
        concretePublicType(canonical.elements.front(), symbolic_generic);
    return element ? std::optional(PublicType::slice(std::move(*element),
                                                     canonical.arg0 != 0))
                   : std::nullopt;
  }
  case CanonicalTypeKind::RawPointer:
    if (canonical.elements.size() != 1)
      return std::nullopt;
    if (auto pointee =
            concretePublicType(canonical.elements.front(), symbolic_generic))
      return PublicType::rawPointer(std::move(*pointee), canonical.arg1 != 0);
    return std::nullopt;
  case CanonicalTypeKind::Function: {
    if (canonical.elements.empty())
      return std::nullopt;
    std::vector<PublicType> parameters;
    for (std::size_t index = 0; index + 1 < canonical.elements.size();
         ++index) {
      auto parameter =
          concretePublicType(canonical.elements[index], symbolic_generic);
      if (!parameter)
        return std::nullopt;
      parameters.push_back(std::move(*parameter));
    }
    auto result =
        concretePublicType(canonical.elements.back(), symbolic_generic);
    return result ? std::optional(PublicType::function(std::move(parameters),
                                                       std::move(*result)))
                  : std::nullopt;
  }
  case CanonicalTypeKind::CFunctionPointer:
  case CanonicalTypeKind::CVariadicFunctionPointer: {
    if (canonical.elements.empty())
      return std::nullopt;
    std::vector<PublicType> parameters;
    parameters.reserve(canonical.elements.size() - 1);
    for (std::size_t index = 0; index + 1 < canonical.elements.size();
         ++index) {
      auto parameter =
          concretePublicType(canonical.elements[index], symbolic_generic);
      if (!parameter)
        return std::nullopt;
      parameters.push_back(std::move(*parameter));
    }
    auto result =
        concretePublicType(canonical.elements.back(), symbolic_generic);
    return result ? std::optional(PublicType::cFunctionPointer(
                        std::move(parameters), std::move(*result),
                        canonical.kind ==
                            CanonicalTypeKind::CVariadicFunctionPointer,
                        canonical.callable_contract,
                        canonical.callable_context_parameter))
                  : std::nullopt;
  }
  case CanonicalTypeKind::CallbackAdapter: {
    if (canonical.elements.size() != 3)
      return std::nullopt;
    auto entry = concretePublicType(canonical.elements[0], symbolic_generic);
    auto context = concretePublicType(canonical.elements[1], symbolic_generic);
    auto release = concretePublicType(canonical.elements[2], symbolic_generic);
    return entry && context && release
               ? std::optional(PublicType::callbackAdapter(std::move(*entry),
                                                           std::move(*context),
                                                           std::move(*release)))
               : std::nullopt;
  }
  case CanonicalTypeKind::CallbackRegistration: {
    if (canonical.elements.size() != 5 && canonical.elements.size() != 7 &&
        canonical.elements.size() != 8 && canonical.elements.size() != 10)
      return std::nullopt;
    std::vector<std::optional<PublicType>> fields(canonical.elements.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fields[index] =
          concretePublicType(canonical.elements[index], symbolic_generic);
      if (!fields[index])
        return std::nullopt;
    }
    return PublicType::callbackRegistration(
        std::move(*fields[0]), std::move(*fields[1]), std::move(*fields[2]),
        std::move(*fields[3]), std::move(*fields[4]),
        static_cast<std::uint8_t>(canonical.registration_authority),
        canonical.registration_entry_parameter,
        canonical.registration_userdata_parameter,
        canonical.registration_release_parameter,
        canonical.registration_bindings,
        fields.size() >= 7 ? std::optional<PublicType>(std::move(*fields[5]))
                           : std::nullopt,
        fields.size() >= 7 ? std::optional<PublicType>(std::move(*fields[6]))
                           : std::nullopt,
        fields.size() >= 8 ? std::optional<PublicType>(std::move(*fields[7]))
                           : std::nullopt,
        fields.size() == 10 ? std::optional<PublicType>(std::move(*fields[8]))
                            : std::nullopt,
        fields.size() == 10 ? std::optional<PublicType>(std::move(*fields[9]))
                            : std::nullopt,
        canonical.registration_arm_parameters,
        canonical.registration_detach_parameters);
  }
  case CanonicalTypeKind::CallbackCompletion: {
    if (canonical.elements.size() != 4 && canonical.elements.size() != 5 &&
        canonical.elements.size() != 7)
      return std::nullopt;
    std::vector<std::optional<PublicType>> fields(canonical.elements.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fields[index] =
          concretePublicType(canonical.elements[index], symbolic_generic);
      if (!fields[index])
        return std::nullopt;
    }
    return PublicType::callbackCompletion(
        std::move(*fields[0]), std::move(*fields[1]), std::move(*fields[2]),
        std::move(*fields[3]),
        fields.size() >= 5 ? std::optional<PublicType>(std::move(*fields[4]))
                           : std::nullopt,
        static_cast<std::uint8_t>(canonical.registration_authority),
        fields.size() == 7 ? std::optional<PublicType>(std::move(*fields[5]))
                           : std::nullopt,
        fields.size() == 7 ? std::optional<PublicType>(std::move(*fields[6]))
                           : std::nullopt,
        canonical.registration_arm_parameters,
        canonical.registration_detach_parameters);
  }
  case CanonicalTypeKind::CallbackWake: {
    if (canonical.elements.size() != 1)
      return std::nullopt;
    auto completion =
        concretePublicType(canonical.elements.front(), symbolic_generic);
    return completion
               ? std::optional(PublicType::callbackWake(std::move(*completion)))
               : std::nullopt;
  }
  case CanonicalTypeKind::Nominal: {
    const auto local_type = sem_ir_.materializeType(type_id);
    if (!local_type.hasValue() ||
        sem_ir_.type(local_type).kind != SemTypeKind::Nominal)
      return std::nullopt;
    const auto &nominal =
        sem_ir_.nominalType(NominalTypeId(sem_ir_.type(local_type).arg0));
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(nominal.canonical_entity);
    if (!entity || entity->kind != PublicEntityKind::NominalType)
      return std::nullopt;
    std::vector<PublicType> arguments;
    arguments.reserve(canonical.elements.size());
    for (const auto argument : canonical.elements) {
      auto mapped = concretePublicType(argument, symbolic_generic);
      if (!mapped)
        return std::nullopt;
      arguments.push_back(std::move(*mapped));
    }
    return PublicType({PublicEntityKind::NominalType,
                       std::string(values_.identifier(entity->package_name)),
                       std::string(values_.identifier(entity->module_name)),
                       std::string(values_.identifier(entity->name)),
                       entity->fingerprint},
                      std::move(arguments));
  }
  case CanonicalTypeKind::Reference: {
    if (canonical.elements.size() != 1)
      return std::nullopt;
    auto pointee =
        concretePublicType(canonical.elements.front(), symbolic_generic);
    if (!pointee)
      return std::nullopt;
    const auto local = sem_ir_.materializeType(type_id);
    if (!local.hasValue())
      return std::nullopt;
    PublicReferenceProvenance provenance;
    if (sem_ir_.referenceProvenanceKind(local) ==
        SemReferenceProvenanceKind::Parameter) {
      provenance.kind = PublicReferenceProvenanceKind::Parameter;
      provenance.index = sem_ir_.referenceProvenanceIndex(local);
    }
    return PublicType(std::move(*pointee),
                      sem_ir_.referenceMutability(local) ==
                              SemReferenceMutability::Mutable
                          ? PublicReferenceMutability::Mutable
                          : PublicReferenceMutability::ReadOnly,
                      provenance);
  }
  case CanonicalTypeKind::AsyncFunction:
  case CanonicalTypeKind::CoroutineScope:
  case CanonicalTypeKind::CoroutineTask:
  case CanonicalTypeKind::CoroutineTaskOutcome:
  case CanonicalTypeKind::CoroutineTaskCompletion:
  case CanonicalTypeKind::CoroutineTaskCompletionSet:
  case CanonicalTypeKind::CoroutineTaskSelection:
  case CanonicalTypeKind::CoroutineChecked:
    return std::nullopt;
  case CanonicalTypeKind::TypeParameter:
    if (symbolic_generic.hasValue() && canonical.arg0 == symbolic_generic.index)
      return PublicType(PublicTypeKind::TypeParameter, canonical.arg1);
    for (std::uint32_t index = 0; index < sem_ir_.interfaceCount(); ++index) {
      const auto interface_id = InterfaceId(index);
      const auto &interface_value = sem_ir_.interface(interface_id);
      if (interface_value.generic != GenericId(canonical.arg0))
        continue;
      const auto requirement = std::ranges::find_if(
          interface_value.requirements,
          [&](const SemInterfaceRequirement &item) {
            return item.kind == SemInterfaceRequirementKind::AssociatedAlias &&
                   item.binding_index == canonical.arg1;
          });
      if (requirement == interface_value.requirements.end())
        continue;
      const auto constraint =
          std::ranges::find(active_resolved_constraints_, interface_id,
                            &SemInterfaceConstraint::interface_id);
      if (constraint == active_resolved_constraints_.end())
        return std::nullopt;
      auto subject = concretePublicType(
          sem_ir_.canonicalType(constraint->subject), symbolic_generic);
      if (!subject)
        return std::nullopt;
      const auto *entity =
          sem_ir_.importIRs().tryGetEntity(interface_value.canonical_entity);
      if (!entity)
        return std::nullopt;
      PublicEntityReferenceArtifact reference{
          PublicEntityKind::Interface,
          std::string(values_.identifier(entity->package_name)),
          std::string(values_.identifier(entity->module_name)),
          std::string(values_.identifier(entity->name)), entity->fingerprint};
      return PublicType::associated(std::move(*subject), std::move(reference),
                                    canonical.arg1);
    }
    return std::nullopt;
  case CanonicalTypeKind::Count:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<PublicEntityReferenceArtifact>
SemanticContext::publicEntityReference(PublicEntityId id) const {
  const auto *entity = sem_ir_.importIRs().tryGetEntity(id);
  if (!entity)
    return std::nullopt;
  return PublicEntityReferenceArtifact{
      PublicEntityKind::Function,
      std::string(values_.identifier(entity->package_name)),
      std::string(values_.identifier(entity->module_name)),
      std::string(values_.identifier(entity->name)), entity->fingerprint};
}

[[nodiscard]] FunctionRefId SemanticContext::tryLoadConcreteSpecific(
    const SemanticContext::CacheableSpecific &requested, NodeId location) {
  if (!specialization_loader_)
    return FunctionRefId::invalid();
  if (const auto found =
          request_function_refs_.find(requested.request_fingerprint.hex());
      found != request_function_refs_.end())
    return found->second;

  auto loaded = specialization_loader_(requested.request_fingerprint);
  const auto hit = loaded.status == ConcreteSpecializationLoadStatus::Found;
  sem_ir_.recordSpecializationLookup(hit);
  if (loaded.status == ConcreteSpecializationLoadStatus::Missing ||
      loaded.status == ConcreteSpecializationLoadStatus::Corrupt)
    return FunctionRefId::invalid();
  if (loaded.status == ConcreteSpecializationLoadStatus::Error) {
    emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
    return FunctionRefId::invalid();
  }
  const auto reject_semantic_cache = [&] {
    sem_ir_.recordSpecializationSemanticRejection();
    return FunctionRefId::invalid();
  };
  if (loaded.components.empty())
    return reject_semantic_cache();

  std::map<std::string, const ConcreteSpecializationComponentArtifact *>
      components;
  std::map<std::string, const ConcreteSpecificNodeArtifact *> nodes;
  for (const auto &component : loaded.components) {
    std::string verify_error;
    const auto component_key = component.fingerprint().hex();
    if (component.semanticOptionsFingerprint() !=
            semantic_options_fingerprint_ ||
        !component.verify(verify_error) ||
        !components.emplace(component_key, &component).second) {
      return reject_semantic_cache();
    }
    for (const auto &node : component.nodes()) {
      if (node.semantic_contract.domain == CallableSemanticDomain::Ordinary &&
          !sem_ir_.importIRs().registry().verifyOwnershipSummaryTypes(
              std::span(node.local_types).first(node.parameter_count),
              node.body.results.back(), node.ownership_summary, verify_error)) {
        return reject_semantic_cache();
      }
      if (!nodes.emplace(node.request_fingerprint.hex(), &node).second)
        return reject_semantic_cache();
    }
  }
  for (const auto &component : loaded.components) {
    for (const auto dependency : component.dependencies())
      if (!components.contains(dependency.hex()))
        return reject_semantic_cache();
    for (const auto &node : component.nodes()) {
      for (const auto &callee : node.callees) {
        if (callee.kind != ConcreteCallTargetKind::ExternalComponent)
          continue;
        const auto target = nodes.find(callee.request_fingerprint.hex());
        if (target == nodes.end())
          return reject_semantic_cache();
        const auto target_component =
            std::ranges::find_if(loaded.components, [&](const auto &candidate) {
              return candidate.fingerprint() == callee.component_fingerprint &&
                     candidate.findNode(callee.request_fingerprint);
            });
        if (target_component == loaded.components.end())
          return reject_semantic_cache();
        const auto *target_node = target->second;
        std::vector<PublicType> target_parameters(
            target_node->body.results.begin(),
            target_node->body.results.begin() + target_node->parameter_count);
        if (callee.callable_type !=
            PublicType::function(std::move(target_parameters),
                                 target_node->body.results.back()))
          return reject_semantic_cache();
      }
    }
  }
  const auto requested_node = nodes.find(requested.request_fingerprint.hex());
  if (requested_node == nodes.end() ||
      requested_node->second->template_entity != requested.entity ||
      requested_node->second->arguments != requested.arguments ||
      requested_node->second->constraint_witnesses !=
          requested.constraint_witnesses) {
    return reject_semantic_cache();
  }

  std::map<std::string, PublicEntityId> node_entities;
  for (const auto &[unused, node] : nodes) {
    (void)unused;
    const auto entity_id = sem_ir_.importIRs().registry().findEntity(
        node->template_entity.canonical_package,
        node->template_entity.canonical_module,
        node->template_entity.canonical_name, PublicEntityKind::Function,
        node->template_entity.expected_fingerprint);
    const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
    if (!entity ||
        entity->fingerprint != node->template_entity.expected_fingerprint ||
        !entity->generic.hasValue() ||
        node->arguments.size() != entity->generic_parameter_count)
      return reject_semantic_cache();
    const auto substitute_public_type =
        [&](const auto &self, const PublicType &source) -> PublicType {
      if (source.kind == PublicTypeKind::TypeParameter)
        return source.binding_index < node->arguments.size()
                   ? node->arguments[source.binding_index]
                   : PublicType{};
      PublicType result = source;
      for (auto &argument : result.arguments)
        argument = self(self, argument);
      return result;
    };
    const auto contains_projection = [&](const auto &self,
                                         const PublicType &type) -> bool {
      return type.kind == PublicTypeKind::TypeProjection ||
             std::ranges::any_of(type.arguments, [&](const auto &argument) {
               return self(self, argument);
             });
    };
    if (node->parameter_count != entity->parameters.size())
      return reject_semantic_cache();
    for (std::size_t parameter = 0; parameter < entity->parameters.size();
         ++parameter) {
      const auto expected = substitute_public_type(
          substitute_public_type, entity->parameters[parameter]);
      if (!contains_projection(contains_projection, expected) &&
          expected != node->body.results[parameter])
        return reject_semantic_cache();
    }
    const auto expected_result =
        substitute_public_type(substitute_public_type, entity->return_type);
    if (!contains_projection(contains_projection, expected_result) &&
        expected_result != node->body.results.back())
      return reject_semantic_cache();
    auto expected_contract = entity->semantic_contract;
    if (expected_contract.owner.kind != PublicTypeKind::Count)
      expected_contract.owner = substitute_public_type(substitute_public_type,
                                                       expected_contract.owner);
    if (expected_contract != node->semantic_contract)
      return reject_semantic_cache();
    node_entities.emplace(unused, entity_id);
  }

  for (const auto &[identity, node] : nodes) {
    const auto entity_id = node_entities.at(identity);
    const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
    std::vector<CanonicalTypeId> arguments;
    std::vector<CanonicalConstantId> constants;
    for (const auto argument : node->arguments) {
      const auto local_type =
          mapImportedPublicType(argument, GenericId::invalid(), location);
      if (!local_type.hasValue()) {
        emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
        return FunctionRefId::invalid();
      }
      const auto type = sem_ir_.canonicalType(local_type);
      if (!type.hasValue()) {
        emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
        return FunctionRefId::invalid();
      }
      arguments.push_back(type);
      constants.push_back(values_.generics().internTypeConstant(
          type, ConstantDependence::Concrete));
    }
    const auto specific = values_.generics().getOrAddSpecific(
        entity->generic, constants, node->constraint_witnesses);
    cacheable_specifics_.insert_or_assign(
        specific.index,
        CacheableSpecific{node->request_fingerprint, node->template_entity,
                          node->arguments, node->constraint_witnesses});
    const auto template_ref = resolvePublicEntity(entity_id, location);
    if (node->body.results.size() != node->parameter_count + 1) {
      emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
      return FunctionRefId::invalid();
    }
    std::vector<TypeId> cached_parameters;
    cached_parameters.reserve(node->parameter_count);
    for (const auto &parameter :
         std::span(node->body.results).first(node->parameter_count)) {
      const auto mapped =
          mapImportedPublicType(parameter, GenericId::invalid(), location);
      if (!mapped.hasValue()) {
        emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
        return FunctionRefId::invalid();
      }
      cached_parameters.push_back(mapped);
    }
    const auto cached_result = mapImportedPublicType(
        node->body.results.back(), GenericId::invalid(), location);
    if (!cached_result.hasValue()) {
      emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
      return FunctionRefId::invalid();
    }
    const auto cached_function_type =
        sem_ir_.addFunctionType(cached_parameters, cached_result);
    const auto reference =
        template_ref.hasValue()
            ? materializeLocalSpecific(template_ref, specific, arguments,
                                       location, false, false, {},
                                       cached_function_type)
            : FunctionRefId::invalid();
    if (!reference.hasValue()) {
      emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
      return FunctionRefId::invalid();
    }
    request_function_refs_.insert_or_assign(node->request_fingerprint.hex(),
                                            reference);
  }

  for (const auto &component : loaded.components) {
    const auto component_fingerprint = component.fingerprint();
    for (const auto &node : component.nodes()) {
      const auto reference =
          request_function_refs_.at(node.request_fingerprint.hex());
      const auto function_ref = sem_ir_.functionRef(reference);
      auto &specific = values_.generics().specific(function_ref.specific);
      specific.dependencies.clear();
      std::string materialize_error;
      if (!sem_ir_.materializeConcreteSpecificBody(
              function_ref.local_function, node,
              sem_ir_.function(function_ref.local_function).name,
              function_ref.local_type, location,
              [&](const ConcreteCallTargetArtifact &callee,
                  std::span<const InstId> call_arguments,
                  NodeId callee_location) {
                const auto expected_type = mapImportedPublicType(
                    callee.callable_type, GenericId::invalid(),
                    callee_location);
                const auto matches_expected_type = [&](FunctionRefId target) {
                  return target.hasValue() && expected_type.hasValue() &&
                         sem_ir_.functionRef(target).local_type ==
                             expected_type;
                };
                if (callee.kind == ConcreteCallTargetKind::ComponentNode ||
                    callee.kind == ConcreteCallTargetKind::ExternalComponent) {
                  const auto target = request_function_refs_.at(
                      callee.request_fingerprint.hex());
                  return matches_expected_type(target)
                             ? target
                             : FunctionRefId::invalid();
                }
                const auto entity_id =
                    sem_ir_.importIRs().registry().findEntity(
                        callee.public_entity.canonical_package,
                        callee.public_entity.canonical_module,
                        callee.public_entity.canonical_name);
                const auto *entity =
                    sem_ir_.importIRs().tryGetEntity(entity_id);
                if (!entity ||
                    entity->fingerprint !=
                        callee.public_entity.expected_fingerprint ||
                    callee.type_arguments.size() !=
                        entity->generic_parameter_count)
                  return FunctionRefId::invalid();
                for (std::uint32_t index = 0;
                     index < sem_ir_.functionRefCount(); ++index) {
                  const auto candidate = FunctionRefId(index);
                  const auto &candidate_ref = sem_ir_.functionRef(candidate);
                  if (candidate_ref.public_entity == entity_id &&
                      matches_expected_type(candidate))
                    return candidate;
                }
                const auto reference =
                    resolvePublicEntity(entity_id, callee_location);
                if (!reference.hasValue())
                  return FunctionRefId::invalid();
                std::vector<CanonicalTypeId> explicit_arguments;
                for (const auto &argument : callee.type_arguments) {
                  const auto local = mapImportedPublicType(
                      argument, GenericId::invalid(), callee_location);
                  if (!local.hasValue())
                    return FunctionRefId::invalid();
                  explicit_arguments.push_back(sem_ir_.canonicalType(local));
                }
                const auto specialized = specializeFunction(
                    reference, call_arguments, callee_location, false,
                    explicit_arguments);
                if (!matches_expected_type(specialized))
                  return FunctionRefId::invalid();
                std::string intrinsic_error;
                const auto canonical = sem_ir_.resolveCanonicalIntrinsic(
                    specialized, intrinsic_error);
                return canonical.value_or(FunctionRefId::invalid());
              },
              materialize_error)) {
        emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
        return FunctionRefId::invalid();
      }
      for (const auto &callee : node.callees) {
        if (callee.kind == ConcreteCallTargetKind::PublicEntity)
          continue;
        const auto target =
            request_function_refs_.at(callee.request_fingerprint.hex());
        const auto target_ref = sem_ir_.functionRef(target);
        specific.dependencies.push_back(
            {.target = target_ref.specific,
             .public_entity = target_ref.public_entity.index,
             .call_ordinal =
                 static_cast<std::uint32_t>(specific.dependencies.size())});
      }
      specific.definition_state = SpecificRegionState::Ready;
      specific.fingerprint_key = component_fingerprint.hex();
      specific_component_fingerprints_.insert_or_assign(
          function_ref.specific.index, component_fingerprint);
    }
    sem_ir_.recordSpecializationComponent(component, false);
  }
  const auto root =
      request_function_refs_.find(requested.request_fingerprint.hex());
  if (root == request_function_refs_.end()) {
    emit(DiagnosticKind::InvalidConcreteSpecializationArtifact, location);
    return FunctionRefId::invalid();
  }
  return root->second;
}
