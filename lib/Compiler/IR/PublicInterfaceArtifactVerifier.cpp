#include "chtholly/Compiler/PublicInterface.h"

#include "PublicInterfaceServices.h"

namespace chtholly::compiler {

bool PublicInterfaceArtifact::verifyBody(std::string &error) const {
  // The verifier service owns phase ordering. The callback factory only binds
  // the policy predicates to this canonical artifact and does not duplicate
  // any of its stores.
  return internal::PublicInterfaceArtifactVerificationService::verify(
      *this, error, makeArtifactVerificationCallbacks(*this));
}

} // namespace chtholly::compiler
