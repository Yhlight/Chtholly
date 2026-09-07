#include "PublicInterfaceServices.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace chtholly::compiler::internal {

bool PublicInterfaceArtifactVerificationService::verifyValuesAndFingerprint(
    const PublicInterfaceArtifact &artifact, std::string &error,
    const ConstantShapeFn &valid_constant_shape,
    const ValueFingerprintFn &value_fingerprint,
    const ArtifactFingerprintFn &artifact_fingerprint) {
  std::string_view previous_value;
  bool has_previous_value = false;
  for (const auto &value : artifact.values()) {
    if (value.kind >= PublicValueKind::Count || value.name.empty() ||
        value.canonical_package.empty() || value.canonical_module.empty() ||
        value.canonical_name.empty() || value.value.type != value.type ||
        !valid_constant_shape(value.value, artifact.nominalTypes()) ||
        !value.entity_fingerprint.hasValue() ||
        value.entity_fingerprint != value_fingerprint(value) ||
        (has_previous_value && previous_value >= value.name)) {
      error = "public interface artifact has an invalid value binding";
      return false;
    }
    previous_value = value.name;
    has_previous_value = true;
  }
  for (const auto &nominal : artifact.nominalTypes()) {
    if (nominal.kind != NominalKind::ForeignResource)
      continue;
    for (const auto &operation : nominal.foreign_resource_operations) {
      const auto target = std::ranges::find_if(
          artifact.functions(), [&](const PublicFunctionArtifact &function) {
            return function.canonical_package ==
                       operation.target.canonical_package &&
                   function.canonical_module ==
                       operation.target.canonical_module &&
                   function.canonical_name == operation.target.canonical_name;
          });
      if (target == artifact.functions().end() ||
          target->entity_fingerprint != operation.target.expected_fingerprint ||
          target->declaration_kind != PublicCallableDeclarationKind::Foreign) {
        error = "foreign resource operation target is missing from its "
                "published ABI closure";
        return false;
      }
    }
  }
  if (artifact.fingerprint() != artifact_fingerprint(
          artifact.packageName(), artifact.moduleName(), artifact.functions(),
          artifact.nominalTypes(), artifact.values(),
          artifact.interfaceDeclarations(), artifact.typeAliases(),
          artifact.interfaceWitnesses())) {
    error = "public interface artifact has an invalid interface fingerprint";
    return false;
  }
  return true;
}

} // namespace chtholly::compiler::internal
