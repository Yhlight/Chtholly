#include "PublicInterfaceServices.h"

#include "PublicInterfaceEncodingInternal.h"

#include <algorithm>
#include <ranges>
#include <set>
#include <string>

namespace chtholly::compiler::internal {
namespace {

void appendForeignOperation(
    std::string &out, const interop::ForeignOperationArtifact &operation) {
  appendU32(out, static_cast<std::uint32_t>(operation.kind));
  appendU32(out, static_cast<std::uint32_t>(operation.capabilities.size()));
  for (const auto &capability : operation.capabilities) {
    appendField(out, capability.path);
    appendU32(out, static_cast<std::uint32_t>(capability.lanes.size()));
    for (const auto lane : capability.lanes)
      appendU32(out, lane);
    appendU32(out, static_cast<std::uint32_t>(capability.literals.size()));
    for (const auto &literal : capability.literals)
      appendField(out, literal);
    appendU32(out, static_cast<std::uint32_t>(capability.arguments.size()));
    for (const auto &argument : capability.arguments) {
      appendU32(out, static_cast<std::uint32_t>(argument.kind));
      appendU32(out, argument.lane);
      appendField(out, argument.literal);
      if (argument.kind ==
              interop::ForeignCapability::ArgumentKind::OperationReference ||
          argument.kind ==
              interop::ForeignCapability::ArgumentKind::AdapterReference)
        appendEntityReference(out, argument.entity);
    }
  }
  appendU32(out, static_cast<std::uint32_t>(operation.ports.size()));
  for (const auto &port : operation.ports) {
    appendU32(out, static_cast<std::uint32_t>(port.kind));
    appendU32(out, static_cast<std::uint32_t>(port.effect));
    appendField(out, port.path);
    appendU32(out, static_cast<std::uint32_t>(port.lanes.size()));
    for (const auto lane : port.lanes)
      appendU32(out, lane);
    appendU32(out, port.success ? 1U : 0U);
    appendU32(out, port.failure ? 1U : 0U);
  }
  appendU32(out, static_cast<std::uint32_t>(operation.success_payload));
  appendU32(out, static_cast<std::uint32_t>(operation.failure_payload));
  appendU32(out, static_cast<std::uint32_t>(operation.success_lanes.size()));
  for (const auto lane : operation.success_lanes)
    appendU32(out, lane);
  appendU32(out, static_cast<std::uint32_t>(operation.failure_lanes.size()));
  for (const auto lane : operation.failure_lanes)
    appendU32(out, lane);
  appendU32(out, static_cast<std::uint32_t>(operation.error_extractor));
  appendU32(out, static_cast<std::uint32_t>(operation.error_predicate));
  appendU32(out, static_cast<std::uint32_t>(operation.error_success_payload));
  appendU32(out, operation.error_predicate_width);
  appendU32(out, operation.error_predicate_signed ? 1U : 0U);
  appendU32(out, operation.error_predicate_inverted ? 1U : 0U);
  appendU32(out, static_cast<std::uint32_t>(operation.error_intervals.size()));
  for (const auto &interval : operation.error_intervals) {
    appendU64(out, interval.lower);
    appendU64(out, interval.upper);
  }
  appendU32(out, static_cast<std::uint32_t>(operation.outcome_projection));
  appendU32(out, operation.outcome_buffer_lane);
  appendU32(out, operation.outcome_capacity_lane);
  appendU32(out, operation.outcome_count_lane);
  appendU32(out, operation.outcome_context_lane);
  appendU32(out, operation.outcome_size_lane);
  appendField(out, operation.outcome_eof_symbol);
  appendField(out, operation.outcome_ferror_symbol);
  appendU32(out, operation.outcome_element_type.has_value() ? 1U : 0U);
  if (operation.outcome_element_type)
    appendType(out, *operation.outcome_element_type);
  appendU32(out, operation.outcome_count_type.has_value() ? 1U : 0U);
  if (operation.outcome_count_type)
    appendType(out, *operation.outcome_count_type);
  appendU32(out, static_cast<std::uint32_t>(operation.argument_sources.size()));
  for (const auto source : operation.argument_sources) {
    appendU32(out, static_cast<std::uint32_t>(source.kind));
    appendU32(out, source.index);
  }
  appendU32(out, operation.callback_authority);
  appendU32(out, static_cast<std::uint32_t>(operation.completion_family));
  appendU32(out, static_cast<std::uint32_t>(operation.completion_projection));
  appendU32(out, static_cast<std::uint32_t>(operation.completion_input_effect));
  appendU32(out, operation.completion_carrier_lane);
  appendU32(out, operation.completion_result_lane);
  appendU32(out, operation.readiness_success_literal);
  appendU32(out,
            static_cast<std::uint32_t>(operation.wake_callback_lanes.size()));
  for (const auto lane : operation.wake_callback_lanes)
    appendU32(out, lane);
  appendU32(out, operation.callback_adapter_layout.has_value() ? 1U : 0U);
  if (operation.callback_adapter_layout)
    appendType(out, *operation.callback_adapter_layout);
  appendU32(out, operation.waker_adapter_layout.has_value() ? 1U : 0U);
  if (operation.waker_adapter_layout)
    appendType(out, *operation.waker_adapter_layout);
  appendU32(out, operation.completion_descriptor.has_value() ? 1U : 0U);
  if (operation.completion_descriptor) {
    const auto &family = *operation.completion_descriptor;
    appendEntityReference(out, family.source);
    appendEntityReference(out, family.wait);
    appendEntityReference(out, family.poll);
    appendEntityReference(out, family.arm);
    appendEntityReference(out, family.detach);
    appendType(out, family.completion_carrier);
    appendType(out, family.registration_callback_adapter);
    appendType(out, family.waker_callback_adapter);
    appendU32(out, family.source_handle_lane);
    appendU32(out, family.token_result_lane);
    appendU32(out, static_cast<std::uint32_t>(family.arm_lane_map.size()));
    for (const auto lane : family.arm_lane_map)
      appendU32(out, lane);
    appendU32(out, static_cast<std::uint32_t>(family.detach_lane_map.size()));
    for (const auto lane : family.detach_lane_map)
      appendU32(out, lane);
    appendU32(out, family.authority);
    appendU32(out, family.readiness_literal);
    appendU32(out, static_cast<std::uint32_t>(family.input_effect));
    appendU32(out, family.abi_epoch);
  }
  appendU32(out, operation.abi_epoch);
  out.append(reinterpret_cast<const char *>(operation.fingerprint.bytes().data()),
             operation.fingerprint.bytes().size());
}

} // namespace

StableFingerprint PublicInterfaceForeignOperationService::fingerprint(
    std::span<const PublicType> parameters, PublicType result,
    interop::ForeignOperationArtifact operation) {
  operation.fingerprint = {};
  std::string input;
  appendField(input, "chtholly.next.foreign-operation.v9");
  appendU32(input, static_cast<std::uint32_t>(parameters.size()));
  for (const auto &parameter : parameters)
    appendType(input, parameter);
  appendType(input, result);
  appendForeignOperation(input, operation);
  return StableFingerprint::fromCanonicalBytes(input);
}

bool PublicInterfaceForeignOperationService::valid(
    const interop::ForeignOperationArtifact &operation,
    std::span<const PublicType> parameters, PublicType result) {
  const auto lane_count = parameters.size() + 1U;
  std::string operation_error;
  if (operation.kind >= interop::ForeignOperationKind::Count ||
      operation.capabilities.empty() || !operation.fingerprint.hasValue() ||
      !operation.verify(operation_error) ||
      operation.fingerprint != fingerprint(parameters, result, operation))
    return false;
  if ((operation.error_extractor ==
       interop::ForeignOperationArtifact::ErrorExtractor::None) !=
          (operation.error_predicate ==
           interop::ForeignOperationArtifact::ErrorPredicate::None) ||
      (operation.error_extractor ==
       interop::ForeignOperationArtifact::ErrorExtractor::None) !=
          (operation.error_success_payload ==
           interop::ForeignOperationArtifact::ErrorSuccessPayload::None) ||
      operation.error_extractor >=
          interop::ForeignOperationArtifact::ErrorExtractor::Count ||
      operation.error_predicate >=
          interop::ForeignOperationArtifact::ErrorPredicate::Count ||
      operation.error_success_payload >=
          interop::ForeignOperationArtifact::ErrorSuccessPayload::Count)
    return false;
  const auto public_type_for_physical =
      [&](std::uint32_t lane) -> const PublicType * {
    if (lane >= operation.argument_sources.size())
      return nullptr;
    const auto source = operation.argument_sources[lane];
    if (source.kind != interop::ForeignOperationArtifact::ArgumentSourceKind::
                           PublicArgument ||
        source.index >= parameters.size())
      return nullptr;
    return &parameters[source.index];
  };
  if (operation.outcome_projection ==
      interop::ForeignOperationArtifact::OutcomeProjection::PosixRead) {
    const auto *buffer =
        public_type_for_physical(operation.outcome_buffer_lane);
    const auto *capacity =
        public_type_for_physical(operation.outcome_capacity_lane);
    if (!buffer || !capacity ||
        operation.outcome_buffer_lane == operation.outcome_capacity_lane ||
        buffer->kind != PublicTypeKind::RawPointer ||
        capacity->kind != PublicTypeKind::Integer || capacity->integer_signed ||
        result.kind != PublicTypeKind::Integer || !result.integer_signed ||
        capacity->scalar_width != result.scalar_width ||
        !operation.outcome_element_type ||
        operation.outcome_element_type->kind != PublicTypeKind::Integer ||
        operation.outcome_element_type->scalar_width != 8)
      return false;
  }
  if (operation.outcome_projection ==
      interop::ForeignOperationArtifact::OutcomeProjection::Win32Read) {
    const auto *buffer =
        public_type_for_physical(operation.outcome_buffer_lane);
    const auto *capacity =
        public_type_for_physical(operation.outcome_capacity_lane);
    if (!buffer || !capacity || buffer->kind != PublicTypeKind::RawPointer ||
        capacity->kind != PublicTypeKind::Integer || capacity->integer_signed ||
        capacity->scalar_width != 32 || result.kind != PublicTypeKind::Integer ||
        result.scalar_width != 32 || !result.integer_signed)
      return false;
  }
  if (operation.outcome_projection ==
      interop::ForeignOperationArtifact::OutcomeProjection::Fread) {
    const auto *buffer =
        public_type_for_physical(operation.outcome_buffer_lane);
    const auto *size = public_type_for_physical(operation.outcome_size_lane);
    const auto *count =
        public_type_for_physical(operation.outcome_capacity_lane);
    const auto *stream =
        public_type_for_physical(operation.outcome_context_lane);
    if (!buffer || !size || !count || !stream ||
        buffer->kind != PublicTypeKind::RawPointer ||
        size->kind != PublicTypeKind::Integer || size->integer_signed ||
        count->kind != PublicTypeKind::Integer || count->integer_signed ||
        size->scalar_width != count->scalar_width ||
        result.kind != PublicTypeKind::Integer || result.integer_signed ||
        result.scalar_width != count->scalar_width ||
        stream->kind != PublicTypeKind::RawPointer ||
        operation.outcome_eof_symbol.empty() ||
        operation.outcome_ferror_symbol.empty() ||
        operation.error_extractor !=
            interop::ForeignOperationArtifact::ErrorExtractor::Errno ||
        operation.error_predicate !=
            interop::ForeignOperationArtifact::ErrorPredicate::IntegerSet ||
        !operation.error_predicate_inverted || operation.error_intervals.size() != 1 ||
        operation.error_intervals.front().lower != 0 ||
        operation.error_intervals.front().upper != 0 ||
        operation.argument_sources.size() != parameters.size())
      return false;
  }
  const auto valid_lanes = [&](std::span<const std::uint32_t> lanes) {
    if (lanes.empty() ||
        !std::ranges::all_of(lanes, [&](auto lane) { return lane < lane_count; }))
      return false;
    std::set<std::uint32_t> unique(lanes.begin(), lanes.end());
    return unique.size() == lanes.size();
  };
  for (std::size_t index = 0; index < operation.capabilities.size(); ++index) {
    const auto &capability = operation.capabilities[index];
    if (capability.path.empty() ||
        (!capability.lanes.empty() && !valid_lanes(capability.lanes)) ||
        std::ranges::any_of(capability.literals,
                            [](const auto &literal) { return literal.empty(); }) ||
        std::ranges::any_of(capability.arguments, [&](const auto &argument) {
          using K = interop::ForeignCapability::ArgumentKind;
          if (argument.kind >= K::Count)
            return true;
          if (argument.kind == K::Lane)
            return argument.lane >= lane_count;
          if (argument.kind == K::Literal)
            return argument.literal.empty();
          if (argument.kind == K::AdapterReference)
            return argument.entity.canonical_name.empty();
          return !PublicInterfaceTypeValidationService::validEntityReference(
              argument.entity, PublicEntityKind::ForeignOperation);
        }) ||
        (index != 0 && operation.capabilities[index - 1] >= capability))
      return false;
  }
  for (std::size_t index = 0; index < operation.ports.size(); ++index) {
    const auto &port = operation.ports[index];
    if (port.kind >= interop::ForeignPortKind::Count ||
        port.effect >= interop::ForeignPortEffect::Count || port.path.empty() ||
        !valid_lanes(port.lanes) ||
        (index != 0 && operation.ports[index - 1] >= port))
      return false;
  }
  for (std::size_t left = 0; left < operation.ports.size(); ++left)
    for (std::size_t right = left + 1; right < operation.ports.size(); ++right) {
      const auto &lhs = operation.ports[left];
      const auto &rhs = operation.ports[right];
      if (lhs.kind != rhs.kind)
        continue;
      if (std::ranges::any_of(lhs.lanes, [&](auto lane) {
            return std::ranges::find(rhs.lanes, lane) != rhs.lanes.end();
          }))
        return false;
    }
  if (std::ranges::count(operation.ports, interop::ForeignPortKind::Callback,
                         &interop::ForeignLogicalPort::kind) > 1)
    return false;
  if (operation.success_payload >= interop::ForeignPayloadKind::Count ||
      operation.failure_payload >= interop::ForeignPayloadKind::Count ||
      operation.completion_family >= interop::ForeignCompletionFamily::Count ||
      operation.completion_projection >=
          interop::ForeignCompletionProjectionKind::Count ||
      operation.completion_input_effect >=
          interop::ForeignCompletionInputEffect::Count ||
      operation.abi_epoch == 0 || operation.callback_authority > 2)
    return false;
  const auto valid_payload = [](interop::ForeignPayloadKind kind,
                                std::span<const std::uint32_t> lanes) {
    return (kind == interop::ForeignPayloadKind::Void && lanes.empty()) ||
           (kind == interop::ForeignPayloadKind::Exact && lanes.size() == 1) ||
           (kind == interop::ForeignPayloadKind::Tuple && lanes.size() > 1);
  };
  if (!valid_payload(operation.success_payload, operation.success_lanes) ||
      !valid_payload(operation.failure_payload, operation.failure_lanes) ||
      !std::ranges::is_sorted(operation.success_lanes) ||
      !std::ranges::is_sorted(operation.failure_lanes) ||
      std::ranges::adjacent_find(operation.success_lanes) !=
          operation.success_lanes.end() ||
      std::ranges::adjacent_find(operation.failure_lanes) !=
          operation.failure_lanes.end() ||
      !std::ranges::all_of(operation.success_lanes,
                           [&](auto lane) { return lane < lane_count; }) ||
      !std::ranges::all_of(operation.failure_lanes,
                           [&](auto lane) { return lane < lane_count; }) ||
      !std::ranges::is_sorted(operation.wake_callback_lanes) ||
      std::ranges::adjacent_find(operation.wake_callback_lanes) !=
          operation.wake_callback_lanes.end() ||
      !std::ranges::all_of(operation.wake_callback_lanes,
                           [&](auto lane) { return lane < lane_count; }))
    return false;
  if (operation.completion_descriptor) {
    const auto &family = *operation.completion_descriptor;
    const auto valid_operation_ref = [&](const auto &reference) {
      return PublicInterfaceTypeValidationService::validEntityReference(
          reference, PublicEntityKind::ForeignOperation);
    };
    if (operation.completion_family != interop::ForeignCompletionFamily::Async ||
        operation.completion_projection !=
            interop::ForeignCompletionProjectionKind::ScalarToCompletion ||
        !valid_operation_ref(family.source) ||
        !valid_operation_ref(family.wait) || !valid_operation_ref(family.poll) ||
        !valid_operation_ref(family.arm) || !valid_operation_ref(family.detach) ||
        family.completion_carrier != result ||
        !PublicInterfaceTypeValidationService::validPublicType(
            family.completion_carrier, 0, false) ||
        !PublicInterfaceTypeValidationService::validPublicType(
            family.registration_callback_adapter, 0, false) ||
        !PublicInterfaceTypeValidationService::validPublicType(
            family.waker_callback_adapter, 0, false) ||
        family.registration_callback_adapter.kind !=
            PublicTypeKind::CallbackAdapter ||
        family.waker_callback_adapter.kind != PublicTypeKind::CallbackAdapter ||
        !validCallbackAdapterContract(family.registration_callback_adapter) ||
        !validCallbackAdapterContract(family.waker_callback_adapter) ||
        family.source_handle_lane == core::AnyId::InvalidIndex ||
        family.token_result_lane != parameters.size() || family.authority > 2 ||
        family.abi_epoch == 0 ||
        family.input_effect >= interop::ForeignCompletionInputEffect::Count ||
        family.arm_lane_map.empty() || family.detach_lane_map.empty() ||
        !std::ranges::all_of(family.arm_lane_map,
                             [&](auto lane) { return lane < family.arm_lane_map.size(); }) ||
        !std::ranges::all_of(family.detach_lane_map,
                             [&](auto lane) { return lane < family.detach_lane_map.size(); }) ||
        !std::ranges::is_sorted(family.arm_lane_map) ||
        !std::ranges::is_sorted(family.detach_lane_map) ||
        std::ranges::adjacent_find(family.arm_lane_map) !=
            family.arm_lane_map.end() ||
        std::ranges::adjacent_find(family.detach_lane_map) !=
            family.detach_lane_map.end())
      return false;
  } else if (operation.completion_family == interop::ForeignCompletionFamily::Async &&
             operation.completion_projection ==
                 interop::ForeignCompletionProjectionKind::ScalarToCompletion) {
    return false;
  }
  if ((operation.callback_adapter_layout &&
       (!PublicInterfaceTypeValidationService::validPublicType(
            *operation.callback_adapter_layout, 0, false) ||
        operation.callback_adapter_layout->kind !=
            PublicTypeKind::CallbackAdapter)) ||
      (operation.waker_adapter_layout &&
       (!PublicInterfaceTypeValidationService::validPublicType(
            *operation.waker_adapter_layout, 0, false) ||
        operation.waker_adapter_layout->kind != PublicTypeKind::CallbackAdapter)))
    return false;
  return true;
}

} // namespace chtholly::compiler::internal
