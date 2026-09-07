#include "SemanticCallResolution.h"

#include <tuple>

namespace chtholly::compiler::semantics_internal {
namespace {

auto key(const SemanticCallCandidateRank &rank) {
  return std::tuple(rank.maximum_conversion_rank, rank.total_conversion_rank,
                    rank.generic);
}

} // namespace

SemanticCallSelection
selectCallCandidate(std::span<const SemanticCallCandidateRank> candidates) {
  if (candidates.empty())
    return {};
  std::size_t best = 0;
  bool ambiguous = false;
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    if (key(candidates[index]) < key(candidates[best])) {
      best = index;
      ambiguous = false;
    } else if (key(candidates[index]) == key(candidates[best])) {
      ambiguous = true;
    }
  }
  return {ambiguous ? SemanticCallSelectionKind::Ambiguous
                    : SemanticCallSelectionKind::Selected,
          best};
}

} // namespace chtholly::compiler::semantics_internal
