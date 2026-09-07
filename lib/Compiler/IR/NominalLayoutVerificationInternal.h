#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <functional>

namespace chtholly::compiler::internal {

struct NominalLayoutVerificationState {
  const NominalTypeLayoutArtifact &layout;
  std::function<StableFingerprint(const NominalTypeLayoutArtifact &)>
      request_fingerprint;
  std::function<StableFingerprint(const NominalTypeLayoutArtifact &)>
      result_fingerprint;
};

struct NominalLayoutVerificationService {
  [[nodiscard]] static bool verify(NominalLayoutVerificationState &state,
                                   std::string &error);
};

} // namespace chtholly::compiler::internal
