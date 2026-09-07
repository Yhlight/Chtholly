// Included by IncrementalDependenciesCodecInternal.h; this fragment keeps
// canonical package-manifest codec helpers in the private namespace.
void appendForeignSignature(
    std::string &out, const std::optional<ForeignAbiSignature> &signature) {
  out.push_back(signature ? 1 : 0);
  if (!signature)
    return;
  out.push_back(signature->is_variadic ? 1 : 0);
  out.push_back(static_cast<char>(signature->unwind_policy));
  out.push_back(static_cast<char>(signature->calling_convention));
  const auto append_value = [&](const ForeignAbiValue &value) {
    out.push_back(static_cast<char>(value.kind));
    appendU32(out, value.width);
    out.push_back(value.pointee_const ? 1 : 0);
  };
  append_value(signature->result);
  appendU32(out, static_cast<std::uint32_t>(signature->parameters.size()));
  for (const auto &parameter : signature->parameters)
    append_value(parameter);
}

bool readForeignSignature(StateReader &reader,
                          std::optional<ForeignAbiSignature> &signature) {
  std::uint8_t present = 0;
  if (!reader.readU8(present) || present > 1)
    return false;
  if (present == 0) {
    signature.reset();
    return true;
  }
  signature.emplace();
  std::uint8_t is_variadic = 0;
  std::uint8_t unwind_policy = 0;
  std::uint8_t calling_convention = 0;
  if (!reader.readU8(is_variadic) || is_variadic > 1 ||
      !reader.readU8(unwind_policy) ||
      unwind_policy >=
          static_cast<std::uint8_t>(ForeignAbiSignature::UnwindPolicy::Count) ||
      !reader.readU8(calling_convention) ||
      calling_convention >=
          static_cast<std::uint8_t>(ForeignCallingConvention::Count))
    return false;
  signature->is_variadic = is_variadic != 0;
  signature->unwind_policy =
      static_cast<ForeignAbiSignature::UnwindPolicy>(unwind_policy);
  signature->calling_convention =
      static_cast<ForeignCallingConvention>(calling_convention);
  const auto read_value = [&](ForeignAbiValue &value) {
    std::uint8_t kind = 0;
    std::uint8_t pointee_const = 0;
    if (!reader.readU8(kind) || !reader.readU32(value.width) ||
        !reader.readU8(pointee_const) || pointee_const > 1)
      return false;
    value.kind = static_cast<ForeignAbiValueKind>(kind);
    value.pointee_const = pointee_const != 0;
    return true;
  };
  std::uint32_t parameter_count = 0;
  if (!read_value(signature->result) || !reader.readU32(parameter_count) ||
      !reader.readRecords(parameter_count, 6))
    return false;
  signature->parameters.resize(parameter_count);
  for (auto &parameter : signature->parameters)
    if (!read_value(parameter))
      return false;
  std::string error;
  return signature->verify(error);
}

void appendForeignOperation(
    std::string &out, const interop::ForeignOperationArtifact &operation) {
  out.push_back(static_cast<char>(operation.kind));
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
      out.push_back(static_cast<char>(argument.kind));
      appendU32(out, argument.lane);
      appendField(out, argument.literal);
      const auto reference =
          argument.kind ==
              interop::ForeignCapability::ArgumentKind::OperationReference ||
          argument.kind ==
              interop::ForeignCapability::ArgumentKind::AdapterReference;
      if (reference)
        appendPublicEntity(out, argument.entity);
    }
  }
  appendU32(out, static_cast<std::uint32_t>(operation.ports.size()));
  for (const auto &port : operation.ports) {
    out.push_back(static_cast<char>(port.kind));
    out.push_back(static_cast<char>(port.effect));
    appendField(out, port.path);
    appendU32(out, static_cast<std::uint32_t>(port.lanes.size()));
    for (const auto lane : port.lanes)
      appendU32(out, lane);
    out.push_back(port.success ? 1 : 0);
    out.push_back(port.failure ? 1 : 0);
  }
  appendU32(out, static_cast<std::uint32_t>(operation.success_payload));
  appendU32(out, static_cast<std::uint32_t>(operation.failure_payload));
  appendU32(out, static_cast<std::uint32_t>(operation.success_lanes.size()));
  for (const auto lane : operation.success_lanes)
    appendU32(out, lane);
  appendU32(out, static_cast<std::uint32_t>(operation.failure_lanes.size()));
  for (const auto lane : operation.failure_lanes)
    appendU32(out, lane);
  appendU32(out, operation.status_lane);
  appendU32(out, static_cast<std::uint32_t>(operation.status_success_literal));
  out.push_back(operation.out_initialized_on_failure ? 1 : 0);
  appendU32(out, static_cast<std::uint32_t>(operation.out_lanes.size()));
  for (const auto lane : operation.out_lanes)
    appendU32(out, lane);
  out.push_back(static_cast<char>(operation.error_extractor));
  out.push_back(static_cast<char>(operation.error_predicate));
  out.push_back(static_cast<char>(operation.error_success_payload));
  appendU32(out, operation.error_predicate_width);
  out.push_back(operation.error_predicate_signed ? 1 : 0);
  out.push_back(operation.error_predicate_inverted ? 1 : 0);
  appendU32(out, static_cast<std::uint32_t>(operation.error_intervals.size()));
  for (const auto &interval : operation.error_intervals) {
    appendU32(out, static_cast<std::uint32_t>(interval.lower));
    appendU32(out, static_cast<std::uint32_t>(interval.lower >> 32U));
    appendU32(out, static_cast<std::uint32_t>(interval.upper));
    appendU32(out, static_cast<std::uint32_t>(interval.upper >> 32U));
  }
  out.push_back(static_cast<char>(operation.outcome_projection));
  appendU32(out, operation.outcome_buffer_lane);
  appendU32(out, operation.outcome_capacity_lane);
  appendU32(out, operation.outcome_count_lane);
  appendU32(out, operation.outcome_context_lane);
  appendU32(out, operation.outcome_size_lane);
  appendField(out, operation.outcome_eof_symbol);
  appendField(out, operation.outcome_ferror_symbol);
  out.push_back(operation.outcome_element_type ? 1 : 0);
  if (operation.outcome_element_type)
    appendPublicType(out, *operation.outcome_element_type);
  out.push_back(operation.outcome_count_type ? 1 : 0);
  if (operation.outcome_count_type)
    appendPublicType(out, *operation.outcome_count_type);
  appendU32(out, static_cast<std::uint32_t>(operation.argument_sources.size()));
  for (const auto source : operation.argument_sources) {
    out.push_back(static_cast<char>(source.kind));
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
  for (const auto *events :
       {&operation.completion_events, &operation.cancel_events,
        &operation.wake_events}) {
    appendU32(out, static_cast<std::uint32_t>(events->size()));
    for (const auto &event : *events)
      appendFingerprint(out, event);
  }
  out.push_back(operation.requires_quiescence ? 1 : 0);
  out.push_back(operation.discharges_quiescence ? 1 : 0);
  out.push_back(operation.callback_adapter_layout ? 1 : 0);
  if (operation.callback_adapter_layout)
    appendPublicType(out, *operation.callback_adapter_layout);
  out.push_back(operation.waker_adapter_layout ? 1 : 0);
  if (operation.waker_adapter_layout)
    appendPublicType(out, *operation.waker_adapter_layout);
  out.push_back(operation.completion_descriptor ? 1 : 0);
  if (operation.completion_descriptor) {
    const auto &family = *operation.completion_descriptor;
    appendPublicEntity(out, family.source);
    appendPublicEntity(out, family.wait);
    appendPublicEntity(out, family.poll);
    appendPublicEntity(out, family.arm);
    appendPublicEntity(out, family.detach);
    appendPublicType(out, family.completion_carrier);
    appendPublicType(out, family.registration_callback_adapter);
    appendPublicType(out, family.waker_callback_adapter);
    appendU32(out, family.source_handle_lane);
    appendU32(out, family.token_result_lane);
    appendU32(out, static_cast<std::uint32_t>(family.arm_lane_map.size()));
    for (const auto lane : family.arm_lane_map)
      appendU32(out, lane);
    appendU32(out, static_cast<std::uint32_t>(family.detach_lane_map.size()));
    for (const auto lane : family.detach_lane_map)
      appendU32(out, lane);
    out.push_back(static_cast<char>(family.authority));
    appendU32(out, family.readiness_literal);
    out.push_back(static_cast<char>(family.input_effect));
    appendU32(out, family.abi_epoch);
  }
  appendU32(out, operation.abi_epoch);
  appendFingerprint(out, operation.fingerprint);
}

void appendInteropArtifactReference(
    std::string &out, const interop::ArtifactReference &reference) {
  appendU32(out, reference.schema_epoch);
  appendField(out, reference.canonical_package);
  appendField(out, reference.canonical_module);
  appendField(out, reference.canonical_name);
  appendFingerprint(out, reference.fingerprint);
}

bool readForeignOperationPayload(
    StateReader &reader,
    std::optional<interop::ForeignOperationArtifact> &operation) {
  std::uint8_t present = 0;
  if (!reader.readU8(present) || present > 1)
    return false;
  if (!present) {
    operation.reset();
    return true;
  }
  operation.emplace();
  auto &value = *operation;
  std::uint8_t kind = 0;
  std::uint32_t count = 0;
  if (!reader.readU8(kind) ||
      kind >= static_cast<std::uint8_t>(interop::ForeignOperationKind::Count) ||
      !reader.readU32(count) || !reader.readRecords(count, 8))
    return false;
  value.kind = static_cast<interop::ForeignOperationKind>(kind);
  value.capabilities.resize(count);
  for (auto &capability : value.capabilities) {
    std::uint32_t lanes = 0, literals = 0;
    if (!reader.readString(capability.path) || !reader.readU32(lanes) ||
        !reader.readRecords(lanes, 4))
      return false;
    capability.lanes.resize(lanes);
    for (auto &lane : capability.lanes)
      if (!reader.readU32(lane))
        return false;
    if (!reader.readU32(literals) || !reader.readRecords(literals, 4))
      return false;
    capability.literals.resize(literals);
    for (auto &literal : capability.literals)
      if (!reader.readString(literal))
        return false;
    std::uint32_t argument_count = 0;
    if (!reader.readU32(argument_count) ||
        !reader.readRecords(argument_count, 8))
      return false;
    capability.arguments.resize(argument_count);
    for (auto &argument : capability.arguments) {
      std::uint8_t argument_kind = 0;
      if (!reader.readU8(argument_kind) ||
          argument_kind >=
              static_cast<std::uint8_t>(
                  interop::ForeignCapability::ArgumentKind::Count) ||
          !reader.readU32(argument.lane) ||
          !reader.readString(argument.literal))
        return false;
      argument.kind =
          static_cast<interop::ForeignCapability::ArgumentKind>(argument_kind);
      if ((argument.kind ==
               interop::ForeignCapability::ArgumentKind::OperationReference ||
           argument.kind ==
               interop::ForeignCapability::ArgumentKind::AdapterReference) &&
          !readPublicEntity(reader, argument.entity))
        return false;
    }
  }
  if (!reader.readU32(count) || !reader.readRecords(count, 8))
    return false;
  value.ports.resize(count);
  for (auto &port : value.ports) {
    std::uint8_t port_kind = 0, effect = 0, success = 0, failure = 0;
    std::uint32_t lanes = 0;
    if (!reader.readU8(port_kind) || !reader.readU8(effect) ||
        port_kind >=
            static_cast<std::uint8_t>(interop::ForeignPortKind::Count) ||
        effect >=
            static_cast<std::uint8_t>(interop::ForeignPortEffect::Count) ||
        !reader.readString(port.path) || !reader.readU32(lanes) ||
        !reader.readRecords(lanes, 4))
      return false;
    port.kind = static_cast<interop::ForeignPortKind>(port_kind);
    port.effect = static_cast<interop::ForeignPortEffect>(effect);
    port.lanes.resize(lanes);
    for (auto &lane : port.lanes)
      if (!reader.readU32(lane))
        return false;
    if (!reader.readU8(success) || !reader.readU8(failure) || success > 1 ||
        failure > 1)
      return false;
    port.success = success != 0;
    port.failure = failure != 0;
  }
  std::uint32_t success_payload = 0;
  std::uint32_t failure_payload = 0;
  std::uint32_t lane_count = 0;
  if (!reader.readU32(success_payload) || !reader.readU32(failure_payload) ||
      success_payload >=
          static_cast<std::uint32_t>(interop::ForeignPayloadKind::Count) ||
      failure_payload >=
          static_cast<std::uint32_t>(interop::ForeignPayloadKind::Count))
    return false;
  value.success_payload =
      static_cast<interop::ForeignPayloadKind>(success_payload);
  value.failure_payload =
      static_cast<interop::ForeignPayloadKind>(failure_payload);
  if (!reader.readU32(lane_count) || !reader.readRecords(lane_count, 4))
    return false;
  value.success_lanes.resize(lane_count);
  for (auto &lane : value.success_lanes)
    if (!reader.readU32(lane))
      return false;
  if (!reader.readU32(lane_count) || !reader.readRecords(lane_count, 4))
    return false;
  value.failure_lanes.resize(lane_count);
  for (auto &lane : value.failure_lanes)
    if (!reader.readU32(lane))
      return false;
  std::uint32_t status_success_literal = 0;
  std::uint8_t out_initialized_on_failure = 0;
  if (!reader.readU32(value.status_lane) ||
      !reader.readU32(status_success_literal) ||
      !reader.readU8(out_initialized_on_failure) ||
      out_initialized_on_failure > 1 || !reader.readU32(lane_count) ||
      !reader.readRecords(lane_count, 4))
    return false;
  value.status_success_literal =
      static_cast<std::int32_t>(status_success_literal);
  value.out_initialized_on_failure = out_initialized_on_failure != 0;
  value.out_lanes.resize(lane_count);
  for (auto &lane : value.out_lanes)
    if (!reader.readU32(lane))
      return false;
  std::uint8_t error_extractor = 0, error_predicate = 0, error_payload = 0;
  std::uint8_t error_signed = 0, error_inverted = 0;
  std::uint32_t error_interval_count = 0;
  if (!reader.readU8(error_extractor) ||
      error_extractor >=
          static_cast<std::uint8_t>(
              interop::ForeignOperationArtifact::ErrorExtractor::Count) ||
      !reader.readU8(error_predicate) ||
      error_predicate >=
          static_cast<std::uint8_t>(
              interop::ForeignOperationArtifact::ErrorPredicate::Count) ||
      !reader.readU8(error_payload) ||
      error_payload >=
          static_cast<std::uint8_t>(
              interop::ForeignOperationArtifact::ErrorSuccessPayload::Count) ||
      !reader.readU32(value.error_predicate_width) ||
      !reader.readU8(error_signed) || error_signed > 1 ||
      !reader.readU8(error_inverted) || error_inverted > 1 ||
      !reader.readU32(error_interval_count) || error_interval_count > 64 ||
      !reader.readRecords(error_interval_count, 16))
    return false;
  value.error_extractor =
      static_cast<interop::ForeignOperationArtifact::ErrorExtractor>(
          error_extractor);
  value.error_predicate =
      static_cast<interop::ForeignOperationArtifact::ErrorPredicate>(
          error_predicate);
  value.error_success_payload =
      static_cast<interop::ForeignOperationArtifact::ErrorSuccessPayload>(
          error_payload);
  value.error_predicate_signed = error_signed != 0;
  value.error_predicate_inverted = error_inverted != 0;
  value.error_intervals.resize(error_interval_count);
  for (auto &interval : value.error_intervals) {
    std::uint32_t lower_low = 0, lower_high = 0, upper_low = 0, upper_high = 0;
    if (!reader.readU32(lower_low) || !reader.readU32(lower_high) ||
        !reader.readU32(upper_low) || !reader.readU32(upper_high))
      return false;
    interval.lower = static_cast<std::uint64_t>(lower_low) |
                     (static_cast<std::uint64_t>(lower_high) << 32U);
    interval.upper = static_cast<std::uint64_t>(upper_low) |
                     (static_cast<std::uint64_t>(upper_high) << 32U);
  }
  std::uint8_t outcome_projection = 0, has_outcome_element = 0,
               has_outcome_count = 0;
  if (!reader.readU8(outcome_projection) ||
      outcome_projection >=
          static_cast<std::uint8_t>(
              interop::ForeignOperationArtifact::OutcomeProjection::Count) ||
      !reader.readU32(value.outcome_buffer_lane) ||
      !reader.readU32(value.outcome_capacity_lane) ||
      !reader.readU32(value.outcome_count_lane) ||
      !reader.readU32(value.outcome_context_lane) ||
      !reader.readU32(value.outcome_size_lane) ||
      !reader.readString(value.outcome_eof_symbol) ||
      !reader.readString(value.outcome_ferror_symbol) ||
      !reader.readU8(has_outcome_element) || has_outcome_element > 1)
    return false;
  value.outcome_projection =
      static_cast<interop::ForeignOperationArtifact::OutcomeProjection>(
          outcome_projection);
  if (has_outcome_element) {
    value.outcome_element_type = PublicType{};
    if (!readPublicType(reader, *value.outcome_element_type))
      return false;
  }
  if (!reader.readU8(has_outcome_count) || has_outcome_count > 1)
    return false;
  if (has_outcome_count) {
    value.outcome_count_type = PublicType{};
    if (!readPublicType(reader, *value.outcome_count_type))
      return false;
  }
  std::uint32_t argument_source_count = 0;
  if (!reader.readU32(argument_source_count) ||
      !reader.readRecords(argument_source_count, 5))
    return false;
  value.argument_sources.resize(argument_source_count);
  for (auto &source : value.argument_sources) {
    std::uint8_t source_kind = 0;
    if (!reader.readU8(source_kind) ||
        source_kind >=
            static_cast<std::uint8_t>(
                interop::ForeignOperationArtifact::ArgumentSourceKind::Count) ||
        !reader.readU32(source.index))
      return false;
    source.kind =
        static_cast<interop::ForeignOperationArtifact::ArgumentSourceKind>(
            source_kind);
  }
  std::uint32_t completion_family = 0;
  std::uint32_t callback_authority = 0;
  std::uint32_t projection = 0;
  std::uint32_t input_effect = 0;
  std::uint32_t wake_lanes = 0;
  if (!reader.readU32(callback_authority) || callback_authority > 255 ||
      !reader.readU32(completion_family) || !reader.readU32(projection) ||
      !reader.readU32(input_effect) ||
      !reader.readU32(value.completion_carrier_lane) ||
      !reader.readU32(value.completion_result_lane) ||
      !reader.readU32(value.readiness_success_literal) ||
      !reader.readU32(wake_lanes) || !reader.readRecords(wake_lanes, 4) ||
      completion_family >=
          static_cast<std::uint32_t>(interop::ForeignCompletionFamily::Count) ||
      projection >= static_cast<std::uint32_t>(
                        interop::ForeignCompletionProjectionKind::Count) ||
      input_effect >= static_cast<std::uint32_t>(
                          interop::ForeignCompletionInputEffect::Count))
    return false;
  value.callback_authority = static_cast<std::uint8_t>(callback_authority);
  value.completion_family =
      static_cast<interop::ForeignCompletionFamily>(completion_family);
  value.completion_projection =
      static_cast<interop::ForeignCompletionProjectionKind>(projection);
  value.completion_input_effect =
      static_cast<interop::ForeignCompletionInputEffect>(input_effect);
  value.wake_callback_lanes.resize(wake_lanes);
  for (auto &lane : value.wake_callback_lanes)
    if (!reader.readU32(lane))
      return false;
  for (auto *events :
       {&value.completion_events, &value.cancel_events, &value.wake_events}) {
    std::uint32_t event_count = 0;
    if (!reader.readU32(event_count) || !reader.readRecords(event_count, 32))
      return false;
    events->resize(event_count);
    for (auto &event : *events)
      if (!reader.readFingerprint(event))
        return false;
  }
  std::uint8_t requires_quiescence = 0, discharges_quiescence = 0;
  if (!reader.readU8(requires_quiescence) ||
      !reader.readU8(discharges_quiescence) || requires_quiescence > 1 ||
      discharges_quiescence > 1)
    return false;
  value.requires_quiescence = requires_quiescence != 0;
  value.discharges_quiescence = discharges_quiescence != 0;
  std::uint8_t has_callback_layout = 0, has_waker_layout = 0;
  if (!reader.readU8(has_callback_layout) || has_callback_layout > 1)
    return false;
  if (has_callback_layout) {
    value.callback_adapter_layout = PublicType{};
    if (!readPublicType(reader, *value.callback_adapter_layout))
      return false;
  }
  if (!reader.readU8(has_waker_layout) || has_waker_layout > 1)
    return false;
  if (has_waker_layout) {
    value.waker_adapter_layout = PublicType{};
    if (!readPublicType(reader, *value.waker_adapter_layout))
      return false;
  }
  std::uint8_t has_family = 0;
  if (!reader.readU8(has_family) || has_family > 1)
    return false;
  if (has_family) {
    value.completion_descriptor = interop::CompletionFamily{};
    auto &family = *value.completion_descriptor;
    std::uint32_t arm_count = 0, detach_count = 0;
    std::uint8_t authority = 0, effect = 0;
    if (!readPublicEntity(reader, family.source) ||
        !readPublicEntity(reader, family.wait) ||
        !readPublicEntity(reader, family.poll) ||
        !readPublicEntity(reader, family.arm) ||
        !readPublicEntity(reader, family.detach) ||
        !readPublicType(reader, family.completion_carrier) ||
        !readPublicType(reader, family.registration_callback_adapter) ||
        !readPublicType(reader, family.waker_callback_adapter) ||
        !reader.readU32(family.source_handle_lane) ||
        !reader.readU32(family.token_result_lane) ||
        !reader.readU32(arm_count) || !reader.readRecords(arm_count, 4))
      return false;
    family.arm_lane_map.resize(arm_count);
    for (auto &lane : family.arm_lane_map)
      if (!reader.readU32(lane))
        return false;
    if (!reader.readU32(detach_count) || !reader.readRecords(detach_count, 4))
      return false;
    family.detach_lane_map.resize(detach_count);
    for (auto &lane : family.detach_lane_map)
      if (!reader.readU32(lane))
        return false;
    if (!reader.readU8(authority) || authority > 2 ||
        !reader.readU32(family.readiness_literal) || !reader.readU8(effect) ||
        effect >= static_cast<std::uint8_t>(
                      interop::ForeignCompletionInputEffect::Count) ||
        !reader.readU32(family.abi_epoch))
      return false;
    family.authority = authority;
    family.input_effect =
        static_cast<interop::ForeignCompletionInputEffect>(effect);
  }
  return reader.readU32(value.abi_epoch) &&
         reader.readFingerprint(value.fingerprint);
}

bool readInteropArtifactReference(
    StateReader &reader, std::optional<interop::ArtifactReference> &reference) {
  std::uint8_t present = 0;
  if (!reader.readU8(present) || present > 1)
    return false;
  if (!present) {
    reference.reset();
    return true;
  }
  reference.emplace();
  auto &value = *reference;
  return reader.readU32(value.schema_epoch) &&
         reader.readString(value.canonical_package) &&
         reader.readString(value.canonical_module) &&
         reader.readString(value.canonical_name) &&
         reader.readFingerprint(value.fingerprint);
}
