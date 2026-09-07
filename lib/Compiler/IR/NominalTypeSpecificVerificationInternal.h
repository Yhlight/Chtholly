#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <functional>

namespace chtholly::compiler::internal {

struct NominalTypeSpecificVerificationState {
  const NominalTypeSpecificArtifact &artifact;
  std::function<bool(const PublicEntityReferenceArtifact &, PublicEntityKind)>
      valid_entity;
  std::function<StableFingerprint(const NominalTypeSpecificArtifact &)>
      structural_fingerprint;
  std::function<StableFingerprint(const PublicEntityReferenceArtifact &,
                                  std::span<const PublicType>,
                                  const StableFingerprint &)>
      request_fingerprint;
  std::function<StableFingerprint(const NominalTypeSpecificArtifact &)>
      result_fingerprint;
  std::function<bool(const PublicType &, std::uint32_t, bool, std::string &)>
      verify_public_type;
};

struct NominalTypeSpecificEncodingState {
  const NominalTypeSpecificArtifact &artifact;
  std::function<void(std::string &, const PublicEntityReferenceArtifact &)>
      append_entity;
  std::function<void(std::string &, const PublicType &)> append_type;
  std::function<void(std::string &, const std::vector<PublicNominalFieldArtifact> &)>
      append_fields;
  std::function<void(std::string &, const std::vector<PublicEnumVariantArtifact> &)>
      append_variants;
  std::function<void(std::string &, const StableFingerprint &)> append_fingerprint;
  std::function<void(std::string &, std::string_view)> append_field;
  std::function<std::string(const NominalSemanticWitnessArtifact &)>
      encode_witness;
};

struct NominalTypeSpecificVerificationService {
  [[nodiscard]] static bool verify(NominalTypeSpecificVerificationState &state,
                                   std::string &error);
  [[nodiscard]] static std::string encode(
      NominalTypeSpecificEncodingState &state);
};

} // namespace chtholly::compiler::internal
