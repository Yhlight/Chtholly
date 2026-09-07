#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <functional>

namespace chtholly::compiler::internal {

struct NominalSemanticWitnessVerificationState {
  const NominalSemanticWitnessArtifact &artifact;
  std::function<bool(const PublicEntityReferenceArtifact &, PublicEntityKind)>
      valid_entity;
  std::function<StableFingerprint(const NominalSemanticWitnessArtifact &)>
      request_fingerprint;
  std::function<StableFingerprint(const NominalSemanticWitnessArtifact &)>
      result_fingerprint;
  std::function<bool(const PublicType &, std::uint32_t, bool, std::string &)>
      verify_public_type;
};

struct NominalSemanticWitnessVerificationService {
  [[nodiscard]] static bool verify(
      NominalSemanticWitnessVerificationState &state, std::string &error);
};

struct NominalSemanticWitnessEncodingState {
  const NominalSemanticWitnessArtifact &artifact;
  std::function<void(std::string &, const PublicEntityReferenceArtifact &)>
      append_entity;
  std::function<void(std::string &, const PublicType &)> append_type;
  std::function<void(
      std::string &,
      const std::optional<PublicEntityReferenceArtifact> &)> append_optional_entity;
  std::function<void(std::string &, const ObjectFieldProjectionArtifact &)>
      append_projection;
  std::function<void(std::string &, std::span<const LifecycleBodyOp>)>
      append_lifecycle_body;
  std::function<void(std::string &, const StableFingerprint &)> append_fingerprint;
};

struct NominalSemanticWitnessEncodingService {
  [[nodiscard]] static std::string encode(
      NominalSemanticWitnessEncodingState &state);
};

} // namespace chtholly::compiler::internal
