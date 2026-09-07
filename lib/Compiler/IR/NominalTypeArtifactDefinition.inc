// Included by NominalTypeArtifact.cpp; this fragment aliases canonical
// nominal artifact encoding, verification, and decode state.
std::string canonicalPublicTypeBytes(const PublicType &type) {
  std::string result;
  appendType(result, type);
  return result;
}

bool verifyPublicType(const PublicType &type,
                      std::uint32_t generic_parameter_count, bool allow_void,
                      std::string &error) {
  if (type.kind >= PublicTypeKind::Count ||
      (!allow_void && type.kind == PublicTypeKind::Void) ||
      (type.kind != PublicTypeKind::Tuple && type.abi_union) ||
      (type.kind != PublicTypeKind::CFunctionPointer &&
       type.foreign_calling_convention != ForeignCallingConvention::C) ||
      type.foreign_calling_convention >= ForeignCallingConvention::Count ||
      (type.kind != PublicTypeKind::CFunctionPointer &&
       (type.callable_variadic ||
        type.callable_context_parameter != core::AnyId::InvalidIndex ||
        type.callable_contract != CallableOwnershipSummary{}))) {
    error = "public type has an invalid kind";
    return false;
  }
  if (type.kind == PublicTypeKind::TypeParameter) {
    if (type.binding_index >= generic_parameter_count) {
      error = "public type has an invalid generic binding";
      return false;
    }
    return type.arguments.empty();
  }
  if (type.binding_index != core::AnyId::InvalidIndex) {
    error = "non-parameter public type has a binding index";
    return false;
  }
  if (type.kind == PublicTypeKind::Never)
    return type.arguments.empty();
  if (type.kind == PublicTypeKind::Integer) {
    if (!type.arguments.empty() ||
        (type.scalar_width != 8 && type.scalar_width != 16 &&
         type.scalar_width != 32 && type.scalar_width != 64)) {
      error = "integer public type has invalid canonical facts";
      return false;
    }
    return true;
  }
  if (type.kind == PublicTypeKind::Float) {
    if (!type.arguments.empty() ||
        (type.scalar_width != 32 && type.scalar_width != 64)) {
      error = "float public type has invalid canonical facts";
      return false;
    }
    return true;
  }
  if (type.kind == PublicTypeKind::Reference) {
    if (type.arguments.size() != 1 ||
        type.reference_mutability >= PublicReferenceMutability::Count ||
        type.reference_provenance.kind >=
            PublicReferenceProvenanceKind::Count ||
        (type.reference_provenance.kind == PublicReferenceProvenanceKind::Erased
             ? type.reference_provenance.index != core::AnyId::InvalidIndex
             : type.reference_provenance.index == core::AnyId::InvalidIndex)) {
      error = "reference public type has invalid canonical facts";
      return false;
    }
    return verifyPublicType(type.arguments.front(), generic_parameter_count,
                            false, error);
  }
  if (type.kind == PublicTypeKind::TypeProjection) {
    const auto associated =
        type.projection_kind == PublicTypeProjectionKind::Associated;
    if (generic_parameter_count == 0 || type.arguments.size() != 1 ||
        type.projection_kind >= PublicTypeProjectionKind::Count ||
        type.projection_index > 0x7fffffffU ||
        (type.projection_kind == PublicTypeProjectionKind::Pointee &&
         type.projection_index != 0) ||
        (associated &&
         !validEntity(type.nominal_entity, PublicEntityKind::Interface)) ||
        (type.arguments.front().kind != PublicTypeKind::TypeParameter &&
         type.arguments.front().kind != PublicTypeKind::TypeProjection)) {
      error = "type projection has invalid canonical facts";
      return false;
    }
    return verifyPublicType(type.arguments.front(), generic_parameter_count,
                            false, error);
  }
  if (type.kind == PublicTypeKind::Array) {
    if (type.arguments.size() != 1 || type.array_bound == 0 ||
        type.arguments.front().kind == PublicTypeKind::Never ||
        type.arguments.front().kind == PublicTypeKind::Void) {
      error = "array public type has invalid canonical facts";
      return false;
    }
    return verifyPublicType(type.arguments.front(), generic_parameter_count,
                            false, error);
  }
  if (type.kind == PublicTypeKind::Tuple) {
    if (type.abi_union && type.arguments.empty()) {
      error = "C union public type has no members";
      return false;
    }
    for (const auto &element : type.arguments)
      if (element.kind == PublicTypeKind::Never ||
          element.kind == PublicTypeKind::Void ||
          !verifyPublicType(element, generic_parameter_count, false, error))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::Slice) {
    if (type.arguments.size() != 1 ||
        type.arguments.front().kind == PublicTypeKind::Never ||
        type.arguments.front().kind == PublicTypeKind::Void) {
      error = "slice public type has invalid canonical facts";
      return false;
    }
    return verifyPublicType(type.arguments.front(), generic_parameter_count,
                            false, error);
  }
  if (type.kind == PublicTypeKind::RawPointer) {
    if (type.arguments.size() != 1) {
      error = "raw pointer public type has invalid canonical facts";
      return false;
    }
    return verifyPublicType(type.arguments.front(), generic_parameter_count,
                            true, error);
  }
  if (type.kind == PublicTypeKind::Function) {
    if (type.arguments.empty()) {
      error = "function public type has no result";
      return false;
    }
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index)
      if (type.arguments[index].kind == PublicTypeKind::Never ||
          !verifyPublicType(type.arguments[index], generic_parameter_count,
                            false, error))
        return false;
    return verifyPublicType(type.arguments.back(), generic_parameter_count,
                            true, error);
  }
  if (type.kind == PublicTypeKind::CFunctionPointer) {
    if (type.arguments.empty()) {
      error = "C function-pointer public type has no result";
      return false;
    }
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index)
      if (type.arguments[index].kind == PublicTypeKind::Never ||
          !verifyPublicType(type.arguments[index], generic_parameter_count,
                            false, error))
        return false;
    const auto parameter_count =
        static_cast<std::uint32_t>(type.arguments.size() - 1);
    return (!type.callable_variadic ||
            type.callable_context_parameter == core::AnyId::InvalidIndex) &&
           (type.callable_context_parameter == core::AnyId::InvalidIndex ||
            type.callable_context_parameter < parameter_count) &&
           verifyPublicType(type.arguments.back(), generic_parameter_count,
                            true, error) &&
           type.callable_contract.verify(parameter_count, error);
  }
  if (type.kind == PublicTypeKind::CallbackAdapter) {
    if (!validCallbackAdapterContract(type)) {
      error = "callback adapter public type has an invalid ABI contract";
      return false;
    }
    for (const auto &field : type.arguments)
      if (!verifyPublicType(field, generic_parameter_count, false, error))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::CallbackRegistration) {
    if (!validCallbackRegistrationContract(type)) {
      error = "callback registration public type has an invalid ABI contract";
      return false;
    }
    for (const auto &field : type.arguments)
      if (!verifyPublicType(field, generic_parameter_count, false, error))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::CallbackCompletion) {
    if (!validCallbackCompletionContract(type)) {
      error = "callback completion public type has an invalid ABI contract";
      return false;
    }
    for (const auto &field : type.arguments)
      if (!verifyPublicType(field, generic_parameter_count, false, error))
        return false;
    return true;
  }
  if (type.kind == PublicTypeKind::CallbackWake) {
    if (!validCallbackWakeContract(type)) {
      error = "callback wake public type has an invalid ABI contract";
      return false;
    }
    return verifyPublicType(type.arguments.front(), generic_parameter_count,
                            false, error);
  }
  if (type.kind != PublicTypeKind::Nominal)
    return type.arguments.empty();
  if (!validEntity(type.nominal_entity, PublicEntityKind::NominalType)) {
    error = "nominal public type has an invalid canonical entity";
    return false;
  }
  for (const auto &argument : type.arguments)
    if (!verifyPublicType(argument, generic_parameter_count, true, error))
      return false;
  return true;
}

PublicNominalTypeArtifact buildPublicNominalTypeArtifact(
    std::string canonical_package, std::string canonical_module,
    std::string canonical_name, std::uint32_t generic_parameter_count,
    std::vector<PublicNominalFieldArtifact> fields,
    std::optional<PublicType> value_repr_pattern,
    std::optional<PublicType> object_repr_pattern,
    NominalRepresentationPolicy representation_policy, NominalKind kind,
    std::vector<PublicEnumVariantArtifact> variants, bool is_exported,
    bool is_value_enum) {
  PublicNominalTypeArtifact result;
  result.entity.kind = PublicEntityKind::NominalType;
  result.entity.canonical_package = std::move(canonical_package);
  result.entity.canonical_module = std::move(canonical_module);
  result.entity.canonical_name = std::move(canonical_name);
  result.generic_parameter_count = generic_parameter_count;
  result.is_exported = is_exported;
  result.fields = std::move(fields);
  result.variants = std::move(variants);
  result.is_value_enum = is_value_enum;
  result.value_repr_pattern = std::move(value_repr_pattern);
  result.object_repr_pattern = std::move(object_repr_pattern);
  result.representation_policy = representation_policy;
  result.kind = kind;
  finalizePublicNominalTypeArtifact(result);
  return result;
}

void finalizePublicNominalTypeArtifact(PublicNominalTypeArtifact &result) {
  result.definition_fingerprint = definitionFingerprint(
      result.entity, result.generic_parameter_count, result.fields,
      result.variants, result.value_repr_pattern, result.object_repr_pattern,
      result.representation_policy, result.kind, result.is_exported,
      result.is_value_enum, &result);
  result.entity.expected_fingerprint = result.definition_fingerprint;
}

internal::NominalTypeArtifactVerificationCallbacks
makeNominalTypeArtifactVerificationCallbacks() {
  return {
      [](const PublicEntityReferenceArtifact &entity, PublicEntityKind kind) {
        return validEntity(entity, kind);
      },
      [](const PublicNominalTypeArtifact &artifact) {
        return definitionFingerprint(
            artifact.entity, artifact.generic_parameter_count, artifact.fields,
            artifact.variants, artifact.value_repr_pattern,
            artifact.object_repr_pattern, artifact.representation_policy,
            artifact.kind, artifact.is_exported, artifact.is_value_enum,
            &artifact);
      },
      [](const PublicType &type, std::uint32_t generic_count, bool allow_void,
         std::string &error) {
        return verifyPublicType(type, generic_count, allow_void, error);
      },
      [](const PublicType &type) { return hasParameterProvenance(type); }};
}

std::string PublicNominalTypeArtifact::encode() const {
  std::string out(DefinitionMagic);
  appendEntity(out, entity);
  appendU8(out, is_exported ? 1U : 0U);
  appendU32(out, generic_parameter_count);
  appendFields(out, fields);
  appendVariants(out, variants);
  appendU8(out, is_value_enum ? 1U : 0U);
  appendU8(out, value_repr_pattern.has_value() ? 1U : 0U);
  if (value_repr_pattern)
    appendType(out, *value_repr_pattern);
  appendU8(out, object_repr_pattern.has_value() ? 1U : 0U);
  if (object_repr_pattern)
    appendType(out, *object_repr_pattern);
  appendU8(out, static_cast<std::uint8_t>(representation_policy));
  appendU8(out, static_cast<std::uint8_t>(kind));
  appendForeignResourceMetadata(out, *this);
  appendFingerprint(out, definition_fingerprint);
  return out;
}

std::optional<PublicNominalTypeArtifact>
PublicNominalTypeArtifact::decode(std::string_view bytes, std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext context(bytes.size());
  if (context.failed()) {
    context.preferBudgetError(error);
    return std::nullopt;
  }
  return internal::decodePublicNominalTypeArtifact(bytes, error, context);
}

std::optional<PublicNominalTypeArtifact>
internal::decodePublicNominalTypeArtifact(
    std::string_view bytes, std::string &error,
    internal::ArtifactDecodeContext &context) {
  Reader reader(bytes, context, error);
  PublicNominalTypeArtifact result;
  std::uint8_t exported = 0, value_enum = 0, present = 0;
  if (!reader.magic(DefinitionMagic) || !reader.entity(result.entity) ||
      !reader.u8(exported) || exported > 1 ||
      !reader.u32(result.generic_parameter_count) ||
      !reader.fields(result.fields) || !reader.variants(result.variants) ||
      !reader.u8(value_enum) || value_enum > 1 || !reader.u8(present)) {
    error = "nominal definition artifact is truncated or malformed";
    return std::nullopt;
  }
  result.is_exported = exported != 0;
  result.is_value_enum = value_enum != 0;
  if (present) {
    result.value_repr_pattern = PublicType{};
    if (!reader.type(*result.value_repr_pattern)) {
      error = "nominal definition has an invalid value representation";
      return std::nullopt;
    }
  }
  if (!reader.u8(present)) {
    error = "nominal definition has a truncated object representation";
    return std::nullopt;
  }
  if (present) {
    result.object_repr_pattern = PublicType{};
    if (!reader.type(*result.object_repr_pattern)) {
      error = "nominal definition has an invalid object representation";
      return std::nullopt;
    }
  }
  std::uint8_t representation_policy = 0, nominal_kind = 0;
  if (!reader.u8(representation_policy) ||
      representation_policy >=
          static_cast<std::uint8_t>(NominalRepresentationPolicy::Count) ||
      !reader.u8(nominal_kind) ||
      nominal_kind >= static_cast<std::uint8_t>(NominalKind::Count)) {
    error = "nominal definition artifact is truncated or malformed";
    return std::nullopt;
  }
  result.representation_policy =
      static_cast<NominalRepresentationPolicy>(representation_policy);
  result.kind = static_cast<NominalKind>(nominal_kind);
  const auto read_optional_type = [&](std::optional<PublicType> &type) {
    if (!reader.u8(present) || present > 1)
      return false;
    if (!present)
      return true;
    type = PublicType{};
    return reader.type(*type);
  };
  std::uint8_t invalid_state = 0;
  std::uint64_t invalid_integer = 0;
  std::string protocol_bytes;
  std::uint32_t operation_count = 0;
  if (!read_optional_type(result.foreign_representation) ||
      !reader.u8(invalid_state) ||
      invalid_state >
          static_cast<std::uint8_t>(ForeignResourceInvalidState::Count) ||
      !reader.u64(invalid_integer) ||
      !read_optional_type(result.foreign_handle_type) ||
      !read_optional_type(result.foreign_completion_handle_type) ||
      !read_optional_type(result.foreign_callback_type) ||
      !read_optional_type(result.foreign_waker_type) ||
      !reader.string(protocol_bytes) || !reader.u32(operation_count) ||
      operation_count > 64 || !reader.records(operation_count, 1)) {
    error = "nominal definition has malformed foreign resource metadata";
    return std::nullopt;
  }
  result.foreign_invalid_state =
      static_cast<ForeignResourceInvalidState>(invalid_state);
  result.foreign_invalid_integer = static_cast<std::int64_t>(invalid_integer);
  const auto protocol_type_count = static_cast<std::uint32_t>(
      1U + (result.foreign_completion_handle_type ? 1U : 0U) +
      (result.foreign_callback_type ? 1U : 0U) + operation_count);
  if (result.kind == NominalKind::ForeignResource) {
    auto protocol = internal::decodeForeignResourceProtocol(
        protocol_bytes, protocol_type_count, error, context);
    if (!protocol)
      return std::nullopt;
    result.foreign_resource_protocol = std::move(*protocol);
  } else if (!protocol_bytes.empty()) {
    error = "non-resource nominal definition contains a foreign protocol";
    return std::nullopt;
  }
  result.foreign_resource_operations.resize(operation_count);
  for (auto &operation : result.foreign_resource_operations) {
    std::uint8_t role = 0;
    if (!reader.string(operation.name) || !reader.u8(role) ||
        role >= static_cast<std::uint8_t>(ForeignResourceRoleKind::Count) ||
        !reader.entity(operation.target) ||
        !reader.u32(operation.resource_parameter) ||
        !reader.u32(operation.completion_parameter)) {
      error = "nominal definition has a malformed foreign operation";
      return std::nullopt;
    }
    operation.role = static_cast<ForeignResourceRoleKind>(role);
  }
  if (!reader.fingerprint(result.definition_fingerprint) || !reader.done()) {
    error = "nominal definition artifact is truncated or malformed";
    return std::nullopt;
  }
  if (!result.verify(error))
    return std::nullopt;
  return result;
}
