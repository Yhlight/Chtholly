// Included by IncrementalDependenciesCodecInternal.h; this fragment keeps
// canonical package-manifest codec helpers in the private namespace.
void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary);
bool readOwnershipSummary(StateReader &reader,
                          CallableOwnershipSummary &summary,
                          std::uint32_t parameter_count);

void appendPublicEntity(std::string &out,
                        const PublicEntityReferenceArtifact &entity) {
  out.push_back(static_cast<char>(entity.kind));
  appendField(out, entity.canonical_package);
  appendField(out, entity.canonical_module);
  appendField(out, entity.canonical_name);
  appendFingerprint(out, entity.expected_fingerprint);
}

void appendPublicType(std::string &out, const PublicType &type) {
  out.push_back(static_cast<char>(type.kind));
  appendU32(out, type.binding_index);
  if (type.kind == PublicTypeKind::Integer) {
    appendU32(out, type.scalar_width);
    out.push_back(type.integer_signed ? 1 : 0);
  } else if (type.kind == PublicTypeKind::Float) {
    appendU32(out, type.scalar_width);
  } else if (type.kind == PublicTypeKind::Nominal) {
    appendPublicEntity(out, type.nominal_entity);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
  } else if (type.kind == PublicTypeKind::Reference) {
    out.push_back(static_cast<char>(type.reference_mutability));
    out.push_back(static_cast<char>(type.reference_provenance.kind));
    appendU32(out, type.reference_provenance.index);
    assert(type.arguments.size() == 1);
    appendPublicType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Array) {
    appendU32(out, type.array_bound);
    assert(type.arguments.size() == 1);
    appendPublicType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Tuple) {
    out.push_back(type.abi_union ? 1 : 0);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
  } else if (type.kind == PublicTypeKind::Slice) {
    out.push_back(type.slice_mutable ? 1 : 0);
    assert(type.arguments.size() == 1);
    appendPublicType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::TypeProjection) {
    out.push_back(static_cast<char>(type.projection_kind));
    appendU32(out, type.projection_index);
    if (type.projection_kind == PublicTypeProjectionKind::Associated)
      appendPublicEntity(out, type.nominal_entity);
    assert(type.arguments.size() == 1);
    appendPublicType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::RawPointer) {
    out.push_back(type.pointer_const ? 1 : 0);
    assert(type.arguments.size() == 1);
    appendPublicType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Function) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
  } else if (type.kind == PublicTypeKind::CFunctionPointer) {
    out.push_back(type.callable_variadic ? 1 : 0);
    out.push_back(static_cast<char>(type.foreign_calling_convention));
    appendU32(out, type.callable_context_parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
    appendOwnershipSummary(out, type.callable_contract);
  } else if (type.kind == PublicTypeKind::CallbackAdapter) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
  } else if (type.kind == PublicTypeKind::CallbackRegistration) {
    out.push_back(static_cast<char>(type.registration_authority));
    appendU32(out, type.registration_entry_parameter);
    appendU32(out, type.registration_userdata_parameter);
    appendU32(out, type.registration_release_parameter);
    appendU32(out,
              static_cast<std::uint32_t>(type.registration_bindings.size()));
    for (const auto &binding : type.registration_bindings) {
      appendField(out, binding.name);
      appendU32(out, binding.parameter_index);
    }
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
  } else if (type.kind == PublicTypeKind::CallbackCompletion) {
    out.push_back(static_cast<char>(type.registration_authority));
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
  } else if (type.kind == PublicTypeKind::CallbackWake) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendPublicType(out, argument);
  } else if (type.kind == PublicTypeKind::ForeignCompletion ||
             type.kind == PublicTypeKind::ForeignWake) {
    appendPublicEntity(out, type.nominal_entity);
  } else if (type.kind == PublicTypeKind::ForeignOperationState) {
    appendPublicEntity(out, type.nominal_entity);
    out.push_back(static_cast<char>(type.foreign_operation_state));
  }
}

void appendConstantValue(std::string &out, const PublicConstantValue &value) {
  out.push_back(static_cast<char>(value.kind));
  appendPublicType(out, value.type);
  appendU32(out, static_cast<std::uint32_t>(value.payload));
  appendU32(out, static_cast<std::uint32_t>(value.payload >> 32U));
  appendField(out, value.string_payload);
  out.push_back(value.target_dependent ? 1 : 0);
  appendU32(out, static_cast<std::uint32_t>(value.elements.size()));
  for (const auto &element : value.elements)
    appendConstantValue(out, element);
}

bool readPublicEntity(StateReader &reader,
                      PublicEntityReferenceArtifact &entity) {
  std::uint8_t kind = 0;
  if (!reader.readU8(kind))
    return false;
  entity.kind = static_cast<PublicEntityKind>(kind);
  return reader.readString(entity.canonical_package) &&
         reader.readString(entity.canonical_module) &&
         reader.readString(entity.canonical_name) &&
         reader.readFingerprint(entity.expected_fingerprint);
}

bool readPublicType(StateReader &reader, PublicType &type) {
  internal::ArtifactDecodeRecursionScope recursion(reader.context());
  std::uint8_t kind = 0;
  if (!recursion.entered() || !reader.readU8(kind) ||
      !reader.readU32(type.binding_index))
    return false;
  type.kind = static_cast<PublicTypeKind>(kind);
  if (type.kind == PublicTypeKind::Integer) {
    std::uint8_t is_signed = 0;
    if (!reader.readU32(type.scalar_width) || !reader.readU8(is_signed) ||
        is_signed > 1)
      return false;
    type.integer_signed = is_signed != 0;
    return true;
  }
  if (type.kind == PublicTypeKind::Float)
    return reader.readU32(type.scalar_width);
  if (type.kind == PublicTypeKind::Reference) {
    std::uint8_t mutability = 0;
    std::uint8_t provenance = 0;
    type.arguments.resize(1);
    if (!reader.readU8(mutability) || !reader.readU8(provenance) ||
        !reader.readU32(type.reference_provenance.index) ||
        !readPublicType(reader, type.arguments.front()))
      return false;
    type.reference_mutability =
        static_cast<PublicReferenceMutability>(mutability);
    type.reference_provenance.kind =
        static_cast<PublicReferenceProvenanceKind>(provenance);
    return true;
  }
  if (type.kind == PublicTypeKind::Array) {
    type.arguments.resize(1);
    return reader.readU32(type.array_bound) && type.array_bound != 0 &&
           readPublicType(reader, type.arguments.front());
  }
  if (type.kind == PublicTypeKind::Tuple) {
    std::uint8_t abi_union = 0;
    std::uint32_t count = 0;
    if (!reader.readU8(abi_union) || abi_union > 1 || !reader.readU32(count) ||
        !reader.readRecords(count, 5))
      return false;
    type.abi_union = abi_union != 0;
    type.arguments.resize(count);
    for (auto &argument : type.arguments)
      if (!readPublicType(reader, argument))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::Slice) {
    std::uint8_t mutable_view = 0;
    type.arguments.resize(1);
    if (!reader.readU8(mutable_view) || mutable_view > 1 ||
        !readPublicType(reader, type.arguments.front()))
      return false;
    type.slice_mutable = mutable_view != 0;
    return true;
  }
  if (type.kind == PublicTypeKind::TypeProjection) {
    std::uint8_t projection = 0;
    type.arguments.resize(1);
    if (!reader.readU8(projection) ||
        projection >=
            static_cast<std::uint8_t>(PublicTypeProjectionKind::Count) ||
        !reader.readU32(type.projection_index))
      return false;
    type.projection_kind = static_cast<PublicTypeProjectionKind>(projection);
    return (type.projection_kind != PublicTypeProjectionKind::Associated ||
            readPublicEntity(reader, type.nominal_entity)) &&
           readPublicType(reader, type.arguments.front());
  }
  if (type.kind == PublicTypeKind::RawPointer) {
    std::uint8_t pointee_const = 0;
    type.arguments.resize(1);
    if (!reader.readU8(pointee_const) || pointee_const > 1 ||
        !readPublicType(reader, type.arguments.front()))
      return false;
    type.pointer_const = pointee_const != 0;
    return true;
  }
  if (type.kind == PublicTypeKind::Function) {
    std::uint32_t count = 0;
    if (!reader.readU32(count) || count == 0 || !reader.readRecords(count, 5))
      return false;
    type.arguments.resize(count);
    for (auto &argument : type.arguments)
      if (!readPublicType(reader, argument))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::CFunctionPointer) {
    std::uint8_t variadic = 0;
    std::uint8_t convention = 0;
    std::uint32_t count = 0;
    if (!reader.readU8(variadic) || variadic > 1 ||
        !reader.readU8(convention) ||
        convention >=
            static_cast<std::uint8_t>(ForeignCallingConvention::Count) ||
        !reader.readU32(type.callable_context_parameter) ||
        !reader.readU32(count) || count == 0 || !reader.readRecords(count, 5))
      return false;
    type.callable_variadic = variadic != 0;
    type.foreign_calling_convention =
        static_cast<ForeignCallingConvention>(convention);
    type.arguments.resize(count);
    for (auto &argument : type.arguments)
      if (!readPublicType(reader, argument))
        return false;
    return readOwnershipSummary(reader, type.callable_contract, count - 1);
  }
  if (type.kind == PublicTypeKind::CallbackAdapter) {
    std::uint32_t count = 0;
    if (!reader.readU32(count) || count != 3 || !reader.readRecords(count, 5))
      return false;
    type.arguments.resize(count);
    for (auto &argument : type.arguments)
      if (!readPublicType(reader, argument))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::CallbackRegistration) {
    std::uint8_t authority = 0;
    std::uint32_t binding_count = 0, count = 0;
    if (!reader.readU8(authority) || authority > 1 ||
        !reader.readU32(type.registration_entry_parameter) ||
        !reader.readU32(type.registration_userdata_parameter) ||
        !reader.readU32(type.registration_release_parameter) ||
        !reader.readU32(binding_count) || !reader.readRecords(binding_count, 8))
      return false;
    type.registration_bindings.resize(binding_count);
    for (auto &binding : type.registration_bindings)
      if (!reader.readString(binding.name) ||
          !reader.readU32(binding.parameter_index))
        return false;
    for (auto &parameter : type.registration_arm_parameters)
      if (!reader.readU32(parameter))
        return false;
    for (auto &parameter : type.registration_detach_parameters)
      if (!reader.readU32(parameter))
        return false;
    if (!reader.readU32(count) || !reader.readRecords(count, 5) ||
        (count != 5 && count != 7 && count != 8 && count != 10))
      return false;
    type.registration_authority = authority;
    type.arguments.resize(count);
    for (auto &argument : type.arguments)
      if (!readPublicType(reader, argument))
        return false;
    std::string protocol_bytes;
    std::string protocol_error;
    if (!reader.readString(protocol_bytes))
      return false;
    auto protocol = internal::decodeForeignResourceProtocol(
        protocol_bytes, count, protocol_error, reader.context());
    if (!protocol)
      return false;
    type.foreign_resource_protocol = std::move(*protocol);
    return type.foreign_resource_protocol ==
           makeCallbackRegistrationProtocol(
               type.registration_authority, type.registration_entry_parameter,
               type.registration_userdata_parameter,
               type.registration_release_parameter, type.registration_bindings,
               count, type.registration_arm_parameters,
               type.registration_detach_parameters);
  }
  if (type.kind == PublicTypeKind::CallbackCompletion) {
    std::uint8_t authority = 0;
    std::uint32_t count = 0;
    if (!reader.readU8(authority) || authority > 1)
      return false;
    for (auto &parameter : type.registration_arm_parameters)
      if (!reader.readU32(parameter))
        return false;
    for (auto &parameter : type.registration_detach_parameters)
      if (!reader.readU32(parameter))
        return false;
    if (!reader.readU32(count) || !reader.readRecords(count, 5) ||
        (count != 4 && count != 5 && count != 7))
      return false;
    type.registration_authority = authority;
    type.arguments.resize(count);
    for (auto &argument : type.arguments)
      if (!readPublicType(reader, argument))
        return false;
    std::string protocol_bytes;
    std::string protocol_error;
    if (!reader.readString(protocol_bytes))
      return false;
    auto protocol = internal::decodeForeignResourceProtocol(
        protocol_bytes, count, protocol_error, reader.context());
    if (!protocol)
      return false;
    type.foreign_resource_protocol = std::move(*protocol);
    return type.foreign_resource_protocol ==
           makeCallbackCompletionProtocol(type.registration_authority, count,
                                          type.registration_arm_parameters,
                                          type.registration_detach_parameters);
  }
  if (type.kind == PublicTypeKind::CallbackWake) {
    std::uint32_t count = 0;
    if (!reader.readU32(count) || count != 1 || !reader.readRecords(count, 5))
      return false;
    type.arguments.resize(1);
    return readPublicType(reader, type.arguments.front());
  }
  if (type.kind == PublicTypeKind::ForeignCompletion ||
      type.kind == PublicTypeKind::ForeignWake)
    return readPublicEntity(reader, type.nominal_entity);
  if (type.kind == PublicTypeKind::ForeignOperationState) {
    std::uint8_t state = 0;
    if (!readPublicEntity(reader, type.nominal_entity) ||
        !reader.readU8(state) ||
        state >= static_cast<std::uint8_t>(ForeignOperationStateKind::Count))
      return false;
    type.foreign_operation_state =
        static_cast<ForeignOperationStateKind>(state);
    return true;
  }
  if (type.kind != PublicTypeKind::Nominal)
    return true;
  std::uint32_t count = 0;
  if (!readPublicEntity(reader, type.nominal_entity) ||
      !reader.readU32(count) || !reader.readRecords(count, 5))
    return false;
  type.arguments.resize(count);
  for (auto &argument : type.arguments)
    if (!readPublicType(reader, argument))
      return false;
  return true;
}

void appendInterfaceConstraint(
    std::string &out, const PublicInterfaceConstraintArtifact &constraint) {
  appendPublicType(out, constraint.subject);
  appendPublicEntity(out, constraint.interface_entity);
  appendU32(out, static_cast<std::uint32_t>(constraint.arguments.size()));
  for (const auto &argument : constraint.arguments)
    appendPublicType(out, argument);
}

bool readInterfaceConstraint(StateReader &reader,
                             PublicInterfaceConstraintArtifact &constraint) {
  std::uint32_t count = 0;
  if (!readPublicType(reader, constraint.subject) ||
      !readPublicEntity(reader, constraint.interface_entity) ||
      !reader.readU32(count) || !reader.readRecords(count, 5))
    return false;
  constraint.arguments.resize(count);
  for (auto &argument : constraint.arguments)
    if (!readPublicType(reader, argument))
      return false;
  return true;
}

void appendInterfaceDeclaration(
    std::string &out, const PublicInterfaceDeclarationArtifact &declaration) {
  appendPublicEntity(out, declaration.entity);
  appendU32(out, declaration.generic_parameter_count);
  appendU32(out, declaration.explicit_parameter_count);
  appendU32(out, static_cast<std::uint32_t>(declaration.constraints.size()));
  for (const auto &constraint : declaration.constraints)
    appendInterfaceConstraint(out, constraint);
  appendU32(out, static_cast<std::uint32_t>(declaration.requirements.size()));
  for (const auto &requirement : declaration.requirements) {
    out.push_back(static_cast<char>(requirement.kind));
    appendField(out, requirement.name);
    appendPublicEntity(out, requirement.function);
    appendPublicType(out, requirement.associated_type);
    appendU32(out, requirement.binding_index);
    out.push_back(requirement.has_default ? 1 : 0);
  }
}

bool readInterfaceDeclaration(StateReader &reader,
                              PublicInterfaceDeclarationArtifact &declaration) {
  std::uint32_t count = 0;
  if (!readPublicEntity(reader, declaration.entity) ||
      !reader.readU32(declaration.generic_parameter_count) ||
      !reader.readU32(declaration.explicit_parameter_count) ||
      !reader.readU32(count) || !reader.readRecords(count, 10))
    return false;
  declaration.constraints.resize(count);
  for (auto &constraint : declaration.constraints)
    if (!readInterfaceConstraint(reader, constraint))
      return false;
  if (!reader.readU32(count) || !reader.readRecords(count, 16))
    return false;
  declaration.requirements.resize(count);
  for (auto &requirement : declaration.requirements) {
    std::uint8_t kind = 0;
    std::uint8_t has_default = 0;
    if (!reader.readU8(kind) || !reader.readString(requirement.name) ||
        !readPublicEntity(reader, requirement.function) ||
        !readPublicType(reader, requirement.associated_type) ||
        !reader.readU32(requirement.binding_index) ||
        !reader.readU8(has_default) || has_default > 1)
      return false;
    requirement.kind = static_cast<PublicInterfaceRequirementKind>(kind);
    requirement.has_default = has_default != 0;
  }
  return true;
}

void appendTypeAlias(std::string &out, const PublicTypeAliasArtifact &alias) {
  appendPublicEntity(out, alias.entity);
  appendU32(out, alias.generic_parameter_count);
  appendPublicType(out, alias.target);
  appendU32(out, static_cast<std::uint32_t>(alias.constraints.size()));
  for (const auto &constraint : alias.constraints)
    appendInterfaceConstraint(out, constraint);
}

bool readTypeAlias(StateReader &reader, PublicTypeAliasArtifact &alias) {
  std::uint32_t count = 0;
  if (!readPublicEntity(reader, alias.entity) ||
      !reader.readU32(alias.generic_parameter_count) ||
      !readPublicType(reader, alias.target) || !reader.readU32(count) ||
      !reader.readRecords(count, 10))
    return false;
  alias.constraints.resize(count);
  for (auto &constraint : alias.constraints)
    if (!readInterfaceConstraint(reader, constraint))
      return false;
  return true;
}

void appendInterfaceWitness(std::string &out,
                            const PublicInterfaceWitnessArtifact &witness) {
  appendPublicEntity(out, witness.interface_entity);
  appendU32(out, witness.generic_parameter_count);
  appendPublicType(out, witness.self_type);
  appendU32(out,
            static_cast<std::uint32_t>(witness.interface_arguments.size()));
  for (const auto &argument : witness.interface_arguments)
    appendPublicType(out, argument);
  appendU32(out, static_cast<std::uint32_t>(witness.constraints.size()));
  for (const auto &constraint : witness.constraints)
    appendInterfaceConstraint(out, constraint);
  appendU32(out, static_cast<std::uint32_t>(witness.entries.size()));
  for (const auto &entry : witness.entries) {
    appendU32(out, entry.requirement);
    appendPublicEntity(out, entry.function);
    appendPublicType(out, entry.associated_type);
  }
  appendFingerprint(out, witness.fingerprint);
}

bool readInterfaceWitness(StateReader &reader,
                          PublicInterfaceWitnessArtifact &witness) {
  std::uint32_t count = 0;
  if (!readPublicEntity(reader, witness.interface_entity) ||
      !reader.readU32(witness.generic_parameter_count) ||
      !readPublicType(reader, witness.self_type) || !reader.readU32(count) ||
      !reader.readRecords(count, 5))
    return false;
  witness.interface_arguments.resize(count);
  for (auto &argument : witness.interface_arguments)
    if (!readPublicType(reader, argument))
      return false;
  if (!reader.readU32(count) || !reader.readRecords(count, 10))
    return false;
  witness.constraints.resize(count);
  for (auto &constraint : witness.constraints)
    if (!readInterfaceConstraint(reader, constraint))
      return false;
  if (!reader.readU32(count) || !reader.readRecords(count, 9))
    return false;
  witness.entries.resize(count);
  for (auto &entry : witness.entries)
    if (!reader.readU32(entry.requirement) ||
        !readPublicEntity(reader, entry.function) ||
        !readPublicType(reader, entry.associated_type))
      return false;
  return reader.readFingerprint(witness.fingerprint);
}

