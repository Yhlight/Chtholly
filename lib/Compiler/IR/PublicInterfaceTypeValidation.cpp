#include "PublicInterfaceServices.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <vector>

namespace chtholly::compiler::internal {
namespace {

bool validEntityReferenceImpl(const PublicEntityReferenceArtifact &entity,
                              PublicEntityKind expected) {
  return entity.kind == expected && !entity.canonical_package.empty() &&
         !entity.canonical_module.empty() && !entity.canonical_name.empty() &&
         entity.expected_fingerprint.hasValue();
}

bool validCallbackRegistrationContractImpl(const PublicType &type);
bool validCallbackCompletionContractImpl(const PublicType &type);

bool validPublicTypeImpl(const PublicType &type,
                         std::uint32_t generic_parameter_count,
                         bool allow_void = false) {
  if (type.kind >= PublicTypeKind::Count ||
      (!allow_void && type.kind == PublicTypeKind::Void) ||
      (type.kind != PublicTypeKind::CFunctionPointer &&
       (type.callable_variadic ||
        type.callable_context_parameter != core::AnyId::InvalidIndex ||
        type.callable_contract != CallableOwnershipSummary{})) ||
      (type.kind != PublicTypeKind::Tuple && type.abi_union) ||
      (type.kind != PublicTypeKind::CFunctionPointer &&
       type.foreign_calling_convention != ForeignCallingConvention::C) ||
      type.foreign_calling_convention >= ForeignCallingConvention::Count ||
      (type.kind != PublicTypeKind::TypeProjection &&
       (type.projection_kind != PublicTypeProjectionKind::Count ||
        type.projection_index != core::AnyId::InvalidIndex)) ||
      (type.kind != PublicTypeKind::ForeignOperationState &&
       type.foreign_operation_state != ForeignOperationStateKind::Count))
    return false;
  if (type.kind == PublicTypeKind::TypeParameter)
    return type.binding_index < generic_parameter_count &&
           type.arguments.empty();
  if (type.binding_index != core::AnyId::InvalidIndex)
    return false;
  if (type.kind == PublicTypeKind::TypeProjection) {
    if (generic_parameter_count == 0 || type.arguments.size() != 1 ||
        type.projection_kind >= PublicTypeProjectionKind::Count ||
        type.projection_index > 0x7fffffffU ||
        (type.projection_kind == PublicTypeProjectionKind::Pointee &&
         type.projection_index != 0) ||
        (type.projection_kind == PublicTypeProjectionKind::Associated &&
         !validEntityReferenceImpl(type.nominal_entity,
                                   PublicEntityKind::Interface)) ||
        (type.arguments.front().kind != PublicTypeKind::TypeParameter &&
         type.arguments.front().kind != PublicTypeKind::TypeProjection))
      return false;
    return validPublicTypeImpl(type.arguments.front(), generic_parameter_count,
                               false);
  }
  if (type.kind == PublicTypeKind::Never)
    return type.arguments.empty();
  if (type.kind == PublicTypeKind::Char)
    return type.arguments.empty();
  if (type.kind == PublicTypeKind::Integer)
    return type.arguments.empty() &&
           (type.scalar_width == 8 || type.scalar_width == 16 ||
            type.scalar_width == 32 || type.scalar_width == 64);
  if (type.kind == PublicTypeKind::Float)
    return type.arguments.empty() &&
           (type.scalar_width == 32 || type.scalar_width == 64) &&
           !type.integer_signed;
  if (type.kind == PublicTypeKind::Reference) {
    if (type.arguments.size() != 1 ||
        type.reference_mutability >= PublicReferenceMutability::Count ||
        type.reference_provenance.kind >=
            PublicReferenceProvenanceKind::Count ||
        (type.reference_provenance.kind ==
             PublicReferenceProvenanceKind::Erased
             ? type.reference_provenance.index != core::AnyId::InvalidIndex
             : type.reference_provenance.index == core::AnyId::InvalidIndex))
      return false;
    return validPublicTypeImpl(type.arguments.front(), generic_parameter_count);
  }
  if (type.kind == PublicTypeKind::Array)
    return type.array_bound != 0 && type.arguments.size() == 1 &&
           type.arguments.front().kind != PublicTypeKind::Never &&
           type.arguments.front().kind != PublicTypeKind::Void &&
           validPublicTypeImpl(type.arguments.front(), generic_parameter_count);
  if (type.kind == PublicTypeKind::Tuple)
    return (!type.abi_union || !type.arguments.empty()) &&
           std::ranges::all_of(type.arguments, [&](const PublicType &element) {
             return element.kind != PublicTypeKind::Never &&
                    element.kind != PublicTypeKind::Void &&
                    validPublicTypeImpl(element, generic_parameter_count);
           });
  if (type.kind == PublicTypeKind::Slice)
    return type.arguments.size() == 1 &&
           type.arguments.front().kind != PublicTypeKind::Never &&
           type.arguments.front().kind != PublicTypeKind::Void &&
           validPublicTypeImpl(type.arguments.front(), generic_parameter_count);
  if (type.kind == PublicTypeKind::RawPointer)
    return type.arguments.size() == 1 &&
           validPublicTypeImpl(type.arguments.front(), generic_parameter_count,
                               true);
  if (type.kind == PublicTypeKind::Function ||
      type.kind == PublicTypeKind::CFunctionPointer) {
    if (type.arguments.empty())
      return false;
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index)
      if (type.arguments[index].kind == PublicTypeKind::Never ||
          !validPublicTypeImpl(type.arguments[index], generic_parameter_count))
        return false;
    if (type.kind == PublicTypeKind::Function)
      return validPublicTypeImpl(type.arguments.back(), generic_parameter_count,
                                 true);
    std::string error;
    const auto parameter_count =
        static_cast<std::uint32_t>(type.arguments.size() - 1);
    return (!type.callable_variadic ||
            type.callable_context_parameter == core::AnyId::InvalidIndex) &&
           (type.callable_context_parameter == core::AnyId::InvalidIndex ||
            type.callable_context_parameter < parameter_count) &&
           validPublicTypeImpl(type.arguments.back(), generic_parameter_count,
                               true) &&
           type.callable_contract.verify(parameter_count, error);
  }
  if (type.kind == PublicTypeKind::CallbackAdapter)
    return validCallbackAdapterContract(type) &&
           std::ranges::all_of(type.arguments, [&](const PublicType &field) {
             return validPublicTypeImpl(field, generic_parameter_count);
           });
  if (type.kind == PublicTypeKind::CallbackRegistration)
    return validCallbackRegistrationContractImpl(type) &&
           std::ranges::all_of(type.arguments, [&](const PublicType &field) {
             return validPublicTypeImpl(field, generic_parameter_count);
           });
  if (type.kind == PublicTypeKind::CallbackCompletion)
    return validCallbackCompletionContractImpl(type) &&
           std::ranges::all_of(type.arguments, [&](const PublicType &field) {
             return validPublicTypeImpl(field, generic_parameter_count);
           });
  if (type.kind == PublicTypeKind::CallbackWake)
    return validCallbackWakeContract(type) &&
           validPublicTypeImpl(type.arguments.front(), generic_parameter_count);
  if (type.kind == PublicTypeKind::ForeignCompletion ||
      type.kind == PublicTypeKind::ForeignWake)
    return type.arguments.empty() &&
           type.nominal_entity.kind == PublicEntityKind::NominalType &&
           !type.nominal_entity.canonical_package.empty() &&
           !type.nominal_entity.canonical_module.empty() &&
           !type.nominal_entity.canonical_name.empty() &&
           type.nominal_entity.expected_fingerprint.hasValue();
  if (type.kind == PublicTypeKind::ForeignOperationState)
    return type.arguments.empty() &&
           type.foreign_operation_state < ForeignOperationStateKind::Count &&
           validEntityReferenceImpl(type.nominal_entity,
                                    PublicEntityKind::ForeignOperation);
  if (type.kind != PublicTypeKind::Nominal)
    return type.arguments.empty();
  if (type.nominal_entity.kind != PublicEntityKind::NominalType ||
      type.nominal_entity.canonical_package.empty() ||
      type.nominal_entity.canonical_module.empty() ||
      type.nominal_entity.canonical_name.empty() ||
      !type.nominal_entity.expected_fingerprint.hasValue())
    return false;
  return std::ranges::all_of(type.arguments, [&](const PublicType &argument) {
    return validPublicTypeImpl(argument, generic_parameter_count, true);
  });
}

bool validCallbackRegistrationContractImpl(const PublicType &type) {
  if (type.kind != PublicTypeKind::CallbackRegistration ||
      (type.arguments.size() != 5 && type.arguments.size() != 7 &&
       type.arguments.size() != 8 && type.arguments.size() != 10) ||
      type.registration_authority > 1)
    return false;
  std::string protocol_error;
  const auto expected_protocol = makeCallbackRegistrationProtocol(
      type.registration_authority, type.registration_entry_parameter,
      type.registration_userdata_parameter, type.registration_release_parameter,
      type.registration_bindings,
      static_cast<std::uint32_t>(type.arguments.size()),
      type.registration_arm_parameters, type.registration_detach_parameters);
  if (!type.foreign_resource_protocol.verify(
          static_cast<std::uint32_t>(type.arguments.size()), protocol_error) ||
      type.foreign_resource_protocol != expected_protocol)
    return false;
  const auto &callback = type.arguments[0];
  const auto &handle = type.arguments[1];
  const auto &reg = type.arguments[2];
  const auto &unreg = type.arguments[3];
  const auto &cancel = type.arguments[4];
  if (!validCallbackAdapterContract(callback) ||
      handle.kind != PublicTypeKind::RawPointer || handle.pointer_const ||
      handle.arguments.size() != 1 ||
      handle.arguments.front().kind != PublicTypeKind::Void)
    return false;
  const auto valid_terminal = [&](const PublicType &fn) {
    return fn.kind == PublicTypeKind::CFunctionPointer &&
           !fn.callable_variadic && fn.arguments.size() == 2 &&
           fn.callable_context_parameter == core::AnyId::InvalidIndex &&
           fn.arguments.front() == handle &&
           fn.arguments.back().kind == PublicTypeKind::Void;
  };
  const OwnershipRegion consumed_root{.parameter_index = 0};
  const auto valid_consuming = [&](const PublicType &fn,
                                   const PublicType &parameter,
                                   const PublicType &result) {
    return fn.kind == PublicTypeKind::CFunctionPointer &&
           !fn.callable_variadic && fn.arguments.size() == 2 &&
           fn.callable_context_parameter == core::AnyId::InvalidIndex &&
           fn.arguments.front() == parameter && fn.arguments.back() == result &&
           fn.callable_contract.effects ==
               std::vector<CallableRegionEffect>{{CallableEffectKind::Take,
                                                   consumed_root}} &&
           fn.callable_contract.postconditions ==
               std::vector<CallableRegionPostcondition>{
                   {consumed_root, CallableOutcomeInvalidate}} &&
           fn.callable_contract.returns_owned &&
           fn.callable_contract.return_provenance.empty();
  };
  const auto expected_register_arguments =
      (type.registration_authority == 1 ? 4U : 3U) +
      type.registration_bindings.size();
  if (reg.kind != PublicTypeKind::CFunctionPointer || reg.callable_variadic ||
      reg.arguments.size() != expected_register_arguments ||
      reg.arguments.back() != handle || !valid_terminal(unreg) ||
      !valid_terminal(cancel))
    return false;
  const auto &entry = callback.arguments[0];
  const auto &context = callback.arguments[1];
  const auto &register_parameters = reg.arguments;
  if (type.registration_entry_parameter >= register_parameters.size() - 1 ||
      type.registration_userdata_parameter >= register_parameters.size() - 1 ||
      type.registration_entry_parameter == type.registration_userdata_parameter ||
      register_parameters[type.registration_entry_parameter] != entry ||
      register_parameters[type.registration_userdata_parameter] != context)
    return false;
  if (type.registration_authority == 1) {
    if (type.registration_release_parameter >= register_parameters.size() - 1 ||
        type.registration_release_parameter == type.registration_entry_parameter ||
        type.registration_release_parameter == type.registration_userdata_parameter ||
        register_parameters[type.registration_release_parameter] !=
            callback.arguments[2])
      return false;
  } else if (type.registration_release_parameter != core::AnyId::InvalidIndex) {
    return false;
  }
  std::vector<bool> occupied(register_parameters.size() - 1);
  occupied[type.registration_entry_parameter] = true;
  occupied[type.registration_userdata_parameter] = true;
  if (type.registration_authority == 1)
    occupied[type.registration_release_parameter] = true;
  std::uint32_t previous = 0;
  bool first_binding = true;
  for (std::size_t binding_index = 0;
       binding_index < type.registration_bindings.size(); ++binding_index) {
    const auto &binding = type.registration_bindings[binding_index];
    if (binding.name.empty() || binding.parameter_index >= occupied.size() ||
        occupied[binding.parameter_index] ||
        (!first_binding && binding.parameter_index <= previous) ||
        std::ranges::any_of(
            std::span(type.registration_bindings).first(binding_index),
            [&](const auto &existing) { return existing.name == binding.name; }))
      return false;
    occupied[binding.parameter_index] = true;
    previous = binding.parameter_index;
    first_binding = false;
  }
  if (std::ranges::any_of(occupied, [](bool value) { return !value; }))
    return false;
  const auto mentions_synthesized = [&](const OwnershipRegion &region) {
    return region.parameter_index >= occupied.size() ||
           std::ranges::none_of(type.registration_bindings,
                                [&](const auto &binding) {
                                  return binding.parameter_index ==
                                         region.parameter_index;
                                });
  };
  if (!reg.callable_contract.returns_owned ||
      !reg.callable_contract.return_provenance.empty() ||
      std::ranges::any_of(reg.callable_contract.effects,
                          [&](const auto &effect) {
                            return mentions_synthesized(effect.region);
                          }) ||
      std::ranges::any_of(reg.callable_contract.postconditions,
                          [&](const auto &postcondition) {
                            return mentions_synthesized(postcondition.region);
                          }))
    return false;
  if (type.arguments.size() >= 7 &&
      (!valid_consuming(type.arguments[5], handle, handle) ||
       !valid_consuming(type.arguments[6], handle,
                        PublicType(PublicTypeKind::Void))))
    return false;
  if (type.arguments.size() >= 8) {
    const auto &poll = type.arguments[7];
    if (poll.kind != PublicTypeKind::CFunctionPointer ||
        poll.callable_variadic || poll.arguments.size() != 2 ||
        poll.callable_context_parameter != core::AnyId::InvalidIndex ||
        poll.arguments.front() != handle ||
        poll.arguments.back().kind != PublicTypeKind::Bool ||
        !poll.callable_contract.effects.empty() ||
        !poll.callable_contract.postconditions.empty() ||
        !poll.callable_contract.returns_owned ||
        !poll.callable_contract.return_provenance.empty())
      return false;
  }
  if (type.arguments.size() == 10) {
    auto completion = PublicType::callbackCompletion(
        callback, handle, handle, type.arguments[6], type.arguments[7],
        type.registration_authority, type.arguments[8], type.arguments[9],
        type.registration_arm_parameters, type.registration_detach_parameters);
    if (!validCallbackCompletionContractImpl(completion))
      return false;
  }
  return true;
}

bool validCallbackCompletionContractImpl(const PublicType &type) {
  if (type.kind != PublicTypeKind::CallbackCompletion ||
      (type.arguments.size() != 4 && type.arguments.size() != 5 &&
       type.arguments.size() != 7) ||
      type.registration_authority > 1)
    return false;
  std::string protocol_error;
  const auto expected_protocol = makeCallbackCompletionProtocol(
      type.registration_authority,
      static_cast<std::uint32_t>(type.arguments.size()),
      type.registration_arm_parameters, type.registration_detach_parameters);
  if (!type.foreign_resource_protocol.verify(
          static_cast<std::uint32_t>(type.arguments.size()), protocol_error) ||
      type.foreign_resource_protocol != expected_protocol)
    return false;
  const auto &callback = type.arguments[0];
  const auto &handle = type.arguments[1];
  const auto &token = type.arguments[2];
  const auto &wait = type.arguments[3];
  const auto valid_pointer = [](const PublicType &pointer) {
    return pointer.kind == PublicTypeKind::RawPointer &&
           !pointer.pointer_const && pointer.arguments.size() == 1 &&
           pointer.arguments.front().kind == PublicTypeKind::Void;
  };
  const OwnershipRegion root{.parameter_index = 0};
  const auto valid_wait =
      validCallbackAdapterContract(callback) && valid_pointer(handle) &&
      valid_pointer(token) && wait.kind == PublicTypeKind::CFunctionPointer &&
      !wait.callable_variadic && wait.arguments.size() == 2 &&
      wait.callable_context_parameter == core::AnyId::InvalidIndex &&
      wait.arguments.front() == token &&
      wait.arguments.back().kind == PublicTypeKind::Void &&
      wait.callable_contract.effects ==
          std::vector<CallableRegionEffect>{{CallableEffectKind::Take, root}} &&
      wait.callable_contract.postconditions ==
          std::vector<CallableRegionPostcondition>{{root,
                                                    CallableOutcomeInvalidate}} &&
      wait.callable_contract.returns_owned &&
      wait.callable_contract.return_provenance.empty();
  if (!valid_wait || type.arguments.size() == 4)
    return valid_wait;
  const auto &poll = type.arguments[4];
  const auto valid_poll =
      poll.kind == PublicTypeKind::CFunctionPointer &&
      !poll.callable_variadic && poll.arguments.size() == 2 &&
      poll.callable_context_parameter == core::AnyId::InvalidIndex &&
      poll.arguments.front() == token &&
      poll.arguments.back().kind == PublicTypeKind::Bool &&
      poll.callable_contract.effects.empty() &&
      poll.callable_contract.postconditions.empty() &&
      poll.callable_contract.returns_owned &&
      poll.callable_contract.return_provenance.empty();
  if (!valid_poll || type.arguments.size() == 5)
    return valid_poll;
  const auto &arm = type.arguments[5];
  const auto &detach = type.arguments[6];
  const auto &arm_roles = type.registration_arm_parameters;
  const auto &detach_roles = type.registration_detach_parameters;
  const auto distinct = [](auto roles) {
    for (std::size_t i = 0; i < roles.size(); ++i)
      for (std::size_t j = i + 1; j < roles.size(); ++j)
        if (roles[i] == roles[j])
          return false;
    return true;
  };
  const auto valid_wake_adapter = [&] {
    if (std::ranges::any_of(arm_roles, [](auto role) { return role >= 4; }))
      return false;
    const auto &entry = arm.arguments[arm_roles[1]];
    return entry.kind == PublicTypeKind::CFunctionPointer &&
           !entry.callable_variadic && entry.arguments.size() == 2 &&
           entry.callable_context_parameter == 0 &&
           entry.arguments.front() == arm.arguments[arm_roles[2]] &&
           entry.arguments.back().kind == PublicTypeKind::Void &&
           validCallbackAdapterContract(
               PublicType::callbackAdapter(entry, arm.arguments[arm_roles[2]],
                                           arm.arguments[arm_roles[3]]));
  };
  if (arm.kind != PublicTypeKind::CFunctionPointer || arm.callable_variadic ||
      arm.arguments.size() != 5 || !distinct(arm_roles) ||
      std::ranges::any_of(arm_roles, [](auto role) { return role >= 4; }) ||
      arm.arguments[arm_roles[0]] != token || !valid_wake_adapter() ||
      arm.arguments[arm_roles[2]] != callback.arguments[1] ||
      arm.arguments[arm_roles[3]] != callback.arguments[2] ||
      arm.arguments.back().kind != PublicTypeKind::Bool ||
      arm.callable_context_parameter != core::AnyId::InvalidIndex ||
      arm.callable_contract != CallableOwnershipSummary{})
    return false;
  if (detach.kind != PublicTypeKind::CFunctionPointer ||
      detach.callable_variadic ||
      detach.callable_context_parameter != core::AnyId::InvalidIndex ||
      detach.arguments.empty() || detach.arguments.back().kind !=
                                      PublicTypeKind::Void)
    return false;
  const OwnershipRegion token_root{.parameter_index = detach_roles[0]};
  CallableOwnershipSummary expected;
  if (type.registration_authority == 0) {
    if (detach.arguments.size() != 4 || !distinct(detach_roles) ||
        std::ranges::any_of(detach_roles, [](auto role) { return role >= 3; }) ||
        detach.arguments[detach_roles[0]] != token ||
        detach.arguments[detach_roles[1]] != callback.arguments[1] ||
        detach.arguments[detach_roles[2]] != callback.arguments[2])
      return false;
    const OwnershipRegion userdata_root{.parameter_index = detach_roles[1]};
    expected.effects = {{CallableEffectKind::Take, token_root},
                        {CallableEffectKind::Take, userdata_root}};
    expected.postconditions = {{token_root, CallableOutcomeInvalidate},
                               {userdata_root, CallableOutcomeInvalidate}};
  } else {
    if (detach.arguments.size() != 2 || detach_roles[0] != 0 ||
        detach_roles[1] != core::AnyId::InvalidIndex ||
        detach_roles[2] != core::AnyId::InvalidIndex ||
        detach.arguments[0] != token)
      return false;
    expected.effects = {{CallableEffectKind::Take, token_root}};
    expected.postconditions = {{token_root, CallableOutcomeInvalidate}};
  }
  expected.canonicalize();
  return detach.callable_contract == expected;
}

} // namespace

bool PublicInterfaceTypeValidationService::validPublicType(
    const PublicType &type, std::uint32_t generic_parameter_count,
    bool allow_void) {
  return validPublicTypeImpl(type, generic_parameter_count, allow_void);
}

bool PublicInterfaceTypeValidationService::validEntityReference(
    const PublicEntityReferenceArtifact &entity, PublicEntityKind expected) {
  return validEntityReferenceImpl(entity, expected);
}

bool PublicInterfaceTypeValidationService::validInterfaceConstraint(
    const PublicInterfaceConstraintArtifact &constraint,
    std::uint32_t generic_parameter_count) {
  return validPublicTypeImpl(constraint.subject, generic_parameter_count, false) &&
         validEntityReferenceImpl(constraint.interface_entity,
                                  PublicEntityKind::Interface) &&
         std::ranges::all_of(constraint.arguments, [&](const auto &argument) {
           return validPublicTypeImpl(argument, generic_parameter_count, false);
         });
}

bool PublicInterfaceTypeValidationService::validCallbackRegistrationContract(
    const PublicType &type) {
  return validCallbackRegistrationContractImpl(type);
}

bool PublicInterfaceTypeValidationService::validCallbackCompletionContract(
    const PublicType &type) {
  return validCallbackCompletionContractImpl(type);
}

bool PublicInterfaceTypeValidationService::validCallbackWakeContract(
    const PublicType &type) {
  return type.kind == PublicTypeKind::CallbackWake &&
         type.arguments.size() == 1 &&
         validCallbackCompletionContractImpl(type.arguments.front()) &&
         type.arguments.front().arguments.size() == 7;
}

} // namespace chtholly::compiler::internal
