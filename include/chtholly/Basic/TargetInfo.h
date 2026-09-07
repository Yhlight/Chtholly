#pragma once

#include <cstdint>
#include <string>

namespace chtholly {

struct TargetInfo {
  std::string triple;
  std::uint32_t pointer_width_bits = 64;
};

} // namespace chtholly
