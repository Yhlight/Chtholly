// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
[[nodiscard]] std::vector<FunctionRefId>
SemanticContext::resolveImportedFunctions(NodeId node, bool diagnose) {
  const auto names = tree_.children(node);
  if (names.size() < 2) {
    if (diagnose)
      emit(DiagnosticKind::UnknownImportedFunction, node);
    return {};
  }
  std::string module_text;
  for (std::size_t index = 0; index + 1 < names.size(); ++index) {
    if (!module_text.empty())
      module_text += "::";
    const auto component = tokenString(names[index]);
    if (!component.hasValue()) {
      if (diagnose)
        emit(DiagnosticKind::UnknownImport, names[index]);
      return {};
    }
    module_text += values_.identifier(component);
  }
  const auto module = values_.internIdentifier(module_text);
  const auto imported = sem_ir_.importIRs().findByModule(module);
  if (!imported.hasValue()) {
    if (diagnose)
      emit(DiagnosticKind::UnknownImport, names[0]);
    return {};
  }
  sem_ir_.addModuleAccessContext(names.back(), imported);
  const auto *interface = sem_ir_.importIRs().tryGetInterface(imported);
  if (!interface) {
    if (diagnose)
      emit(DiagnosticKind::UnknownImport, names[0]);
    return {};
  }
  const auto requested_name = tokenString(names.back());
  const auto public_bindings = interface->findFunctions(requested_name);
  if (public_bindings.empty()) {
    if (diagnose)
      emit(DiagnosticKind::UnknownImportedFunction, names.back());
    return {};
  }
  std::vector<FunctionRefId> result;
  result.reserve(public_bindings.size());
  for (const auto public_binding : public_bindings) {
    const auto import_inst =
        sem_ir_.addImportIRInst({imported, public_binding});
    std::string canonical_error;
    const auto canonical =
        sem_ir_.importIRs().canonicalEntity(import_inst, canonical_error);
    if (!canonical.hasValue())
      continue;
    const auto reference = resolvePublicEntity(canonical, node, import_inst);
    if (reference.hasValue())
      result.push_back(reference);
  }
  if (result.empty() && diagnose)
    emit(DiagnosticKind::UnknownImportedFunction, names.back());
  return result;
}

[[nodiscard]] FunctionRefId
SemanticContext::resolveImportedFunction(NodeId node) {
  const auto functions = resolveImportedFunctions(node);
  if (functions.size() != 1) {
    if (functions.size() > 1)
      emit(DiagnosticKind::AmbiguousCall, node);
    return FunctionRefId::invalid();
  }
  return functions.front();
}

[[nodiscard]] std::optional<ConstantId>
SemanticContext::materializePublicConstantValue(
    const PublicConstantValue &source, NodeId location) {
  const auto local_type =
      mapImportedPublicType(source.type, GenericId::invalid(), location);
  if (!local_type.hasValue())
    return std::nullopt;
  std::vector<ConstantId> elements;
  elements.reserve(source.elements.size());
  for (const auto &source_element : source.elements) {
    auto element = materializePublicConstantValue(source_element, location);
    if (!element)
      return std::nullopt;
    elements.push_back(*element);
  }
  std::uint64_t payload = 0;
  if (source.kind == PublicConstantValueKind::String) {
    payload = values_.internStringLiteral(source.string_payload).index;
  } else if (source.kind != PublicConstantValueKind::Null &&
             source.kind != PublicConstantValueKind::Array &&
             source.kind != PublicConstantValueKind::Aggregate) {
    payload =
        sem_ir_.addInteger(std::bit_cast<std::int64_t>(source.payload)).index;
  }
  return sem_ir_.addConstantValue(
      {static_cast<ConstantValueKind>(source.kind), local_type, payload,
       sem_ir_.addConstantBlock(elements), source.target_dependent});
}

[[nodiscard]] ConstantEntityId
SemanticContext::materializeImportedDefaultArgument(
    const PublicConstantValue &source, TypeId parameter_type, NodeId location) {
  const auto value = materializePublicConstantValue(source, location);
  if (!value || sem_ir_.constantValue(*value).type != parameter_type)
    return ConstantEntityId::invalid();
  const auto name = sem_ir_.addName(values_.internIdentifier(
      "$import-default$" + std::to_string(imported_default_counter_++)));
  const auto identity =
      std::string(sem_ir_.identifier(sem_ir_.name(name).text));
  return sem_ir_.addConstantEntity(
      {name,
       parameter_type,
       InstBlockId::invalid(),
       InstId::invalid(),
       location,
       SemConstantPublic | SemConstantModule | SemConstantImported,
       {ConstantEvalState::Concrete, *value},
       StableFingerprint::fromCanonicalBytes(
           "chtholly.next.imported-default.v1\n" + identity),
       values_.internIdentifier("$compiler"),
       values_.internIdentifier("$defaults"),
       values_.internIdentifier(identity)});
}

[[nodiscard]] std::optional<InstId>
SemanticContext::resolveImportedValue(NodeId node, std::vector<InstId> &block) {
  const auto names = tree_.children(node);
  if (names.size() < 2)
    return std::nullopt;
  std::string module_text;
  for (std::size_t index = 0; index + 1 < names.size(); ++index) {
    const auto component = tokenString(names[index]);
    if (!component.hasValue())
      return std::nullopt;
    if (!module_text.empty())
      module_text += "::";
    module_text += values_.identifier(component);
  }
  const auto imported =
      sem_ir_.importIRs().findByModule(values_.internIdentifier(module_text));
  if (!imported.hasValue())
    return std::nullopt;
  sem_ir_.addModuleAccessContext(names.back(), imported);
  const auto *interface = sem_ir_.importIRs().tryGetInterface(imported);
  const auto requested_name = tokenString(names.back());
  if (!interface || !requested_name.hasValue())
    return std::nullopt;
  const auto *artifact = interface->findValue(requested_name);
  if (!artifact)
    return std::nullopt;
  const auto key = artifact->entity_fingerprint.hex();
  ConstantEntityId entity;
  if (const auto found = imported_constant_entities_.find(key);
      found != imported_constant_entities_.end()) {
    entity = found->second;
  } else {
    const auto type = mapImportedPublicType(artifact->type,
                                            GenericId::invalid(), names.back());
    if (!type.hasValue()) {
      emit(DiagnosticKind::InvalidImportedValueArtifact, names.back());
      return appendInvalid(block, node);
    }
    const auto value =
        materializePublicConstantValue(artifact->value, names.back());
    if (!value) {
      emit(DiagnosticKind::InvalidImportedValueArtifact, names.back());
      return appendInvalid(block, node);
    }
    std::uint32_t flags =
        SemConstantPublic | SemConstantModule | SemConstantImported;
    if (artifact->kind == PublicValueKind::Static)
      flags |= SemConstantStatic;
    entity = sem_ir_.addConstantEntity(
        {sem_ir_.addName(requested_name),
         type,
         InstBlockId::invalid(),
         InstId::invalid(),
         names.back(),
         flags,
         {ConstantEvalState::Concrete, *value},
         artifact->entity_fingerprint,
         values_.internIdentifier(artifact->canonical_package),
         values_.internIdentifier(artifact->canonical_module),
         values_.internIdentifier(artifact->canonical_name)});
    imported_constant_entities_.emplace(key, entity);
  }
  sem_ir_.addConstantOccurrence(names.back(),
                                SemSymbolOccurrenceKind::Reference, entity);
  const auto &constant = sem_ir_.constantEntity(entity);
  return (constant.flags & SemConstantStatic) != 0
             ? appendInst<SemStaticRef>(block, node, constant.type, entity)
             : appendInst<SemConstantRef>(block, node, constant.type, entity);
}

[[nodiscard]] std::vector<FunctionRefId>
SemanticContext::resolveImportedMemberFunctions(PublicEntityId owner,
                                                IdentifierId method_name,
                                                NodeId location) {
  const auto *owner_entity = sem_ir_.importIRs().tryGetEntity(owner);
  if (!owner_entity || owner_entity->kind != PublicEntityKind::NominalType)
    return {};
  const PublicEntityReferenceArtifact owner_ref{
      PublicEntityKind::NominalType,
      std::string(sem_ir_.identifier(owner_entity->package_name)),
      std::string(sem_ir_.identifier(owner_entity->module_name)),
      std::string(sem_ir_.identifier(owner_entity->name)),
      owner_entity->fingerprint};
  std::vector<FunctionRefId> result;
  for (std::uint32_t index = 0; index < sem_ir_.importIRs().size(); ++index) {
    const auto import_id = ImportIRId(index);
    const auto *interface = sem_ir_.importIRs().tryGetInterface(import_id);
    if (!interface)
      continue;
    for (const auto binding :
         interface->findMemberFunctions(owner_ref, method_name)) {
      const auto import_inst = sem_ir_.addImportIRInst({import_id, binding});
      std::string canonical_error;
      const auto canonical =
          sem_ir_.importIRs().canonicalEntity(import_inst, canonical_error);
      if (!canonical.hasValue())
        continue;
      const auto reference =
          resolvePublicEntity(canonical, location, import_inst);
      if (reference.hasValue() &&
          std::ranges::find(result, reference) == result.end())
        result.push_back(reference);
    }
  }
  return result;
}

[[nodiscard]] FunctionRefId SemanticContext::resolveImportedMemberFunction(
    PublicEntityId owner, IdentifierId method_name, NodeId location) {
  const auto functions =
      resolveImportedMemberFunctions(owner, method_name, location);
  return functions.size() == 1 ? functions.front() : FunctionRefId::invalid();
}

[[nodiscard]] std::optional<SemanticContext::ResolvedAssociatedFunctionValue>
SemanticContext::resolveAssociatedFunctionValue(NodeId node,
                                                TypeId expected_type) {
  const auto components = tree_.children(node);
  if (components.size() < 2)
    return std::nullopt;

  TypeId owner_type;
  NominalTypeId owner;
  const auto occurrence = components.back();
  if (tree_.kind(node) == NodeKind::TypeMemberExpr && components.size() == 2) {
    owner_type = checkType(components.front());
    if (sem_ir_.type(owner_type).kind == SemTypeKind::Nominal)
      owner = NominalTypeId(sem_ir_.type(owner_type).arg0);
    if (!owner.hasValue()) {
      emit(DiagnosticKind::InvalidMember, occurrence);
      return std::nullopt;
    }
  } else if (tree_.kind(node) == NodeKind::QualifiedName) {
    const auto owner_components = std::span<const NodeId>(components).first(components.size() - 1);
    owner_type = resolveNominalTypeComponents(owner_components, node);
    owner = owner_type.hasValue() ? NominalTypeId(sem_ir_.type(owner_type).arg0)
                                : resolveNominalComponents(owner_components, node);
    if (!owner.hasValue())
      return std::nullopt;
  } else {
    return std::nullopt;
  }

  // Resolving imported members can materialize nominal shells.
  const auto nominal = sem_ir_.nominalType(owner);
  sem_ir_.addMemberAccessContext(occurrence, owner,
                                 SemMemberAccessKind::Associated);
  const auto member_name = nameFor(occurrence);
  std::vector<FunctionRefId> candidates;
  for (const auto &member : nominal.member_functions)
    if (member.name == member_name &&
        (member.flags & SemNominalMemberFunctionAssociated) != 0)
      candidates.push_back(member.target);
  if (candidates.empty() && nominal.canonical_entity.hasValue())
    for (const auto candidate : resolveImportedMemberFunctions(
             nominal.canonical_entity, sem_ir_.name(member_name).text,
             occurrence)) {
      const auto *entity = sem_ir_.importIRs().tryGetEntity(
          sem_ir_.functionRef(candidate).public_entity);
      if (entity &&
          entity->member_kind == PublicFunctionArtifact::MemberKind::Associated)
        candidates.push_back(candidate);
    }
  if (candidates.empty()) {
    emit(DiagnosticKind::InvalidMember, occurrence);
    return ResolvedAssociatedFunctionValue{FunctionRefId::invalid(),
                                           occurrence};
  }

  std::vector<CanonicalTypeId> explicit_arguments;
  if (owner_type.hasValue()) {
    const auto &semantic_owner = sem_ir_.type(owner_type);
    for (const auto argument :
         sem_ir_.typeBlock(TypeBlockId(semantic_owner.arg1)))
      explicit_arguments.push_back(sem_ir_.canonicalType(argument));
  }
  FunctionRefId function;
  for (auto candidate : candidates) {
    if (sem_ir_.functionRef(candidate).generic.hasValue()) {
      candidate =
          specializeFunction(candidate, {}, node, false, explicit_arguments);
      if (!candidate.hasValue())
        continue;
    }
    if (expected_type.hasValue() &&
        sem_ir_.functionRef(candidate).local_type != expected_type)
      continue;
    if (function.hasValue()) {
      emit(DiagnosticKind::AmbiguousCall, occurrence);
      return ResolvedAssociatedFunctionValue{FunctionRefId::invalid(),
                                             occurrence};
    }
    function = candidate;
  }
  if (!function.hasValue()) {
    emit(candidates.size() > 1 ? DiagnosticKind::AmbiguousCall
                               : DiagnosticKind::TypeMismatch,
         occurrence);
    return ResolvedAssociatedFunctionValue{FunctionRefId::invalid(),
                                           occurrence};
  }
  return ResolvedAssociatedFunctionValue{function, occurrence};
}
