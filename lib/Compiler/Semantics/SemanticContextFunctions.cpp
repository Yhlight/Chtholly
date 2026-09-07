#include "SemanticContext.h"

namespace chtholly::compiler::semantics_internal {
#include "SemanticContextFunctionHelpers.inc"

#include "SemanticContextFunctionPredeclaration.inc"
#include "SemanticContextFunctionReference.inc"

#include "SemanticContextInterfacePredeclaration.inc"

#include "SemanticContextCanonicalPredeclaration.inc"

#include "SemanticContextFunctionValidation.inc"

#include "SemanticContextFunctionBody.inc"

#include "SemanticContextImportedTypes.inc"
#include "SemanticContextImportedFunctions.inc"
#include "SemanticContextFunctionValues.inc"

} // namespace chtholly::compiler::semantics_internal
