#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace chtholly::compiler {

enum class CarrierViewViolationKind : std::uint8_t {
  Invalid,
  Escape,
  Region,
};

struct CarrierViewViolation {
  InstId instruction;
  CarrierViewViolationKind kind = CarrierViewViolationKind::Escape;
};

// Replays carrier provenance from explicit CarrierView instructions. This is
// intentionally independent from semantic checking so persisted specifics are
// subject to the same non-escape and projector-region contract.
[[nodiscard]] std::vector<CarrierViewViolation>
analyzeCarrierViews(const SemIR &sem_ir);

[[nodiscard]] bool verifyCarrierViews(const SemIR &sem_ir, std::string &error);

} // namespace chtholly::compiler
