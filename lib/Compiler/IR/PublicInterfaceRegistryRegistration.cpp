#include "chtholly/Compiler/PublicInterface.h"

#include "PublicInterfaceConstructionInternal.h"
#include "PublicInterfaceEncodingInternal.h"
#include "PublicInterfaceServices.h"

#include "chtholly/Compiler/SharedValueStores.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler {

namespace {

using internal::appendType;
using internal::appendU32;
using internal::canonicalDefaultArguments;
using internal::canonicalParameterNames;
using internal::ForeignNominalResolver;
using internal::foreignSignatureMatches;
using internal::entityFingerprint;
using internal::interfaceFingerprint;
using internal::interfaceWitnessFingerprint;
using internal::interfaceWitnessesMayOverlap;
using internal::memberSignatureMatchesOwner;
using internal::ownershipMatchesSignature;
using internal::sameSignature;
using internal::semanticContractMatchesEffects;
using internal::semanticContractMatchesSignature;
using internal::templateMatchesSignature;
using internal::validOwnershipSummaryTypes;
using internal::validCallbackOwnershipTypes;
using internal::validCoroutineConstructorABI;
using internal::validNominalConstructorABI;
using internal::validReturnLoanTypes;
using internal::valueFingerprint;

std::string moduleKey(std::string_view package, std::string_view module) {
  return internal::PublicInterfaceIdentityService::moduleKey(package, module);
}
std::string entityKey(std::string_view package, std::string_view module,
                      std::string_view name,
                      PublicEntityKind kind = PublicEntityKind::Function) {
  return internal::PublicInterfaceIdentityService::entityKey(package, module,
                                                              name, kind);
}
std::string overloadEntityKey(
    std::string_view package, std::string_view module, std::string_view name,
    const std::optional<PublicEntityReferenceArtifact> &member_owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::uint32_t generic_parameter_count,
    std::span<const PublicType> parameters) {
  return internal::PublicInterfaceIdentityService::overloadEntityKey(
      package, module, name, member_owner, member_kind,
      generic_parameter_count, parameters);
}
std::string memberFunctionBindingKey(
    const PublicEntityReferenceArtifact &owner, std::string_view name) {
  return internal::PublicInterfaceIdentityService::memberFunctionBindingKey(
      owner, name);
}
bool validPublicType(const PublicType &type, std::uint32_t generic_count,
                     bool allow_void = false) {
  return internal::PublicInterfaceTypeValidationService::validPublicType(
      type, generic_count, allow_void);
}
bool validEntityReference(const PublicEntityReferenceArtifact &entity,
                          PublicEntityKind expected) {
  return internal::PublicInterfaceTypeValidationService::validEntityReference(
      entity, expected);
}
bool validConstantShape(const PublicConstantValue &value,
                        std::span<const PublicNominalTypeArtifact> nominals) {
  return internal::PublicInterfaceConstantValidationService::validShape(
      value, nominals);
}
bool validInterfaceConstraint(
    const PublicInterfaceConstraintArtifact &constraint,
    std::uint32_t generic_count) {
  return internal::PublicInterfaceTypeValidationService::validInterfaceConstraint(
      constraint, generic_count);
}
bool validForeignOperation(const interop::ForeignOperationArtifact &operation,
                           std::span<const PublicType> parameters,
                           PublicType result) {
  return internal::PublicInterfaceForeignOperationService::valid(
      operation, parameters, result);
}
bool validForeignOperation(const interop::ArtifactReference &reference,
                           std::span<const PublicType>, PublicType) {
  std::string error;
  return reference.verify(error);
}

} // namespace

PublicInterfaceRegistry::PublicInterfaceRegistry(SharedValueStores &values)
    : values_(&values) {}
PublicInterfaceRegistry::~PublicInterfaceRegistry() = default;

IdentifierId PublicInterfaceRegistry::internIdentifier(std::string_view value) {
  return values_->internIdentifier(value);
}

PublicInterfaceId PublicInterfaceRegistry::registerInterface(
    CheckIRId check_ir_id, IdentifierId package_name, IdentifierId module_name,
    std::span<const PublicFunctionBindingSpec> functions, std::string &error,
    std::span<const PublicNominalTypeArtifact> nominal_types,
    std::span<const PublicValueArtifact> values,
    std::span<const PublicInterfaceDeclarationArtifact> interfaces,
    std::span<const PublicTypeAliasArtifact> type_aliases,
    std::span<const PublicInterfaceWitnessArtifact> interface_witnesses) {
  error.clear();
  if (!package_name.hasValue() || !module_name.hasValue() ||
      package_name.index >= values_->identifierCount() ||
      module_name.index >= values_->identifierCount()) {
    error = "cannot register a public interface with an invalid identity";
    return PublicInterfaceId::invalid();
  }
  const auto package = values_->identifier(package_name);
  const auto module = values_->identifier(module_name);
  const auto module_key = moduleKey(package, module);
  if (modules_.contains(module_key) ||
      (check_ir_id.hasValue() && check_irs_.contains(check_ir_id.index))) {
    error = "public interface registry contains a duplicate module or check IR";
    return PublicInterfaceId::invalid();
  }
  auto unique_values =
      internal::PublicInterfaceRegistryConstructionService::collectUniqueValues(
          values, nominal_types, error,
          [](const PublicConstantValue &value,
             std::span<const PublicNominalTypeArtifact> nominals) {
            return validConstantShape(value, nominals);
          },
          [](const PublicValueArtifact &value) {
            return valueFingerprint(value);
          });
  if (!unique_values)
    return PublicInterfaceId::invalid();
  std::unordered_set<std::string> exported_value_names;
  for (const auto &value : *unique_values)
    exported_value_names.insert(value.name);

  const auto interface_id =
      PublicInterfaceId(static_cast<std::uint32_t>(interfaces_.size()));
  std::unordered_map<std::string, const PublicFunctionBindingSpec *>
      exported_names;
  ForeignNominalResolver resolve_foreign_nominal;
  resolve_foreign_nominal =
      [&](const PublicType &type) -> std::optional<PublicType> {
    const auto found = std::ranges::find_if(
        nominal_types, [&](const PublicNominalTypeArtifact &nominal) {
          return nominal.entity == type.nominal_entity;
        });
    if (found == nominal_types.end())
      return std::nullopt;
    if (found->kind == NominalKind::ForeignHandle)
      return found->foreign_representation;
    if (found->kind == NominalKind::ForeignResource &&
        found->foreign_handle_type)
      return resolve_foreign_nominal(*found->foreign_handle_type);
    return std::nullopt;
  };
  std::vector<const PublicFunctionBindingSpec *> unique_functions;
  unique_functions.reserve(functions.size());
  for (const auto &function : functions) {
    const auto declaration_valid =
        function.declaration_kind < PublicCallableDeclarationKind::Count &&
        (function.declaration_kind == PublicCallableDeclarationKind::Foreign
             ? function.execution_kind ==
                       PublicFunctionExecutionKind::Immediate &&
                   function.is_unsafe && function.foreign_abi == "C" &&
                   !function.external_symbol.empty() &&
                   foreignSignatureMatches(
                       function.parameters, function.return_type,
                       function.foreign_signature, resolve_foreign_nominal,
                       function.interop_artifact.has_value())
             : (!function.is_unsafe ||
                function.intrinsic_role != CompilerIntrinsicRole::None) &&
                   function.foreign_abi.empty() && !function.foreign_signature);
    const auto complete_declaration_valid =
        declaration_valid &&
        (function.declaration_kind == PublicCallableDeclarationKind::Foreign ||
         function.external_symbol.empty());
    std::string contract_error;
    if (!function.semantic_contract.verify(function.generic_parameter_count,
                                           contract_error)) {
      error = "cannot register callable semantic contract: " + contract_error;
      return PublicInterfaceId::invalid();
    }
    if (!semanticContractMatchesSignature(function.parameters,
                                          function.return_type,
                                          function.semantic_contract)) {
      error = "callable semantic contract disagrees with its signature";
      return PublicInterfaceId::invalid();
    }
    if (!semanticContractMatchesEffects(function.semantic_contract,
                                        function.ownership_summary)) {
      error = "callable semantic contract disagrees with its effect summary " +
              std::to_string(
                  static_cast<unsigned>(function.semantic_contract.role));
      if (!function.ownership_summary.effects.empty()) {
        const auto &region = function.ownership_summary.effects.front().region;
        error +=
            " (parameter " + std::to_string(region.parameter_index) + ", path";
        for (const auto &step : region.path)
          error += " " + std::to_string(static_cast<unsigned>(step.kind)) +
                   ":" + std::to_string(step.index);
        error += ")";
      }
      return PublicInterfaceId::invalid();
    }
    if (!complete_declaration_valid ||
        (function.is_const &&
         (function.declaration_kind !=
              PublicCallableDeclarationKind::Definition ||
          function.execution_kind != PublicFunctionExecutionKind::Immediate ||
          function.is_unsafe)) ||
        !function.name.hasValue() ||
        function.name.index >= values_->identifierCount() ||
        function.intrinsic_role >= CompilerIntrinsicRole::Count ||
        !validPublicType(function.return_type, function.generic_parameter_count,
                         true) ||
        function.execution_kind >= PublicFunctionExecutionKind::Count ||
        !validCoroutineConstructorABI(function.execution_kind,
                                      function.coroutine_constructor) ||
        !validNominalConstructorABI(function.semantic_contract,
                                    function.return_type,
                                    function.nominal_constructor) ||
        (function.execution_kind == PublicFunctionExecutionKind::Immediate &&
         function.error_type.has_value()) ||
        (function.error_type &&
         !validPublicType(*function.error_type,
                          function.generic_parameter_count, true)) ||
        !function.ownership_summary.verify(
            static_cast<std::uint32_t>(function.parameters.size()), error) ||
        !ownershipMatchesSignature(function.parameters, function.return_type,
                                   function.ownership_summary) ||
        values_->identifier(function.name) == "main" ||
        (!function.parameter_names.empty() &&
         function.parameter_names.size() != function.parameters.size()) ||
        (!function.default_arguments.empty() &&
         function.default_arguments.size() != function.parameters.size()) ||
        std::ranges::any_of(function.parameter_names,
                            [](const auto &name) { return name.empty(); }) ||
        ([&] {
          std::unordered_set<std::string_view> names;
          return std::ranges::any_of(
              function.parameter_names,
              [&](const auto &name) { return !names.insert(name).second; });
        }()) ||
        ([&] {
          bool saw_default = false;
          for (std::size_t index = 0; index < function.default_arguments.size();
               ++index) {
            const auto &value = function.default_arguments[index];
            if (!value) {
              if (saw_default)
                return true;
              continue;
            }
            saw_default = true;
            if (value->type != function.parameters[index] ||
                !validConstantShape(*value, nominal_types))
              return true;
          }
          return false;
        }()) ||
        ((function.declaration_kind == PublicCallableDeclarationKind::Foreign ||
          (function.foreign_signature &&
           function.foreign_signature->is_variadic)) &&
         std::ranges::any_of(
             function.default_arguments,
             [](const auto &value) { return value.has_value(); })) ||
        !memberSignatureMatchesOwner(
            function.member_owner, function.member_kind, function.parameters) ||
        std::ranges::any_of(function.parameters,
                            [&](PublicType type) {
                              return !validPublicType(
                                  type, function.generic_parameter_count);
                            }) ||
        (function.generic_template.has_value() !=
         ((function.generic_parameter_count != 0 || function.is_const) &&
          function.declaration_kind ==
              PublicCallableDeclarationKind::Definition)) ||
        (function.generic_template &&
         (!function.generic_template->verify(error) ||
          !templateMatchesSignature(
              *function.generic_template, function.generic_parameter_count,
              function.parameters, function.return_type))) ||
        (function.interop_artifact &&
         (function.declaration_kind != PublicCallableDeclarationKind::Foreign ||
          !validForeignOperation(*function.interop_artifact,
                                 function.parameters, function.return_type)))) {
      error = "cannot register an invalid public function `" +
              std::string(values_->identifier(function.name)) + "`" +
              (error.empty() ? std::string{} : ": " + error);
      return PublicInterfaceId::invalid();
    }
    const auto binding_key =
        function.member_owner
            ? memberFunctionBindingKey(*function.member_owner,
                                       values_->identifier(function.name))
            : std::string("free:") +
                  std::string(values_->identifier(function.name));
    auto overload_binding_key = binding_key;
    appendU32(overload_binding_key, function.generic_parameter_count);
    for (const auto &parameter : function.parameters)
      appendType(overload_binding_key, parameter);
    if (!function.member_owner && exported_value_names.contains(std::string(
                                      values_->identifier(function.name)))) {
      error = "public interface has a value and function with the same name";
      return PublicInterfaceId::invalid();
    }
    if (const auto [position, inserted] =
            exported_names.emplace(overload_binding_key, &function);
        !inserted) {
      const auto &previous = *position->second;
      if (!function.canonical_entity.hasValue() ||
          function.canonical_entity != previous.canonical_entity ||
          function.generic_parameter_count !=
              previous.generic_parameter_count ||
          function.parameters != previous.parameters ||
          function.return_type != previous.return_type ||
          function.error_type != previous.error_type ||
          function.execution_kind != previous.execution_kind ||
          function.coroutine_constructor != previous.coroutine_constructor ||
          function.nominal_constructor != previous.nominal_constructor ||
          function.semantic_contract != previous.semantic_contract ||
          function.intrinsic_role != previous.intrinsic_role ||
          function.ownership_summary != previous.ownership_summary ||
          function.declaration_kind != previous.declaration_kind ||
          function.is_unsafe != previous.is_unsafe ||
          function.is_const != previous.is_const ||
          function.foreign_abi != previous.foreign_abi ||
          function.external_symbol != previous.external_symbol ||
          function.foreign_signature != previous.foreign_signature ||
          function.interop_artifact != previous.interop_artifact ||
          function.constraints != previous.constraints ||
          canonicalParameterNames(function.parameters.size(),
                                  function.parameter_names) !=
              canonicalParameterNames(previous.parameters.size(),
                                      previous.parameter_names) ||
          canonicalDefaultArguments(function.parameters.size(),
                                    function.default_arguments) !=
              canonicalDefaultArguments(previous.parameters.size(),
                                        previous.default_arguments)) {
        error = "public interface has conflicting exported function bindings";
        return PublicInterfaceId::invalid();
      }
      continue;
    }
    unique_functions.push_back(&function);
    if (function.canonical_entity.hasValue()) {
      const auto *entity = tryGetEntity(function.canonical_entity);
      if (!entity || !sameSignature(*entity, function) ||
          (entity->foreign_abi.hasValue()
               ? values_->identifier(entity->foreign_abi)
               : std::string_view{}) != function.foreign_abi ||
          (entity->external_symbol.hasValue()
               ? values_->identifier(entity->external_symbol)
               : std::string_view{}) != function.external_symbol) {
        error = "forwarded public function does not match its canonical entity";
        return PublicInterfaceId::invalid();
      }
    } else {
      const auto name = values_->identifier(function.canonical_name.hasValue()
                                                ? function.canonical_name
                                                : function.name);
      const auto key = overloadEntityKey(
          package, module, name, function.member_owner, function.member_kind,
          function.generic_parameter_count, function.parameters);
      if (const auto found = entity_keys_.find(key);
          found != entity_keys_.end()) {
        auto &entity = entities_.get(found->second);
        if (!sameSignature(entity, function) ||
            (entity.foreign_abi.hasValue()
                 ? values_->identifier(entity.foreign_abi)
                 : std::string_view{}) != function.foreign_abi ||
            (entity.external_symbol.hasValue()
                 ? values_->identifier(entity.external_symbol)
                 : std::string_view{}) != function.external_symbol) {
          if (entity.interop_artifact != function.interop_artifact) {
            error = "public callable registration requires Interop metadata "
                    "to be finalized before binding registration for `" +
                    std::string(package) + "::" + std::string(module) +
                    "::" + std::string(name) + "`";
            return PublicInterfaceId::invalid();
          }
          error = "public entity identity has a conflicting signature for `" +
                  std::string(package) + "::" + std::string(module) +
                  "::" + std::string(name) + "`";
          return PublicInterfaceId::invalid();
        }
      }
    }
  }
  std::unordered_map<std::string, const PublicNominalTypeArtifact *>
      exported_nominal_names;
  for (const auto &nominal : nominal_types) {
    if (!nominal.verify(error)) {
      error = "cannot register invalid public nominal type `" +
              nominal.entity.canonical_name + "`: " + error;
      return PublicInterfaceId::invalid();
    }
    if (!nominal.is_exported)
      continue;
    if (const auto [position, inserted] = exported_nominal_names.emplace(
            nominal.entity.canonical_name, &nominal);
        !inserted) {
      if (*position->second != nominal) {
        error = "public interface has conflicting exported nominal bindings";
        return PublicInterfaceId::invalid();
      }
      continue;
    }
    const auto key = entityKey(
        nominal.entity.canonical_package, nominal.entity.canonical_module,
        nominal.entity.canonical_name, PublicEntityKind::NominalType);
    if (const auto found = entity_keys_.find(key);
        found != entity_keys_.end()) {
      const auto &entity = entities_.get(found->second);
      if (entity.kind != PublicEntityKind::NominalType ||
          entity.generic_parameter_count != nominal.generic_parameter_count ||
          entity.nominal_fields != nominal.fields ||
          entity.nominal_variants != nominal.variants ||
          entity.nominal_is_value_enum != nominal.is_value_enum ||
          entity.nominal_value_repr_pattern != nominal.value_repr_pattern ||
          entity.nominal_object_repr_pattern != nominal.object_repr_pattern ||
          entity.nominal_representation_policy !=
              nominal.representation_policy ||
          entity.nominal_kind != nominal.kind ||
          entity.nominal_foreign_representation !=
              nominal.foreign_representation ||
          entity.nominal_foreign_invalid_state !=
              nominal.foreign_invalid_state ||
          entity.nominal_foreign_invalid_integer !=
              nominal.foreign_invalid_integer ||
          entity.nominal_foreign_handle_type != nominal.foreign_handle_type ||
          entity.nominal_foreign_completion_handle_type !=
              nominal.foreign_completion_handle_type ||
          entity.nominal_foreign_callback_type !=
              nominal.foreign_callback_type ||
          entity.nominal_foreign_waker_type != nominal.foreign_waker_type ||
          entity.nominal_foreign_resource_protocol !=
              nominal.foreign_resource_protocol ||
          entity.nominal_foreign_resource_operations !=
              nominal.foreign_resource_operations ||
          entity.fingerprint != nominal.definition_fingerprint) {
        error = "public nominal identity has a conflicting definition";
        return PublicInterfaceId::invalid();
      }
    }
  }

  const auto resolve_nominal = [&](const PublicEntityReferenceArtifact &ref)
      -> std::optional<internal::OwnershipNominalDefinition> {
    const auto local = std::ranges::find_if(
        nominal_types, [&](const PublicNominalTypeArtifact &nominal) {
          return nominal.entity.kind == ref.kind &&
                 nominal.entity.canonical_package == ref.canonical_package &&
                 nominal.entity.canonical_module == ref.canonical_module &&
                 nominal.entity.canonical_name == ref.canonical_name;
        });
    if (local != nominal_types.end())
      return internal::OwnershipNominalDefinition{local->generic_parameter_count,
                                        local->fields, local->variants,
                                        local->definition_fingerprint};
    const auto id =
        findEntity(ref.canonical_package, ref.canonical_module,
                   ref.canonical_name, PublicEntityKind::NominalType);
    const auto *entity = tryGetEntity(id);
    if (!entity || entity->kind != PublicEntityKind::NominalType)
      return std::nullopt;
    return internal::OwnershipNominalDefinition{
        entity->generic_parameter_count, entity->nominal_fields,
        entity->nominal_variants, entity->fingerprint};
  };
  for (const auto &nominal : nominal_types) {
    const auto valid_type = [&](const PublicType &type) {
      return validCallbackOwnershipTypes(type, resolve_nominal);
    };
    if (std::ranges::any_of(
            nominal.fields,
            [&](const auto &field) { return !valid_type(field.type); }) ||
        (nominal.value_repr_pattern &&
         !valid_type(*nominal.value_repr_pattern)) ||
        (nominal.object_repr_pattern &&
         !valid_type(*nominal.object_repr_pattern))) {
      error = "cannot register public nominal type `" +
              nominal.entity.canonical_name +
              "` with a type-invalid callback contract";
      return PublicInterfaceId::invalid();
    }
  }
  for (const auto *function : unique_functions) {
    const auto name = values_->identifier(function->name);
    const auto &contract = function->semantic_contract;
    if (contract.domain != CallableSemanticDomain::Ordinary) {
      const auto owner = resolve_nominal(contract.owner.nominal_entity);
      const auto &owner_ref = contract.owner.nominal_entity;
      // A helper for a private nominal is persisted in its defining module so
      // local lowering can recover the contract, but the nominal itself is not
      // an exported binding. Its canonical identity and definition fingerprint
      // are already part of the fingerprinted interface identity. Any owner
      // outside this module must resolve through the public nominal closure.
      const bool local_private_owner = !owner &&
                                       owner_ref.canonical_package == package &&
                                       owner_ref.canonical_module == module;
      if ((!owner && !local_private_owner) ||
          (owner &&
           (owner->fingerprint != owner_ref.expected_fingerprint ||
            owner->generic_parameter_count != contract.owner.arguments.size() ||
            (contract.domain == CallableSemanticDomain::ObjectProjection &&
             contract.projector_field >= owner->fields.size())))) {
        error = "cannot register public function `" + std::string(name) +
                "` with an invalid semantic owner contract";
        return PublicInterfaceId::invalid();
      }
    }
    if (function->semantic_contract.domain ==
            CallableSemanticDomain::Ordinary &&
        !validOwnershipSummaryTypes(function->parameters,
                                    function->ownership_summary,
                                    resolve_nominal)) {
      error = "cannot register public function `" + std::string(name) +
              "` with a type-invalid ownership region path";
      return PublicInterfaceId::invalid();
    }
    if (!validReturnLoanTypes(function->parameters, function->return_type,
                              function->ownership_summary, resolve_nominal)) {
      error = "cannot register public function `" + std::string(name) +
              "` with invalid return loan provenance";
      return PublicInterfaceId::invalid();
    }
    if (!validCallbackOwnershipTypes(function->return_type, resolve_nominal) ||
        std::ranges::any_of(function->parameters, [&](const auto &parameter) {
          return !validCallbackOwnershipTypes(parameter, resolve_nominal);
        })) {
      error = "cannot register public function `" + std::string(name) +
              "` with a type-invalid callback contract";
      return PublicInterfaceId::invalid();
    }
  }

  auto interface = std::unique_ptr<PublicInterface>(new PublicInterface(
      arena_, *values_, check_ir_id, interface_id, package_name, module_name));
  for (const auto *function_ptr : unique_functions) {
    const auto &function = *function_ptr;
    auto entity_id = function.canonical_entity;
    if (!entity_id.hasValue()) {
      const auto canonical_name = function.canonical_name.hasValue()
                                      ? function.canonical_name
                                      : function.name;
      const auto name = values_->identifier(canonical_name);
      const auto parameter_names = canonicalParameterNames(
          function.parameters.size(), function.parameter_names);
      const auto default_arguments = canonicalDefaultArguments(
          function.parameters.size(), function.default_arguments);
      const auto key = overloadEntityKey(
          package, module, name, function.member_owner, function.member_kind,
          function.generic_parameter_count, function.parameters);
      if (const auto found = entity_keys_.find(key);
          found != entity_keys_.end()) {
        entity_id = found->second;
      } else {
        entity_id = entities_.add(
            {.kind = PublicEntityKind::Function,
             .package_name = package_name,
             .module_name = module_name,
             .name = canonical_name,
             .member_owner = function.member_owner,
             .member_kind = function.member_kind,
             .generic_parameter_count = function.generic_parameter_count,
             .parameters = function.parameters,
             .parameter_names = parameter_names,
             .default_arguments = default_arguments,
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
             .foreign_abi =
                 function.foreign_abi.empty()
                     ? IdentifierId::invalid()
                     : values_->internIdentifier(function.foreign_abi),
             .external_symbol =
                 function.external_symbol.empty()
                     ? IdentifierId::invalid()
                     : values_->internIdentifier(function.external_symbol),
             .foreign_signature = function.foreign_signature,
             .interop_artifact = function.interop_artifact,
             .generic = function.generic,
             .generic_template = function.generic_template,
             .constraints = function.constraints,
             .fingerprint = entityFingerprint(
                 package, module, name, function.member_owner,
                 function.member_kind, function.generic_parameter_count,
                 function.parameters, function.return_type, function.error_type,
                 function.execution_kind, function.coroutine_constructor,
                 function.nominal_constructor, function.semantic_contract,
                 function.intrinsic_role, function.ownership_summary,
                 function.generic_template, function.declaration_kind,
                 function.is_unsafe, function.is_const, function.foreign_abi,
                 function.foreign_signature, parameter_names, default_arguments,
                 function.constraints, function.interop_artifact,
                 function.external_symbol)});
        entity_keys_.emplace(key, entity_id);
      }
    }
    (void)interface->addFunction(function, entity_id);
    if (function.interop_artifact) {
      const auto canonical_name = function.canonical_name.hasValue()
                                      ? function.canonical_name
                                      : function.name;
      const auto key =
          entityKey(package, module, values_->identifier(canonical_name),
                    PublicEntityKind::ForeignOperation);
      if (!entity_keys_.contains(key)) {
        auto operation_entity = entities_.get(entity_id);
        operation_entity.kind = PublicEntityKind::ForeignOperation;
        operation_entity.fingerprint = function.interop_artifact->fingerprint;
        entity_keys_.emplace(key, entities_.add(std::move(operation_entity)));
      }
    }
  }
  interface->nominal_artifacts_.assign(nominal_types.begin(),
                                       nominal_types.end());
  interface->value_artifacts_ = std::move(*unique_values);
  interface->interface_artifacts_.assign(interfaces.begin(), interfaces.end());
  interface->type_alias_artifacts_.assign(type_aliases.begin(),
                                          type_aliases.end());
  interface->interface_witness_artifacts_.assign(interface_witnesses.begin(),
                                                 interface_witnesses.end());
  std::ranges::sort(interface->value_artifacts_, {},
                    &PublicValueArtifact::name);
  internal::PublicInterfaceRegistryConstructionService::registerNominalTypes(
      *this, *interface, check_ir_id, package, module, nominal_types);
  if (!internal::PublicInterfaceRegistryConstructionService::registerInterfaceDeclarations(
          *this, check_ir_id, package, module, interfaces, error))
    return PublicInterfaceId::invalid();
  if (!internal::PublicInterfaceRegistryConstructionService::registerTypeAliases(
          *this, check_ir_id, package, module, type_aliases, error))
    return PublicInterfaceId::invalid();
  if (!internal::PublicInterfaceRegistryConstructionService::registerInterfaceWitnesses(
          *this, interface_witnesses,
          [](const PublicType &type, std::uint32_t generic_count,
             bool allow_void) {
            return validPublicType(type, generic_count, allow_void);
          },
          [](const PublicInterfaceConstraintArtifact &constraint,
             std::uint32_t generic_count) {
            return validInterfaceConstraint(constraint, generic_count);
          },
          [](const PublicInterfaceWitnessArtifact &witness) {
            return interfaceWitnessFingerprint(witness);
          },
          [](const PublicInterfaceWitnessArtifact &lhs,
             const PublicInterfaceWitnessArtifact &rhs) {
            return interfaceWitnessesMayOverlap(lhs, rhs);
          },
          error))
    return PublicInterfaceId::invalid();
  interface->setFingerprint(interfaceFingerprint(*interface, *this, *values_));

  interfaces_.push_back(std::move(interface));
  modules_.emplace(module_key, interface_id);
  if (check_ir_id.hasValue())
    check_irs_.emplace(check_ir_id.index, interface_id);
  return interface_id;
}



} // namespace chtholly::compiler
