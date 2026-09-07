// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
[[nodiscard]] NominalTypeId
SemanticContext::materializeImportedNominalShell(PublicEntityId entity_id,
                                                 NodeId location) {
  const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
  if (!entity || entity->kind != PublicEntityKind::NominalType)
    return NominalTypeId::invalid();
  auto found = imported_nominal_types_.find(entity_id.index);
  if (found == imported_nominal_types_.end()) {
    const auto local =
        sem_ir_.addNominalTypeDecl({sem_ir_.addName(entity->name),
                                    entity->generic,
                                    {},
                                    location,
                                    0,
                                    entity_id});
    found = imported_nominal_types_.emplace(entity_id.index, local).first;
    completion_.registerShell(
        local,
        std::string(values_.identifier(entity->package_name)) +
            "::" + std::string(values_.identifier(entity->module_name)) +
            "::" + std::string(values_.identifier(entity->name)),
        location);
  }
  return found->second;
}

[[nodiscard]] NominalTypeId
SemanticContext::materializeImportedNominal(PublicEntityId entity_id,
                                            NodeId location) {
  const auto local = materializeImportedNominalShell(entity_id, location);
  if (!local.hasValue())
    return local;
  const auto result = completion_.requireComplete(local, location, [&] {
    return completeImportedNominal(entity_id, local, location);
  });
  auto nominal = sem_ir_.nominalType(local);
  nominal.completion_state = result.succeeded()
                                 ? SemNominalCompletionState::Complete
                                 : SemNominalCompletionState::Failed;
  sem_ir_.setNominalType(local, std::move(nominal));
  if (!result.succeeded()) {
    if (completion_.markFailureDiagnosed(local))
      emit(result.failure &&
                   result.failure->kind == NominalCompletionFailureKind::Cycle
               ? DiagnosticKind::NominalCompletionCycle
               : DiagnosticKind::NominalCompletionFailed,
           location);
    return NominalTypeId::invalid();
  }
  return local;
}

[[nodiscard]] PublicEntityId SemanticContext::resolveArtifactEntity(
    const PublicEntityReferenceArtifact &reference) const {
  const auto entity = sem_ir_.importIRs().registry().findEntity(
      reference.canonical_package, reference.canonical_module,
      reference.canonical_name, reference.kind, reference.expected_fingerprint);
  const auto *value = sem_ir_.importIRs().tryGetEntity(entity);
  return value && value->kind == reference.kind &&
                 value->fingerprint == reference.expected_fingerprint
             ? entity
             : PublicEntityId::invalid();
}

[[nodiscard]] InterfaceId
SemanticContext::materializeImportedInterface(PublicEntityId entity_id,
                                              NodeId location) {
  const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
  if (!entity || entity->kind != PublicEntityKind::Interface ||
      !entity->interface_declaration)
    return InterfaceId::invalid();
  if (const auto found = imported_interfaces_.find(entity_id.index);
      found != imported_interfaces_.end()) {
    const auto state = imported_interface_states_[entity_id.index];
    return state == 3 ? InterfaceId::invalid() : found->second;
  }

  const auto &artifact = *entity->interface_declaration;
  const auto local = sem_ir_.addInterface({sem_ir_.addName(entity->name),
                                           entity->generic,
                                           artifact.explicit_parameter_count,
                                           {},
                                           {},
                                           location,
                                           0,
                                           entity->fingerprint,
                                           entity_id});
  imported_interfaces_.emplace(entity_id.index, local);
  imported_interface_states_[entity_id.index] = 1;
  const auto fail = [&] {
    imported_interface_states_[entity_id.index] = 3;
    return InterfaceId::invalid();
  };

  auto value = sem_ir_.interface(local);
  for (const auto &source : artifact.constraints) {
    const auto subject =
        mapImportedPublicType(source.subject, entity->generic, location);
    const auto referenced = resolveArtifactEntity(source.interface_entity);
    const auto referenced_interface =
        materializeImportedInterface(referenced, location);
    std::vector<TypeId> arguments;
    for (const auto &argument : source.arguments)
      arguments.push_back(
          mapImportedPublicType(argument, entity->generic, location));
    if (!subject.hasValue() || !referenced_interface.hasValue() ||
        std::ranges::any_of(arguments,
                            [](TypeId type) { return !type.hasValue(); }))
      return fail();
    value.constraints.push_back({subject, referenced_interface,
                                 sem_ir_.addTypeBlock(arguments),
                                 InterfaceWitnessId::invalid()});
  }
  for (const auto &requirement : artifact.requirements) {
    if (requirement.kind == PublicInterfaceRequirementKind::Function) {
      const auto target_entity = resolveArtifactEntity(requirement.function);
      const auto *target_signature =
          sem_ir_.importIRs().tryGetEntity(target_entity);
      if (!target_signature ||
          target_signature->kind != PublicEntityKind::Function)
        return fail();
      std::vector<TypeId> parameters;
      parameters.reserve(target_signature->parameters.size());
      for (const auto &parameter : target_signature->parameters) {
        const auto mapped =
            mapImportedPublicType(parameter, entity->generic, location);
        if (!mapped.hasValue())
          return fail();
        parameters.push_back(mapped);
      }
      const auto result = mapImportedPublicType(target_signature->return_type,
                                                entity->generic, location);
      if (!result.hasValue())
        return fail();
      std::optional<TypeId> error_type;
      if (target_signature->error_type) {
        error_type = mapImportedPublicType(*target_signature->error_type,
                                           entity->generic, location);
        if (!error_type->hasValue())
          return fail();
      }
      const auto requirement_type =
          target_signature->execution_kind == PublicFunctionExecutionKind::Async
              ? sem_ir_.addAsyncFunctionType(parameters, result, error_type)
              : sem_ir_.addFunctionType(parameters, result);
      const auto target = resolvePublicEntity(target_entity, location);
      if (!target.hasValue())
        return fail();
      value.requirements.push_back(
          {SemInterfaceRequirementKind::Function,
           sem_ir_.addName(values_.internIdentifier(requirement.name)),
           requirement_type, target, core::AnyId::InvalidIndex,
           requirement.has_default, location});
    } else {
      TypeId associated_type;
      if (requirement.has_default)
        associated_type = mapImportedPublicType(requirement.associated_type,
                                                entity->generic, location);
      if (requirement.has_default && !associated_type.hasValue())
        return fail();
      value.requirements.push_back(
          {SemInterfaceRequirementKind::AssociatedAlias,
           sem_ir_.addName(values_.internIdentifier(requirement.name)),
           associated_type, FunctionRefId::invalid(), requirement.binding_index,
           requirement.has_default, location});
    }
  }
  sem_ir_.setInterface(local, std::move(value));
  imported_interface_states_[entity_id.index] = 2;
  return local;
}

[[nodiscard]] TypeAliasId
SemanticContext::materializeImportedTypeAlias(PublicEntityId entity_id,
                                              NodeId location) {
  const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
  if (!entity || entity->kind != PublicEntityKind::TypeAlias ||
      !entity->type_alias)
    return TypeAliasId::invalid();
  if (const auto found = imported_type_aliases_.find(entity_id.index);
      found != imported_type_aliases_.end()) {
    const auto &alias = sem_ir_.typeAlias(found->second);
    return alias.state == SemTypeAliasState::Complete ? found->second
                                                      : TypeAliasId::invalid();
  }
  const auto local = sem_ir_.addTypeAlias({sem_ir_.addName(entity->name),
                                           entity->generic,
                                           TypeId::invalid(),
                                           {},
                                           location,
                                           0,
                                           SemTypeAliasState::Resolving,
                                           entity->fingerprint,
                                           entity_id});
  imported_type_aliases_.emplace(entity_id.index, local);
  auto alias = sem_ir_.typeAlias(local);
  alias.target = mapImportedPublicType(entity->type_alias->target,
                                       entity->generic, location);
  for (const auto &source : entity->type_alias->constraints) {
    const auto subject =
        mapImportedPublicType(source.subject, entity->generic, location);
    const auto referenced = resolveArtifactEntity(source.interface_entity);
    const auto referenced_interface =
        materializeImportedInterface(referenced, location);
    std::vector<TypeId> arguments;
    for (const auto &argument : source.arguments)
      arguments.push_back(
          mapImportedPublicType(argument, entity->generic, location));
    if (!subject.hasValue() || !referenced_interface.hasValue() ||
        std::ranges::any_of(arguments,
                            [](TypeId type) { return !type.hasValue(); })) {
      alias.state = SemTypeAliasState::Invalid;
      sem_ir_.setTypeAlias(local, std::move(alias));
      return TypeAliasId::invalid();
    }
    alias.constraints.push_back({subject, referenced_interface,
                                 sem_ir_.addTypeBlock(arguments),
                                 InterfaceWitnessId::invalid()});
  }
  alias.state = alias.target.hasValue() ? SemTypeAliasState::Complete
                                        : SemTypeAliasState::Invalid;
  sem_ir_.setTypeAlias(local, std::move(alias));
  return sem_ir_.typeAlias(local).state == SemTypeAliasState::Complete
             ? local
             : TypeAliasId::invalid();
}

[[nodiscard]] bool
SemanticContext::completeImportedNominal(PublicEntityId entity_id,
                                         NominalTypeId local, NodeId location) {
  const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
  if (!entity || entity->kind != PublicEntityKind::NominalType)
    return false;
  auto nominal = sem_ir_.nominalType(local);
  nominal.representation_policy = entity->nominal_representation_policy;
  nominal.kind = entity->nominal_kind;
  const auto is_atomic_storage = [&] {
    const auto &registry = sem_ir_.importIRs().registry();
    for (std::uint32_t index = 0; index < registry.entityCount(); ++index) {
      const auto *candidate = registry.tryGetEntity(PublicEntityId(index));
      if (!candidate || candidate->kind != PublicEntityKind::Function ||
          !isAtomicCompilerIntrinsic(candidate->intrinsic_role) ||
          !candidate->member_owner)
        continue;
      const auto &owner = *candidate->member_owner;
      if (owner.canonical_package == values_.identifier(entity->package_name) &&
          owner.canonical_module == values_.identifier(entity->module_name) &&
          owner.canonical_name == values_.identifier(entity->name) &&
          owner.expected_fingerprint == entity->fingerprint)
        return true;
    }
    return false;
  }();
  if (is_atomic_storage) {
    nominal.lifecycle_copy =
        static_cast<std::uint8_t>(SemLifecycleCopyPolicy::Delete);
    nominal.lifecycle_move =
        static_cast<std::uint8_t>(SemLifecycleMovePolicy::Default);
    nominal.lifecycle_drop =
        static_cast<std::uint8_t>(SemLifecycleDropPolicy::Default);
  }
  nominal.foreign_invalid_state = entity->nominal_foreign_invalid_state;
  nominal.foreign_invalid_integer = entity->nominal_foreign_invalid_integer;
  nominal.is_value_enum = entity->nominal_is_value_enum;
  for (const auto &field : entity->nominal_fields) {
    const auto field_type =
        mapImportedPublicType(field.type, entity->generic, location);
    if (!field_type.hasValue())
      return false;
    nominal.fields.push_back(
        {sem_ir_.addName(values_.internIdentifier(field.name)), field_type,
         location, field.is_public ? 1U : 0U,
         [&] {
           std::vector<NameId> path;
           for (const auto &component : field.storage_path)
             path.push_back(
                 sem_ir_.addName(values_.internIdentifier(component)));
           return path;
         }(),
         field.projection_kind,
         field.projector_name.empty()
             ? NameId::invalid()
             : sem_ir_.addName(values_.internIdentifier(field.projector_name)),
         [&] {
           std::vector<NameId> path;
           for (const auto &component : field.projection_region_path)
             path.push_back(
                 sem_ir_.addName(values_.internIdentifier(component)));
           return path;
         }(),
         field.bit_begin, field.bit_end});
  }
  for (const auto &source_variant : entity->nominal_variants) {
    SemEnumVariant variant;
    variant.name =
        sem_ir_.addName(values_.internIdentifier(source_variant.name));
    variant.declaration = location;
    variant.shape = source_variant.shape == PublicEnumPayloadShape::Unit
                        ? SemEnumPayloadShape::Unit
                    : source_variant.shape == PublicEnumPayloadShape::Tuple
                        ? SemEnumPayloadShape::Tuple
                        : SemEnumPayloadShape::Struct;
    variant.discriminant = source_variant.discriminant;
    for (const auto &field : source_variant.fields) {
      const auto field_type =
          mapImportedPublicType(field.type, entity->generic, location);
      if (!field_type.hasValue())
        return false;
      variant.fields.push_back(
          {sem_ir_.addName(values_.internIdentifier(field.name)), field_type,
           location, field.is_public ? 1U : 0U});
    }
    nominal.variants.push_back(std::move(variant));
  }
  if (entity->nominal_object_repr_pattern) {
    nominal.object_repr_pattern = mapImportedPublicType(
        *entity->nominal_object_repr_pattern, entity->generic, location);
    if (!nominal.object_repr_pattern.hasValue())
      return false;
  }
  if (entity->nominal_value_repr_pattern) {
    nominal.value_repr_pattern = mapImportedPublicType(
        *entity->nominal_value_repr_pattern, entity->generic, location);
    if (!nominal.value_repr_pattern.hasValue())
      return false;
  }
  if (entity->nominal_foreign_representation) {
    nominal.foreign_representation = mapImportedPublicType(
        *entity->nominal_foreign_representation, entity->generic, location);
    if (!nominal.foreign_representation.hasValue())
      return false;
  }
  if (entity->nominal_foreign_handle_type) {
    nominal.foreign_handle_type = mapImportedPublicType(
        *entity->nominal_foreign_handle_type, entity->generic, location);
    if (!nominal.foreign_handle_type.hasValue())
      return false;
  }
  if (entity->nominal_foreign_completion_handle_type) {
    nominal.foreign_completion_handle_type =
        mapImportedPublicType(*entity->nominal_foreign_completion_handle_type,
                              entity->generic, location);
    if (!nominal.foreign_completion_handle_type.hasValue())
      return false;
  }
  if (entity->nominal_foreign_callback_type) {
    nominal.foreign_callback_type = mapImportedPublicType(
        *entity->nominal_foreign_callback_type, entity->generic, location);
    if (!nominal.foreign_callback_type.hasValue())
      return false;
  }
  if (entity->nominal_foreign_waker_type) {
    nominal.foreign_waker_type = mapImportedPublicType(
        *entity->nominal_foreign_waker_type, entity->generic, location);
    if (!nominal.foreign_waker_type.hasValue())
      return false;
  }
  if (nominal.kind == NominalKind::ForeignResource) {
    CanonicalForeignResourceProtocol protocol;
    protocol.facts = entity->nominal_foreign_resource_protocol;
    protocol.types.push_back(
        sem_ir_.canonicalType(sem_ir_.addNominalType(local)));
    if (nominal.foreign_completion_handle_type.hasValue())
      protocol.types.push_back(
          sem_ir_.canonicalType(sem_ir_.addForeignCompletionType(local)));
    if (nominal.foreign_callback_type.hasValue())
      protocol.types.push_back(
          sem_ir_.canonicalType(nominal.foreign_callback_type));
    for (const auto &operation : entity->nominal_foreign_resource_operations) {
      const auto target = sem_ir_.importIRs().registry().findEntity(
          operation.target.canonical_package, operation.target.canonical_module,
          operation.target.canonical_name, PublicEntityKind::Function,
          operation.target.expected_fingerprint);
      const auto *target_entity = sem_ir_.importIRs().tryGetEntity(target);
      if (!target.hasValue() || !target_entity ||
          target_entity->fingerprint != operation.target.expected_fingerprint)
        return false;
      const auto target_ref = resolvePublicEntity(target, location);
      if (!target_ref.hasValue())
        return false;
      protocol.types.push_back(
          sem_ir_.canonicalType(sem_ir_.functionRef(target_ref).local_type));
      const auto role = protocol.facts.findRole(operation.role);
      if (!role || role->callable_type_index + 1 != protocol.types.size())
        return false;
      const auto &target_type =
          sem_ir_.type(sem_ir_.functionRef(target_ref).local_type);
      const auto result = TypeId(target_type.arg1);
      nominal.foreign_resource_operations.push_back(
          {sem_ir_.addName(values_.internIdentifier(operation.name)),
           operation.role, target_ref, operation.resource_parameter,
           operation.completion_parameter,
           operation.role == ForeignResourceRoleKind::AcquireOwned
               ? sem_ir_.addNominalType(local)
           : operation.role == ForeignResourceRoleKind::CancelAsync
               ? sem_ir_.addForeignCompletionType(local)
               : result});
    }
    std::string protocol_error;
    if (!protocol.facts.verify(
            static_cast<std::uint32_t>(protocol.types.size()), protocol_error))
      return false;
    nominal.foreign_resource_protocol =
        values_.generics().internForeignResourceProtocol(std::move(protocol));
    if (nominal.foreign_callback_type.hasValue()) {
      const auto operation_for = [&](ForeignResourceRoleKind role) {
        return std::ranges::find(nominal.foreign_resource_operations, role,
                                 &SemForeignResourceOperation::role);
      };
      const auto end = nominal.foreign_resource_operations.end();
      const auto acquire = operation_for(ForeignResourceRoleKind::AcquireOwned);
      const auto close = operation_for(ForeignResourceRoleKind::CloseQuiescent);
      const auto cancel =
          operation_for(ForeignResourceRoleKind::CancelQuiescent);
      const auto cancel_async =
          operation_for(ForeignResourceRoleKind::CancelAsync);
      const auto wait = operation_for(ForeignResourceRoleKind::WaitCompletion);
      const auto poll = operation_for(ForeignResourceRoleKind::InspectReady);
      const auto arm = operation_for(ForeignResourceRoleKind::ArmOneShot);
      const auto detach =
          operation_for(ForeignResourceRoleKind::DetachCompletion);
      if (acquire == end || close == end || cancel == end ||
          cancel_async == end || wait == end)
        return false;
      const auto &facts =
          values_.generics()
              .foreignResourceProtocol(nominal.foreign_resource_protocol)
              .facts;
      const auto parameter = [&](const ForeignResourceRole *role,
                                 ForeignResourceParameterKind kind) {
        if (!role)
          return core::AnyId::InvalidIndex;
        const auto found = std::ranges::find(
            role->parameters, kind, &ForeignResourceParameterBinding::kind);
        return found == role->parameters.end() ? core::AnyId::InvalidIndex
                                               : found->parameter_index;
      };
      const auto *acquire_role =
          facts.findRole(ForeignResourceRoleKind::AcquireOwned);
      std::vector<CallbackRegistrationBinding> bindings;
      for (const auto &binding : acquire_role->parameters)
        if (binding.kind == ForeignResourceParameterKind::Bound)
          bindings.push_back({binding.name, binding.parameter_index});
      std::array<std::uint32_t, 4> arm_parameters{
          core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
          core::AnyId::InvalidIndex, core::AnyId::InvalidIndex};
      if (arm != end) {
        const auto *role = facts.findRole(ForeignResourceRoleKind::ArmOneShot);
        arm_parameters = {
            parameter(role, ForeignResourceParameterKind::Completion),
            parameter(role, ForeignResourceParameterKind::WakerEntry),
            parameter(role, ForeignResourceParameterKind::WakerUserdata),
            parameter(role, ForeignResourceParameterKind::WakerRelease)};
      }
      std::array<std::uint32_t, 3> detach_parameters{core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex};
      if (detach != end) {
        const auto *role =
            facts.findRole(ForeignResourceRoleKind::DetachCompletion);
        detach_parameters = {
            parameter(role, ForeignResourceParameterKind::Completion),
            parameter(role, ForeignResourceParameterKind::WakerUserdata),
            parameter(role, ForeignResourceParameterKind::WakerRelease)};
      }
      const auto physical_handle =
          sem_ir_.foreignRepresentationType(nominal.foreign_handle_type);
      const auto physical_completion = sem_ir_.foreignRepresentationType(
          nominal.foreign_completion_handle_type);
      if (!physical_handle.hasValue() ||
          physical_handle != physical_completion ||
          sem_ir_.type(physical_handle).kind != SemTypeKind::RawPointer)
        return false;
      const auto pointer = [&](const auto &operation) {
        return foreignResourceTargetPointer(operation->target);
      };
      const auto authority =
          static_cast<CallbackReleaseAuthority>(facts.release_authority);
      nominal.foreign_registration_storage_type =
          sem_ir_.addCallbackRegistrationType(
              nominal.foreign_callback_type, physical_handle, pointer(acquire),
              pointer(close), pointer(cancel), authority,
              parameter(acquire_role,
                        ForeignResourceParameterKind::CallbackEntry),
              parameter(acquire_role,
                        ForeignResourceParameterKind::CallbackUserdata),
              parameter(acquire_role,
                        ForeignResourceParameterKind::CallbackRelease),
              std::move(bindings), pointer(cancel_async), pointer(wait),
              poll != end ? pointer(poll) : TypeId::invalid(),
              arm != end ? pointer(arm) : TypeId::invalid(),
              detach != end ? pointer(detach) : TypeId::invalid(),
              arm_parameters, detach_parameters);
      nominal.foreign_completion_storage_type =
          sem_ir_.addCallbackCompletionType(
              nominal.foreign_callback_type, physical_handle,
              physical_completion, pointer(wait),
              poll != end ? pointer(poll) : TypeId::invalid(), authority,
              arm != end ? pointer(arm) : TypeId::invalid(),
              detach != end ? pointer(detach) : TypeId::invalid(),
              arm_parameters, detach_parameters);
      if (arm != end)
        nominal.foreign_wake_storage_type = sem_ir_.addCallbackWakeType(
            nominal.foreign_completion_storage_type);
    }
  }
  sem_ir_.setNominalType(local, std::move(nominal));
  return true;
}

[[nodiscard]] NominalTypeId SemanticContext::resolveNominalType(NodeId node) {
  if (tree_.kind(node) == NodeKind::Name ||
      tree_.kind(node) == NodeKind::TypeName) {
    const auto name = tokenString(node);
    const auto found = name.hasValue() ? nominal_names_.find(name.index)
                                       : nominal_names_.end();
    return found == nominal_names_.end() ? NominalTypeId::invalid()
                                         : found->second;
  }
  if (tree_.kind(node) != NodeKind::QualifiedType &&
      tree_.kind(node) != NodeKind::QualifiedName)
    return NominalTypeId::invalid();
  const auto components = tree_.children(node);
  if (components.size() < 2)
    return NominalTypeId::invalid();
  std::string module_name;
  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    if (!module_name.empty())
      module_name += "::";
    module_name += values_.identifier(tokenString(components[index]));
  }
  const auto import =
      sem_ir_.importIRs().findByModule(values_.internIdentifier(module_name));
  const auto *interface = sem_ir_.importIRs().tryGetInterface(import);
  if (!interface)
    return NominalTypeId::invalid();
  const auto binding =
      interface->findNominalType(tokenString(components.back()));
  if (!binding.hasValue())
    return NominalTypeId::invalid();
  return materializeImportedNominal(
      interface->nominalType(binding).canonical_entity, node);
}

[[nodiscard]] PublicEntityId
SemanticContext::resolveImportedTypeEntity(NodeId node,
                                           PublicEntityKind kind) const {
  if (tree_.kind(node) != NodeKind::QualifiedType &&
      tree_.kind(node) != NodeKind::QualifiedName)
    return PublicEntityId::invalid();
  const auto components = tree_.children(node);
  if (components.size() < 2)
    return PublicEntityId::invalid();
  std::string module_name;
  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    if (!module_name.empty())
      module_name += "::";
    const auto component = tokenString(components[index]);
    if (!component.hasValue())
      return PublicEntityId::invalid();
    module_name += values_.identifier(component);
  }
  const auto import =
      sem_ir_.importIRs().findByModule(values_.internIdentifier(module_name));
  const auto *interface = sem_ir_.importIRs().tryGetInterface(import);
  if (!interface)
    return PublicEntityId::invalid();
  const auto name = tokenString(components.back());
  if (kind == PublicEntityKind::Interface) {
    const auto *artifact = interface->findInterface(name);
    return artifact ? resolveArtifactEntity(artifact->entity)
                    : PublicEntityId::invalid();
  }
  if (kind == PublicEntityKind::TypeAlias) {
    const auto *artifact = interface->findTypeAlias(name);
    return artifact ? resolveArtifactEntity(artifact->entity)
                    : PublicEntityId::invalid();
  }
  return PublicEntityId::invalid();
}
