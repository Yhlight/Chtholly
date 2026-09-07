#include "NominalTypeArtifactVerificationInternal.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool NominalTypeArtifactVerificationService::verify(
    const PublicNominalTypeArtifact &artifact, std::string &error,
    const NominalTypeArtifactVerificationCallbacks &callbacks) {
  error.clear();
  const auto &entity = artifact.entity;
  const auto &definition_fingerprint = artifact.definition_fingerprint;
  const auto generic_parameter_count = artifact.generic_parameter_count;
  const auto &fields = artifact.fields;
  const auto &variants = artifact.variants;
  const auto &is_value_enum = artifact.is_value_enum;
  const auto &value_repr_pattern = artifact.value_repr_pattern;
  const auto &object_repr_pattern = artifact.object_repr_pattern;
  const auto &representation_policy = artifact.representation_policy;
  const auto &kind = artifact.kind;
  const auto &foreign_representation = artifact.foreign_representation;
  const auto &foreign_invalid_state = artifact.foreign_invalid_state;
  const auto &foreign_invalid_integer = artifact.foreign_invalid_integer;
  const auto &foreign_handle_type = artifact.foreign_handle_type;
  const auto &foreign_completion_handle_type = artifact.foreign_completion_handle_type;
  const auto &foreign_callback_type = artifact.foreign_callback_type;
  const auto &foreign_waker_type = artifact.foreign_waker_type;
  const auto &foreign_resource_protocol = artifact.foreign_resource_protocol;
  const auto &foreign_resource_operations = artifact.foreign_resource_operations;
  if (!callbacks.valid_entity(entity, PublicEntityKind::NominalType) ||
      definition_fingerprint != entity.expected_fingerprint ||
      definition_fingerprint != callbacks.definition_fingerprint(artifact) ||
      representation_policy >= NominalRepresentationPolicy::Count ||
      kind >= NominalKind::Count ||
      (kind == NominalKind::Enum &&
       (variants.empty() || !fields.empty() ||
        representation_policy != NominalRepresentationPolicy::Opaque)) ||
      (is_value_enum &&
       (kind != NominalKind::Enum || generic_parameter_count != 0 ||
        value_repr_pattern || object_repr_pattern)) ||
      (kind != NominalKind::Enum && (!variants.empty() || is_value_enum)) ||
      (kind == NominalKind::Union &&
       representation_policy != NominalRepresentationPolicy::C)) {
    error = "nominal definition has an invalid identity fingerprint";
    return false;
  }
  if (representation_policy == NominalRepresentationPolicy::C &&
      (fields.empty() || value_repr_pattern || object_repr_pattern ||
       std::ranges::any_of(fields, [](const auto &field) {
         const auto kind = field.type.kind;
         return field.projection_kind !=
                    PublicObjectProjectionKind::StableAddress ||
                !field.storage_path.empty() ||
                !(kind == PublicTypeKind::Bool ||
                  kind == PublicTypeKind::Integer ||
                  kind == PublicTypeKind::Float ||
                  kind == PublicTypeKind::Reference ||
                  kind == PublicTypeKind::RawPointer ||
                  kind == PublicTypeKind::CFunctionPointer ||
                  kind == PublicTypeKind::Array ||
                  kind == PublicTypeKind::Nominal ||
                  kind == PublicTypeKind::TypeParameter);
       }))) {
    error = "repr(C) nominal definition is not C-layout eligible";
    return false;
  }
  std::unordered_set<std::string> names;
  for (const auto &field : fields) {
    if (field.name.empty() || !names.insert(field.name).second ||
        field.type.kind == PublicTypeKind::Never ||
        !callbacks.verify_public_type(field.type, generic_parameter_count, false, error) ||
        callbacks.has_parameter_provenance(field.type)) {
      if (error.empty())
        error = "nominal definition has an invalid field";
      return false;
    }
    for (const auto &component : field.storage_path)
      if (component.empty()) {
        error = "nominal definition has an invalid storage projection";
        return false;
      }
    for (const auto &component : field.projection_region_path)
      if (component.empty()) {
        error = "nominal definition has an invalid projection region";
        return false;
      }
    if (field.projection_kind >= PublicObjectProjectionKind::Count ||
        (field.projection_kind == PublicObjectProjectionKind::Computed) !=
            !field.projector_name.empty() ||
        (field.projection_kind != PublicObjectProjectionKind::Computed &&
         !field.projection_region_path.empty()) ||
        (field.projection_kind == PublicObjectProjectionKind::BitPacked &&
         (field.storage_path.empty() || field.bit_begin >= field.bit_end)) ||
        (field.projection_kind != PublicObjectProjectionKind::BitPacked &&
         (field.bit_begin != 0 || field.bit_end != 0))) {
      error = "nominal definition has an invalid field projection";
      return false;
    }
  }
  std::unordered_set<std::string> variant_names;
  std::unordered_set<std::int64_t> variant_discriminants;
  for (std::size_t variant_index = 0; variant_index < variants.size();
       ++variant_index) {
    const auto &variant = variants[variant_index];
    if (variant.name.empty() || !variant_names.insert(variant.name).second ||
        variant.shape >= PublicEnumPayloadShape::Count ||
        (variant.shape == PublicEnumPayloadShape::Unit) !=
            variant.fields.empty()) {
      error = "enum definition has an invalid variant";
      return false;
    }
    if ((is_value_enum &&
         (variant.shape != PublicEnumPayloadShape::Unit ||
          variant.discriminant < std::numeric_limits<std::int32_t>::min() ||
          variant.discriminant > std::numeric_limits<std::int32_t>::max() ||
          !variant_discriminants.insert(variant.discriminant).second)) ||
        (!is_value_enum &&
         variant.discriminant != static_cast<std::int64_t>(variant_index))) {
      error = "enum definition has invalid discriminant metadata";
      return false;
    }
    std::unordered_set<std::string> payload_names;
    for (std::size_t index = 0; index < variant.fields.size(); ++index) {
      const auto &field = variant.fields[index];
      if (field.name.empty() || !payload_names.insert(field.name).second ||
          (variant.shape == PublicEnumPayloadShape::Tuple &&
           field.name != std::to_string(index)) ||
          field.type.kind == PublicTypeKind::Never ||
          !callbacks.verify_public_type(field.type, generic_parameter_count, false,
                            error) ||
          callbacks.has_parameter_provenance(field.type) || !field.storage_path.empty() ||
          field.projection_kind != PublicObjectProjectionKind::StableAddress ||
          !field.projector_name.empty() ||
          !field.projection_region_path.empty() || field.bit_begin != 0 ||
          field.bit_end != 0) {
        if (error.empty())
          error = "enum definition has an invalid payload field";
        return false;
      }
    }
  }
  if (value_repr_pattern &&
      (!callbacks.verify_public_type(*value_repr_pattern, generic_parameter_count, false,
                         error) ||
       callbacks.has_parameter_provenance(*value_repr_pattern))) {
    if (error.empty())
      error = "nominal definition has an invalid value representation";
    return false;
  }
  if (object_repr_pattern &&
      (!callbacks.verify_public_type(*object_repr_pattern, generic_parameter_count, false,
                         error) ||
       object_repr_pattern->kind != PublicTypeKind::Nominal ||
       callbacks.has_parameter_provenance(*object_repr_pattern))) {
    if (error.empty())
      error = "nominal definition has an invalid object representation";
    return false;
  }
  if (!object_repr_pattern &&
      std::ranges::any_of(fields, [](const auto &field) {
        return !field.storage_path.empty() ||
               field.projection_kind !=
                   PublicObjectProjectionKind::StableAddress;
      })) {
    error = "field storage projections require a custom object representation";
    return false;
  }

  const auto empty_protocol = ForeignResourceProtocol{};
  const auto has_foreign_metadata =
      foreign_representation.has_value() ||
      foreign_invalid_state != ForeignResourceInvalidState::Count ||
      foreign_invalid_integer != 0 || foreign_handle_type.has_value() ||
      foreign_completion_handle_type.has_value() ||
      foreign_callback_type.has_value() || foreign_waker_type.has_value() ||
      foreign_resource_protocol != empty_protocol ||
      !foreign_resource_operations.empty();
  if (kind != NominalKind::ForeignHandle &&
      kind != NominalKind::ForeignResource) {
    if (has_foreign_metadata) {
      error = "ordinary nominal definition contains foreign resource metadata";
      return false;
    }
    return true;
  }
  if (generic_parameter_count != 0 || !fields.empty() || !variants.empty() ||
      value_repr_pattern || object_repr_pattern ||
      representation_policy != NominalRepresentationPolicy::Opaque) {
    error = "foreign nominal definition has an invalid structural shape";
    return false;
  }
  if (kind == NominalKind::ForeignHandle) {
    if (foreign_handle_type || foreign_completion_handle_type ||
        foreign_callback_type || foreign_waker_type ||
        foreign_resource_protocol != empty_protocol ||
        !foreign_resource_operations.empty() ||
        (foreign_representation &&
         !callbacks.verify_public_type(*foreign_representation, 0, false, error)) ||
        (!foreign_representation &&
         foreign_invalid_state != ForeignResourceInvalidState::Count) ||
        (foreign_representation &&
         foreign_representation->kind != PublicTypeKind::Integer &&
         foreign_representation->kind != PublicTypeKind::RawPointer &&
         foreign_representation->kind != PublicTypeKind::Tuple &&
         foreign_representation->kind != PublicTypeKind::Array) ||
        (foreign_invalid_state == ForeignResourceInvalidState::Integer &&
         (!foreign_representation ||
          foreign_representation->kind != PublicTypeKind::Integer)) ||
        (foreign_invalid_state ==
             ForeignResourceInvalidState::PointerBitPattern &&
         (!foreign_representation ||
          foreign_representation->kind != PublicTypeKind::RawPointer ||
          foreign_representation->pointer_const ||
          foreign_invalid_integer == 0)) ||
        (foreign_invalid_state == ForeignResourceInvalidState::Null &&
         (!foreign_representation ||
          foreign_representation->kind != PublicTypeKind::RawPointer ||
          foreign_representation->pointer_const)) ||
        (foreign_invalid_state == ForeignResourceInvalidState::Count &&
         foreign_invalid_integer != 0) ||
        ((foreign_invalid_state == ForeignResourceInvalidState::Null ||
          foreign_invalid_state == ForeignResourceInvalidState::Count) &&
         foreign_invalid_integer != 0)) {
      if (error.empty())
        error = "foreign handle definition has invalid representation metadata";
      return false;
    }
    return true;
  }

  if (foreign_representation ||
      foreign_invalid_state != ForeignResourceInvalidState::Count ||
      foreign_invalid_integer != 0 || !foreign_handle_type ||
      !callbacks.verify_public_type(*foreign_handle_type, 0, false, error) ||
      foreign_handle_type->kind != PublicTypeKind::Nominal ||
      (foreign_completion_handle_type &&
       (!callbacks.verify_public_type(*foreign_completion_handle_type, 0, false, error) ||
        foreign_completion_handle_type->kind != PublicTypeKind::Nominal)) ||
      (foreign_callback_type &&
       (!callbacks.verify_public_type(*foreign_callback_type, 0, false, error) ||
        foreign_callback_type->kind != PublicTypeKind::CallbackAdapter)) ||
      (foreign_waker_type &&
       (!callbacks.verify_public_type(*foreign_waker_type, 0, false, error) ||
        foreign_waker_type->kind != PublicTypeKind::CallbackAdapter)) ||
      (foreign_waker_type && !foreign_callback_type)) {
    if (error.empty())
      error = "foreign resource definition has invalid handle metadata";
    return false;
  }
  const auto type_count = static_cast<std::uint32_t>(
      1U + (foreign_completion_handle_type ? 1U : 0U) +
      (foreign_callback_type ? 1U : 0U) + foreign_resource_operations.size());
  if (!foreign_resource_protocol.verify(type_count, error) ||
      foreign_resource_protocol.callback_type_index !=
          (foreign_callback_type
               ? 1U + (foreign_completion_handle_type ? 1U : 0U)
               : core::AnyId::InvalidIndex) ||
      foreign_resource_protocol.resource_type_index != 0 ||
      foreign_resource_protocol.completion_type_index !=
          (foreign_completion_handle_type ? 1U : core::AnyId::InvalidIndex) ||
      foreign_resource_protocol.release_authority > 1 ||
      foreign_resource_operations.empty() ||
      foreign_resource_operations.size() !=
          foreign_resource_protocol.roles.size()) {
    if (error.empty())
      error = "foreign resource definition has an invalid canonical protocol";
    return false;
  }
  std::unordered_set<std::string> operation_names;
  std::unordered_set<std::uint32_t> operation_roles;
  const auto callable_base = 1U + (foreign_completion_handle_type ? 1U : 0U) +
                             (foreign_callback_type ? 1U : 0U);
  for (std::size_t index = 0; index < foreign_resource_operations.size();
       ++index) {
    const auto &operation = foreign_resource_operations[index];
    const auto *role = foreign_resource_protocol.findRole(operation.role);
    const auto mapped_parameter = [&](ForeignResourceParameterKind kind,
                                      std::uint32_t physical) {
      const auto found = std::ranges::find(
          role->parameters, kind, &ForeignResourceParameterBinding::kind);
      return physical == core::AnyId::InvalidIndex
                 ? found == role->parameters.end()
                 : found != role->parameters.end() &&
                       found->parameter_index == physical;
    };
    if (operation.name.empty() ||
        !operation_names.insert(operation.name).second ||
        operation.role >= ForeignResourceRoleKind::Count || !role ||
        !operation_roles.insert(static_cast<std::uint32_t>(operation.role))
             .second ||
        role->callable_type_index != callable_base + index ||
        !callbacks.valid_entity(operation.target, PublicEntityKind::Function) ||
        !mapped_parameter(ForeignResourceParameterKind::Resource,
                          operation.resource_parameter) ||
        !mapped_parameter(ForeignResourceParameterKind::Completion,
                          operation.completion_parameter)) {
      error = "foreign resource definition has an invalid operation descriptor";
      return false;
    }
  }
  return true;
}



} // namespace chtholly::compiler::internal

namespace chtholly::compiler {

bool PublicNominalTypeArtifact::verify(std::string &error) const {
  return internal::NominalTypeArtifactVerificationService::verify(
      *this, error, makeNominalTypeArtifactVerificationCallbacks());
}

} // namespace chtholly::compiler
