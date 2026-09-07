#pragma once

#include "chtholly/Core/Id.h"

#include <string>
#include <string_view>

namespace chtholly::compiler::interop {

// Session-local identity for a verified interop artifact. It must never be
// serialized; published references use the stable fingerprint carried by the
// artifact boundary instead.
struct InteropArtifactId : core::IndexBase<InteropArtifactId> {
  using IndexBase::IndexBase;
};

struct ArtifactBundle;

[[nodiscard]] bool writeArtifactBundle(const ArtifactBundle &bundle,
                                       const std::string &path,
                                       std::string &error);
[[nodiscard]] bool readArtifactBundle(const std::string &path,
                                      ArtifactBundle &bundle,
                                      std::string &error);

} // namespace chtholly::compiler::interop
