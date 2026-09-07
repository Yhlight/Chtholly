#include "SemanticContext.h"

namespace chtholly::compiler::semantics_internal {

#include "SemanticContextCoreLifecycle.inc"

#include "SemanticContextCoreDiagnostics.inc"
#include "SemanticContextCoreTypes.inc"

#include "SemanticContextCoreImports.inc"

#include "SemanticContextCoreCoroutine.inc"
#include "SemanticContextCoreOwnership.inc"
#include "SemanticContextCoreConversions.inc"
#include "SemanticContextCoreCarrier.inc"
#include "SemanticContextCoreLiteralAttributes.inc"
#include "SemanticContextCoreDeclarations.inc"

} // namespace chtholly::compiler::semantics_internal
