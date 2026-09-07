#pragma once

#include <cstdint>

namespace chtholly::toolchain_internal {

// The install preflight is deliberately a pure calculation so callers can
// exercise it without allocating a large archive or modifying a manager root.
// `index_bytes` accounts for the signed release index that is staged alongside
// the verified payload files.
struct ToolchainInstallSpaceEstimate {
  std::uint64_t payload_bytes = 0;
  std::uint64_t index_bytes = 0;
  std::uint64_t required_bytes = 0;
  std::uint64_t available_bytes = 0;
  bool sufficient = false;
};

[[nodiscard]] ToolchainInstallSpaceEstimate
estimateToolchainInstallSpace(std::uint64_t payload_bytes,
                              std::uint64_t index_bytes,
                              std::uint64_t available_bytes) noexcept;

} // namespace chtholly::toolchain_internal
