#include "PublicInterfaceServices.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace chtholly::compiler::internal {

bool PublicInterfaceArtifactVerificationService::verifyNominalsAndInterfaces(
    const PublicInterfaceArtifact &artifact, std::string &error,
    const NominalVerifyFn &verify_nominal,
    const EntityReferenceFn &valid_entity_reference,
    const ConstraintVerifyFn &valid_constraint, const TypeVerifyFn &valid_type,
    const InterfaceFingerprintFn &interface_fingerprint) {
  std::tuple<std::string_view, std::string_view, std::string_view>
      previous_nominal;
  bool has_previous_nominal = false;
  for (const auto &nominal : artifact.nominalTypes()) {
    const auto identity = std::tuple(
        std::string_view(nominal.entity.canonical_package),
        std::string_view(nominal.entity.canonical_module),
        std::string_view(nominal.entity.canonical_name));
    if (!verify_nominal(nominal, error) ||
        (has_previous_nominal && previous_nominal >= identity)) {
      error = "public interface artifact has an invalid nominal binding";
      return false;
    }
    previous_nominal = identity;
    has_previous_nominal = true;
  }
  std::string_view previous_interface;
  for (const auto &declaration : artifact.interfaceDeclarations()) {
    if (!valid_entity_reference(declaration.entity, PublicEntityKind::Interface) ||
        declaration.explicit_parameter_count + 1U >
            declaration.generic_parameter_count ||
        declaration.entity.expected_fingerprint !=
            interface_fingerprint(declaration) ||
        (!previous_interface.empty() &&
         previous_interface >= declaration.entity.canonical_name) ||
        std::ranges::any_of(declaration.constraints, [&](const auto &constraint) {
          return !valid_constraint(constraint, declaration.generic_parameter_count);
        })) {
      error = "public interface artifact has an invalid interface declaration";
      return false;
    }
    for (const auto &requirement : declaration.requirements) {
      const auto valid_function =
          requirement.kind == PublicInterfaceRequirementKind::Function &&
          valid_entity_reference(requirement.function,
                                 PublicEntityKind::Function) &&
          requirement.associated_type.kind == PublicTypeKind::Count &&
          requirement.binding_index == core::AnyId::InvalidIndex;
      const auto valid_alias =
          requirement.kind == PublicInterfaceRequirementKind::AssociatedAlias &&
          !requirement.function.expected_fingerprint.hasValue() &&
          requirement.binding_index < declaration.generic_parameter_count &&
          (!requirement.has_default ||
           valid_type(requirement.associated_type,
                      declaration.generic_parameter_count, true));
      if (requirement.name.empty() || (!valid_function && !valid_alias)) {
        error = "public interface declaration has an invalid requirement";
        return false;
      }
    }
    previous_interface = declaration.entity.canonical_name;
  }
  return true;
}

bool PublicInterfaceArtifactVerificationService::verifyAliasesAndWitnesses(
    const PublicInterfaceArtifact &artifact, std::string &error,
    const EntityReferenceFn &valid_entity_reference,
    const TypeVerifyFn &valid_type,
    const std::function<StableFingerprint(const PublicTypeAliasArtifact &)>
        &alias_fingerprint,
    const std::function<StableFingerprint(
        const PublicInterfaceWitnessArtifact &)> &witness_fingerprint,
    const std::function<bool(const PublicInterfaceConstraintArtifact &,
                             std::uint32_t)> &constraint_verify) {
  std::string_view previous_alias;
  for (const auto &alias : artifact.typeAliases()) {
    if (!valid_entity_reference(alias.entity, PublicEntityKind::TypeAlias) ||
        alias.entity.expected_fingerprint != alias_fingerprint(alias) ||
        !valid_type(alias.target, alias.generic_parameter_count, true) ||
        (!previous_alias.empty() &&
         previous_alias >= alias.entity.canonical_name) ||
        std::ranges::any_of(alias.constraints, [&](const auto &constraint) {
          return !constraint_verify(constraint, alias.generic_parameter_count);
        })) {
      error = "public interface artifact has an invalid type alias";
      return false;
    }
    previous_alias = alias.entity.canonical_name;
  }
  std::string previous_witness;
  for (const auto &witness : artifact.interfaceWitnesses()) {
    const auto key = witness.fingerprint.hex();
    if (!valid_entity_reference(witness.interface_entity,
                                PublicEntityKind::Interface) ||
        !valid_type(witness.self_type, witness.generic_parameter_count, false) ||
        std::ranges::any_of(witness.interface_arguments,
                            [&](const auto &argument) {
                              return !valid_type(argument,
                                                  witness.generic_parameter_count,
                                                  false);
                            }) ||
        std::ranges::any_of(witness.constraints, [&](const auto &constraint) {
          return !constraint_verify(constraint, witness.generic_parameter_count);
        }) ||
        witness.fingerprint != witness_fingerprint(witness) ||
        (!previous_witness.empty() && previous_witness >= key)) {
      error = "public interface artifact has an invalid interface witness";
      return false;
    }
    for (const auto &entry : witness.entries) {
      const auto has_function = entry.function.expected_fingerprint.hasValue();
      if ((has_function &&
           !valid_entity_reference(entry.function, PublicEntityKind::Function)) ||
          (!has_function &&
           !valid_type(entry.associated_type, witness.generic_parameter_count,
                       true))) {
        error = "public interface witness has an invalid entry";
        return false;
      }
    }
    previous_witness = key;
  }
  return true;
}


} // namespace chtholly::compiler::internal
