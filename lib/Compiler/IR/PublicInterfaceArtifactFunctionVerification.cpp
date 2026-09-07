#include "PublicInterfaceServices.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace chtholly::compiler::internal {

bool PublicInterfaceArtifactVerificationService::identity(
    const PublicInterfaceArtifact &artifact, std::string &error) {
  if (artifact.packageName().empty() || artifact.moduleName().empty() ||
      artifact.packageName().size() > std::numeric_limits<std::uint32_t>::max() ||
      artifact.moduleName().size() > std::numeric_limits<std::uint32_t>::max() ||
      artifact.functions().size() > std::numeric_limits<std::uint32_t>::max() ||
      artifact.values().size() > std::numeric_limits<std::uint32_t>::max() ||
      !artifact.fingerprint().hasValue()) {
    error = "public interface artifact has an invalid identity";
    return false;
  }
  return true;
}


bool PublicInterfaceArtifactVerificationService::verifyFunctionIdentity(
    const PublicFunctionArtifact &function, FunctionOrderKey &previous_key,
    bool &has_previous_key, const FunctionOrderKeyFn &order_key,
    const FunctionFingerprintFn &fingerprint, std::string &error) {
  const auto key = order_key(function);
  if (has_previous_key && previous_key >= key) {
    error = "public interface artifact has an invalid function binding `" +
            function.name + "`";
    return false;
  }
  previous_key = key;
  has_previous_key = true;
  if (function.entity_fingerprint != fingerprint(function)) {
    error = "public interface artifact has an invalid entity fingerprint";
    return false;
  }
  return true;
}

bool PublicInterfaceArtifactVerificationService::verifyFunctions(
    const PublicInterfaceArtifact &artifact, std::string &error,
    const FunctionVerifyFn &verify_function,
    const FunctionOrderKeyFn &order_key,
    const FunctionFingerprintFn &fingerprint) {
  FunctionOrderKey previous_key;
  bool has_previous_key = false;
  for (const auto &function : artifact.functions()) {
    if (!verify_function(function, error)) {
      return false;
    }
    if (!verifyFunctionIdentity(function, previous_key, has_previous_key,
                                order_key, fingerprint, error))
      return false;
  }
  return true;
}

} // namespace chtholly::compiler::internal
