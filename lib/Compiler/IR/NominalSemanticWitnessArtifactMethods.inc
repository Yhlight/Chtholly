// Included by NominalTypeArtifact.cpp after the shared reader and verifier
// helpers; methods retain the original artifact state and wire format.

bool NominalSemanticWitnessArtifact::verify(std::string &error) const {
  internal::NominalSemanticWitnessVerificationState state{
      *this,
      [](const PublicEntityReferenceArtifact &entity, PublicEntityKind kind) {
        return validEntity(entity, kind);
      },
      [](const NominalSemanticWitnessArtifact &witness) {
        return semanticWitnessRequestFingerprint(witness);
      },
      [](const NominalSemanticWitnessArtifact &witness) {
        return semanticWitnessResultFingerprint(witness);
      },
      [](const PublicType &type, std::uint32_t generic_count, bool allow_void,
         std::string &type_error) {
        return verifyPublicType(type, generic_count, allow_void, type_error);
      }};
  return internal::NominalSemanticWitnessVerificationService::verify(state,
                                                                     error);
}

std::string NominalSemanticWitnessArtifact::encode() const {
  internal::NominalSemanticWitnessEncodingState state{
      *this,
      [](std::string &out, const PublicEntityReferenceArtifact &entity) {
        appendEntity(out, entity);
      },
      [](std::string &out, const PublicType &type) { appendType(out, type); },
      [](std::string &out,
         const std::optional<PublicEntityReferenceArtifact> &entity) {
        appendOptionalEntity(out, entity);
      },
      [](std::string &out, const ObjectFieldProjectionArtifact &projection) {
        appendObjectProjection(out, projection);
      },
      [](std::string &out, std::span<const LifecycleBodyOp> body) {
        appendLifecycleBody(out, body);
      },
      [](std::string &out, const StableFingerprint &fingerprint) {
        appendFingerprint(out, fingerprint);
      }};
  return internal::NominalSemanticWitnessEncodingService::encode(state);
}

std::optional<NominalSemanticWitnessArtifact>
NominalSemanticWitnessArtifact::decode(std::string_view bytes,
                                       std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext context(bytes.size());
  if (context.failed()) {
    context.preferBudgetError(error);
    return std::nullopt;
  }
  return internal::decodeNominalSemanticWitnessArtifact(bytes, error, context);
}

std::optional<NominalSemanticWitnessArtifact>
internal::decodeNominalSemanticWitnessArtifact(
    std::string_view bytes, std::string &error,
    internal::ArtifactDecodeContext &context) {
  Reader reader(bytes, context, error);
  NominalSemanticWitnessArtifact result;
  std::uint32_t count = 0;
  std::uint8_t value_repr = 0, init_repr = 0, ownership = 0, copy = 0, move = 0,
               destroy = 0, object_repr = 0, transferable = 0, shareable = 0,
               present = 0, nominal_kind = 0;
  if (!reader.magic(WitnessMagic) || !reader.entity(result.nominal_template) ||
      !reader.u32(count) || !reader.records(count, 5)) {
    error = "nominal semantic witness artifact is malformed";
    return std::nullopt;
  }
  result.arguments.resize(count);
  for (auto &argument : result.arguments)
    if (!reader.type(argument)) {
      error = "nominal semantic witness has an invalid argument";
      return std::nullopt;
    }
  if (!reader.fingerprint(result.semantic_options_fingerprint) ||
      !reader.fingerprint(result.structural_specific_fingerprint) ||
      !reader.u8(nominal_kind) ||
      nominal_kind >= static_cast<std::uint8_t>(NominalKind::Count) ||
      !reader.u8(value_repr) || !reader.u8(init_repr) ||
      !reader.u8(ownership) || !reader.u8(copy) || !reader.u8(move) ||
      !reader.u8(destroy) || !reader.u8(object_repr) ||
      !reader.u8(transferable) || transferable > 1 || !reader.u8(shareable) ||
      shareable > 1 || !reader.u8(present)) {
    error = "nominal semantic witness artifact is truncated";
    return std::nullopt;
  }
  result.kind = static_cast<NominalKind>(nominal_kind);
  result.concurrency = {transferable != 0, shareable != 0};
  if (present) {
    result.copy_target.emplace();
    if (!reader.entity(*result.copy_target))
      return error = "nominal semantic witness has a truncated copy target",
             std::nullopt;
  }
  if (!reader.u8(present))
    return error = "nominal semantic witness artifact is truncated",
           std::nullopt;
  if (present) {
    result.destroy_target.emplace();
    if (!reader.entity(*result.destroy_target))
      return error = "nominal semantic witness has a truncated destroy target",
             std::nullopt;
  }
  if (!reader.u8(present))
    return error = "nominal semantic witness artifact is truncated",
           std::nullopt;
  if (present) {
    result.pack_target.emplace();
    if (!reader.entity(*result.pack_target))
      return error = "nominal semantic witness has a truncated pack target",
             std::nullopt;
  }
  if (!reader.u8(present))
    return error = "nominal semantic witness artifact is truncated",
           std::nullopt;
  if (present) {
    result.init_target.emplace();
    if (!reader.entity(*result.init_target))
      return error = "nominal semantic witness has a truncated init target",
             std::nullopt;
  }
  if (!reader.u8(present))
    return error = "nominal semantic witness artifact is truncated",
           std::nullopt;
  if (present) {
    result.value_repr_carrier = PublicType{};
    if (!reader.type(*result.value_repr_carrier))
      return error = "nominal semantic witness has an invalid carrier",
             std::nullopt;
  }
  if (!reader.u8(present))
    return error = "nominal semantic witness artifact is truncated",
           std::nullopt;
  if (present) {
    result.object_repr_carrier = PublicType{};
    if (!reader.type(*result.object_repr_carrier))
      return error = "nominal semantic witness has an invalid object carrier",
             std::nullopt;
  }
  if (!reader.u32(count) || !reader.records(count, 4))
    return error = "nominal semantic witness has invalid object projections",
           std::nullopt;
  result.object_field_projections.resize(count);
  for (auto &projection : result.object_field_projections)
    if (!readObjectProjection(reader, projection))
      return error = "nominal semantic witness has invalid object projections",
             std::nullopt;
  if (!readOptionalEntity(reader, result.object_init_target) ||
      !readOptionalEntity(reader, result.object_copy_init_target) ||
      !readOptionalEntity(reader, result.object_move_init_target) ||
      !readOptionalEntity(reader, result.object_drop_target))
    return error = "nominal semantic witness has truncated object lifecycle",
           std::nullopt;
  if (!readLifecycleBody(reader, result.copy_body) ||
      !readLifecycleBody(reader, result.drop_body))
    return error = "nominal semantic witness has an invalid generated body",
           std::nullopt;
  result.representation = {static_cast<ValueReprKind>(value_repr),
                           static_cast<InitReprKind>(init_repr),
                           static_cast<OwnershipReprKind>(ownership),
                           static_cast<CopyReprKind>(copy),
                           static_cast<MoveReprKind>(move),
                           static_cast<DestroyReprKind>(destroy),
                           static_cast<ObjectReprKind>(object_repr)};
  if (!reader.u32(count) ||
      !reader.records(count, StableFingerprint::ByteCount))
    return error = "nominal semantic witness has a truncated closure",
           std::nullopt;
  result.transitive_specific_fingerprints.resize(count);
  for (auto &fingerprint : result.transitive_specific_fingerprints)
    if (!reader.fingerprint(fingerprint))
      return error = "nominal semantic witness has a truncated closure",
             std::nullopt;
  if (!reader.fingerprint(result.request_fingerprint) ||
      !reader.fingerprint(result.result_fingerprint) || !reader.done())
    return error = "nominal semantic witness has trailing or missing data",
           std::nullopt;
  if (!result.verify(error))
    return std::nullopt;
  return result;
}
