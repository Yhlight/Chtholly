#include "SemanticWitnessResolution.h"

#include <algorithm>
#include <vector>

namespace chtholly::compiler::semantics_internal {

SemanticObligationWorklist::SemanticObligationWorklist(std::size_t size)
    : resolved_(size) {}

SemanticObligationWorklistResult SemanticObligationWorklist::run(
    const std::function<SemanticObligationState(std::size_t)> &resolve) {
  std::size_t remaining = resolved_.size();
  while (remaining != 0) {
    bool progress = false;
    for (std::size_t index = 0; index < resolved_.size(); ++index) {
      if (resolved_[index])
        continue;
      switch (resolve(index)) {
      case SemanticObligationState::Deferred:
        break;
      case SemanticObligationState::Resolved:
        resolved_[index] = true;
        --remaining;
        progress = true;
        break;
      case SemanticObligationState::Failed:
        return SemanticObligationWorklistResult::Failed;
      }
    }
    if (!progress)
      return SemanticObligationWorklistResult::Stalled;
  }
  return SemanticObligationWorklistResult::Complete;
}

SemanticWitnessLookupResult
selectSemanticWitness(std::span<const SemanticWitnessCandidate> candidates) {
  if (candidates.empty())
    return {};
  std::vector<SemanticWitnessCandidate> ordered(candidates.begin(),
                                                candidates.end());
  std::ranges::sort(ordered, [](const auto &lhs, const auto &rhs) {
    if (lhs.fingerprint.hex() != rhs.fingerprint.hex())
      return lhs.fingerprint.hex() < rhs.fingerprint.hex();
    return lhs.witness.index < rhs.witness.index;
  });
  if (ordered.size() != 1)
    return {.kind = SemanticWitnessLookupKind::Ambiguous};
  return {.kind = SemanticWitnessLookupKind::Found,
          .witness = ordered.front().witness};
}

} // namespace chtholly::compiler::semantics_internal
