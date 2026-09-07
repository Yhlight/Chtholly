#include "NominalTypeSpecificVerificationInternal.h"

#include <limits>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool NominalTypeSpecificVerificationService::verify(
    NominalTypeSpecificVerificationState &state, std::string &error) {
  const auto &artifact = state.artifact;
  error.clear();
  if (!state.valid_entity(artifact.template_entity,
                          PublicEntityKind::NominalType) ||
      !artifact.semantic_options_fingerprint.hasValue() ||
      artifact.structural_fingerprint != state.structural_fingerprint(artifact) ||
      artifact.representation_policy >= NominalRepresentationPolicy::Count ||
      artifact.kind >= NominalKind::Count ||
      !artifact.nominal_semantic_witness.verify(error) ||
      artifact.nominal_semantic_witness.kind != artifact.kind ||
      artifact.nominal_semantic_witness.nominal_template !=
          artifact.template_entity ||
      artifact.nominal_semantic_witness.arguments != artifact.arguments ||
      artifact.nominal_semantic_witness.semantic_options_fingerprint !=
          artifact.semantic_options_fingerprint ||
      artifact.nominal_semantic_witness.structural_specific_fingerprint !=
          artifact.structural_fingerprint ||
      artifact.nominal_semantic_witness.object_repr_carrier !=
          artifact.object_repr_carrier ||
      artifact.request_fingerprint != state.request_fingerprint(
          artifact.template_entity, artifact.arguments,
          artifact.semantic_options_fingerprint) ||
      artifact.result_fingerprint != state.result_fingerprint(artifact)) {
    error = "nominal type specific has an invalid identity or representation";
    return false;
  }
  for (const auto &argument : artifact.arguments)
    if (!state.verify_public_type(argument, 0, true, error))
      return false;
  if (artifact.object_repr_carrier &&
      (!state.verify_public_type(*artifact.object_repr_carrier, 0, false,
                                 error) ||
       artifact.object_repr_carrier->kind != PublicTypeKind::Nominal))
    return false;
  std::unordered_set<std::string> names;
  std::size_t nominal_field_count = 0;
  for (const auto &field : artifact.fields) {
    if (field.name.empty() || !names.insert(field.name).second ||
        !state.verify_public_type(field.type, 0, false, error))
      return false;
    if (field.type.kind == PublicTypeKind::Nominal)
      ++nominal_field_count;
  }
  std::unordered_set<std::string> variant_names;
  std::unordered_set<std::int64_t> variant_discriminants;
  for (std::size_t variant_index = 0; variant_index < artifact.variants.size();
       ++variant_index) {
    const auto &variant = artifact.variants[variant_index];
    if (variant.name.empty() || !variant_names.insert(variant.name).second ||
        variant.shape >= PublicEnumPayloadShape::Count ||
        (variant.shape == PublicEnumPayloadShape::Unit) !=
            variant.fields.empty())
      return false;
    if ((artifact.is_value_enum &&
         (variant.shape != PublicEnumPayloadShape::Unit ||
          variant.discriminant < std::numeric_limits<std::int32_t>::min() ||
          variant.discriminant > std::numeric_limits<std::int32_t>::max() ||
          !variant_discriminants.insert(variant.discriminant).second)) ||
        (!artifact.is_value_enum &&
         variant.discriminant != static_cast<std::int64_t>(variant_index)))
      return false;
    std::unordered_set<std::string> payload_names;
    for (const auto &field : variant.fields) {
      if (field.name.empty() || !payload_names.insert(field.name).second ||
          !state.verify_public_type(field.type, 0, true, error))
        return false;
      if (field.type.kind == PublicTypeKind::Nominal)
        ++nominal_field_count;
    }
  }
  if ((artifact.kind == NominalKind::Enum) != !artifact.variants.empty() ||
      (artifact.kind == NominalKind::Enum && !artifact.fields.empty()) ||
      (artifact.kind != NominalKind::Enum && artifact.is_value_enum)) {
    error = "nominal specific has inconsistent enum variants";
    return false;
  }
  if (artifact.child_specific_fingerprints.size() != nominal_field_count) {
    error = "nominal type specific has an inconsistent child closure";
    return false;
  }
  return std::ranges::all_of(artifact.child_specific_fingerprints,
                             &StableFingerprint::hasValue);
}

std::string NominalTypeSpecificVerificationService::encode(
    NominalTypeSpecificEncodingState &state) {
  const auto &artifact = state.artifact;
  std::string out("CHNXSPE32");
  state.append_entity(out, artifact.template_entity);
  auto append_u8 = [&](std::uint8_t value) {
    out.push_back(static_cast<char>(value));
  };
  auto append_u32 = [&](std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
      out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  };
  append_u32(static_cast<std::uint32_t>(artifact.arguments.size()));
  for (const auto &argument : artifact.arguments)
    state.append_type(out, argument);
  state.append_fingerprint(out, artifact.semantic_options_fingerprint);
  state.append_fields(out, artifact.fields);
  state.append_variants(out, artifact.variants);
  append_u8(artifact.is_value_enum ? 1U : 0U);
  append_u8(static_cast<std::uint8_t>(artifact.representation_policy));
  append_u8(static_cast<std::uint8_t>(artifact.kind));
  append_u8(artifact.object_repr_carrier.has_value() ? 1U : 0U);
  if (artifact.object_repr_carrier)
    state.append_type(out, *artifact.object_repr_carrier);
  append_u32(static_cast<std::uint32_t>(
      artifact.child_specific_fingerprints.size()));
  for (const auto &fingerprint : artifact.child_specific_fingerprints)
    state.append_fingerprint(out, fingerprint);
  state.append_fingerprint(out, artifact.structural_fingerprint);
  state.append_field(out, state.encode_witness(artifact.nominal_semantic_witness));
  state.append_fingerprint(out, artifact.request_fingerprint);
  state.append_fingerprint(out, artifact.result_fingerprint);
  return out;
}

} // namespace chtholly::compiler::internal
