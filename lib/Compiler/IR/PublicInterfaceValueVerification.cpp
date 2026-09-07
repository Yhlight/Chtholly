#include "PublicInterfaceServices.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include <algorithm>

namespace chtholly::compiler::internal {

bool PublicInterfaceVerifyService::interfaceValue(
    const PublicInterface &interface_value, std::string &error,
    const ValueVerificationState &state) {
  const auto &self = interface_value;
  error.clear();
  if (!self.interface_id_.hasValue() || !self.package_name_.hasValue() ||
      !self.module_name_.hasValue() ||
      self.package_name_.index >= self.values_->identifierCount() ||
      self.module_name_.index >= self.values_->identifierCount()) {
    error = "public interface has an invalid identity";
    return false;
  }
  std::size_t indexed_functions = 0;
  for (const auto &[name, overloads] : self.function_names_) {
    (void)name;
    indexed_functions += overloads.size();
  }
  for (const auto &[name, overloads] : self.member_function_names_) {
    (void)name;
    indexed_functions += overloads.size();
  }
  if (indexed_functions != self.functions_.size()) {
    error = "public interface has duplicate function names";
    return false;
  }
  if (self.nominal_type_names_.size() != self.nominal_types_.size()) {
    error = "public interface has duplicate nominal type names";
    return false;
  }
  for (std::uint32_t index = 0; index < self.functions_.size(); ++index) {
    const auto id = PublicBindingId(index);
    const auto &value = self.function(id);
    if (!value.name.hasValue() ||
        value.name.index >= self.values_->identifierCount() ||
        !value.parameters.hasValue() ||
        value.parameters.index >= self.parameter_blocks_.size() ||
        !state.valid_type(value.return_type, value.generic_parameter_count,
                          true) ||
        !state.valid_reference_provenance(
            value.return_type, self.parameterTypes(value.parameters).size(),
            true) ||
        !value.canonical_entity.hasValue() ||
        self.values_->identifier(value.name) == "main") {
      error = "public interface has an invalid function binding";
      return false;
    }
    for (const auto parameter : self.parameterTypes(value.parameters)) {
      if (!state.valid_type(parameter, value.generic_parameter_count, false)) {
        error = "public interface has an invalid parameter type";
        return false;
      }
      if (!state.valid_reference_provenance(
              parameter, self.parameterTypes(value.parameters).size(), true)) {
        error = "public interface has invalid reference provenance";
        return false;
      }
    }
    const auto indexed =
        value.member_owner
            ? self.member_function_names_
                  .find(state.member_function_binding_key(
                      *value.member_owner, self.values_->identifier(value.name)))
                  ->second
            : self.function_names_.find(value.name.index)->second;
    if (std::ranges::find(indexed, id) == indexed.end()) {
      error = "public interface has an inconsistent name index";
      return false;
    }
  }
  for (std::uint32_t index = 0; index < self.nominal_types_.size(); ++index) {
    const auto &value = self.nominalType(PublicBindingId(index));
    if (!value.name.hasValue() ||
        value.name.index >= self.values_->identifierCount() ||
        !value.canonical_entity.hasValue()) {
      error = "public interface has an invalid nominal binding";
      return false;
    }
  }
  std::string_view previous_value;
  bool has_previous_value = false;
  for (const auto &value : self.value_artifacts_) {
    if (value.kind >= PublicValueKind::Count || value.name.empty() ||
        value.canonical_package.empty() || value.canonical_module.empty() ||
        value.canonical_name.empty() || value.value.type != value.type ||
        !state.valid_constant_shape(value.value, self.nominal_artifacts_) ||
        !value.entity_fingerprint.hasValue() ||
        value.entity_fingerprint != state.value_fingerprint(value) ||
        (has_previous_value && previous_value >= value.name)) {
      error = "public interface has an invalid value binding";
      return false;
    }
    previous_value = value.name;
    has_previous_value = true;
  }
  return true;
}

} // namespace chtholly::compiler::internal
