// Included by NominalTypeArtifact.cpp after the shared reader and verifier
// helpers; methods retain the original artifact state and wire format.

bool NominalTypeLayoutArtifact::verify(std::string &error) const {
  internal::NominalLayoutVerificationState state{
      *this,
      [](const NominalTypeLayoutArtifact &layout) {
        return layoutRequestFingerprint(layout);
      },
      [](const NominalTypeLayoutArtifact &layout) {
        return layoutResultFingerprint(layout);
      }};
  return internal::NominalLayoutVerificationService::verify(state, error);
}

std::string NominalTypeLayoutArtifact::encode() const {
  std::string out(LayoutMagic);
  appendFingerprint(out, type_specific_fingerprint);
  appendFingerprint(out, target_fingerprint);
  appendU8(out, static_cast<std::uint8_t>(kind));
  appendU64(out, size);
  appendU64(out, alignment);
  appendU32(out, static_cast<std::uint32_t>(fields.size()));
  for (const auto &field : fields) {
    appendField(out, field.name);
    appendType(out, field.type);
    appendU8(out, static_cast<std::uint8_t>(field.kind));
    appendU8(out, field.storage_type.has_value() ? 1U : 0U);
    if (field.storage_type)
      appendType(out, *field.storage_type);
    appendU64(out, field.offset);
    appendU64(out, field.size);
    appendU64(out, field.alignment);
    appendU32(out, field.bit_begin);
    appendU32(out, field.bit_end);
  }
  appendU32(out, tag_size);
  appendU64(out, payload_offset);
  appendU32(out, static_cast<std::uint32_t>(variants.size()));
  for (const auto &variant : variants) {
    appendField(out, variant.name);
    appendU64(out, variant.size);
    appendU64(out, variant.alignment);
    appendU32(out, static_cast<std::uint32_t>(variant.fields.size()));
    for (const auto &field : variant.fields) {
      appendField(out, field.name);
      appendType(out, field.type);
      appendU64(out, field.offset);
      appendU64(out, field.size);
      appendU64(out, field.alignment);
    }
  }
  appendFingerprint(out, request_fingerprint);
  appendFingerprint(out, result_fingerprint);
  return out;
}

std::optional<NominalTypeLayoutArtifact>
NominalTypeLayoutArtifact::decode(std::string_view bytes, std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext context(bytes.size());
  Reader reader(bytes, context, error);
  if (context.failed())
    return std::nullopt;
  NominalTypeLayoutArtifact result;
  std::uint32_t count = 0;
  std::uint8_t nominal_kind = 0;
  if (!reader.magic(LayoutMagic) ||
      !reader.fingerprint(result.type_specific_fingerprint) ||
      !reader.fingerprint(result.target_fingerprint) ||
      !reader.u8(nominal_kind) ||
      nominal_kind >= static_cast<std::uint8_t>(NominalKind::Count) ||
      !reader.u64(result.size) || !reader.u64(result.alignment) ||
      !reader.u32(count) || !reader.records(count, 38)) {
    error = "nominal layout artifact is malformed";
    return std::nullopt;
  }
  result.kind = static_cast<NominalKind>(nominal_kind);
  result.fields.resize(count);
  for (auto &field : result.fields) {
    std::uint8_t kind = 0, has_storage = 0;
    if (!reader.string(field.name) || !reader.type(field.type) ||
        !reader.u8(kind) || !reader.u8(has_storage) || has_storage > 1 ||
        kind >= static_cast<std::uint8_t>(ObjectFieldProjectionKind::Count)) {
      error = "nominal layout artifact has a truncated field";
      return std::nullopt;
    }
    field.kind = static_cast<ObjectFieldProjectionKind>(kind);
    if (has_storage) {
      PublicType storage;
      if (!reader.type(storage)) {
        error = "nominal layout artifact has a truncated storage type";
        return std::nullopt;
      }
      field.storage_type = std::move(storage);
    }
    if (!reader.u64(field.offset) || !reader.u64(field.size) ||
        !reader.u64(field.alignment) || !reader.u32(field.bit_begin) ||
        !reader.u32(field.bit_end)) {
      error = "nominal layout artifact has a truncated field";
      return std::nullopt;
    }
  }
  if (!reader.u32(result.tag_size) || !reader.u64(result.payload_offset) ||
      !reader.u32(count) || !reader.records(count, 24)) {
    error = "nominal layout artifact has truncated enum metadata";
    return std::nullopt;
  }
  result.variants.resize(count);
  for (auto &variant : result.variants) {
    if (!reader.string(variant.name) || !reader.u64(variant.size) ||
        !reader.u64(variant.alignment) || !reader.u32(count) ||
        !reader.records(count, 28)) {
      error = "nominal layout artifact has a truncated enum variant";
      return std::nullopt;
    }
    variant.fields.resize(count);
    for (auto &field : variant.fields) {
      if (!reader.string(field.name) || !reader.type(field.type) ||
          !reader.u64(field.offset) || !reader.u64(field.size) ||
          !reader.u64(field.alignment)) {
        error = "nominal layout artifact has a truncated payload field";
        return std::nullopt;
      }
      field.kind = ObjectFieldProjectionKind::StableAddress;
    }
  }
  if (!reader.fingerprint(result.request_fingerprint) ||
      !reader.fingerprint(result.result_fingerprint) || !reader.done()) {
    error = "nominal layout artifact has trailing or missing data";
    return std::nullopt;
  }
  if (!result.verify(error))
    return std::nullopt;
  return result;
}

