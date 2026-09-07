#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace chtholly::compiler::semantics_internal {

enum class SemanticWitnessLookupKind : std::uint8_t {
  Found,
  Missing,
  Ambiguous,
  Cycle,
  Invalid,
};

struct SemanticWitnessLookupResult {
  SemanticWitnessLookupKind kind = SemanticWitnessLookupKind::Missing;
  InterfaceWitnessId witness;

  [[nodiscard]] bool hasValue() const {
    return kind == SemanticWitnessLookupKind::Found && witness.hasValue();
  }
};

struct SemanticWitnessCandidate {
  InterfaceWitnessId witness;
  StableFingerprint fingerprint;
};

enum class SemanticObligationState : std::uint8_t {
  Deferred,
  Resolved,
  Failed,
};

enum class SemanticObligationWorklistResult : std::uint8_t {
  Complete,
  Failed,
  Stalled,
};

class SemanticObligationWorklist {
public:
  explicit SemanticObligationWorklist(std::size_t size);

  [[nodiscard]] SemanticObligationWorklistResult
  run(const std::function<SemanticObligationState(std::size_t)> &resolve);

private:
  std::vector<bool> resolved_;
};

[[nodiscard]] SemanticWitnessLookupResult
selectSemanticWitness(std::span<const SemanticWitnessCandidate> candidates);

} // namespace chtholly::compiler::semantics_internal
