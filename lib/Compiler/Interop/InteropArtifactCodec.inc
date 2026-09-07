// Included by InteropArtifact.cpp; this fragment retains the canonical
// codec/protocol/registry state and ownership boundaries.
constexpr std::uint32_t MaxItems = 1U << 20U;

bool referenceLess(const ArtifactReference &lhs, const ArtifactReference &rhs) {
  return std::tuple{lhs.canonical_package, lhs.canonical_module,
                    lhs.canonical_name, lhs.fingerprint.hex()} <
         std::tuple{rhs.canonical_package, rhs.canonical_module,
                    rhs.canonical_name, rhs.fingerprint.hex()};
}

struct Writer {
  std::string bytes;
  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
      bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
      bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
  void string(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes.append(value);
  }
  void boolean(bool value) {
    u32(value ? 1U : 0U);
  }
  void fingerprint(const StableFingerprint &value) {
    string(value.hex());
  }
};

struct Reader {
  std::string_view bytes;
  std::size_t offset = 0;
  std::string error;

  bool take(std::size_t count, std::string_view &result) {
    if (count > bytes.size() - offset) {
      error = "interop bundle is truncated";
      return false;
    }
    result = bytes.substr(offset, count);
    offset += count;
    return true;
  }
  bool u32(std::uint32_t &value) {
    std::string_view data;
    if (!take(4, data))
      return false;
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1]))
             << 8U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2]))
             << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]))
             << 24U);
    return true;
  }
  bool u64(std::uint64_t &value) {
    std::string_view data;
    if (!take(8, data))
      return false;
    value = 0;
    for (unsigned index = 0; index != 8; ++index)
      value |=
          static_cast<std::uint64_t>(static_cast<unsigned char>(data[index]))
          << (index * 8U);
    return true;
  }
  bool string(std::string &value) {
    std::uint32_t size = 0;
    if (!u32(size) || size > MaxItems) {
      if (error.empty())
        error = "interop bundle string exceeds input limit";
      return false;
    }
    std::string_view data;
    if (!take(size, data))
      return false;
    value.assign(data);
    return true;
  }
  bool boolean(bool &value) {
    std::uint32_t raw = 0;
    if (!u32(raw) || raw > 1) {
      if (error.empty())
        error = "interop bundle contains an invalid boolean";
      return false;
    }
    value = raw != 0;
    return true;
  }
  bool count(std::uint32_t &value) {
    return u32(value) &&
           (value <= MaxItems ||
            (error = "interop bundle item count exceeds input limit", false));
  }
  bool fingerprint(StableFingerprint &value) {
    std::string hex;
    if (!string(hex) || hex.size() != StableFingerprint::ByteCount * 2)
      return error.empty()
                 ? (error = "interop bundle contains an invalid fingerprint",
                    false)
                 : false;
    std::array<std::uint8_t, StableFingerprint::ByteCount> decoded_bytes{};
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    for (std::size_t i = 0; i != decoded_bytes.size(); ++i) {
      const int high = digit(hex[i * 2]), low = digit(hex[i * 2 + 1]);
      if (high < 0 || low < 0)
        return error = "interop bundle contains an invalid fingerprint", false;
      decoded_bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    value = StableFingerprint(decoded_bytes);
    return true;
  }
};

template <typename T> void writeEnum(Writer &writer, T value) {
  writer.u32(static_cast<std::uint32_t>(value));
}

template <typename T> bool readEnum(Reader &reader, T &value, T count) {
  std::uint32_t raw = 0;
  if (!reader.u32(raw) || raw >= static_cast<std::uint32_t>(count)) {
    if (reader.error.empty())
      reader.error = "interop bundle contains invalid enum value " +
                     std::to_string(raw) + " with upper bound " +
                     std::to_string(static_cast<std::uint32_t>(count));
    return false;
  }
  value = static_cast<T>(raw);
  return true;
}

void writeReference(Writer &writer,
                    const PublicEntityReferenceArtifact &value) {
  writeEnum(writer, value.kind);
  writer.string(value.canonical_package);
  writer.string(value.canonical_module);
  writer.string(value.canonical_name);
  writer.fingerprint(value.expected_fingerprint);
}

bool readReference(Reader &reader, PublicEntityReferenceArtifact &value) {
  return readEnum(reader, value.kind, PublicEntityKind::Count) &&
         reader.string(value.canonical_package) &&
         reader.string(value.canonical_module) &&
         reader.string(value.canonical_name) &&
         reader.fingerprint(value.expected_fingerprint);
}

void writeRegion(Writer &writer, const OwnershipRegion &value) {
  writer.u32(value.parameter_index);
  writer.u32(static_cast<std::uint32_t>(value.path.size()));
  for (const auto &step : value.path) {
    writeEnum(writer, step.kind);
    writer.u32(step.index);
  }
  writer.boolean(value.has_bit_range);
  writer.u32(value.bit_begin);
  writer.u32(value.bit_end);
}

bool readRegion(Reader &reader, OwnershipRegion &value) {
  std::uint32_t count = 0;
  if (!reader.u32(value.parameter_index) || !reader.count(count))
    return false;
  value.path.resize(count);
  for (auto &step : value.path)
    if (!readEnum(reader, step.kind, OwnershipRegionStepKind::Count) ||
        !reader.u32(step.index))
      return false;
  return reader.boolean(value.has_bit_range) && reader.u32(value.bit_begin) &&
         reader.u32(value.bit_end);
}

void writeCondition(Writer &writer, const CallableConditionDescriptor &value) {
  writer.u32(static_cast<std::uint32_t>(value.clauses.size()));
  for (const auto &clause : value.clauses) {
    writer.u32(static_cast<std::uint32_t>(clause.atoms.size()));
    for (const auto &atom : clause.atoms) {
      writer.u32(atom.parameter_index);
      writer.boolean(atom.expected);
    }
  }
}

bool readCondition(Reader &reader, CallableConditionDescriptor &value) {
  std::uint32_t clause_count = 0;
  if (!reader.count(clause_count))
    return false;
  value.clauses.resize(clause_count);
  for (auto &clause : value.clauses) {
    std::uint32_t atom_count = 0;
    if (!reader.count(atom_count))
      return false;
    clause.atoms.resize(atom_count);
    for (auto &atom : clause.atoms)
      if (!reader.u32(atom.parameter_index) || !reader.boolean(atom.expected))
        return false;
  }
  return true;
}

void writeOwnership(Writer &writer, const CallableOwnershipSummary &value) {
  writer.u32(static_cast<std::uint32_t>(value.effects.size()));
  for (const auto &effect : value.effects) {
    writeEnum(writer, effect.kind);
    writeRegion(writer, effect.region);
  }
  writer.u32(static_cast<std::uint32_t>(value.postconditions.size()));
  for (const auto &post : value.postconditions) {
    writeRegion(writer, post.region);
    writer.u32(post.outcomes);
    writeCondition(writer, post.condition);
  }
  writer.boolean(value.returns_owned);
  writer.u32(static_cast<std::uint32_t>(value.return_provenance.size()));
  for (const auto &source : value.return_provenance) {
    writeRegion(writer, source.region);
    writer.u32(static_cast<std::uint32_t>(source.carrier_path.size()));
    for (const auto &step : source.carrier_path) {
      writeEnum(writer, step.kind);
      writer.u32(step.index);
    }
    writeCondition(writer, source.condition);
  }
}

bool readOwnership(Reader &reader, CallableOwnershipSummary &value) {
  std::uint32_t count = 0;
  if (!reader.count(count))
    return false;
  value.effects.resize(count);
  for (auto &effect : value.effects)
    if (!readEnum(reader, effect.kind, CallableEffectKind::Count) ||
        !readRegion(reader, effect.region))
      return false;
  if (!reader.count(count))
    return false;
  value.postconditions.resize(count);
  for (auto &post : value.postconditions) {
    std::uint32_t outcomes = 0;
    if (!readRegion(reader, post.region) || !reader.u32(outcomes) ||
        outcomes > CallableOutcomeAll || !readCondition(reader, post.condition))
      return false;
    post.outcomes = static_cast<std::uint8_t>(outcomes);
  }
  if (!reader.boolean(value.returns_owned) || !reader.count(count))
    return false;
  value.return_provenance.resize(count);
  for (auto &source : value.return_provenance) {
    std::uint32_t path_count = 0;
    if (!readRegion(reader, source.region) || !reader.count(path_count) ||
        path_count > 256)
      return false;
    source.carrier_path.resize(path_count);
    for (auto &step : source.carrier_path)
      if (!readEnum(reader, step.kind,
                    CallableReturnSource::CarrierStepKind::Count) ||
          !reader.u32(step.index))
        return false;
    if (!readCondition(reader, source.condition))
      return false;
  }
  value.canonicalize();
  return true;
}

void writeType(Writer &writer, const PublicType &value) {
  writeEnum(writer, value.kind);
  writer.u32(value.binding_index);
  writer.u32(value.scalar_width);
  writer.boolean(value.integer_signed);
  writeReference(writer, value.nominal_entity);
  writer.u32(static_cast<std::uint32_t>(value.arguments.size()));
  for (const auto &argument : value.arguments)
    writeType(writer, argument);
  writer.u32(value.array_bound);
  writeEnum(writer, value.reference_mutability);
  writeEnum(writer, value.reference_provenance.kind);
  writer.u32(value.reference_provenance.index);
  writer.boolean(value.pointer_const);
  writer.boolean(value.slice_mutable);
  writeEnum(writer, value.projection_kind);
  writer.u32(value.projection_index);
  writer.boolean(value.callable_variadic);
  writer.boolean(value.abi_union);
  writeEnum(writer, value.foreign_calling_convention);
  writer.u32(value.callable_context_parameter);
  writeOwnership(writer, value.callable_contract);
  writer.u32(value.registration_authority);
  writer.u32(value.registration_entry_parameter);
  writer.u32(value.registration_userdata_parameter);
  writer.u32(value.registration_release_parameter);
  writer.u32(static_cast<std::uint32_t>(value.registration_bindings.size()));
  for (const auto &binding : value.registration_bindings) {
    writer.string(binding.name);
    writer.u32(binding.parameter_index);
  }
  for (auto parameter : value.registration_arm_parameters)
    writer.u32(parameter);
  for (auto parameter : value.registration_detach_parameters)
    writer.u32(parameter);
  writer.string(encodeForeignResourceProtocol(value.foreign_resource_protocol));
  writeEnum(writer, value.foreign_operation_state);
}

bool readType(Reader &reader, PublicType &value) {
  std::uint32_t count = 0;
  if (!readEnum(reader, value.kind, PublicTypeKind::Count) ||
      !reader.u32(value.binding_index) || !reader.u32(value.scalar_width) ||
      !reader.boolean(value.integer_signed) ||
      !readReference(reader, value.nominal_entity) || !reader.count(count))
    return false;
  value.arguments.resize(count);
  for (auto &argument : value.arguments)
    if (!readType(reader, argument))
      return false;
  if (!reader.u32(value.array_bound) ||
      !readEnum(reader, value.reference_mutability,
                PublicReferenceMutability::Count) ||
      !readEnum(reader, value.reference_provenance.kind,
                PublicReferenceProvenanceKind::Count) ||
      !reader.u32(value.reference_provenance.index) ||
      !reader.boolean(value.pointer_const) ||
      !reader.boolean(value.slice_mutable) ||
      !readEnum(reader, value.projection_kind,
                PublicTypeProjectionKind::Count) ||
      !reader.u32(value.projection_index) ||
      !reader.boolean(value.callable_variadic) ||
      !reader.boolean(value.abi_union) ||
      !readEnum(reader, value.foreign_calling_convention,
                ForeignCallingConvention::Count) ||
      !reader.u32(value.callable_context_parameter) ||
      !readOwnership(reader, value.callable_contract))
    return false;
  if (!reader.u32(count) || count > 255) {
    if (reader.error.empty())
      reader.error = "interop bundle contains an invalid authority";
    return false;
  }
  value.registration_authority = static_cast<std::uint8_t>(count);
  if (!reader.u32(value.registration_entry_parameter) ||
      !reader.u32(value.registration_userdata_parameter) ||
      !reader.u32(value.registration_release_parameter) || !reader.count(count))
    return false;
  value.registration_bindings.resize(count);
  for (auto &binding : value.registration_bindings)
    if (!reader.string(binding.name) || !reader.u32(binding.parameter_index))
      return false;
  for (auto &parameter : value.registration_arm_parameters)
    if (!reader.u32(parameter))
      return false;
  for (auto &parameter : value.registration_detach_parameters)
    if (!reader.u32(parameter))
      return false;
  std::string protocol;
  if (!reader.string(protocol))
    return false;
  auto decoded =
      decodeForeignResourceProtocol(protocol, MaxItems, reader.error);
  if (!decoded)
    return false;
  value.foreign_resource_protocol = std::move(*decoded);
  return readEnum(reader, value.foreign_operation_state,
                  ForeignOperationStateKind::Count);
}

void writeArtifact(Writer &writer, const ForeignOperationArtifact &value) {
  writeEnum(writer, value.kind);
  writer.u32(static_cast<std::uint32_t>(value.capabilities.size()));
  for (const auto &capability : value.capabilities) {
    writer.string(capability.path);
    writer.u32(static_cast<std::uint32_t>(capability.lanes.size()));
    for (auto lane : capability.lanes)
      writer.u32(lane);
    writer.u32(static_cast<std::uint32_t>(capability.literals.size()));
    for (const auto &literal : capability.literals)
      writer.string(literal);
    writer.u32(static_cast<std::uint32_t>(capability.arguments.size()));
    for (const auto &argument : capability.arguments) {
      writeEnum(writer, argument.kind);
      writer.u32(argument.lane);
      writer.string(argument.literal);
      writeReference(writer, argument.entity);
    }
  }
  writer.u32(static_cast<std::uint32_t>(value.protocol_identities.size()));
  for (const auto &identity : value.protocol_identities) {
    writeEnum(writer, identity.kind);
    writer.string(identity.canonical_package);
    writer.string(identity.canonical_module);
    writer.string(identity.canonical_resource);
    writer.string(identity.canonical_name);
    writer.string(identity.owner_callable);
    writer.fingerprint(identity.fingerprint);
  }
  writer.u32(static_cast<std::uint32_t>(value.protocol_edges.size()));
  for (const auto &edge : value.protocol_edges) {
    writeEnum(writer, edge.kind);
    writer.fingerprint(edge.source);
    writer.fingerprint(edge.target);
    writer.string(edge.predicate);
    writer.string(edge.owner_callable);
  }
  writer.u32(static_cast<std::uint32_t>(value.ports.size()));
  for (const auto &port : value.ports) {
    writeEnum(writer, port.kind);
    writeEnum(writer, port.effect);
    writer.string(port.path);
    writer.u32(static_cast<std::uint32_t>(port.lanes.size()));
    for (auto lane : port.lanes)
      writer.u32(lane);
    writer.boolean(port.success);
    writer.boolean(port.failure);
  }
  writeEnum(writer, value.success_payload);
  writeEnum(writer, value.failure_payload);
  for (const auto *lanes : {&value.success_lanes, &value.failure_lanes}) {
    writer.u32(static_cast<std::uint32_t>(lanes->size()));
    for (auto lane : *lanes)
      writer.u32(lane);
  }
  writer.u32(value.status_lane);
  writer.u64(static_cast<std::uint64_t>(value.status_success_literal));
  writer.boolean(value.out_initialized_on_failure);
  writer.u32(static_cast<std::uint32_t>(value.out_lanes.size()));
  for (auto lane : value.out_lanes)
    writer.u32(lane);
  writeEnum(writer, value.error_extractor);
  writeEnum(writer, value.error_predicate);
  writeEnum(writer, value.error_success_payload);
  writer.u32(value.error_predicate_width);
  writer.boolean(value.error_predicate_signed);
  writer.boolean(value.error_predicate_inverted);
  writer.u32(static_cast<std::uint32_t>(value.error_intervals.size()));
  for (const auto &interval : value.error_intervals) {
    writer.u64(interval.lower);
    writer.u64(interval.upper);
  }
  writeEnum(writer, value.outcome_projection);
  writer.u32(value.outcome_buffer_lane);
  writer.u32(value.outcome_capacity_lane);
  writer.u32(value.outcome_count_lane);
  writer.u32(value.outcome_context_lane);
  writer.u32(value.outcome_size_lane);
  writer.string(value.outcome_eof_symbol);
  writer.string(value.outcome_ferror_symbol);
  writer.boolean(value.outcome_element_type.has_value());
  if (value.outcome_element_type) {
    writer.u32(value.outcome_element_type->scalar_width);
    writer.boolean(value.outcome_element_type->integer_signed);
  }
  writer.boolean(value.outcome_count_type.has_value());
  if (value.outcome_count_type) {
    writer.u32(value.outcome_count_type->scalar_width);
    writer.boolean(value.outcome_count_type->integer_signed);
  }
  writer.u32(static_cast<std::uint32_t>(value.argument_sources.size()));
  for (const auto source : value.argument_sources) {
    writeEnum(writer, source.kind);
    writer.u32(source.index);
  }
  writer.u32(value.callback_authority);
  writeEnum(writer, value.completion_family);
  writeEnum(writer, value.completion_projection);
  writeEnum(writer, value.completion_input_effect);
  writer.u32(value.completion_carrier_lane);
  writer.u32(value.completion_result_lane);
  writer.u32(value.readiness_success_literal);
  writer.u32(static_cast<std::uint32_t>(value.wake_callback_lanes.size()));
  for (auto lane : value.wake_callback_lanes)
    writer.u32(lane);
  for (const auto *events :
       {&value.completion_events, &value.cancel_events, &value.wake_events}) {
    writer.u32(static_cast<std::uint32_t>(events->size()));
    for (const auto &event : *events)
      writer.fingerprint(event);
  }
  writer.boolean(value.requires_quiescence);
  writer.boolean(value.discharges_quiescence);
  writer.boolean(value.callback_adapter_layout.has_value());
  if (value.callback_adapter_layout)
    writeType(writer, *value.callback_adapter_layout);
  writer.boolean(value.waker_adapter_layout.has_value());
  if (value.waker_adapter_layout)
    writeType(writer, *value.waker_adapter_layout);
  writer.boolean(value.completion_descriptor.has_value());
  if (value.completion_descriptor) {
    const auto &family = *value.completion_descriptor;
    for (const auto *reference : {&family.source, &family.wait, &family.poll,
                                  &family.arm, &family.detach})
      writeReference(writer, *reference);
    writeType(writer, family.completion_carrier);
    writeType(writer, family.registration_callback_adapter);
    writeType(writer, family.waker_callback_adapter);
    writer.u32(family.source_handle_lane);
    writer.u32(family.token_result_lane);
    for (const auto *lanes : {&family.arm_lane_map, &family.detach_lane_map}) {
      writer.u32(static_cast<std::uint32_t>(lanes->size()));
      for (auto lane : *lanes)
        writer.u32(lane);
    }
    writer.u32(family.authority);
    writer.u32(family.readiness_literal);
    writeEnum(writer, family.input_effect);
    writer.u32(family.abi_epoch);
  }
  writer.u32(value.abi_epoch);
  writer.u32(value.cfdl_semantic_epoch);
  writer.fingerprint(value.fingerprint);
}

bool readArtifact(Reader &reader, ForeignOperationArtifact &value) {
  std::uint32_t count = 0;
  if (!readEnum(reader, value.kind, ForeignOperationKind::Count) ||
      !reader.count(count))
    return false;
  value.capabilities.resize(count);
  for (auto &capability : value.capabilities) {
    if (!reader.string(capability.path) || !reader.count(count))
      return false;
    capability.lanes.resize(count);
    for (auto &lane : capability.lanes)
      if (!reader.u32(lane))
        return false;
    if (!reader.count(count))
      return false;
    capability.literals.resize(count);
    for (auto &literal : capability.literals)
      if (!reader.string(literal))
        return false;
    if (!reader.count(count))
      return false;
    capability.arguments.resize(count);
    for (auto &argument : capability.arguments)
      if (!readEnum(reader, argument.kind,
                    ForeignCapability::ArgumentKind::Count) ||
          !reader.u32(argument.lane) || !reader.string(argument.literal) ||
          !readReference(reader, argument.entity))
        return false;
  }
  if (!reader.count(count))
    return false;
  value.protocol_identities.resize(count);
  for (auto &identity : value.protocol_identities) {
    if (!readEnum(reader, identity.kind, ForeignProtocolIdentityKind::Count) ||
        !reader.string(identity.canonical_package) ||
        !reader.string(identity.canonical_module) ||
        !reader.string(identity.canonical_resource) ||
        !reader.string(identity.canonical_name) ||
        !reader.string(identity.owner_callable) ||
        !reader.fingerprint(identity.fingerprint))
      return false;
  }
  if (!reader.count(count))
    return false;
  value.protocol_edges.resize(count);
  for (auto &edge : value.protocol_edges) {
    if (!readEnum(reader, edge.kind, ForeignProtocolEdgeKind::Count) ||
        !reader.fingerprint(edge.source) || !reader.fingerprint(edge.target) ||
        !reader.string(edge.predicate) || !reader.string(edge.owner_callable))
      return false;
  }
  if (!reader.count(count))
    return false;
  value.ports.resize(count);
  for (auto &port : value.ports) {
    if (!readEnum(reader, port.kind, ForeignPortKind::Count) ||
        !readEnum(reader, port.effect, ForeignPortEffect::Count) ||
        !reader.string(port.path) || !reader.count(count))
      return false;
    port.lanes.resize(count);
    for (auto &lane : port.lanes)
      if (!reader.u32(lane))
        return false;
    if (!reader.boolean(port.success) || !reader.boolean(port.failure))
      return false;
  }
  if (!readEnum(reader, value.success_payload, ForeignPayloadKind::Count) ||
      !readEnum(reader, value.failure_payload, ForeignPayloadKind::Count))
    return false;
  for (auto *lanes : {&value.success_lanes, &value.failure_lanes}) {
    if (!reader.count(count))
      return false;
    lanes->resize(count);
    for (auto &lane : *lanes)
      if (!reader.u32(lane))
        return false;
  }
  std::uint64_t status_success_literal = 0;
  if (!reader.u32(value.status_lane) || !reader.u64(status_success_literal) ||
      !reader.boolean(value.out_initialized_on_failure) || !reader.count(count))
    return false;
  value.status_success_literal =
      static_cast<std::int64_t>(status_success_literal);
  value.out_lanes.resize(count);
  for (auto &lane : value.out_lanes)
    if (!reader.u32(lane))
      return false;
  if (!readEnum(reader, value.error_extractor,
                ForeignOperationArtifact::ErrorExtractor::Count) ||
      !readEnum(reader, value.error_predicate,
                ForeignOperationArtifact::ErrorPredicate::Count) ||
      !readEnum(reader, value.error_success_payload,
                ForeignOperationArtifact::ErrorSuccessPayload::Count) ||
      !reader.u32(value.error_predicate_width) ||
      !reader.boolean(value.error_predicate_signed) ||
      !reader.boolean(value.error_predicate_inverted) || !reader.count(count) ||
      count > 64)
    return false;
  value.error_intervals.resize(count);
  for (auto &interval : value.error_intervals)
    if (!reader.u64(interval.lower) || !reader.u64(interval.upper))
      return false;
  bool has_outcome_element = false, has_outcome_count = false;
  if (!readEnum(reader, value.outcome_projection,
                ForeignOperationArtifact::OutcomeProjection::Count) ||
      !reader.u32(value.outcome_buffer_lane) ||
      !reader.u32(value.outcome_capacity_lane) ||
      !reader.u32(value.outcome_count_lane) ||
      !reader.u32(value.outcome_context_lane) ||
      !reader.u32(value.outcome_size_lane) ||
      !reader.string(value.outcome_eof_symbol) ||
      !reader.string(value.outcome_ferror_symbol) ||
      !reader.boolean(has_outcome_element))
    return false;
  if (has_outcome_element) {
    std::uint32_t width = 0;
    bool is_signed = false;
    if (!reader.u32(width) || !reader.boolean(is_signed))
      return false;
    value.outcome_element_type = PublicType::integer(width, is_signed);
  }
  if (!reader.boolean(has_outcome_count))
    return false;
  if (has_outcome_count) {
    std::uint32_t width = 0;
    bool is_signed = false;
    if (!reader.u32(width) || !reader.boolean(is_signed))
      return false;
    value.outcome_count_type = PublicType::integer(width, is_signed);
  }
  if (!reader.count(count))
    return false;
  value.argument_sources.resize(count);
  for (auto &source : value.argument_sources)
    if (!readEnum(reader, source.kind,
                  ForeignOperationArtifact::ArgumentSourceKind::Count) ||
        !reader.u32(source.index))
      return false;
  std::uint32_t callback_authority = 0;
  if (!reader.u32(callback_authority) || callback_authority > 255 ||
      !readEnum(reader, value.completion_family,
                ForeignCompletionFamily::Count) ||
      !readEnum(reader, value.completion_projection,
                ForeignCompletionProjectionKind::Count) ||
      !readEnum(reader, value.completion_input_effect,
                ForeignCompletionInputEffect::Count) ||
      !reader.u32(value.completion_carrier_lane) ||
      !reader.u32(value.completion_result_lane) ||
      !reader.u32(value.readiness_success_literal) || !reader.count(count))
    return false;
  value.callback_authority = static_cast<std::uint8_t>(callback_authority);
  value.wake_callback_lanes.resize(count);
  for (auto &lane : value.wake_callback_lanes)
    if (!reader.u32(lane))
      return false;
  for (auto *events :
       {&value.completion_events, &value.cancel_events, &value.wake_events}) {
    if (!reader.count(count))
      return false;
    events->resize(count);
    for (auto &event : *events)
      if (!reader.fingerprint(event))
        return false;
  }
  if (!reader.boolean(value.requires_quiescence) ||
      !reader.boolean(value.discharges_quiescence))
    return false;
  bool present = false;
  if (!reader.boolean(present))
    return false;
  if (present) {
    value.callback_adapter_layout = PublicType{};
    if (!readType(reader, *value.callback_adapter_layout))
      return false;
  }
  if (!reader.boolean(present))
    return false;
  if (present) {
    value.waker_adapter_layout = PublicType{};
    if (!readType(reader, *value.waker_adapter_layout))
      return false;
  }
  if (!reader.boolean(present))
    return false;
  if (present) {
    value.completion_descriptor = CompletionFamily{};
    auto &family = *value.completion_descriptor;
    for (auto *reference : {&family.source, &family.wait, &family.poll,
                            &family.arm, &family.detach})
      if (!readReference(reader, *reference))
        return false;
    if (!readType(reader, family.completion_carrier) ||
        !readType(reader, family.registration_callback_adapter) ||
        !readType(reader, family.waker_callback_adapter) ||
        !reader.u32(family.source_handle_lane) ||
        !reader.u32(family.token_result_lane))
      return false;
    for (auto *lanes : {&family.arm_lane_map, &family.detach_lane_map}) {
      if (!reader.count(count))
        return false;
      lanes->resize(count);
      for (auto &lane : *lanes)
        if (!reader.u32(lane))
          return false;
    }
    std::uint32_t authority = 0;
    if (!reader.u32(authority) || authority > 255 ||
        !reader.u32(family.readiness_literal) ||
        !readEnum(reader, family.input_effect,
                  ForeignCompletionInputEffect::Count) ||
        !reader.u32(family.abi_epoch))
      return false;
    family.authority = static_cast<std::uint8_t>(authority);
  }
  if (!reader.u32(value.abi_epoch) || !reader.u32(value.cfdl_semantic_epoch) ||
      !reader.fingerprint(value.fingerprint))
    return false;
  return value.verify(reader.error);
}

void writeArtifactReference(Writer &writer,
                            const ArtifactReference &reference) {
  writer.u32(reference.schema_epoch);
  writer.string(reference.canonical_package);
  writer.string(reference.canonical_module);
  writer.string(reference.canonical_name);
  writer.fingerprint(reference.fingerprint);
}

bool readArtifactReference(Reader &reader, ArtifactReference &reference) {
  return reader.u32(reference.schema_epoch) &&
         reader.string(reference.canonical_package) &&
         reader.string(reference.canonical_module) &&
         reader.string(reference.canonical_name) &&
         reader.fingerprint(reference.fingerprint);
}
