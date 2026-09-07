#pragma once

#include "chtholly/Basic/AbiVersion.h"
#include "chtholly/Basic/TargetInfo.h"

#include <sstream>
#include <string>
#include <string_view>

namespace chtholly {

inline constexpr std::string_view HostedRuntimeAbiVersionV1 = "v1";
inline constexpr std::string_view HostedRuntimeAbiVersionV2 = "v2";
// Stable default for packages that do not request a v2 standard-library
// capability. Component ABI-1 is intentionally pinned to this value.
inline constexpr std::string_view HostedRuntimeAbiVersion =
    HostedRuntimeAbiVersionV1;

[[nodiscard]] constexpr bool isHostedRuntimeAbiVersion(
    std::string_view version) {
  return version == HostedRuntimeAbiVersionV1 ||
         version == HostedRuntimeAbiVersionV2;
}

// Shared semantic inputs for CSI, package artifacts, and workspace caches.
struct ArtifactCompatibilityKey {
  TargetInfo target;
  AbiVersion abi_version = DefaultChthollyAbiVersion;
  std::string semantic_interface_format;
  std::string contract_schema;
  std::string runtime_abi;

  friend bool operator==(const ArtifactCompatibilityKey &lhs,
                         const ArtifactCompatibilityKey &rhs) {
    return lhs.target.triple == rhs.target.triple &&
           lhs.target.pointer_width_bits == rhs.target.pointer_width_bits &&
           lhs.abi_version == rhs.abi_version &&
           lhs.semantic_interface_format == rhs.semantic_interface_format &&
           lhs.contract_schema == rhs.contract_schema &&
           lhs.runtime_abi == rhs.runtime_abi;
  }
};

inline std::string canonicalArtifactCompatibilityKey(
    const ArtifactCompatibilityKey &key) {
  std::ostringstream out;
  out << "artifact-compatibility-v1\n"
      << "target\t" << key.target.triple << '\n'
      << "pointer-width\t" << key.target.pointer_width_bits << '\n'
      << "abi-version\t" << abiVersionSpelling(key.abi_version) << '\n'
      << "semantic-interface-format\t" << key.semantic_interface_format
      << '\n'
      << "contract-schema\t" << key.contract_schema << '\n'
      << "runtime-abi\t" << key.runtime_abi << '\n';
  return out.str();
}

} // namespace chtholly
