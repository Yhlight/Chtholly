#include "PublicInterfaceServices.h"

#include "chtholly/Compiler/SharedValueStores.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool PublicInterfaceRegistryService::verify(
    const PublicInterfaceRegistry &registry, std::string &error,
    const ValidationCallbacks &callbacks) {
  error.clear();
  if (registry.modules_.size() != registry.interfaces_.size() ||
      registry.entity_keys_.size() != registry.entities_.size()) {
    error = "public interface registry indexes are inconsistent";
    return false;
  }

  NominalResolver resolve_foreign_nominal;
  resolve_foreign_nominal =
      [&](const PublicType &type) -> std::optional<PublicType> {
    if (type.kind != PublicTypeKind::Nominal)
      return std::nullopt;
    const auto id = registry.findEntity(
        type.nominal_entity.canonical_package,
        type.nominal_entity.canonical_module,
        type.nominal_entity.canonical_name, PublicEntityKind::NominalType);
    const auto *entity = registry.tryGetEntity(id);
    if (!entity || entity->kind != PublicEntityKind::NominalType ||
        entity->fingerprint != type.nominal_entity.expected_fingerprint)
      return std::nullopt;
    if (entity->nominal_kind == NominalKind::ForeignHandle)
      return entity->nominal_foreign_representation;
    if (entity->nominal_kind == NominalKind::ForeignResource &&
        entity->nominal_foreign_handle_type)
      return resolve_foreign_nominal(*entity->nominal_foreign_handle_type);
    return std::nullopt;
  };

  for (std::uint32_t index = 0; index < registry.entities_.size(); ++index) {
    const auto id = PublicEntityId(index);
    const auto &entity = registry.entities_.get(id);
    if (entity.kind >= PublicEntityKind::Count ||
        !entity.package_name.hasValue() || !entity.module_name.hasValue() ||
        !entity.name.hasValue() ||
        entity.package_name.index >= registry.values_->identifierCount() ||
        entity.module_name.index >= registry.values_->identifierCount() ||
        entity.name.index >= registry.values_->identifierCount() ||
        (entity.generic_parameter_count == 0) != !entity.generic.hasValue() ||
        (entity.generic.hasValue() &&
         (entity.generic.index >=
              registry.values_->generics().genericCount() ||
          registry.values_->generics().generic(entity.generic).binding_count !=
              entity.generic_parameter_count ||
          registry.values_->generics().generic(entity.generic).module_name !=
              entity.module_name ||
          (entity.kind != PublicEntityKind::Function &&
           registry.values_->generics().generic(entity.generic).name !=
               entity.name))) ||
        (entity.kind == PublicEntityKind::Function &&
         (entity.generic_template.has_value() !=
          ((entity.generic_parameter_count != 0 || entity.is_const) &&
           entity.declaration_kind ==
               PublicCallableDeclarationKind::Definition))) ||
        (entity.kind == PublicEntityKind::Function &&
         entity.generic_template &&
         (!entity.generic_template->verify(error) ||
          !callbacks.template_matches_signature(
              *entity.generic_template, entity.generic_parameter_count,
              entity.parameters, entity.return_type))) ||
        (entity.kind == PublicEntityKind::Function &&
         !callbacks.valid_type(entity.return_type,
                               entity.generic_parameter_count, true)) ||
        (entity.kind == PublicEntityKind::Function &&
         (entity.execution_kind >= PublicFunctionExecutionKind::Count ||
          entity.intrinsic_role >= CompilerIntrinsicRole::Count ||
          (entity.intrinsic_role != CompilerIntrinsicRole::None &&
           (registry.values_->identifier(entity.package_name) != "std" ||
            (registry.values_->identifier(entity.module_name) != "std" &&
             !registry.values_->identifier(entity.module_name)
                  .starts_with("std::")))) ||
          !callbacks.valid_coroutine_constructor(
              entity.execution_kind, entity.coroutine_constructor) ||
          !callbacks.valid_nominal_constructor(
              entity.semantic_contract, entity.return_type,
              entity.nominal_constructor) ||
          (entity.execution_kind == PublicFunctionExecutionKind::Immediate &&
           entity.error_type.has_value()) ||
          (entity.error_type &&
           !callbacks.valid_type(*entity.error_type,
                                 entity.generic_parameter_count, true)))) ||
        (entity.kind == PublicEntityKind::Function &&
         !callbacks.valid_reference_provenance(
             entity.return_type, entity.parameters.size(), true)) ||
        (entity.kind == PublicEntityKind::Function && entity.error_type &&
         !callbacks.valid_reference_provenance(
             *entity.error_type, entity.parameters.size(), true)) ||
        (entity.kind == PublicEntityKind::Function &&
         (!entity.semantic_contract.verify(entity.generic_parameter_count,
                                           error) ||
          !callbacks.semantic_contract_matches_signature(
              entity.parameters, entity.return_type,
              entity.semantic_contract) ||
          !callbacks.semantic_contract_matches_effects(
              entity.semantic_contract, entity.ownership_summary))) ||
        (entity.kind == PublicEntityKind::Function &&
         !entity.ownership_summary.verify(
             static_cast<std::uint32_t>(entity.parameters.size()), error)) ||
        (entity.kind == PublicEntityKind::Function &&
         entity.semantic_contract.domain ==
             CallableSemanticDomain::Ordinary &&
         !registry.verifyOwnershipSummaryTypes(
             entity.parameters, entity.return_type, entity.ownership_summary,
             error)) ||
        (entity.kind == PublicEntityKind::Function &&
         !callbacks.ownership_matches_signature(
             entity.parameters, entity.return_type,
             entity.ownership_summary)) ||
        (entity.kind == PublicEntityKind::Function &&
         (entity.declaration_kind == PublicCallableDeclarationKind::Foreign
              ? (entity.execution_kind !=
                     PublicFunctionExecutionKind::Immediate ||
                 !entity.is_unsafe || !entity.foreign_abi.hasValue() ||
                 !entity.external_symbol.hasValue() ||
                 registry.values_->identifier(entity.foreign_abi) != "C" ||
                 !callbacks.foreign_signature_matches(
                     entity.parameters, entity.return_type,
                     entity.foreign_signature, resolve_foreign_nominal,
                     entity.interop_artifact.has_value()))
              : ((entity.is_unsafe &&
                  entity.intrinsic_role == CompilerIntrinsicRole::None) ||
                 entity.foreign_abi.hasValue() ||
                 entity.external_symbol.hasValue() ||
                 entity.foreign_signature.has_value()))) ||
        (entity.kind == PublicEntityKind::Function && entity.is_const &&
         (entity.declaration_kind !=
              PublicCallableDeclarationKind::Definition ||
          entity.execution_kind != PublicFunctionExecutionKind::Immediate ||
          entity.is_unsafe)) ||
        (entity.kind == PublicEntityKind::Function &&
         (entity.parameter_names.size() != entity.parameters.size() ||
          entity.default_arguments.size() != entity.parameters.size() ||
          std::ranges::any_of(entity.parameter_names,
                              [](const auto &name) { return name.empty(); }) ||
          ([&] {
            std::unordered_set<std::string_view> names;
            return std::ranges::any_of(
                entity.parameter_names,
                [&](const auto &name) { return !names.insert(name).second; });
          }()) ||
          ([&] {
            bool saw_default = false;
            for (std::size_t parameter = 0;
                 parameter < entity.default_arguments.size(); ++parameter) {
              const auto &value = entity.default_arguments[parameter];
              if (!value) {
                if (saw_default)
                  return true;
                continue;
              }
              saw_default = true;
              if (value->type != entity.parameters[parameter])
                return true;
            }
            return false;
          }()))) ||
        (entity.kind == PublicEntityKind::Function &&
         std::ranges::any_of(entity.parameters, [&](PublicType type) {
           return !callbacks.valid_type(type, entity.generic_parameter_count,
                                        false);
         })) ||
        (entity.kind == PublicEntityKind::Function &&
         std::ranges::any_of(entity.parameters, [&](const PublicType &type) {
           return !callbacks.valid_reference_provenance(
               type, entity.parameters.size(), true);
         })) ||
        (entity.kind == PublicEntityKind::Function &&
         entity.interop_artifact.has_value() &&
         !callbacks.valid_foreign_operation(
             *entity.interop_artifact, entity.parameters,
             entity.return_type)) ||
        (entity.kind == PublicEntityKind::Function &&
         std::ranges::any_of(entity.constraints, [&](const auto &constraint) {
           return !callbacks.valid_constraint(
               constraint, entity.generic_parameter_count);
         })) ||
        (entity.kind == PublicEntityKind::Interface) !=
            entity.interface_declaration.has_value() ||
        (entity.kind == PublicEntityKind::TypeAlias) !=
            entity.type_alias.has_value()) {
      const auto detail = std::move(error);
      error =
          "public interface registry contains an invalid " +
          std::string(
              entity.kind == PublicEntityKind::Function      ? "function `"
              : entity.kind == PublicEntityKind::NominalType ? "nominal type `"
              : entity.kind == PublicEntityKind::Interface   ? "interface `"
                                                             : "type alias `") +
          std::string(registry.values_->identifier(entity.name)) + "` (" +
          std::string(registry.values_->identifier(entity.package_name)) +
          "::" +
          std::string(registry.values_->identifier(entity.module_name)) +
          ")" + (detail.empty() ? std::string{} : ": " + detail);
      return false;
    }

    const auto package = registry.values_->identifier(entity.package_name);
    const auto module = registry.values_->identifier(entity.module_name);
    const auto name = registry.values_->identifier(entity.name);
    const auto key = callbacks.entity_key(entity, package, module, name);
    const auto found = registry.entity_keys_.find(key);
    const auto expected =
        callbacks.entity_fingerprint(entity, package, module, name);
    if (found == registry.entity_keys_.end() || found->second != id ||
        entity.fingerprint != expected) {
      error = "public interface registry has an inconsistent entity identity";
      return false;
    }
  }

  for (std::uint32_t index = 0; index < registry.interfaces_.size(); ++index) {
    const auto id = PublicInterfaceId(index);
    const auto *interface = registry.tryGet(id);
    if (!interface || interface->interfaceId() != id ||
        registry.findByModule(interface->packageName(),
                              interface->moduleName()) != id ||
        (interface->checkIRId().hasValue() &&
         registry.findByCheckIR(interface->checkIRId()) != id) ||
        !interface->verify(error))
      return false;
    for (std::uint32_t binding = 0; binding < interface->bindingCount();
         ++binding) {
      const auto &function = interface->function(PublicBindingId(binding));
      const auto *entity = registry.tryGetEntity(function.canonical_entity);
      const auto parameters =
          interface->parameterTypes(function.parameters);
      if (!entity ||
          function.generic_parameter_count != entity->generic_parameter_count ||
          function.return_type != entity->return_type ||
          parameters.size() != entity->parameters.size() ||
          !std::equal(parameters.begin(), parameters.end(),
                      entity->parameters.begin())) {
        error = "public interface function binding `" +
                std::string(registry.values_->identifier(function.name)) +
                "` disagrees with its canonical entity";
        return false;
      }
    }
    for (std::uint32_t binding = 0;
         binding < interface->nominalTypeBindingCount(); ++binding) {
      const auto &nominal =
          interface->nominalType(PublicBindingId(binding));
      const auto *entity = registry.tryGetEntity(nominal.canonical_entity);
      if (!entity || entity->kind != PublicEntityKind::NominalType ||
          nominal.name != entity->name ||
          nominal.generic_parameter_count != entity->generic_parameter_count) {
        error = "public nominal binding disagrees with its entity";
        return false;
      }
    }
    if (interface->fingerprint() !=
        callbacks.interface_fingerprint(*interface)) {
      error = "public interface registry has an inconsistent fingerprint";
      return false;
    }
  }
  return true;
}


} // namespace chtholly::compiler::internal

