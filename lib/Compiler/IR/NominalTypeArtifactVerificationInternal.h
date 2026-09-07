#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <functional>

namespace chtholly::compiler::internal {

struct NominalTypeArtifactVerificationCallbacks {
  std::function<bool(const PublicEntityReferenceArtifact &, PublicEntityKind)>
      valid_entity;
  std::function<StableFingerprint(const PublicNominalTypeArtifact &)>
      definition_fingerprint;
  std::function<bool(const PublicType &, std::uint32_t, bool, std::string &)>
      verify_public_type;
  std::function<bool(const PublicType &)> has_parameter_provenance;
};

struct NominalTypeArtifactVerificationService {
  [[nodiscard]] static bool verify(
      const PublicNominalTypeArtifact &artifact, std::string &error,
      const NominalTypeArtifactVerificationCallbacks &callbacks);
};

} // namespace chtholly::compiler::internal

namespace chtholly::compiler {

[[nodiscard]] internal::NominalTypeArtifactVerificationCallbacks
makeNominalTypeArtifactVerificationCallbacks();

} // namespace chtholly::compiler
