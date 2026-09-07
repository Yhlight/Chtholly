#include "PublicInterfaceServices.h"

#include "chtholly/Compiler/SharedValueStores.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {
namespace {
bool publicTypesMayOverlap(const PublicType &lhs, const PublicType &rhs) {
  if (lhs.kind == PublicTypeKind::TypeParameter ||
      rhs.kind == PublicTypeKind::TypeParameter ||
      lhs.kind == PublicTypeKind::TypeProjection ||
      rhs.kind == PublicTypeKind::TypeProjection)
    return true;
  if (lhs.kind != rhs.kind)
    return false;
  if (lhs.kind == PublicTypeKind::Nominal &&
      lhs.nominal_entity != rhs.nominal_entity)
    return false;
  if (lhs.arguments.size() != rhs.arguments.size())
    return false;
  if (lhs.arguments.empty())
    return lhs == rhs;
  for (std::size_t index = 0; index < lhs.arguments.size(); ++index)
    if (!publicTypesMayOverlap(lhs.arguments[index], rhs.arguments[index]))
      return false;
  return true;
}

bool interfaceWitnessesMayOverlap(const PublicInterfaceWitnessArtifact &lhs,
                                  const PublicInterfaceWitnessArtifact &rhs) {
  if (lhs.interface_entity != rhs.interface_entity ||
      lhs.interface_arguments.size() != rhs.interface_arguments.size() ||
      !publicTypesMayOverlap(lhs.self_type, rhs.self_type))
    return false;
  for (std::size_t index = 0; index < lhs.interface_arguments.size(); ++index)
    if (!publicTypesMayOverlap(lhs.interface_arguments[index],
                               rhs.interface_arguments[index]))
      return false;
  return true;
}

} // namespace

bool PublicInterfaceRegistryService::registerArtifactClosure(
    PublicInterfaceRegistry &registry,
    std::span<const PublicInterfaceArtifact *const> artifacts,
    std::string &error) {
  auto *values_ = registry.values_;
  auto &entities_ = registry.entities_;
  auto &entity_keys_ = registry.entity_keys_;
  error.clear();
  for (const auto *artifact : artifacts) {
    if (!artifact || !artifact->verify(error))
      return false;
    for (const auto &function : artifact->functions()) {
      const auto key = PublicInterfaceIdentityService::overloadEntityKey(
          function.canonical_package, function.canonical_module,
          function.canonical_name, function.member_owner, function.member_kind,
          function.generic_parameter_count, function.parameters);
      if (const auto found = entity_keys_.find(key);
          found != entity_keys_.end()) {
        const auto &entity = entities_.get(found->second);
        if (values_->identifier(entity.package_name) !=
                function.canonical_package ||
            values_->identifier(entity.module_name) !=
                function.canonical_module ||
            values_->identifier(entity.name) != function.canonical_name ||
            entity.member_owner != function.member_owner ||
            entity.member_kind != function.member_kind ||
            entity.generic_parameter_count !=
                function.generic_parameter_count ||
            entity.parameters != function.parameters ||
            entity.parameter_names != function.parameter_names ||
            entity.default_arguments != function.default_arguments ||
            entity.return_type != function.return_type ||
            entity.error_type != function.error_type ||
            entity.execution_kind != function.execution_kind ||
            entity.coroutine_constructor != function.coroutine_constructor ||
            entity.nominal_constructor != function.nominal_constructor ||
            entity.semantic_contract != function.semantic_contract ||
            entity.intrinsic_role != function.intrinsic_role ||
            entity.ownership_summary != function.ownership_summary ||
            entity.declaration_kind != function.declaration_kind ||
            entity.is_unsafe != function.is_unsafe ||
            entity.is_const != function.is_const ||
            (entity.foreign_abi.hasValue()
                 ? values_->identifier(entity.foreign_abi)
                 : std::string_view{}) != function.foreign_abi ||
            entity.foreign_signature != function.foreign_signature ||
            entity.generic_template != function.generic_template ||
            entity.interop_artifact != function.interop_artifact ||
            entity.constraints != function.constraints ||
            entity.fingerprint != function.entity_fingerprint) {
          error = "public artifact closure has conflicting canonical entities";
          return false;
        }
        continue;
      }
      const auto package =
          values_->internIdentifier(function.canonical_package);
      const auto module = values_->internIdentifier(function.canonical_module);
      const auto name = values_->internIdentifier(function.canonical_name);
      GenericId generic;
      if (function.generic_parameter_count != 0)
        generic =
            values_->generics().addGeneric(CheckIRId::invalid(), module, name,
                                           function.generic_parameter_count);
      const auto id = entities_.add(
          {.kind = PublicEntityKind::Function,
           .package_name = package,
           .module_name = module,
           .name = name,
           .member_owner = function.member_owner,
           .member_kind = function.member_kind,
           .generic_parameter_count = function.generic_parameter_count,
           .parameters = function.parameters,
           .parameter_names = function.parameter_names,
           .default_arguments = function.default_arguments,
           .return_type = function.return_type,
           .error_type = function.error_type,
           .execution_kind = function.execution_kind,
           .coroutine_constructor = function.coroutine_constructor,
           .nominal_constructor = function.nominal_constructor,
           .semantic_contract = function.semantic_contract,
           .intrinsic_role = function.intrinsic_role,
           .ownership_summary = function.ownership_summary,
           .declaration_kind = function.declaration_kind,
           .is_unsafe = function.is_unsafe,
           .is_const = function.is_const,
           .foreign_abi = function.foreign_abi.empty()
                              ? IdentifierId::invalid()
                              : values_->internIdentifier(function.foreign_abi),
           .external_symbol =
               function.external_symbol.empty()
                   ? IdentifierId::invalid()
                   : values_->internIdentifier(function.external_symbol),
           .foreign_signature = function.foreign_signature,
           .interop_artifact = function.interop_artifact,
           .generic = generic,
           .generic_template = function.generic_template,
           .constraints = function.constraints,
           .fingerprint = function.entity_fingerprint});
      entity_keys_.emplace(std::move(key), id);
    }
    for (const auto &nominal : artifact->nominalTypes()) {
      const auto key = PublicInterfaceIdentityService::entityKey(
          nominal.entity.canonical_package, nominal.entity.canonical_module,
          nominal.entity.canonical_name, PublicEntityKind::NominalType);
      if (const auto found = entity_keys_.find(key);
          found != entity_keys_.end()) {
        if (entities_.get(found->second).fingerprint !=
            nominal.definition_fingerprint) {
          error = "public artifact closure has conflicting nominal entities";
          return false;
        }
        continue;
      }
      const auto package =
          values_->internIdentifier(nominal.entity.canonical_package);
      const auto module =
          values_->internIdentifier(nominal.entity.canonical_module);
      const auto name =
          values_->internIdentifier(nominal.entity.canonical_name);
      GenericId generic;
      if (nominal.generic_parameter_count != 0)
        generic =
            values_->generics().addGeneric(CheckIRId::invalid(), module, name,
                                           nominal.generic_parameter_count);
      const auto entity = entities_.add(
          {.kind = PublicEntityKind::NominalType,
           .package_name = package,
           .module_name = module,
           .name = name,
           .generic_parameter_count = nominal.generic_parameter_count,
           .generic = generic,
           .fingerprint = nominal.definition_fingerprint,
           .nominal_is_exported = nominal.is_exported,
           .nominal_fields = nominal.fields,
           .nominal_variants = nominal.variants,
           .nominal_is_value_enum = nominal.is_value_enum,
           .nominal_value_repr_pattern = nominal.value_repr_pattern,
           .nominal_object_repr_pattern = nominal.object_repr_pattern,
           .nominal_representation_policy = nominal.representation_policy,
           .nominal_kind = nominal.kind,
           .nominal_foreign_representation = nominal.foreign_representation,
           .nominal_foreign_invalid_state = nominal.foreign_invalid_state,
           .nominal_foreign_invalid_integer = nominal.foreign_invalid_integer,
           .nominal_foreign_handle_type = nominal.foreign_handle_type,
           .nominal_foreign_completion_handle_type =
               nominal.foreign_completion_handle_type,
           .nominal_foreign_callback_type = nominal.foreign_callback_type,
           .nominal_foreign_waker_type = nominal.foreign_waker_type,
           .nominal_foreign_resource_protocol =
               nominal.foreign_resource_protocol,
           .nominal_foreign_resource_operations =
               nominal.foreign_resource_operations});
      entity_keys_.emplace(key, entity);
    }
    for (const auto &declaration : artifact->interfaceDeclarations()) {
      const auto key = PublicInterfaceIdentityService::entityKey(declaration.entity.canonical_package,
                                 declaration.entity.canonical_module,
                                 declaration.entity.canonical_name,
                                 PublicEntityKind::Interface);
      if (const auto found = entity_keys_.find(key);
          found != entity_keys_.end()) {
        const auto &entity = entities_.get(found->second);
        if (entity.kind != PublicEntityKind::Interface ||
            entity.interface_declaration != declaration ||
            entity.fingerprint != declaration.entity.expected_fingerprint) {
          error = "public artifact closure has conflicting interface entities";
          return false;
        }
        continue;
      }
      const auto module =
          values_->internIdentifier(declaration.entity.canonical_module);
      const auto name =
          values_->internIdentifier(declaration.entity.canonical_name);
      const auto generic =
          values_->generics().addGeneric(CheckIRId::invalid(), module, name,
                                         declaration.generic_parameter_count);
      const auto entity = entities_.add(
          {.kind = PublicEntityKind::Interface,
           .package_name =
               values_->internIdentifier(declaration.entity.canonical_package),
           .module_name = module,
           .name = name,
           .generic_parameter_count = declaration.generic_parameter_count,
           .generic = generic,
           .fingerprint = declaration.entity.expected_fingerprint,
           .interface_declaration = declaration});
      entity_keys_.emplace(key, entity);
    }
    for (const auto &alias : artifact->typeAliases()) {
      const auto key = PublicInterfaceIdentityService::entityKey(
          alias.entity.canonical_package, alias.entity.canonical_module,
          alias.entity.canonical_name, PublicEntityKind::TypeAlias);
      if (const auto found = entity_keys_.find(key);
          found != entity_keys_.end()) {
        const auto &entity = entities_.get(found->second);
        if (entity.kind != PublicEntityKind::TypeAlias ||
            entity.type_alias != alias ||
            entity.fingerprint != alias.entity.expected_fingerprint) {
          error = "public artifact closure has conflicting type alias entities";
          return false;
        }
        continue;
      }
      const auto module =
          values_->internIdentifier(alias.entity.canonical_module);
      const auto name = values_->internIdentifier(alias.entity.canonical_name);
      GenericId generic;
      if (alias.generic_parameter_count != 0)
        generic = values_->generics().addGeneric(
            CheckIRId::invalid(), module, name, alias.generic_parameter_count);
      const auto entity = entities_.add(
          {.kind = PublicEntityKind::TypeAlias,
           .package_name =
               values_->internIdentifier(alias.entity.canonical_package),
           .module_name = module,
           .name = name,
           .generic_parameter_count = alias.generic_parameter_count,
           .generic = generic,
           .fingerprint = alias.entity.expected_fingerprint,
           .type_alias = alias});
      entity_keys_.emplace(key, entity);
    }
  }
  for (const auto *artifact : artifacts) {
    const auto package = values_->internIdentifier(artifact->packageName());
    const auto module = values_->internIdentifier(artifact->moduleName());
    if (registry.findByModule(package, module).hasValue())
      continue;
    if (!registry.registerExternalArtifact(*artifact, error).hasValue())
      return false;
  }
  return registry.verify(error);
}


} // namespace chtholly::compiler::internal
