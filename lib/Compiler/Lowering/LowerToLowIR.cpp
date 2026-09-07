#include "chtholly/Compiler/LowerToLowIR.h"

#include "LowerToLowIRInternal.h"

namespace chtholly::compiler {

LowIR lowerToLowIR(
    const SemIR &sem_ir, core::Arena &arena,
    std::string_view normalized_target_triple,
    std::span<const NominalTypeLayoutArtifact> nominal_layouts,
    std::span<const LowNominalLayoutBinding> nominal_layout_bindings) {
  return internal::lowerToLowIRContext(
      sem_ir, arena, normalized_target_triple, nominal_layouts,
      nominal_layout_bindings);
}

} // namespace chtholly::compiler
