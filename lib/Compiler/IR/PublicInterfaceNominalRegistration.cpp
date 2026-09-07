#include "PublicInterfaceServices.h"

#include "chtholly/Compiler/SharedValueStores.h"

namespace chtholly::compiler::internal {

void PublicInterfaceRegistryConstructionService::registerNominalTypes(
    PublicInterfaceRegistry &registry, PublicInterface &interface_value,
    CheckIRId check_ir_id, std::string_view package, std::string_view module,
    std::span<const PublicNominalTypeArtifact> nominal_types) {
  for (const auto &nominal : nominal_types) {
    const auto key = PublicInterfaceIdentityService::entityKey(
        nominal.entity.canonical_package, nominal.entity.canonical_module,
        nominal.entity.canonical_name, PublicEntityKind::NominalType);
    PublicEntityId entity_id;
    if (const auto found = registry.entity_keys_.find(key);
        found != registry.entity_keys_.end()) {
      entity_id = found->second;
    } else {
      const auto name = registry.values_->internIdentifier(
          nominal.entity.canonical_name);
      const auto canonical_package = registry.values_->internIdentifier(
          nominal.entity.canonical_package);
      const auto canonical_module = registry.values_->internIdentifier(
          nominal.entity.canonical_module);
      GenericId generic;
      if (nominal.generic_parameter_count != 0)
        generic = registry.values_->generics().addGeneric(
            nominal.entity.canonical_package == package &&
                    nominal.entity.canonical_module == module
                ? check_ir_id
                : CheckIRId::invalid(),
            canonical_module, name, nominal.generic_parameter_count);
      entity_id = registry.entities_.add(
          {.kind = PublicEntityKind::NominalType,
           .package_name = canonical_package,
           .module_name = canonical_module,
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
      registry.entity_keys_.emplace(key, entity_id);
    }
    if (nominal.is_exported)
      (void)interface_value.addNominalType(nominal, entity_id);
  }
}

} // namespace chtholly::compiler::internal
