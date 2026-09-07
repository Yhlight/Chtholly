#include "PublicInterfaceServices.h"

#include "chtholly/Compiler/SharedValueStores.h"

#include <ranges>

namespace chtholly::compiler::internal {

bool PublicInterfaceRegistryConstructionService::registerInterfaceDeclarations(
    PublicInterfaceRegistry &registry, CheckIRId check_ir_id,
    std::string_view package, std::string_view module,
    std::span<const PublicInterfaceDeclarationArtifact> declarations,
    std::string &error) {
  for (const auto &declaration : declarations) {
    if (declaration.entity.kind != PublicEntityKind::Interface ||
        declaration.entity.canonical_package.empty() ||
        declaration.entity.canonical_module.empty() ||
        declaration.entity.canonical_name.empty() ||
        !declaration.entity.expected_fingerprint.hasValue() ||
        declaration.explicit_parameter_count + 1U >
            declaration.generic_parameter_count) {
      error = "cannot register an invalid public interface declaration";
      return false;
    }
    const auto key = PublicInterfaceIdentityService::entityKey(
        declaration.entity.canonical_package,
        declaration.entity.canonical_module, declaration.entity.canonical_name,
        PublicEntityKind::Interface);
    if (const auto found = registry.entity_keys_.find(key);
        found != registry.entity_keys_.end()) {
      const auto &entity = registry.entities_.get(found->second);
      if (entity.kind != PublicEntityKind::Interface ||
          entity.interface_declaration != declaration ||
          entity.fingerprint != declaration.entity.expected_fingerprint) {
        error = "public interface identity has a conflicting definition";
        return false;
      }
      continue;
    }
    const auto canonical_module = registry.values_->internIdentifier(
        declaration.entity.canonical_module);
    const auto name = registry.values_->internIdentifier(
        declaration.entity.canonical_name);
    const auto generic = registry.values_->generics().addGeneric(
        declaration.entity.canonical_package == package &&
                declaration.entity.canonical_module == module
            ? check_ir_id
            : CheckIRId::invalid(),
        canonical_module, name, declaration.generic_parameter_count);
    const auto entity = registry.entities_.add(
        {.kind = PublicEntityKind::Interface,
         .package_name = registry.values_->internIdentifier(
             declaration.entity.canonical_package),
         .module_name = canonical_module,
         .name = name,
         .generic_parameter_count = declaration.generic_parameter_count,
         .generic = generic,
         .fingerprint = declaration.entity.expected_fingerprint,
         .interface_declaration = declaration});
    registry.entity_keys_.emplace(key, entity);
  }
  return true;
}

bool PublicInterfaceRegistryConstructionService::registerTypeAliases(
    PublicInterfaceRegistry &registry, CheckIRId check_ir_id,
    std::string_view package, std::string_view module,
    std::span<const PublicTypeAliasArtifact> aliases, std::string &error) {
  for (const auto &alias : aliases) {
    if (alias.entity.kind != PublicEntityKind::TypeAlias ||
        alias.entity.canonical_package.empty() ||
        alias.entity.canonical_module.empty() ||
        alias.entity.canonical_name.empty() ||
        !alias.entity.expected_fingerprint.hasValue() ||
        !GenericTemplateValidationService::validType(
            alias.target, alias.generic_parameter_count, true)) {
      error = "cannot register an invalid public type alias";
      return false;
    }
    const auto key = PublicInterfaceIdentityService::entityKey(
        alias.entity.canonical_package, alias.entity.canonical_module,
        alias.entity.canonical_name, PublicEntityKind::TypeAlias);
    if (const auto found = registry.entity_keys_.find(key);
        found != registry.entity_keys_.end()) {
      const auto &entity = registry.entities_.get(found->second);
      if (entity.kind != PublicEntityKind::TypeAlias ||
          entity.type_alias != alias ||
          entity.fingerprint != alias.entity.expected_fingerprint) {
        error = "public type alias identity has a conflicting definition";
        return false;
      }
      continue;
    }
    const auto canonical_module =
        registry.values_->internIdentifier(alias.entity.canonical_module);
    const auto name =
        registry.values_->internIdentifier(alias.entity.canonical_name);
    GenericId generic;
    if (alias.generic_parameter_count != 0)
      generic = registry.values_->generics().addGeneric(
          alias.entity.canonical_package == package &&
                  alias.entity.canonical_module == module
              ? check_ir_id
              : CheckIRId::invalid(),
          canonical_module, name, alias.generic_parameter_count);
    const auto entity = registry.entities_.add(
        {.kind = PublicEntityKind::TypeAlias,
         .package_name = registry.values_->internIdentifier(
             alias.entity.canonical_package),
         .module_name = canonical_module,
         .name = name,
         .generic_parameter_count = alias.generic_parameter_count,
         .generic = generic,
         .fingerprint = alias.entity.expected_fingerprint,
         .type_alias = alias});
    registry.entity_keys_.emplace(key, entity);
  }
  return true;
}

bool PublicInterfaceRegistryConstructionService::registerInterfaceWitnesses(
    PublicInterfaceRegistry &registry,
    std::span<const PublicInterfaceWitnessArtifact> witnesses,
    const std::function<bool(const PublicType &, std::uint32_t, bool)>
        &valid_type,
    const std::function<bool(const PublicInterfaceConstraintArtifact &,
                             std::uint32_t)> &valid_constraint,
    const std::function<StableFingerprint(
        const PublicInterfaceWitnessArtifact &)> &fingerprint,
    const std::function<bool(const PublicInterfaceWitnessArtifact &,
                             const PublicInterfaceWitnessArtifact &)>
        &may_overlap,
    std::string &error) {
  const auto same_witness_key = [](const auto &lhs, const auto &rhs) {
    return lhs.interface_entity == rhs.interface_entity &&
           lhs.self_type == rhs.self_type &&
           lhs.interface_arguments == rhs.interface_arguments;
  };
  for (std::size_t index = 0; index < witnesses.size(); ++index) {
    const auto &witness = witnesses[index];
    if (witness.interface_entity.kind != PublicEntityKind::Interface ||
        !witness.interface_entity.expected_fingerprint.hasValue() ||
        !witness.fingerprint.hasValue() ||
        !valid_type(witness.self_type, witness.generic_parameter_count, false) ||
        std::ranges::any_of(witness.interface_arguments,
                            [&](const auto &type) {
                              return !valid_type(
                                  type, witness.generic_parameter_count, false);
                            }) ||
        std::ranges::any_of(witness.constraints,
                            [&](const auto &constraint) {
                              return !valid_constraint(
                                  constraint, witness.generic_parameter_count);
                            }) ||
        witness.fingerprint != fingerprint(witness)) {
      error = "cannot register an invalid public interface witness";
      return false;
    }
    const auto interface_key = PublicInterfaceIdentityService::entityKey(
        witness.interface_entity.canonical_package,
        witness.interface_entity.canonical_module,
        witness.interface_entity.canonical_name, PublicEntityKind::Interface);
    const auto interface_found = registry.entity_keys_.find(interface_key);
    if (interface_found == registry.entity_keys_.end()) {
      error = "public interface witness references an unknown interface";
      return false;
    }
    const auto &interface_entity = registry.entities_.get(interface_found->second);
    if (interface_entity.fingerprint !=
            witness.interface_entity.expected_fingerprint ||
        !interface_entity.interface_declaration) {
      error = "public interface witness references a mismatched interface";
      return false;
    }
    const auto &requirements = interface_entity.interface_declaration->requirements;
    if (witness.interface_arguments.size() !=
            interface_entity.interface_declaration->explicit_parameter_count ||
        witness.entries.size() != requirements.size()) {
      error = "public interface witness has an incomplete interface shape";
      return false;
    }
    std::vector<bool> populated(requirements.size());
    for (const auto &entry : witness.entries) {
      if (entry.requirement >= requirements.size() ||
          populated[entry.requirement]) {
        error = "public interface witness has an invalid requirement slot";
        return false;
      }
      const auto expects_function = requirements[entry.requirement].kind ==
                                    PublicInterfaceRequirementKind::Function;
      if (expects_function != entry.function.expected_fingerprint.hasValue()) {
        error = "public interface witness has a mismatched requirement entry";
        return false;
      }
      populated[entry.requirement] = true;
    }
    for (std::size_t previous = 0; previous < index; ++previous)
      if (same_witness_key(witness, witnesses[previous]) ||
          ((witness.generic_parameter_count != 0 ||
            witnesses[previous].generic_parameter_count != 0) &&
           may_overlap(witness, witnesses[previous]))) {
        error = "public interface contains duplicate conformance witnesses";
        return false;
      }
    for (const auto &registered : registry.interfaces_)
      for (const auto &other : registered->interface_witness_artifacts_)
        if ((same_witness_key(witness, other) ||
             ((witness.generic_parameter_count != 0 ||
               other.generic_parameter_count != 0) &&
              may_overlap(witness, other))) &&
            witness != other) {
          error = "package closure contains conflicting conformance witnesses";
          return false;
        }
  }
  return true;
}

} // namespace chtholly::compiler::internal
