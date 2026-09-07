#pragma once

#include <cstdint>
#include <string_view>

namespace chtholly {

enum class OptimizationLevel : std::uint8_t {
  O0,
  O1,
  O2,
  O3,
  Os,
  Oz,
};

enum class DebugInfoKind : std::uint8_t {
  None,
  LineTablesOnly,
  Full,
};

inline constexpr std::string_view
optimizationLevelSpelling(OptimizationLevel level) {
  switch (level) {
  case OptimizationLevel::O0:
    return "O0";
  case OptimizationLevel::O1:
    return "O1";
  case OptimizationLevel::O2:
    return "O2";
  case OptimizationLevel::O3:
    return "O3";
  case OptimizationLevel::Os:
    return "Os";
  case OptimizationLevel::Oz:
    return "Oz";
  }
  return "O0";
}

inline constexpr std::string_view debugInfoKindSpelling(DebugInfoKind kind) {
  switch (kind) {
  case DebugInfoKind::None:
    return "none";
  case DebugInfoKind::LineTablesOnly:
    return "line-tables-only";
  case DebugInfoKind::Full:
    return "full";
  }
  return "none";
}

} // namespace chtholly
