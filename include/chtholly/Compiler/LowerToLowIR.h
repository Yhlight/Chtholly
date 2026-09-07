#pragma once

#include "chtholly/Compiler/LowIR.h"

#include <span>

namespace chtholly::compiler {
[[nodiscard]] LowIR
lowerToLowIR(const SemIR &sem_ir, core::Arena &arena,
             std::string_view normalized_target_triple = {},
             std::span<const NominalTypeLayoutArtifact> nominal_layouts = {},
             std::span<const LowNominalLayoutBinding>
                 nominal_layout_bindings = {});
} // namespace chtholly::compiler
