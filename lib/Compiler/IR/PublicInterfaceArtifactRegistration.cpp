#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include "PublicInterfaceServices.h"

#include <cassert>
#include <vector>

namespace chtholly::compiler {

PublicInterfaceId PublicInterfaceRegistry::registerArtifact(
    CheckIRId check_ir_id, const PublicInterfaceArtifact &artifact,
    std::string &error) {
  if (!internal::PublicInterfaceVerifyService::artifact(artifact, error))
    return PublicInterfaceId::invalid();

  const auto package_name = values_->internIdentifier(artifact.packageName());
  const auto module_name = values_->internIdentifier(artifact.moduleName());
  std::vector<PublicFunctionBindingSpec> functions;
  functions.reserve(artifact.functions().size());
  for (const auto &function : artifact.functions()) {
    const auto name = values_->internIdentifier(function.name);
    const auto canonical_key =
        internal::PublicInterfaceIdentityService::overloadEntityKey(
            function.canonical_package, function.canonical_module,
            function.canonical_name, function.member_owner, function.member_kind,
            function.generic_parameter_count, function.parameters);
    const auto canonical_found = entity_keys_.find(canonical_key);
    GenericId generic;
    if (function.generic_parameter_count != 0) {
      if (canonical_found != entity_keys_.end()) {
        generic = entities_.get(canonical_found->second).generic;
      } else {
        const auto canonical_module =
            values_->internIdentifier(function.canonical_module);
        const auto canonical_name =
            values_->internIdentifier(function.canonical_name);
        generic = values_->generics().addGeneric(
            CheckIRId::invalid(), canonical_module, canonical_name,
            function.generic_parameter_count);
      }
    }
    PublicEntityId canonical_entity;
    if (function.canonical_package == artifact.packageName() &&
        function.canonical_module == artifact.moduleName()) {
      if (function.name != function.canonical_name && !function.member_owner &&
          !function.name.starts_with("$foreign$")) {
        error = "local public artifact binding does not match its entity name";
        return PublicInterfaceId::invalid();
      }
    } else {
      if (canonical_found == entity_keys_.end()) {
        const auto canonical_package =
            values_->internIdentifier(function.canonical_package);
        const auto canonical_module =
            values_->internIdentifier(function.canonical_module);
        const auto canonical_name =
            values_->internIdentifier(function.canonical_name);
        canonical_entity = entities_.add(
            {.package_name = canonical_package,
             .module_name = canonical_module,
             .name = canonical_name,
             .member_owner = function.member_owner,
             .member_kind = function.member_kind,
             .generic_parameter_count = function.generic_parameter_count,
             .parameters = function.parameters,
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
             .generic = generic,
             .generic_template = function.generic_template,
             .constraints = function.constraints,
             .fingerprint = function.entity_fingerprint});
        entity_keys_.emplace(canonical_key, canonical_entity);
      } else {
        const auto &entity = entities_.get(canonical_found->second);
        if (values_->identifier(entity.package_name) !=
                function.canonical_package ||
            values_->identifier(entity.module_name) !=
                function.canonical_module ||
            values_->identifier(entity.name) != function.canonical_name ||
            entity.member_owner != function.member_owner ||
            entity.external_symbol.hasValue() !=
                !function.external_symbol.empty() ||
            (entity.external_symbol.hasValue() &&
             values_->identifier(entity.external_symbol) !=
                 function.external_symbol) ||
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
            (entity.external_symbol.hasValue()
                 ? values_->identifier(entity.external_symbol)
                 : std::string_view{}) != function.external_symbol ||
            entity.foreign_signature != function.foreign_signature ||
            entity.interop_artifact != function.interop_artifact ||
            entity.generic_template != function.generic_template ||
            entity.constraints != function.constraints ||
            entity.fingerprint != function.entity_fingerprint) {
          error =
              "public interface artifact disagrees with its canonical entity";
          return PublicInterfaceId::invalid();
        }
        canonical_entity = canonical_found->second;
      }
    }
    functions.push_back(
        {.name = name,
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
         .foreign_abi = function.foreign_abi,
         .external_symbol = function.external_symbol,
         .foreign_signature = function.foreign_signature,
         .interop_artifact = function.interop_artifact,
         .generic = generic,
         .generic_template = function.generic_template,
         .constraints = function.constraints,
         .canonical_entity = canonical_entity,
         .canonical_name = values_->internIdentifier(function.canonical_name)});
  }

  const auto id =
      registerInterface(check_ir_id, package_name, module_name, functions,
                        error, artifact.nominalTypes(), artifact.values(),
                        artifact.interfaceDeclarations(),
                        artifact.typeAliases(), artifact.interfaceWitnesses());
  const auto *interface = tryGet(id);
  if (interface)
    assert(interface->fingerprint() == artifact.fingerprint());
  return id;
}

} // namespace chtholly::compiler
