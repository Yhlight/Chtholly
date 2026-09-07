#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/CallableOwnership.h"

#include <array>
#include <cassert>
#include <vector>

namespace chtholly::compiler {

TypeId SemIR::addAsyncFunctionType(std::span<const TypeId> parameters,
                                   TypeId success,
                                   std::optional<TypeId> error) {
  std::vector<TypeId> outcomes{success};
  if (error)
    outcomes.push_back(*error);
  return addType({SemTypeKind::AsyncFunction, addTypeBlock(parameters).index,
                  addTypeBlock(outcomes).index});
}

TypeId SemIR::addCoroutineTaskType(TypeId success,
                                   std::optional<TypeId> error) {
  return addType({SemTypeKind::CoroutineTask, success.index,
                  error ? error->index : core::AnyId::InvalidIndex});
}

TypeId SemIR::addCoroutineTaskOutcomeType(TypeId success,
                                          std::optional<TypeId> error) {
  return addType({SemTypeKind::CoroutineTaskOutcome, success.index,
                  error ? error->index : core::AnyId::InvalidIndex});
}

TypeId SemIR::addCoroutineCheckedType(TypeId payload) {
  return addType({SemTypeKind::CoroutineChecked, payload.index});
}

TypeId SemIR::coroutineTaskSuccessType(TypeId id) const {
  const auto &value = type(id);
  assert(value.kind == SemTypeKind::CoroutineTask ||
         value.kind == SemTypeKind::CoroutineTaskOutcome);
  return TypeId(value.arg0);
}

std::optional<TypeId> SemIR::coroutineTaskErrorType(TypeId id) const {
  const auto &value = type(id);
  assert(value.kind == SemTypeKind::CoroutineTask ||
         value.kind == SemTypeKind::CoroutineTaskOutcome);
  return value.arg1 == core::AnyId::InvalidIndex
             ? std::nullopt
             : std::optional<TypeId>(TypeId(value.arg1));
}

TypeId SemIR::coroutineCheckedPayloadType(TypeId id) const {
  assert(type(id).kind == SemTypeKind::CoroutineChecked);
  return TypeId(type(id).arg0);
}

TypeId SemIR::asyncSuccessType(TypeId id) const {
  const auto &value = type(id);
  if (value.kind != SemTypeKind::AsyncFunction)
    return TypeId::invalid();
  const auto outcomes = typeBlock(TypeBlockId(value.arg1));
  return outcomes.empty() ? TypeId::invalid() : outcomes.front();
}

std::optional<TypeId> SemIR::asyncErrorType(TypeId id) const {
  const auto &value = type(id);
  if (value.kind != SemTypeKind::AsyncFunction)
    return std::nullopt;
  const auto outcomes = typeBlock(TypeBlockId(value.arg1));
  return outcomes.size() == 2 ? std::optional<TypeId>(outcomes.back())
                              : std::nullopt;
}

TypeId SemIR::addTypeParameter(GenericId generic, std::uint32_t binding_index) {
  return addType({SemTypeKind::TypeParameter, generic.index, binding_index,
                  core::AnyId::InvalidIndex});
}

TypeId SemIR::addCFunctionPointerType(std::span<const TypeId> parameters,
                                      TypeId result, bool is_variadic,
                                      CallableOwnershipSummary contract,
                                      std::uint32_t context_parameter,
                                      ForeignCallingConvention convention) {
  contract = effectiveCallableOwnershipSummary(std::move(contract));
  CanonicalType canonical;
  canonical.kind = is_variadic ? CanonicalTypeKind::CVariadicFunctionPointer
                               : CanonicalTypeKind::CFunctionPointer;
  for (const auto parameter : parameters)
    canonical.elements.push_back(canonicalType(parameter));
  canonical.elements.push_back(canonicalType(result));
  canonical.callable_contract = std::move(contract);
  canonical.callable_context_parameter = context_parameter;
  canonical.foreign_calling_convention = convention;
  const auto canonical_id =
      values_->generics().internType(std::move(canonical));
  const auto block = addTypeBlock(parameters);
  return addType({is_variadic ? SemTypeKind::CVariadicFunctionPointer
                              : SemTypeKind::CFunctionPointer,
                  block.index, result.index, canonical_id.index});
}

ForeignCallingConvention
SemIR::cFunctionCallingConvention(TypeId type_id) const {
  const auto &value = type(type_id);
  assert((value.kind == SemTypeKind::CFunctionPointer ||
          value.kind == SemTypeKind::CVariadicFunctionPointer) &&
         value.reserved != core::AnyId::InvalidIndex);
  return values_->generics()
      .type(CanonicalTypeId(value.reserved))
      .foreign_calling_convention;
}

TypeId SemIR::addCallbackAdapterType(TypeId entry, TypeId context,
                                     TypeId release) {
  const std::array elements{entry, context, release};
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::CallbackAdapter;
  for (const auto element : elements)
    canonical.elements.push_back(canonicalType(element));
  const auto canonical_id =
      values_->generics().internType(std::move(canonical));
  return addType({SemTypeKind::CallbackAdapter, addTypeBlock(elements).index,
                  core::AnyId::InvalidIndex, canonical_id.index});
}

TypeId SemIR::addCallbackCompletionType(
    TypeId callback, TypeId handle, TypeId token, TypeId wait_type,
    TypeId poll_type, CallbackReleaseAuthority authority, TypeId arm_type,
    TypeId detach_type, std::array<std::uint32_t, 4> arm_parameters,
    std::array<std::uint32_t, 3> detach_parameters) {
  std::vector<TypeId> elements{callback, handle, token, wait_type};
  if (poll_type.hasValue())
    elements.push_back(poll_type);
  if (arm_type.hasValue() && detach_type.hasValue()) {
    elements.push_back(arm_type);
    elements.push_back(detach_type);
  }
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::CallbackCompletion;
  for (const auto element : elements)
    canonical.elements.push_back(canonicalType(element));
  canonical.registration_authority = static_cast<std::uint32_t>(authority);
  canonical.registration_arm_parameters = arm_parameters;
  canonical.registration_detach_parameters = detach_parameters;
  CanonicalForeignResourceProtocol protocol;
  for (const auto element : elements)
    protocol.types.push_back(canonicalType(element));
  protocol.facts = makeCallbackCompletionProtocol(
      static_cast<std::uint8_t>(authority),
      static_cast<std::uint32_t>(elements.size()), arm_parameters,
      detach_parameters);
  canonical.foreign_resource_protocol =
      values_->generics().internForeignResourceProtocol(std::move(protocol));
  const auto canonical_id =
      values_->generics().internType(std::move(canonical));
  return addType({SemTypeKind::CallbackCompletion, addTypeBlock(elements).index,
                  static_cast<std::uint32_t>(authority), canonical_id.index});
}

TypeId SemIR::addCallbackWakeType(TypeId completion) {
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::CallbackWake;
  canonical.elements.push_back(canonicalType(completion));
  const auto canonical_id =
      values_->generics().internType(std::move(canonical));
  const std::array elements{completion};
  return addType({SemTypeKind::CallbackWake, addTypeBlock(elements).index,
                  core::AnyId::InvalidIndex, canonical_id.index});
}

TypeId SemIR::addForeignCompletionType(NominalTypeId resource) {
  const auto resource_type = addNominalType(resource);
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::ForeignCompletion;
  canonical.nominal_key =
      values_->generics().type(canonicalType(resource_type)).nominal_key;
  return addType({SemTypeKind::ForeignCompletion, resource.index,
                  core::AnyId::InvalidIndex,
                  values_->generics().internType(std::move(canonical)).index});
}

TypeId SemIR::addForeignWakeType(NominalTypeId resource) {
  const auto resource_type = addNominalType(resource);
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::ForeignWake;
  canonical.nominal_key =
      values_->generics().type(canonicalType(resource_type)).nominal_key;
  return addType({SemTypeKind::ForeignWake, resource.index,
                  core::AnyId::InvalidIndex,
                  values_->generics().internType(std::move(canonical)).index});
}

TypeId SemIR::markForeignOperationState(TypeId storage,
                                        PublicEntityReferenceArtifact operation,
                                        ForeignOperationStateKind state) {
  if (!storage.hasValue() || state >= ForeignOperationStateKind::Count)
    return TypeId::invalid();
  for (const auto &[index, owner] : foreign_operation_state_owners_)
    if (owner.operation == operation && owner.state == state)
      return TypeId(index);
  auto result = storage;
  // Subscription has the same physical carrier as the foreign handle, but it
  // must remain a distinct semantic type or importing the state would retag
  // every occurrence of the physical ABI handle. Reuse the existing nominal
  // declaration (and therefore its frozen layout) while assigning a unique
  // canonical identity to this operation-owned view. Completion and Wake
  // already have distinct aggregate storage types.
  if (state == ForeignOperationStateKind::Subscription &&
      type(storage).kind == SemTypeKind::Nominal) {
    CanonicalType canonical;
    canonical.kind = CanonicalTypeKind::Nominal;
    canonical.nominal_key =
        values_->generics().type(canonicalType(storage)).nominal_key +
        "$operation$" + operation.expected_fingerprint.hex().substr(0, 12) +
        "$Subscription";
    const auto canonical_id =
        values_->generics().internType(std::move(canonical));
    result = addType({SemTypeKind::Nominal, type(storage).arg0,
                      type(storage).arg1, canonical_id.index});
  }
  foreign_operation_state_owners_[result.index] =
      ForeignOperationStateOwner{std::move(operation), state};
  return result;
}

std::optional<ForeignOperationStateOwner>
SemIR::foreignOperationStateOwner(TypeId type) const {
  const auto found = foreign_operation_state_owners_.find(type.index);
  return found == foreign_operation_state_owners_.end()
             ? std::optional<ForeignOperationStateOwner>{}
             : std::optional(found->second);
}

TypeId SemIR::addCallbackRegistrationType(
    TypeId callback, TypeId handle, TypeId register_type,
    TypeId unregister_type, TypeId cancel_type,
    CallbackReleaseAuthority authority, std::uint32_t entry_parameter,
    std::uint32_t userdata_parameter, std::uint32_t release_parameter,
    std::vector<CallbackRegistrationBinding> bindings, TypeId cancel_async_type,
    TypeId wait_type, TypeId poll_type, TypeId arm_type, TypeId detach_type,
    std::array<std::uint32_t, 4> arm_parameters,
    std::array<std::uint32_t, 3> detach_parameters) {
  std::vector<TypeId> elements{callback, handle, register_type, unregister_type,
                               cancel_type};
  if (cancel_async_type.hasValue() && wait_type.hasValue()) {
    elements.push_back(cancel_async_type);
    elements.push_back(wait_type);
    if (poll_type.hasValue())
      elements.push_back(poll_type);
    if (arm_type.hasValue() && detach_type.hasValue()) {
      elements.push_back(arm_type);
      elements.push_back(detach_type);
    }
  }
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::CallbackRegistration;
  for (const auto element : elements)
    canonical.elements.push_back(canonicalType(element));
  canonical.registration_authority = static_cast<std::uint32_t>(authority);
  canonical.registration_entry_parameter = entry_parameter;
  canonical.registration_userdata_parameter = userdata_parameter;
  canonical.registration_release_parameter = release_parameter;
  canonical.registration_bindings = bindings;
  canonical.registration_arm_parameters = arm_parameters;
  canonical.registration_detach_parameters = detach_parameters;
  CanonicalForeignResourceProtocol protocol;
  for (const auto element : elements)
    protocol.types.push_back(canonicalType(element));
  protocol.facts = makeCallbackRegistrationProtocol(
      static_cast<std::uint8_t>(authority), entry_parameter, userdata_parameter,
      release_parameter, bindings, static_cast<std::uint32_t>(elements.size()),
      arm_parameters, detach_parameters);
  canonical.foreign_resource_protocol =
      values_->generics().internForeignResourceProtocol(std::move(protocol));
  const auto canonical_id =
      values_->generics().internType(std::move(canonical));
  const auto registration =
      addType({SemTypeKind::CallbackRegistration, addTypeBlock(elements).index,
               static_cast<std::uint32_t>(authority), canonical_id.index});
  if (elements.size() >= 7) {
    (void)addCallbackCompletionType(callback, handle, handle, wait_type,
                                    poll_type, authority, arm_type, detach_type,
                                    arm_parameters, detach_parameters);
  }
  return registration;
}

CallbackReleaseAuthority
SemIR::callbackRegistrationAuthority(TypeId type_id) const {
  assert(type(type_id).kind == SemTypeKind::CallbackRegistration);
  return static_cast<CallbackReleaseAuthority>(type(type_id).arg1);
}

CallbackReleaseAuthority
SemIR::callbackCompletionAuthority(TypeId type_id) const {
  assert(type(type_id).kind == SemTypeKind::CallbackCompletion);
  return static_cast<CallbackReleaseAuthority>(type(type_id).arg1);
}

ForeignResourceProtocolId
SemIR::foreignResourceProtocolId(TypeId type_id) const {
  const auto &value = type(type_id);
  if (value.kind == SemTypeKind::Nominal) {
    const auto &nominal = nominalType(NominalTypeId(value.arg0));
    assert(nominal.kind == NominalKind::ForeignResource);
    return nominal.foreign_resource_protocol;
  }
  if (value.kind == SemTypeKind::ForeignCompletion ||
      value.kind == SemTypeKind::ForeignWake)
    return nominalType(NominalTypeId(value.arg0)).foreign_resource_protocol;
  assert((value.kind == SemTypeKind::CallbackRegistration ||
          value.kind == SemTypeKind::CallbackCompletion) &&
         value.reserved != core::AnyId::InvalidIndex);
  return values_->generics()
      .type(CanonicalTypeId(value.reserved))
      .foreign_resource_protocol;
}

const CanonicalForeignResourceProtocol &
SemIR::foreignResourceProtocol(TypeId type_id) const {
  return values_->generics().foreignResourceProtocol(
      foreignResourceProtocolId(type_id));
}

std::array<std::uint32_t, 3>
SemIR::callbackRegistrationParameters(TypeId type_id) const {
  assert(type(type_id).kind == SemTypeKind::CallbackRegistration);
  const auto &canonical =
      values_->generics().type(CanonicalTypeId(type(type_id).reserved));
  return {canonical.registration_entry_parameter,
          canonical.registration_userdata_parameter,
          canonical.registration_release_parameter};
}

std::span<const CallbackRegistrationBinding>
SemIR::callbackRegistrationBindings(TypeId type_id) const {
  assert(type(type_id).kind == SemTypeKind::CallbackRegistration);
  return values_->generics()
      .type(CanonicalTypeId(type(type_id).reserved))
      .registration_bindings;
}

std::array<std::uint32_t, 4>
SemIR::callbackArmParameters(TypeId type_id) const {
  assert(type(type_id).kind == SemTypeKind::CallbackRegistration ||
         type(type_id).kind == SemTypeKind::CallbackCompletion);
  return values_->generics()
      .type(CanonicalTypeId(type(type_id).reserved))
      .registration_arm_parameters;
}

std::array<std::uint32_t, 3>
SemIR::callbackDetachParameters(TypeId type_id) const {
  assert(type(type_id).kind == SemTypeKind::CallbackRegistration ||
         type(type_id).kind == SemTypeKind::CallbackCompletion);
  return values_->generics()
      .type(CanonicalTypeId(type(type_id).reserved))
      .registration_detach_parameters;
}

std::uint32_t SemIR::callbackContextParameter(TypeId type_id) const {
  const auto &value = type(type_id);
  assert((value.kind == SemTypeKind::CFunctionPointer ||
          value.kind == SemTypeKind::CVariadicFunctionPointer) &&
         value.reserved != core::AnyId::InvalidIndex);
  return values_->generics()
      .type(CanonicalTypeId(value.reserved))
      .callable_context_parameter;
}

const CallableOwnershipSummary &SemIR::callbackContract(TypeId type_id) const {
  const auto &value = type(type_id);
  assert((value.kind == SemTypeKind::CFunctionPointer ||
          value.kind == SemTypeKind::CVariadicFunctionPointer) &&
         value.reserved != core::AnyId::InvalidIndex);
  return values_->generics()
      .type(CanonicalTypeId(value.reserved))
      .callable_contract;
}



} // namespace chtholly::compiler

