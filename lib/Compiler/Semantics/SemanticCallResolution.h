#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace chtholly::compiler::semantics_internal {

struct SemanticCallCandidateRank {
  std::uint32_t maximum_conversion_rank = 0;
  std::uint32_t total_conversion_rank = 0;
  bool generic = false;

  friend bool operator==(const SemanticCallCandidateRank &,
                         const SemanticCallCandidateRank &) = default;
};

enum class SemanticCallSelectionKind : std::uint8_t {
  None,
  Selected,
  Ambiguous,
};

struct SemanticCallSelection {
  SemanticCallSelectionKind kind = SemanticCallSelectionKind::None;
  std::size_t index = 0;
};

[[nodiscard]] SemanticCallSelection
selectCallCandidate(std::span<const SemanticCallCandidateRank> candidates);

} // namespace chtholly::compiler::semantics_internal
