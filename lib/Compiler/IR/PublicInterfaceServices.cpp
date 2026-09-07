#include "PublicInterfaceServices.h"

#include <limits>
#include <ranges>
#include <algorithm>

namespace chtholly::compiler::internal {


bool PublicInterfaceArtifactVerificationService::verify(
    const PublicInterfaceArtifact &artifact, std::string &error,
    const VerificationCallbacks &callbacks) {
  error.clear();
  if (!identity(artifact, error))
    return false;
  if (!verifyFunctions(
          artifact, error,
          [&](const PublicFunctionArtifact &function, std::string &function_error) {
            return verifyFunctionContract(function, callbacks.function,
                                           function_error);
          },
          callbacks.order_key, callbacks.function_fingerprint))
    return false;
  if (!verifyNominalsAndInterfaces(
          artifact, error, callbacks.verify_nominal,
          callbacks.valid_entity_reference, callbacks.valid_constraint,
          callbacks.valid_type, callbacks.interface_fingerprint))
    return false;
  if (!verifyAliasesAndWitnesses(
          artifact, error, callbacks.valid_entity_reference,
          callbacks.valid_type, callbacks.alias_fingerprint,
          callbacks.witness_fingerprint, callbacks.valid_constraint))
    return false;
  return verifyValuesAndFingerprint(
      artifact, error, callbacks.valid_constant_shape,
      callbacks.value_fingerprint, callbacks.artifact_fingerprint);
}



void PublicInterfaceCanonicalizeService::callableOwnership(
    CallableOwnershipSummary &summary) {
  summary.canonicalize();
}

void PublicInterfaceCanonicalizeService::condition(
    CallableConditionDescriptor &condition) {
  condition.canonicalize();
}

void PublicInterfaceCanonicalizeService::foreignProtocol(
    ForeignResourceProtocol &protocol) {
  protocol.canonicalize();
}

bool PublicInterfaceVerifyService::artifact(
    const PublicInterfaceArtifact &artifact, std::string &error) {
  return artifact.verifyBody(error);
}

PublicInterfaceId PublicInterfaceRegistryService::artifact(
    PublicInterfaceRegistry &registry, CheckIRId check_ir_id,
    const PublicInterfaceArtifact &artifact, std::string &error) {
  return registry.registerArtifact(check_ir_id, artifact, error);
}

} // namespace chtholly::compiler::internal
