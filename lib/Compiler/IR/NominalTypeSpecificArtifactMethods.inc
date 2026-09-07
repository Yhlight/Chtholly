// Included by NominalTypeArtifact.cpp after the shared reader and verifier
// helpers; methods retain the original artifact state and wire format.

bool NominalTypeSpecificArtifact::verify(std::string &error) const {
  internal::NominalTypeSpecificVerificationState state{
      *this,
      [](const PublicEntityReferenceArtifact &entity, PublicEntityKind kind) {
        return validEntity(entity, kind);
      },
      [](const NominalTypeSpecificArtifact &specific) {
        return structuralSpecificFingerprint(
            specific.template_entity, specific.arguments,
            specific.semantic_options_fingerprint, specific.fields,
            specific.variants, specific.representation_policy, specific.kind,
            specific.is_value_enum, specific.object_repr_carrier,
            specific.child_specific_fingerprints);
      },
      [](const PublicEntityReferenceArtifact &entity,
         std::span<const PublicType> arguments,
         const StableFingerprint &semantic_options_fingerprint) {
        return specificRequestFingerprint(entity, arguments,
                                           semantic_options_fingerprint);
      },
      [](const NominalTypeSpecificArtifact &specific) {
        return specificResultFingerprint(specific);
      },
      [](const PublicType &type, std::uint32_t generic_count, bool allow_void,
         std::string &type_error) {
        return verifyPublicType(type, generic_count, allow_void, type_error);
      }};
  return internal::NominalTypeSpecificVerificationService::verify(state,
                                                                  error);
}

std::string NominalTypeSpecificArtifact::encode() const {
  internal::NominalTypeSpecificEncodingState state{
      *this,
      [](std::string &out, const PublicEntityReferenceArtifact &entity) {
        appendEntity(out, entity);
      },
      [](std::string &out, const PublicType &type) { appendType(out, type); },
      [](std::string &out,
         const std::vector<PublicNominalFieldArtifact> &fields) {
        appendFields(out, fields);
      },
      [](std::string &out,
         const std::vector<PublicEnumVariantArtifact> &variants) {
        appendVariants(out, variants);
      },
      [](std::string &out, const StableFingerprint &fingerprint) {
        appendFingerprint(out, fingerprint);
      },
      [](std::string &out, std::string_view value) {
        appendField(out, value);
      },
      [](const NominalSemanticWitnessArtifact &witness) {
        return witness.encode();
      }};
  return internal::NominalTypeSpecificVerificationService::encode(state);
}

std::optional<NominalTypeSpecificArtifact>
NominalTypeSpecificArtifact::decode(std::string_view bytes,
                                    std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext context(bytes.size());
  Reader reader(bytes, context, error);
  if (context.failed())
    return std::nullopt;
  NominalTypeSpecificArtifact result;
  std::uint32_t count = 0;
  std::uint8_t object_present = 0;
  if (!reader.magic(SpecificMagic) || !reader.entity(result.template_entity) ||
      !reader.u32(count) || !reader.records(count, 5)) {
    error = "nominal type specific artifact is malformed";
    return std::nullopt;
  }
  result.arguments.resize(count);
  for (auto &argument : result.arguments)
    if (!reader.type(argument)) {
      error = "nominal type specific artifact has an invalid argument";
      return std::nullopt;
    }
  std::uint8_t value_enum = 0, representation_policy = 0, nominal_kind = 0;
  if (!reader.fingerprint(result.semantic_options_fingerprint) ||
      !reader.fields(result.fields) || !reader.variants(result.variants) ||
      !reader.u8(value_enum) || value_enum > 1 ||
      !reader.u8(representation_policy) ||
      representation_policy >=
          static_cast<std::uint8_t>(NominalRepresentationPolicy::Count) ||
      !reader.u8(nominal_kind) ||
      nominal_kind >= static_cast<std::uint8_t>(NominalKind::Count) ||
      !reader.u8(object_present)) {
    error = "nominal type specific artifact is truncated";
    return std::nullopt;
  }
  result.representation_policy =
      static_cast<NominalRepresentationPolicy>(representation_policy);
  result.kind = static_cast<NominalKind>(nominal_kind);
  result.is_value_enum = value_enum != 0;
  if (object_present) {
    result.object_repr_carrier = PublicType{};
    if (!reader.type(*result.object_repr_carrier)) {
      error = "nominal type specific artifact has an invalid object carrier";
      return std::nullopt;
    }
  }
  if (!reader.u32(count) ||
      !reader.records(count, StableFingerprint::ByteCount)) {
    error = "nominal type specific artifact is truncated";
    return std::nullopt;
  }
  result.child_specific_fingerprints.resize(count);
  for (auto &fingerprint : result.child_specific_fingerprints)
    if (!reader.fingerprint(fingerprint)) {
      error = "nominal type specific artifact has a truncated child closure";
      return std::nullopt;
    }
  std::string lifecycle_bytes;
  if (!reader.fingerprint(result.structural_fingerprint) ||
      !reader.string(lifecycle_bytes)) {
    error = "nominal type specific artifact has a truncated nominal semantic "
            "witness";
    return std::nullopt;
  }
  auto lifecycle = internal::decodeNominalSemanticWitnessArtifact(
      lifecycle_bytes, error, context);
  if (!lifecycle)
    return std::nullopt;
  result.nominal_semantic_witness = std::move(*lifecycle);
  if (!reader.fingerprint(result.request_fingerprint) ||
      !reader.fingerprint(result.result_fingerprint) || !reader.done()) {
    error = "nominal type specific artifact has trailing or missing data";
    return std::nullopt;
  }
  if (!result.verify(error))
    return std::nullopt;
  return result;
}
