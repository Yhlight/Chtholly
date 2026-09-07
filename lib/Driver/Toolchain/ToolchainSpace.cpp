#include "ToolchainSpace.h"

#include <limits>

namespace chtholly::toolchain_internal {

ToolchainInstallSpaceEstimate estimateToolchainInstallSpace(
    std::uint64_t payload_bytes, std::uint64_t index_bytes,
    std::uint64_t available_bytes) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto required_bytes =
      payload_bytes > maximum - index_bytes ? maximum
                                             : payload_bytes + index_bytes;
  return ToolchainInstallSpaceEstimate{payload_bytes, index_bytes,
                                       required_bytes, available_bytes,
                                       required_bytes <= available_bytes};
}

} // namespace chtholly::toolchain_internal
