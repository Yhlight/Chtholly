// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
[[nodiscard]] TypeId
SemanticContext::mapImportedPublicType(const PublicType &type,
                                       GenericId generic, NodeId location,
                                       bool require_nominal_complete) {
  switch (type.kind) {
  case PublicTypeKind::Void:
    return sem_ir_.voidType();
  case PublicTypeKind::Never:
    return sem_ir_.neverType();
  case PublicTypeKind::Bool:
    return sem_ir_.boolType();
  case PublicTypeKind::Char:
    return sem_ir_.charType();
  case PublicTypeKind::Integer:
    return sem_ir_.addIntegerType(type.scalar_width, type.integer_signed);
  case PublicTypeKind::Float:
    return sem_ir_.addFloatType(type.scalar_width);
  case PublicTypeKind::String:
    return sem_ir_.stringType();
  case PublicTypeKind::Array: {
    if (type.arguments.size() != 1 || type.array_bound == 0)
      return TypeId::invalid();
    const auto element =
        mapImportedPublicType(type.arguments.front(), generic, location);
    return element.hasValue()
               ? sem_ir_.addType({SemTypeKind::Array, element.index,
                                  type.array_bound, core::AnyId::InvalidIndex})
               : TypeId::invalid();
  }
  case PublicTypeKind::Tuple: {
    std::vector<TypeId> elements;
    elements.reserve(type.arguments.size());
    for (const auto &argument : type.arguments) {
      const auto element = mapImportedPublicType(argument, generic, location);
      if (!element.hasValue())
        return TypeId::invalid();
      elements.push_back(element);
    }
    return sem_ir_.addTupleType(elements);
  }
  case PublicTypeKind::Slice: {
    if (type.arguments.size() != 1)
      return TypeId::invalid();
    const auto element =
        mapImportedPublicType(type.arguments.front(), generic, location);
    return element.hasValue()
               ? sem_ir_.addSliceType(element, type.slice_mutable)
               : TypeId::invalid();
  }
  case PublicTypeKind::TypeParameter:
    return generic.hasValue()
               ? sem_ir_.addTypeParameter(generic, type.binding_index)
               : TypeId::invalid();
  case PublicTypeKind::TypeProjection: {
    if (!generic.hasValue() || type.arguments.size() != 1 ||
        type.projection_kind >= PublicTypeProjectionKind::Count ||
        type.projection_index > 0x7fffffffU)
      return TypeId::invalid();
    const auto source =
        mapImportedPublicType(type.arguments.front(), generic, location);
    if (!source.hasValue())
      return TypeId::invalid();
    if (type.projection_kind == PublicTypeProjectionKind::Associated) {
      const auto entity_id = resolveArtifactEntity(type.nominal_entity);
      const auto *entity = sem_ir_.importIRs().tryGetEntity(entity_id);
      if (!entity || entity->kind != PublicEntityKind::Interface ||
          !entity->interface_declaration || !entity->generic.hasValue() ||
          type.projection_index >= entity->generic_parameter_count ||
          std::ranges::none_of(
              entity->interface_declaration->requirements,
              [&](const PublicInterfaceRequirementArtifact &requirement) {
                return requirement.kind ==
                           PublicInterfaceRequirementKind::AssociatedAlias &&
                       requirement.binding_index == type.projection_index;
              }))
        return TypeId::invalid();
      return sem_ir_.addTypeParameter(entity->generic, type.projection_index);
    }
    return source.hasValue()
               ? sem_ir_.addType(
                     {SemTypeKind::TypeProjection, source.index,
                      (static_cast<std::uint32_t>(type.projection_kind)
                       << 31U) |
                          type.projection_index})
               : TypeId::invalid();
  }
  case PublicTypeKind::Reference: {
    if (type.arguments.size() != 1)
      return TypeId::invalid();
    const auto pointee =
        mapImportedPublicType(type.arguments.front(), generic, location, false);
    if (!pointee.hasValue())
      return TypeId::invalid();
    return sem_ir_.addReferenceType(
        pointee,
        type.reference_mutability == PublicReferenceMutability::Mutable
            ? SemReferenceMutability::Mutable
            : SemReferenceMutability::ReadOnly,
        type.reference_provenance.kind ==
                PublicReferenceProvenanceKind::Parameter
            ? SemReferenceProvenanceKind::Parameter
            : SemReferenceProvenanceKind::Erased,
        type.reference_provenance.index);
  }
  case PublicTypeKind::RawPointer: {
    if (type.arguments.size() != 1)
      return TypeId::invalid();
    const auto pointee =
        mapImportedPublicType(type.arguments.front(), generic, location, false);
    return pointee.hasValue()
               ? sem_ir_.addRawPointerType(pointee, type.pointer_const)
               : TypeId::invalid();
  }
  case PublicTypeKind::CFunctionPointer: {
    if (type.arguments.empty())
      return TypeId::invalid();
    std::vector<TypeId> parameters;
    parameters.reserve(type.arguments.size() - 1);
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
      const auto parameter =
          mapImportedPublicType(type.arguments[index], generic, location);
      if (!parameter.hasValue())
        return TypeId::invalid();
      parameters.push_back(parameter);
    }
    const auto result =
        mapImportedPublicType(type.arguments.back(), generic, location);
    return result.hasValue()
               ? sem_ir_.addCFunctionPointerType(
                     parameters, result, type.callable_variadic,
                     type.callable_contract, type.callable_context_parameter,
                     type.foreign_calling_convention)
               : TypeId::invalid();
  }
  case PublicTypeKind::Function: {
    if (type.arguments.empty())
      return TypeId::invalid();
    std::vector<TypeId> parameters;
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
      const auto parameter =
          mapImportedPublicType(type.arguments[index], generic, location);
      if (!parameter.hasValue())
        return TypeId::invalid();
      parameters.push_back(parameter);
    }
    const auto result =
        mapImportedPublicType(type.arguments.back(), generic, location);
    return result.hasValue() ? sem_ir_.addFunctionType(parameters, result)
                             : TypeId::invalid();
  }
  case PublicTypeKind::CallbackAdapter: {
    if (type.arguments.size() != 3)
      return TypeId::invalid();
    const auto entry =
        mapImportedPublicType(type.arguments[0], generic, location);
    const auto context =
        mapImportedPublicType(type.arguments[1], generic, location);
    const auto release =
        mapImportedPublicType(type.arguments[2], generic, location);
    return entry.hasValue() && context.hasValue() && release.hasValue()
               ? sem_ir_.addCallbackAdapterType(entry, context, release)
               : TypeId::invalid();
  }
  case PublicTypeKind::CallbackRegistration: {
    if (type.arguments.size() != 5 && type.arguments.size() != 7 &&
        type.arguments.size() != 8 && type.arguments.size() != 10)
      return TypeId::invalid();
    std::vector<TypeId> fields(type.arguments.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fields[index] =
          mapImportedPublicType(type.arguments[index], generic, location);
      if (!fields[index].hasValue())
        return TypeId::invalid();
    }
    return sem_ir_.addCallbackRegistrationType(
        fields[0], fields[1], fields[2], fields[3], fields[4],
        static_cast<CallbackReleaseAuthority>(type.registration_authority),
        type.registration_entry_parameter, type.registration_userdata_parameter,
        type.registration_release_parameter, type.registration_bindings,
        fields.size() >= 7 ? fields[5] : TypeId::invalid(),
        fields.size() >= 7 ? fields[6] : TypeId::invalid(),
        fields.size() >= 8 ? fields[7] : TypeId::invalid(),
        fields.size() == 10 ? fields[8] : TypeId::invalid(),
        fields.size() == 10 ? fields[9] : TypeId::invalid(),
        type.registration_arm_parameters, type.registration_detach_parameters);
  }
  case PublicTypeKind::CallbackCompletion: {
    if (type.arguments.size() != 4 && type.arguments.size() != 5 &&
        type.arguments.size() != 7)
      return TypeId::invalid();
    std::vector<TypeId> fields(type.arguments.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fields[index] =
          mapImportedPublicType(type.arguments[index], generic, location);
      if (!fields[index].hasValue())
        return TypeId::invalid();
    }
    return sem_ir_.addCallbackCompletionType(
        fields[0], fields[1], fields[2], fields[3],
        fields.size() >= 5 ? fields[4] : TypeId::invalid(),
        static_cast<CallbackReleaseAuthority>(type.registration_authority),
        fields.size() == 7 ? fields[5] : TypeId::invalid(),
        fields.size() == 7 ? fields[6] : TypeId::invalid(),
        type.registration_arm_parameters, type.registration_detach_parameters);
  }
  case PublicTypeKind::CallbackWake: {
    if (type.arguments.size() != 1)
      return TypeId::invalid();
    const auto completion =
        mapImportedPublicType(type.arguments.front(), generic, location);
    return completion.hasValue() ? sem_ir_.addCallbackWakeType(completion)
                                 : TypeId::invalid();
  }
  case PublicTypeKind::ForeignOperationState: {
    std::string error;
    const auto result =
        sem_ir_.materializePublicTypeForImport(type, generic, location, error);
    return result;
  }
  case PublicTypeKind::ForeignCompletion:
  case PublicTypeKind::ForeignWake: {
    PublicType owner(type.nominal_entity);
    const auto owner_type = mapImportedPublicType(owner, generic, location);
    if (!owner_type.hasValue() ||
        sem_ir_.type(owner_type).kind != SemTypeKind::Nominal)
      return TypeId::invalid();
    const auto owner_id = NominalTypeId(sem_ir_.type(owner_type).arg0);
    const auto &nominal = sem_ir_.nominalType(owner_id);
    if (nominal.kind != NominalKind::ForeignResource ||
        (type.kind == PublicTypeKind::ForeignCompletion &&
         !nominal.foreign_completion_handle_type.hasValue()) ||
        (type.kind == PublicTypeKind::ForeignWake &&
         !nominal.foreign_wake_storage_type.hasValue()))
      return TypeId::invalid();
    return type.kind == PublicTypeKind::ForeignCompletion
               ? sem_ir_.addForeignCompletionType(owner_id)
               : sem_ir_.addForeignWakeType(owner_id);
  }
  case PublicTypeKind::Nominal: {
    const auto canonical = sem_ir_.importIRs().registry().findEntity(
        type.nominal_entity.canonical_package,
        type.nominal_entity.canonical_module,
        type.nominal_entity.canonical_name, PublicEntityKind::NominalType);
    const auto *entity = sem_ir_.importIRs().tryGetEntity(canonical);
    if (!entity ||
        entity->fingerprint != type.nominal_entity.expected_fingerprint)
      return TypeId::invalid();
    const auto nominal =
        require_nominal_complete
            ? materializeImportedNominal(canonical, location)
            : materializeImportedNominalShell(canonical, location);
    if (!nominal.hasValue())
      return TypeId::invalid();
    std::vector<TypeId> arguments;
    for (const auto &argument : type.arguments) {
      const auto mapped = mapImportedPublicType(argument, generic, location);
      if (!mapped.hasValue())
        return TypeId::invalid();
      arguments.push_back(mapped);
    }
    return sem_ir_.addNominalType(nominal, arguments);
  }
  case PublicTypeKind::Count:
    return TypeId::invalid();
  }
  return TypeId::invalid();
}

[[nodiscard]] FunctionRefId
SemanticContext::resolvePublicEntity(PublicEntityId canonical, NodeId node,
                                     ImportIRInstId import_inst) {
  if (const auto existing = imported_function_refs_.find(canonical.index);
      existing != imported_function_refs_.end())
    return existing->second;
  const auto *function = sem_ir_.importIRs().tryGetEntity(canonical);
  if (!function || function->kind != PublicEntityKind::Function)
    return FunctionRefId::invalid();
  std::vector<TypeId> parameter_types;
  for (const auto parameter : function->parameters) {
    const auto mapped =
        mapImportedPublicType(parameter, function->generic, node);
    if (!mapped.hasValue())
      return FunctionRefId::invalid();
    parameter_types.push_back(mapped);
  }
  const auto return_type =
      mapImportedPublicType(function->return_type, function->generic, node);
  if (!return_type.hasValue())
    return FunctionRefId::invalid();
  std::optional<TypeId> error_type;
  if (function->error_type) {
    error_type =
        mapImportedPublicType(*function->error_type, function->generic, node);
    if (!error_type->hasValue())
      return FunctionRefId::invalid();
  }
  if (function->interop_artifact) {
    const auto *operation = sem_ir_.importIRs().interopRegistry().resolve(
        *function->interop_artifact);
    if (operation &&
        operation->outcome_projection ==
            interop::ForeignOperationArtifact::OutcomeProjection::Win32Read) {
      if (!operation->outcome_count_type ||
          operation->outcome_count_type->kind != PublicTypeKind::Integer ||
          operation->outcome_count_type->scalar_width != 32 ||
          operation->outcome_count_type->integer_signed)
        return FunctionRefId::invalid();
      const auto count = sem_ir_.addIntegerType(32, false);
      (void)sem_ir_.addReferenceType(count, SemReferenceMutability::Mutable);
    }
  }
  const auto is_async =
      function->execution_kind == PublicFunctionExecutionKind::Async;
  const auto is_interface_member =
      values_.identifier(function->name).starts_with("$interface$");
  const auto local_type =
      is_async ? sem_ir_.addAsyncFunctionType(parameter_types, return_type,
                                              error_type)
               : sem_ir_.addType({SemTypeKind::Function,
                                  sem_ir_.addTypeBlock(parameter_types).index,
                                  return_type.index, 0});
  NominalTypeId imported_owner;
  if (function->semantic_contract.domain != CallableSemanticDomain::Ordinary) {
    const auto owner_type = mapImportedPublicType(
        function->semantic_contract.owner, function->generic, node);
    if (!owner_type.hasValue() ||
        sem_ir_.type(owner_type).kind != SemTypeKind::Nominal)
      return FunctionRefId::invalid();
    if (function->semantic_contract.domain !=
            CallableSemanticDomain::NominalConstruction &&
        (parameter_types.empty() ||
         sem_ir_.type(parameter_types.front()).kind != SemTypeKind::Reference ||
         sem_ir_.referencePointee(parameter_types.front()) != owner_type))
      return FunctionRefId::invalid();
    imported_owner = NominalTypeId(sem_ir_.type(owner_type).arg0);
  }
  NameId imported_projector;
  if (function->semantic_contract.domain ==
      CallableSemanticDomain::ObjectProjection) {
    const auto &owner = sem_ir_.nominalType(imported_owner);
    if (function->semantic_contract.projector_field >= owner.fields.size())
      return FunctionRefId::invalid();
    imported_projector =
        owner.fields[function->semantic_contract.projector_field]
            .projector_name;
  }
  FunctionId local_function;
  if (function->generic.hasValue() || function->generic_template) {
    const std::vector<LocalId> no_parameters;
    const std::vector<InstId> no_body;
    auto imported_record = SemFunction{
        sem_ir_.addName(function->name),
        local_type,
        sem_ir_.addLocalBlock(no_parameters),
        sem_ir_.addInstBlock(no_body),
        (function->generic.hasValue() ? SemFunctionTemplate
                                      : SemFunctionConst) |
            SemFunctionEvaluatorArtifact | (is_async ? SemFunctionAsync : 0) |
            (is_interface_member ? SemFunctionInterfaceMember : 0),
        function->generic,
        function->generic.hasValue()
            ? values_.generics().generic(function->generic).self_specific
            : SpecificId::invalid(),
        imported_owner,
        static_cast<std::uint8_t>(function->semantic_contract.role),
        imported_projector};
    imported_record.intrinsic_role = function->intrinsic_role;
    local_function = sem_ir_.addFunction(std::move(imported_record));
    if (function->intrinsic_role != CompilerIntrinsicRole::None ||
        function->declaration_kind == PublicCallableDeclarationKind::Forward)
      sem_ir_.setFunctionDeclaration(
          local_function, {.kind = SemCallableDeclarationKind::Forward,
                           .is_unsafe = function->is_unsafe});
    sem_ir_.setFunctionSemanticContract(
        local_function,
        {function->semantic_contract.domain, function->semantic_contract.role,
         function->semantic_contract.capability, imported_owner,
         function->semantic_contract.projector_field,
         function->semantic_contract.whole_carrier,
         function->semantic_contract.carrier_path,
         function->semantic_contract.has_bit_range,
         function->semantic_contract.bit_begin,
         function->semantic_contract.bit_end});
    sem_ir_.setFunctionOwnership(local_function, function->ownership_summary);
    sem_ir_.expectFunctionOwnership(local_function,
                                    function->ownership_summary);
    std::vector<SemInterfaceConstraint> constraints;
    for (const auto &source : function->constraints) {
      const auto subject =
          mapImportedPublicType(source.subject, function->generic, node);
      const auto interface_entity =
          resolveArtifactEntity(source.interface_entity);
      const auto interface_id =
          materializeImportedInterface(interface_entity, node);
      std::vector<TypeId> arguments;
      for (const auto &argument : source.arguments)
        arguments.push_back(
            mapImportedPublicType(argument, function->generic, node));
      if (!subject.hasValue() || !interface_id.hasValue() ||
          std::ranges::any_of(arguments,
                              [](TypeId type) { return !type.hasValue(); }))
        return FunctionRefId::invalid();
      constraints.push_back({subject, interface_id,
                             sem_ir_.addTypeBlock(arguments),
                             InterfaceWitnessId::invalid()});
    }
    sem_ir_.setFunctionConstraints(local_function, std::move(constraints));
  }
  const auto evaluator_reference = sem_ir_.addFunctionRef(
      {local_function, import_inst, local_type, canonical, function->generic,
       function->generic.hasValue()
           ? values_.generics().generic(function->generic).self_specific
           : SpecificId::invalid()});
  const auto reference =
      function->generic.hasValue() || !function->generic_template
          ? evaluator_reference
          : sem_ir_.addFunctionRef({FunctionId::invalid(), import_inst,
                                    local_type, canonical, GenericId::invalid(),
                                    SpecificId::invalid()});
  imported_function_refs_.emplace(canonical.index, reference);
  if (function->generic.hasValue())
    generic_template_refs_.emplace(function->generic.index,
                                   evaluator_reference);
  if (function->generic_template &&
      function->intrinsic_role == CompilerIntrinsicRole::None) {
    std::string template_error;
    if (!sem_ir_.materializeTemplateBody(
            local_function, *function->generic_template, function->generic,
            sem_ir_.addName(function->name), local_type, node,
            [&](const PublicType &type) {
              return mapImportedPublicType(type, function->generic, node);
            },
            [&](PublicEntityId callee, std::span<const InstId> arguments,
                std::span<const PublicType> concrete_arguments,
                NodeId location) {
              const auto reference = resolvePublicEntity(callee, location);
              if (!reference.hasValue())
                return FunctionRefId::invalid();
              std::vector<CanonicalTypeId> explicit_arguments;
              for (const auto &argument : concrete_arguments) {
                const auto local = mapImportedPublicType(
                    argument, sem_ir_.function(local_function).generic,
                    location);
                if (!local.hasValue())
                  return FunctionRefId::invalid();
                explicit_arguments.push_back(sem_ir_.canonicalType(local));
              }
              const auto specialized = specializeFunction(
                  reference, arguments, location, false, explicit_arguments);
              if (!specialized.hasValue())
                return FunctionRefId::invalid();
              auto symbolic = std::ranges::any_of(
                  explicit_arguments, [&](CanonicalTypeId argument) {
                    const auto kind = values_.generics().type(argument).kind;
                    return kind == CanonicalTypeKind::TypeParameter ||
                           kind == CanonicalTypeKind::TypeProjection;
                  });
              const auto &specialized_reference =
                  sem_ir_.functionRef(specialized);
              if (!symbolic && specialized_reference.specific.hasValue())
                symbolic = std::ranges::any_of(
                    values_.generics()
                        .specific(specialized_reference.specific)
                        .arguments,
                    [&](CanonicalConstantId argument) {
                      const auto &constant =
                          values_.generics().constant(argument);
                      const auto kind =
                          values_.generics().type(constant.type).kind;
                      return constant.dependence !=
                                 ConstantDependence::Concrete ||
                             kind == CanonicalTypeKind::TypeParameter ||
                             kind == CanonicalTypeKind::TypeProjection;
                    });
              if (symbolic)
                return specialized;
              std::string intrinsic_error;
              const auto canonical = sem_ir_.resolveCanonicalIntrinsic(
                  specialized, intrinsic_error);
              return canonical.value_or(FunctionRefId::invalid());
            },
            template_error)) {
      imported_function_refs_.erase(canonical.index);
      if (function->generic.hasValue())
        generic_template_refs_.erase(function->generic.index);
      return FunctionRefId::invalid();
    }
  }
  return reference;
}
