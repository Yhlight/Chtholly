// Included by SemanticContextDeclarations.cpp; this fragment aliases the
// single SemanticContext and preserves declaration-phase ordering.
SemanticContext::callableEnvironmentIdentity(SemCallableEnvironmentKind kind,
                                             std::uint32_t ordinal,
                                             FunctionRefId target) {
  std::string canonical = "chtholly.next.callable-environment.v1\n";
  canonical += std::string(values_.identifier(sem_ir_.moduleName())) + "\n";
  const auto &owner = sem_ir_.function(current_function_);
  canonical +=
      std::string(values_.identifier(sem_ir_.name(owner.name).text)) + "\n";
  if (const auto public_type =
          concretePublicType(sem_ir_.canonicalType(owner.type), owner.generic))
    canonical += publicTypeFingerprint(*public_type).hex();
  else
    canonical += "module-local";
  canonical += "\n" + std::to_string(static_cast<std::uint8_t>(kind)) + "\n" +
               std::to_string(ordinal);
  if (target.hasValue()) {
    const auto &reference = sem_ir_.functionRef(target);
    if (const auto *entity =
            sem_ir_.importIRs().tryGetEntity(reference.public_entity))
      canonical += "\n" + entity->fingerprint.hex();
    else if (reference.local_function.hasValue()) {
      const auto &function = sem_ir_.function(reference.local_function);
      canonical +=
          "\n" +
          std::string(values_.identifier(sem_ir_.name(function.name).text));
      if (const auto public_type = concretePublicType(
              sem_ir_.canonicalType(function.type), function.generic))
        canonical += "\n" + publicTypeFingerprint(*public_type).hex();
    }
  }
  return StableFingerprint::fromCanonicalBytes(canonical);
}

// Callable interfaces use one tuple argument while closure bodies retain
// their source-level parameter list. Synthesize a small checked adapter so
// both paths share the existing hidden body ABI.
void SemanticContext::synthesizeCallableWitness(
    TypeId environment_type, TypeId result_type,
    std::span<const TypeId> source_parameters, FunctionRefId hidden_ref,
    GenericId witness_generic, SemCallableEnvironmentCapability capability,
    std::string_view interface_name, NodeId location,
    std::string_view interface_module, std::string_view requirement_name,
    std::optional<std::string_view> output_requirement,
    bool direct_parameters) {
  const auto entity = sem_ir_.importIRs().registry().findEntity(
      "std", interface_module, interface_name, PublicEntityKind::Interface);
  const auto *interface_entity =
      sem_ir_.importIRs().registry().tryGetEntity(entity);
  const auto interface_id = materializeImportedInterface(entity, location);
  const auto hidden_function = sem_ir_.functionRef(hidden_ref).local_function;
  if (!interface_entity || !interface_id.hasValue() ||
      !hidden_function.hasValue())
    return;
  const auto args_type = sem_ir_.addTupleType(source_parameters);
  const auto environment_parameter =
      capability == SemCallableEnvironmentCapability::Consuming
          ? environment_type
          : sem_ir_.addReferenceType(
                environment_type,
                capability == SemCallableEnvironmentCapability::Mutable
                    ? SemReferenceMutability::Mutable
                    : SemReferenceMutability::ReadOnly);
  std::vector<TypeId> adapter_parameters{environment_parameter};
  if (direct_parameters)
    adapter_parameters.insert(adapter_parameters.end(),
                              source_parameters.begin(),
                              source_parameters.end());
  else
    adapter_parameters.push_back(args_type);
  const auto adapter_type =
      sem_ir_.addFunctionType(adapter_parameters, result_type);
  const auto interface_value = sem_ir_.interface(interface_id);
  const auto self_index = interface_value.explicit_parameter_count;
  const auto generic = interface_value.generic;
  const auto binding_count = values_.generics().generic(generic).binding_count;
  std::vector<CanonicalTypeId> substitutions(binding_count,
                                             CanonicalTypeId::invalid());
  if (interface_value.explicit_parameter_count !=
          (direct_parameters ? source_parameters.size() : 1U) ||
      self_index >= substitutions.size())
    return;
  if (direct_parameters) {
    for (std::uint32_t index = 0; index < source_parameters.size(); ++index)
      substitutions[index] = sem_ir_.canonicalType(source_parameters[index]);
  } else {
    substitutions[0] = sem_ir_.canonicalType(args_type);
  }
  substitutions[self_index] = sem_ir_.canonicalType(environment_type);

  std::optional<std::uint32_t> invoke_slot;
  std::optional<std::uint32_t> output_slot;
  for (std::uint32_t slot = 0; slot < interface_value.requirements.size();
       ++slot) {
    const auto &requirement = interface_value.requirements[slot];
    const auto name = values_.identifier(sem_ir_.name(requirement.name).text);
    if (name == requirement_name &&
        requirement.kind == SemInterfaceRequirementKind::Function)
      invoke_slot = slot;
    if (output_requirement && name == *output_requirement &&
        requirement.kind == SemInterfaceRequirementKind::AssociatedAlias)
      output_slot = slot;
  }
  if (!invoke_slot ||
      (output_requirement &&
       (!output_slot ||
        interface_value.requirements[*output_slot].binding_index >=
            substitutions.size())))
    return;
  if (output_slot)
    substitutions[interface_value.requirements[*output_slot].binding_index] =
        sem_ir_.canonicalType(result_type);

  const auto expected = substituteType(
      interface_value.requirements[*invoke_slot].type, generic, substitutions);
  if (!expected.hasValue() ||
      sem_ir_.canonicalType(expected) != sem_ir_.canonicalType(adapter_type))
    return;
  const auto hidden_name = values_.identifier(
      sem_ir_.name(sem_ir_.function(hidden_function).name).text);
  const auto adapter_name =
      values_.internIdentifier("$callable$" + std::string(interface_name) +
                               "$" + std::string(hidden_name));
  const auto adapter_generic = witness_generic;
  const auto adapter_flags = adapter_generic.hasValue()
                                 ? (SemFunctionTemplate | SemFunctionPublic)
                                 : 0U;
  const auto adapter_function = sem_ir_.addFunction(
      {sem_ir_.addName(adapter_name), adapter_type, LocalBlockId::invalid(),
       InstBlockId::invalid(), adapter_flags, adapter_generic,
       adapter_generic.hasValue()
           ? values_.generics().generic(adapter_generic).self_specific
           : SpecificId::invalid()});
  SemCallableDeclaration declaration;
  declaration.parameter_names = {values_.internIdentifier("self")};
  if (direct_parameters)
    for (std::uint32_t index = 0; index < source_parameters.size(); ++index)
      declaration.parameter_names.push_back(
          values_.internIdentifier("arg" + std::to_string(index)));
  else
    declaration.parameter_names.push_back(values_.internIdentifier("args"));
  sem_ir_.setFunctionDeclaration(adapter_function, std::move(declaration));
  const auto adapter_ref = sem_ir_.addFunctionRef(
      {adapter_function, ImportIRInstId::invalid(), adapter_type,
       PublicEntityId::invalid(), adapter_generic,
       adapter_generic.hasValue()
           ? values_.generics().generic(adapter_generic).self_specific
           : SpecificId::invalid()});
  const auto self_local =
      sem_ir_.addLocal({sem_ir_.addName(values_.internIdentifier("self")),
                        environment_parameter, location, SemLocalMutable});
  std::vector<LocalId> parameters{self_local};
  std::vector<LocalId> argument_locals;
  const auto semantic_adapter_parameters =
      std::span<const TypeId>(adapter_parameters).subspan(1);
  for (std::uint32_t index = 0; index < semantic_adapter_parameters.size();
       ++index) {
    const auto local = sem_ir_.addLocal(
        {sem_ir_.addName(values_.internIdentifier(
             direct_parameters ? "arg" + std::to_string(index) : "args")),
         semantic_adapter_parameters[index], location, 0});
    parameters.push_back(local);
    argument_locals.push_back(local);
  }
  std::vector<InstId> body;
  (void)appendInst<SemParameter>(body, location, environment_parameter,
                                 self_local);
  for (std::uint32_t index = 0; index < argument_locals.size(); ++index)
    (void)appendInst<SemParameter>(body, location,
                                   semantic_adapter_parameters[index],
                                   argument_locals[index]);
  auto self_value =
      appendInst<SemNameRef>(body, location, environment_parameter, self_local);
  if (capability == SemCallableEnvironmentCapability::Consuming)
    self_value =
        appendInst<SemMove>(body, location, environment_type, self_value);
  std::vector<InstId> call_arguments{self_value};
  if (direct_parameters) {
    for (std::uint32_t index = 0; index < argument_locals.size(); ++index)
      call_arguments.push_back(appendInst<SemNameRef>(
          body, location, source_parameters[index], argument_locals[index]));
  } else {
    const auto args_value = appendInst<SemNameRef>(body, location, args_type,
                                                   argument_locals.front());
    for (std::uint32_t index = 0; index < source_parameters.size(); ++index)
      call_arguments.push_back(appendInst<SemStructFieldAccess>(
          body, location, source_parameters[index], args_value,
          sem_ir_.addInteger(index)));
  }
  const auto result =
      appendInst<SemCall>(body, location, result_type, hidden_ref,
                          sem_ir_.addInstBlock(call_arguments));
  (void)appendInst<SemReturn>(body, location, sem_ir_.voidType(), result);
  sem_ir_.setFunction(
      adapter_function,
      {sem_ir_.addName(adapter_name), adapter_type,
       sem_ir_.addLocalBlock(parameters), sem_ir_.addInstBlock(body),
       adapter_flags, adapter_generic,
       adapter_generic.hasValue()
           ? values_.generics().generic(adapter_generic).self_specific
           : SpecificId::invalid()});
  std::vector<TypeId> interface_arguments;
  if (direct_parameters)
    interface_arguments.assign(source_parameters.begin(),
                               source_parameters.end());
  else
    interface_arguments.push_back(args_type);
  SemInterfaceWitness witness{
      interface_id, environment_type, sem_ir_.addTypeBlock(interface_arguments),
      {},           location,         SemInterfaceWitnessState::Complete};
  witness.generic = adapter_generic;
  for (std::uint32_t slot = 0; slot < interface_value.requirements.size();
       ++slot) {
    if (slot == *invoke_slot)
      witness.entries.push_back({slot, adapter_ref, TypeId::invalid()});
    else if (output_slot && slot == *output_slot)
      witness.entries.push_back({slot, FunctionRefId::invalid(), result_type});
    else if (interface_value.requirements[slot].kind ==
             SemInterfaceRequirementKind::Function)
      witness.entries.push_back({slot,
                                 interface_value.requirements[slot].function,
                                 TypeId::invalid()});
    else
      witness.entries.push_back({slot, FunctionRefId::invalid(),
                                 interface_value.requirements[slot].type});
  }
  std::string identity = "chtholly.next.callable-witness.v4\n";
  identity +=
      interface_entity->fingerprint.hex() + "\n" +
      std::string(values_.identifier(sem_ir_.moduleName())) + "\n" +
      std::string(hidden_name) + "\n" + std::string(interface_module) + "\n" +
      std::string(interface_name) + "\n" + std::string(requirement_name) +
      "\n" + std::to_string(static_cast<std::uint8_t>(capability)) + "\n" +
      std::to_string(source_parameters.size()) + "\n" +
      std::to_string(
          adapter_generic.hasValue()
              ? values_.generics().generic(adapter_generic).binding_count
              : 0U);
  witness.fingerprint = StableFingerprint::fromCanonicalBytes(identity);
  (void)sem_ir_.addInterfaceWitness(std::move(witness));
}

[[nodiscard]] InstId SemanticContext::synthesizeBoundMethod(
    NodeId node, std::vector<InstId> &block, InstId receiver,
    FunctionRefId target_template, FunctionRefId target,
    SemCallableEnvironmentCapability capability,
    std::span<const TypeId> source_parameters, TypeId result_type,
    std::span<const CanonicalTypeId> explicit_member_arguments) {
  const auto ordinal = closure_counter_++;
  const auto environment_identity = callableEnvironmentIdentity(
      SemCallableEnvironmentKind::BoundMethod, ordinal, target);
  const auto hidden_text = values_.internIdentifier(
      "$bound$" + environment_identity.hex().substr(0, 24));
  const auto hidden_name = sem_ir_.addName(hidden_text);
  const auto outer_generic = current_generic_;
  GenericId hidden_generic;
  std::uint32_t generic_count = 0;
  if (outer_generic.hasValue()) {
    generic_count = values_.generics().generic(outer_generic).binding_count;
    hidden_generic = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(), hidden_text, generic_count);
  }
  std::function<TypeId(TypeId)> remap_type;
  remap_type = [&](TypeId source) -> TypeId {
    if (!hidden_generic.hasValue())
      return source;
    const auto &type = sem_ir_.type(source);
    if (type.kind == SemTypeKind::TypeParameter &&
        type.arg0 == outer_generic.index)
      return sem_ir_.addTypeParameter(hidden_generic, type.arg1);
    if (type.kind == SemTypeKind::Array)
      return sem_ir_.addType({SemTypeKind::Array,
                              remap_type(TypeId(type.arg0)).index, type.arg1,
                              type.reserved});
    if (type.kind == SemTypeKind::Tuple) {
      std::vector<TypeId> elements;
      for (const auto element : sem_ir_.typeBlock(TypeBlockId(type.arg0)))
        elements.push_back(remap_type(element));
      return sem_ir_.addTupleType(elements);
    }
    if (type.kind == SemTypeKind::Slice)
      return sem_ir_.addSliceType(remap_type(TypeId(type.arg0)),
                                  type.arg1 != 0);
    if (type.kind == SemTypeKind::Reference)
      return sem_ir_.addReferenceType(remap_type(TypeId(type.arg0)),
                                      sem_ir_.referenceMutability(source));
    if (type.kind == SemTypeKind::RawPointer)
      return sem_ir_.addRawPointerType(remap_type(TypeId(type.arg0)),
                                       type.arg1 != 0);
    if (type.kind == SemTypeKind::Function) {
      std::vector<TypeId> parameters;
      for (const auto parameter : sem_ir_.typeBlock(TypeBlockId(type.arg0)))
        parameters.push_back(remap_type(parameter));
      return sem_ir_.addFunctionType(parameters, remap_type(TypeId(type.arg1)));
    }
    if (type.kind == SemTypeKind::Nominal) {
      std::vector<TypeId> arguments;
      for (const auto argument : sem_ir_.typeBlock(TypeBlockId(type.arg1)))
        arguments.push_back(remap_type(argument));
      return sem_ir_.addNominalType(NominalTypeId(type.arg0), arguments);
    }
    return source;
  };

  std::vector<TypeId> hidden_type_arguments;
  std::vector<TypeId> outer_type_arguments;
  for (std::uint32_t index = 0; index < generic_count; ++index) {
    outer_type_arguments.push_back(
        sem_ir_.addTypeParameter(outer_generic, index));
    hidden_type_arguments.push_back(
        sem_ir_.addTypeParameter(hidden_generic, index));
  }
  const auto hidden_receiver_type = remap_type(instType(receiver));
  std::vector<TypeId> hidden_source_parameters;
  hidden_source_parameters.reserve(source_parameters.size());
  for (const auto parameter : source_parameters)
    hidden_source_parameters.push_back(remap_type(parameter));
  const auto hidden_result_type = remap_type(result_type);
  const auto environment_decl = [&]() {
    SemNominalType record;
    record.name = hidden_name;
    record.generic = hidden_generic;
    record.declaration = node;
    record.flags |= SemNominalTypeBoundMethodEnvironment;
    if (hidden_generic.hasValue())
      record.flags |= SemNominalTypeArtifactDependency;
    record.completion_state = SemNominalCompletionState::Complete;
    record.fields.push_back(
        {sem_ir_.addName(values_.internIdentifier("receiver")),
         hidden_receiver_type, node});
    return sem_ir_.addNominalTypeDecl(std::move(record));
  }();
  const auto environment_type =
      sem_ir_.addNominalType(environment_decl, outer_type_arguments);
  const auto hidden_environment_type =
      sem_ir_.addNominalType(environment_decl, hidden_type_arguments);
  const auto environment_parameter =
      capability == SemCallableEnvironmentCapability::Consuming
          ? environment_type
          : sem_ir_.addReferenceType(
                environment_type,
                capability == SemCallableEnvironmentCapability::Mutable
                    ? SemReferenceMutability::Mutable
                    : SemReferenceMutability::ReadOnly);
  const auto hidden_environment_parameter =
      capability == SemCallableEnvironmentCapability::Consuming
          ? hidden_environment_type
          : sem_ir_.addReferenceType(
                hidden_environment_type,
                capability == SemCallableEnvironmentCapability::Mutable
                    ? SemReferenceMutability::Mutable
                    : SemReferenceMutability::ReadOnly);
  std::vector<TypeId> hidden_parameters{hidden_environment_parameter};
  hidden_parameters.insert(hidden_parameters.end(),
                           hidden_source_parameters.begin(),
                           hidden_source_parameters.end());
  const auto hidden_type =
      sem_ir_.addFunctionType(hidden_parameters, hidden_result_type);
  std::vector<TypeId> call_parameters{environment_parameter};
  call_parameters.insert(call_parameters.end(), source_parameters.begin(),
                         source_parameters.end());
  const auto call_type = sem_ir_.addFunctionType(call_parameters, result_type);

  FunctionRefId hidden_target = target;
  if (target_template.hasValue() &&
      sem_ir_.functionRef(target_template).generic.hasValue()) {
    if (explicit_member_arguments.size() != generic_count) {
      emit(DiagnosticKind::UnsupportedBoundMethod, node);
      return appendInvalid(block, node);
    }
    std::vector<CanonicalTypeId> hidden_arguments;
    hidden_arguments.reserve(generic_count);
    for (std::uint32_t index = 0; index < generic_count; ++index)
      hidden_arguments.push_back(
          sem_ir_.canonicalType(hidden_type_arguments[index]));
    hidden_target =
        specializeFunction(target_template, {}, node, false, hidden_arguments);
    if (!hidden_target.hasValue())
      return appendInvalid(block, node);
  }
  const auto hidden_flags = hidden_generic.hasValue()
                                ? (SemFunctionTemplate | SemFunctionPublic)
                                : 0U;
  const auto hidden_function = sem_ir_.addFunction(
      {hidden_name, hidden_type, LocalBlockId::invalid(),
       InstBlockId::invalid(), hidden_flags, hidden_generic,
       hidden_generic.hasValue()
           ? values_.generics().generic(hidden_generic).self_specific
           : SpecificId::invalid()});
  SemCallableDeclaration declaration;
  declaration.parameter_names.push_back(hidden_text);
  for (std::uint32_t index = 0; index < source_parameters.size(); ++index)
    declaration.parameter_names.push_back(
        values_.internIdentifier("arg" + std::to_string(index)));
  sem_ir_.setFunctionDeclaration(hidden_function, std::move(declaration));
  const auto hidden_ref = sem_ir_.addFunctionRef(
      {hidden_function, ImportIRInstId::invalid(), call_type,
       PublicEntityId::invalid(), hidden_generic,
       hidden_generic.hasValue()
           ? values_.generics().generic(hidden_generic).self_specific
           : SpecificId::invalid()});
  if (hidden_generic.hasValue()) {
    generic_template_refs_.insert_or_assign(hidden_generic.index, hidden_ref);
    std::vector<PublicType> symbolic_arguments;
    symbolic_arguments.reserve(generic_count);
    for (std::uint32_t index = 0; index < generic_count; ++index)
      symbolic_arguments.emplace_back(PublicTypeKind::TypeParameter, index);
    sem_ir_.setFunctionRefConcreteArguments(hidden_ref,
                                            std::move(symbolic_arguments));
  }

  std::vector<LocalId> parameters;
  std::vector<InstId> body;
  const auto environment_local = sem_ir_.addLocal(
      {hidden_name, hidden_environment_parameter, node, SemLocalMutable});
  parameters.push_back(environment_local);
  (void)appendInst<SemParameter>(body, node, hidden_environment_parameter,
                                 environment_local);
  std::vector<LocalId> argument_locals;
  argument_locals.reserve(hidden_source_parameters.size());
  for (std::uint32_t index = 0; index < hidden_source_parameters.size();
       ++index) {
    const auto local =
        sem_ir_.addLocal({sem_ir_.addName(values_.internIdentifier(
                              "arg" + std::to_string(index))),
                          hidden_source_parameters[index], node, 0});
    parameters.push_back(local);
    argument_locals.push_back(local);
    (void)appendInst<SemParameter>(body, node, hidden_source_parameters[index],
                                   local);
  }
  auto receiver_value = appendInst<SemNameRef>(
      body, node, hidden_environment_parameter, environment_local);
  if (capability != SemCallableEnvironmentCapability::Consuming)
    receiver_value = appendInst<SemDereference>(
        body, node, hidden_environment_type, receiver_value);
  receiver_value = appendInst<SemStructFieldAccess>(
      body, node, hidden_receiver_type, receiver_value, sem_ir_.addInteger(0));
  if (capability == SemCallableEnvironmentCapability::Consuming)
    receiver_value =
        appendInst<SemMove>(body, node, hidden_receiver_type, receiver_value);
  std::vector<InstId> call_arguments{receiver_value};
  for (std::uint32_t index = 0; index < argument_locals.size(); ++index)
    call_arguments.push_back(appendInst<SemNameRef>(
        body, node, hidden_source_parameters[index], argument_locals[index]));
  const auto result =
      appendInst<SemCall>(body, node, hidden_result_type, hidden_target,
                          sem_ir_.addInstBlock(call_arguments));
  (void)appendInst<SemReturn>(body, node, sem_ir_.voidType(), result);
  sem_ir_.setFunction(
      hidden_function,
      {hidden_name, hidden_type, sem_ir_.addLocalBlock(parameters),
       sem_ir_.addInstBlock(body), hidden_flags, hidden_generic,
       hidden_generic.hasValue()
           ? values_.generics().generic(hidden_generic).self_specific
           : SpecificId::invalid()});
  sem_ir_.setCallableEnvironment({SemCallableEnvironmentKind::BoundMethod,
                                  environment_type, hidden_ref, target,
                                  capability, environment_identity});
  synthesizeCallableWitness(
      environment_type, result_type, source_parameters, hidden_ref,
      outer_generic, capability,
      capability == SemCallableEnvironmentCapability::ReadOnly  ? "Invoke"
      : capability == SemCallableEnvironmentCapability::Mutable ? "InvokeMut"
                                                                : "InvokeOnce",
      node);
  if (capability == SemCallableEnvironmentCapability::ReadOnly &&
      result_type == sem_ir_.boolType() &&
      sem_ir_.languageVersion() >= FrozenV16LanguageVersion)
    synthesizeCallableWitness(environment_type, result_type, source_parameters,
                              hidden_ref, outer_generic, capability,
                              "Predicate", node, "std::iter::adapters", "test",
                              std::nullopt);
  if (capability == SemCallableEnvironmentCapability::ReadOnly &&
      source_parameters.size() == 2 &&
      result_type == source_parameters.front() &&
      sem_ir_.languageVersion() >= FrozenV17LanguageVersion)
    synthesizeCallableWitness(environment_type, result_type, source_parameters,
                              hidden_ref, outer_generic, capability, "Folder",
                              node, "std::iter::algorithms", "combine",
                              std::nullopt, true);
  return appendInst<SemBoundMethod>(block, node, environment_type, receiver,
                                    target);
}

[[nodiscard]] InstId
SemanticContext::checkBoundMethodValue(NodeId node, InstId receiver,
                                       NodeId receiver_node, NameId name,
                                       std::vector<InstId> &block) {
  if (!requireVersion(FrozenV12LanguageVersion,
                      DiagnosticKind::CallableRequires12, node))
    return appendInvalid(block, node);
  auto receiver_type = instType(receiver);
  while (sem_ir_.type(receiver_type).kind == SemTypeKind::Reference)
    receiver_type = sem_ir_.referencePointee(receiver_type);
  if (sem_ir_.type(receiver_type).kind != SemTypeKind::Nominal)
    return appendInvalid(block, node);
  const auto nominal_id = NominalTypeId(sem_ir_.type(receiver_type).arg0);
  const auto nominal = sem_ir_.nominalType(nominal_id);
  std::vector<FunctionRefId> candidates;
  for (const auto &member : nominal.member_functions)
    if (member.name == name &&
        (member.flags & SemNominalMemberFunctionAssociated) == 0 &&
        member.target.hasValue())
      candidates.push_back(member.target);
  if (candidates.empty() && nominal.canonical_entity.hasValue())
    candidates = resolveImportedMemberFunctions(
        nominal.canonical_entity, sem_ir_.name(name).text, receiver_node);
  if (candidates.empty()) {
    emit(DiagnosticKind::InvalidMember, node);
    return appendInvalid(block, node);
  }
  std::vector<CanonicalTypeId> explicit_arguments;
  for (const auto argument :
       sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(receiver_type).arg1)))
    explicit_arguments.push_back(sem_ir_.canonicalType(argument));
  struct Viable {
    FunctionRefId template_ref;
    FunctionRefId target;
  };
  std::vector<Viable> viable;
  for (const auto candidate : candidates) {
    const auto &reference = sem_ir_.functionRef(candidate);
    auto target = candidate;
    if (reference.generic.hasValue()) {
      const auto binding_count =
          values_.generics().generic(reference.generic).binding_count;
      if (binding_count != explicit_arguments.size())
        continue;
      target =
          specializeFunction(candidate, {}, node, false, explicit_arguments);
    }
    if (!target.hasValue())
      continue;
    const auto &target_reference = sem_ir_.functionRef(target);
    const auto target_type = sem_ir_.type(target_reference.local_type);
    if (target_type.kind != SemTypeKind::Function)
      continue;
    const auto parameters = sem_ir_.typeBlock(TypeBlockId(target_type.arg0));
    if (parameters.empty() ||
        !canBindMethodReceiver(receiver, parameters.front(), receiver_node))
      continue;
    if (target_reference.local_function.hasValue()) {
      const auto &declaration =
          sem_ir_.functionDeclaration(target_reference.local_function);
      if (declaration.is_unsafe || declaration.foreign_signature ||
          (sem_ir_.function(target_reference.local_function).flags &
           SemFunctionAsync) != 0)
        continue;
    } else if (const auto *entity = sem_ir_.importIRs().tryGetEntity(
                   target_reference.public_entity);
               entity && (entity->is_unsafe || entity->foreign_signature ||
                          entity->execution_kind !=
                              PublicFunctionExecutionKind::Immediate)) {
      continue;
    }
    viable.push_back({candidate, target});
  }
  if (viable.empty()) {
    emit(DiagnosticKind::UnsupportedBoundMethod, node);
    return appendInvalid(block, node);
  }
  if (viable.size() != 1) {
    emit(DiagnosticKind::AmbiguousBoundMethod, node);
    return appendInvalid(block, node);
  }
  const auto target = viable.front().target;
  const auto target_type = sem_ir_.type(sem_ir_.functionRef(target).local_type);
  const auto parameters = sem_ir_.typeBlock(TypeBlockId(target_type.arg0));
  auto adjusted_receiver = receiver;
  if (!adjustMethodReceiver(adjusted_receiver, parameters.front(),
                            receiver_node, block))
    return appendInvalid(block, node);
  const auto receiver_parameter = sem_ir_.type(parameters.front());
  const auto capability =
      receiver_parameter.kind == SemTypeKind::Reference
          ? sem_ir_.referenceMutability(parameters.front()) ==
                    SemReferenceMutability::Mutable
                ? SemCallableEnvironmentCapability::Mutable
                : SemCallableEnvironmentCapability::ReadOnly
          : SemCallableEnvironmentCapability::Consuming;
  return synthesizeBoundMethod(node, block, adjusted_receiver,
                               viable.front().template_ref, target, capability,
                               parameters.subspan(1), TypeId(target_type.arg1),
                               explicit_arguments);
}
