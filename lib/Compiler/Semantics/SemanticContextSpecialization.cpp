#include "SemanticContext.h"

// Keep the specialization translation unit dependent on witness diagnostics
// included through its semantic fragments.

namespace chtholly::compiler::semantics_internal {

#include "SemanticContextSpecializationSpecific.inc"
#include "SemanticContextSpecializationConcrete.inc"
#include "SemanticContextSpecializationComponents.inc"
#include "SemanticContextSpecializationFunctions.inc"
#include "SemanticContextSpecializationResult.inc"
#include "SemanticContextSpecializationExpressions.inc"

} // namespace chtholly::compiler::semantics_internal
