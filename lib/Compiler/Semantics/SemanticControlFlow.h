#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <span>

namespace chtholly::compiler::semantics_internal {

[[nodiscard]] bool blockFallsThrough(const SemIR &sem_ir,
                                     std::span<const InstId> block);

} // namespace chtholly::compiler::semantics_internal
