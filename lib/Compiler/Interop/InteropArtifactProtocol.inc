// Included by InteropArtifact.cpp; this fragment retains the canonical
// codec/protocol/registry state and ownership boundaries.
bool lessForeignCapabilityArgument(const ForeignCapability::Argument &lhs,
                                   const ForeignCapability::Argument &rhs) {
  return std::tuple{static_cast<std::uint8_t>(lhs.kind), lhs.lane, lhs.literal,
                    static_cast<std::uint8_t>(lhs.entity.kind),
                    lhs.entity.canonical_package, lhs.entity.canonical_module,
                    lhs.entity.canonical_name} <
         std::tuple{static_cast<std::uint8_t>(rhs.kind), rhs.lane, rhs.literal,
                    static_cast<std::uint8_t>(rhs.entity.kind),
                    rhs.entity.canonical_package, rhs.entity.canonical_module,
                    rhs.entity.canonical_name};
}

bool lessForeignCapability(const ForeignCapability &lhs,
                           const ForeignCapability &rhs) {
  if (lhs.path != rhs.path)
    return lhs.path < rhs.path;
  if (lhs.lanes != rhs.lanes)
    return std::lexicographical_compare(lhs.lanes.begin(), lhs.lanes.end(),
                                        rhs.lanes.begin(), rhs.lanes.end());
  if (lhs.literals != rhs.literals)
    return std::lexicographical_compare(lhs.literals.begin(), lhs.literals.end(),
                                        rhs.literals.begin(), rhs.literals.end());
  return std::lexicographical_compare(lhs.arguments.begin(), lhs.arguments.end(),
                                      rhs.arguments.begin(), rhs.arguments.end(),
                                      lessForeignCapabilityArgument);
}

void ForeignOperationArtifact::canonicalize() {
  for (auto &capability : capabilities) {
    std::ranges::sort(capability.lanes);
    std::ranges::sort(capability.literals);
    std::ranges::sort(capability.arguments);
  }
  std::sort(capabilities.begin(), capabilities.end(), lessForeignCapability);
  std::ranges::sort(protocol_identities);
  std::ranges::sort(protocol_edges);
  for (auto &port : ports)
    std::ranges::sort(port.lanes);
  std::ranges::sort(ports);
  std::ranges::sort(success_lanes);
  std::ranges::sort(failure_lanes);
  std::ranges::sort(out_lanes);
  std::ranges::sort(wake_callback_lanes);
  const auto valid_predicate_width =
      error_predicate_width == 8 || error_predicate_width == 16 ||
      error_predicate_width == 32 || error_predicate_width == 64;
  if (valid_predicate_width && !error_intervals.empty()) {
    const auto mask = error_predicate_width == 64
                          ? std::numeric_limits<std::uint64_t>::max()
                          : (std::uint64_t{1} << error_predicate_width) - 1;
    const auto sign = error_predicate_signed
                          ? std::uint64_t{1} << (error_predicate_width - 1)
                          : 0;
    const auto key = [&](std::uint64_t value) { return (value & mask) ^ sign; };
    std::ranges::sort(error_intervals, [&](const auto &lhs, const auto &rhs) {
      return std::tuple(key(lhs.lower), key(lhs.upper)) <
             std::tuple(key(rhs.lower), key(rhs.upper));
    });
    std::vector<ErrorInterval> merged;
    for (const auto &interval : error_intervals) {
      if (!merged.empty()) {
        const auto upper = key(merged.back().upper);
        const auto lower = key(interval.lower);
        if (lower <= upper || (upper != mask && lower == upper + 1)) {
          if (key(interval.upper) > upper)
            merged.back().upper = interval.upper;
          continue;
        }
      }
      merged.push_back(interval);
    }
    error_intervals = std::move(merged);
  }
  const auto fingerprint_less = [](const auto &lhs, const auto &rhs) {
    return lhs.hex() < rhs.hex();
  };
  std::ranges::sort(completion_events, fingerprint_less);
  std::ranges::sort(cancel_events, fingerprint_less);
  std::ranges::sort(wake_events, fingerprint_less);
}

bool ForeignOperationArtifact::verify(std::string &error) const {
  error.clear();
  if (kind >= ForeignOperationKind::Count || capabilities.empty() ||
      abi_epoch == 0 || cfdl_semantic_epoch != CurrentCFDLSemanticEpoch ||
      callback_authority > 2 || !fingerprint.hasValue()) {
    error = "interop foreign operation artifact has an invalid header";
    return false;
  }
  if ((error_extractor == ErrorExtractor::None) !=
          (error_predicate == ErrorPredicate::None) ||
      (error_extractor == ErrorExtractor::None) !=
          (error_success_payload == ErrorSuccessPayload::None) ||
      error_extractor >= ErrorExtractor::Count ||
      error_predicate >= ErrorPredicate::Count ||
      error_success_payload >= ErrorSuccessPayload::Count) {
    error = "interop foreign error contract is invalid";
    return false;
  }
  const auto no_error = error_extractor == ErrorExtractor::None;
  if (no_error != (error_predicate_width == 0) ||
      (no_error && (!error_intervals.empty() || error_predicate_signed ||
                    error_predicate_inverted)) ||
      (!no_error && error_predicate_width != 8 && error_predicate_width != 16 &&
       error_predicate_width != 32 && error_predicate_width != 64) ||
      (error_predicate == ErrorPredicate::IntegerSet &&
       (error_intervals.empty() || error_intervals.size() > 64)) ||
      (error_predicate == ErrorPredicate::Null &&
       (!error_intervals.empty() || error_predicate_signed ||
        error_predicate_inverted)) ||
      (error_predicate == ErrorPredicate::InvalidSentinel &&
       (error_intervals.size() != 1 ||
        error_intervals.front().lower != error_intervals.front().upper ||
        error_predicate_inverted)) ||
      (error_predicate != ErrorPredicate::IntegerSet &&
       error_predicate_inverted)) {
    error = "interop foreign error predicate shape is invalid";
    return false;
  }
  if (!error_intervals.empty()) {
    const auto mask = error_predicate_width == 64
                          ? std::numeric_limits<std::uint64_t>::max()
                          : (std::uint64_t{1} << error_predicate_width) - 1;
    const auto sign = error_predicate_signed
                          ? std::uint64_t{1} << (error_predicate_width - 1)
                          : 0;
    const auto key = [&](std::uint64_t value) { return (value & mask) ^ sign; };
    for (std::size_t index = 0; index < error_intervals.size(); ++index) {
      const auto &interval = error_intervals[index];
      if ((interval.lower & ~mask) != 0 || (interval.upper & ~mask) != 0 ||
          key(interval.lower) > key(interval.upper) ||
          (index != 0 &&
           (key(error_intervals[index - 1].upper) >= key(interval.lower) ||
            (key(error_intervals[index - 1].upper) != mask &&
             key(error_intervals[index - 1].upper) + 1 ==
                 key(interval.lower))))) {
        error = "interop foreign error intervals are not canonical";
        return false;
      }
    }
  }
  if ((error_extractor == ErrorExtractor::ReturnedCode &&
       error_success_payload != ErrorSuccessPayload::Void) ||
      ((error_extractor == ErrorExtractor::Errno ||
        error_extractor == ErrorExtractor::Win32LastError) &&
       error_success_payload != ErrorSuccessPayload::Raw) ||
      (error_predicate == ErrorPredicate::Null &&
       error_extractor == ErrorExtractor::ReturnedCode)) {
    error = "interop foreign error extractor and payload disagree";
    return false;
  }
  const auto no_outcome = outcome_projection == OutcomeProjection::None;
  const auto win32_read = outcome_projection == OutcomeProjection::Win32Read;
  const auto fread = outcome_projection == OutcomeProjection::Fread;
  const auto valid_argument_sources = [&] {
    if (no_outcome)
      return argument_sources.empty();
    std::uint32_t next_public = 0;
    for (std::uint32_t lane = 0; lane < argument_sources.size(); ++lane) {
      const auto source = argument_sources[lane];
      if (source.kind == ArgumentSourceKind::PublicArgument) {
        if (source.index != next_public++)
          return false;
      } else if (source.index != core::AnyId::InvalidIndex ||
                 (source.kind != ArgumentSourceKind::OutcomeStorage &&
                  source.kind != ArgumentSourceKind::NullPointer)) {
        return false;
      }
    }
    return true;
  }();
  if (outcome_projection >= OutcomeProjection::Count ||
      no_outcome != (outcome_buffer_lane == core::AnyId::InvalidIndex) ||
      no_outcome != (outcome_capacity_lane == core::AnyId::InvalidIndex) ||
      no_outcome != !outcome_element_type.has_value() ||
      !valid_argument_sources ||
      (!no_outcome &&
       (outcome_buffer_lane == outcome_capacity_lane || !outcome_element_type ||
        outcome_element_type->kind != PublicTypeKind::Integer ||
        outcome_element_type->scalar_width != 8)) ||
      (outcome_projection == OutcomeProjection::PosixRead &&
       (error_extractor != ErrorExtractor::Errno ||
        error_predicate != ErrorPredicate::IntegerSet ||
        error_predicate_inverted || error_intervals.size() != 1 ||
        error_intervals.front().lower != error_intervals.front().upper))) {
    error = "interop foreign outcome projection is invalid";
    return false;
  }
  if (win32_read &&
      (outcome_count_lane >= argument_sources.size() ||
       outcome_context_lane >= argument_sources.size() ||
       outcome_count_lane == outcome_context_lane || !outcome_count_type ||
       outcome_count_type->kind != PublicTypeKind::Integer ||
       outcome_count_type->scalar_width != 32 ||
       outcome_count_type->integer_signed ||
       argument_sources[outcome_count_lane].kind !=
           ArgumentSourceKind::OutcomeStorage ||
       argument_sources[outcome_context_lane].kind !=
           ArgumentSourceKind::NullPointer ||
       error_extractor != ErrorExtractor::Win32LastError ||
       error_predicate != ErrorPredicate::IntegerSet ||
       error_predicate_inverted || error_intervals.size() != 1 ||
       error_intervals.front().lower != 0 ||
       error_intervals.front().upper != 0)) {
    error = "interop Win32 read projection is invalid";
    return false;
  }
  if (fread && (outcome_size_lane >= argument_sources.size() ||
                outcome_capacity_lane >= argument_sources.size() ||
                outcome_context_lane >= argument_sources.size() ||
                outcome_size_lane == outcome_capacity_lane ||
                outcome_size_lane == outcome_context_lane ||
                outcome_capacity_lane == outcome_context_lane ||
                outcome_eof_symbol.empty() || outcome_ferror_symbol.empty() ||
                error_extractor != ErrorExtractor::Errno ||
                error_predicate != ErrorPredicate::IntegerSet ||
                !error_predicate_inverted || error_intervals.size() != 1 ||
                error_intervals.front().lower != 0 ||
                error_intervals.front().upper != 0)) {
    error = "interop fread outcome projection is invalid";
    return false;
  }
  if (!fread &&
      (outcome_size_lane != core::AnyId::InvalidIndex ||
       !outcome_eof_symbol.empty() || !outcome_ferror_symbol.empty())) {
    error = "interop ordinary outcome carries fread lanes";
    return false;
  }
  if (!win32_read && !fread &&
      (outcome_count_lane != core::AnyId::InvalidIndex ||
       outcome_context_lane != core::AnyId::InvalidIndex ||
       outcome_count_type)) {
    error = "interop ordinary outcome carries Win32 lanes";
    return false;
  }
  if (!std::is_sorted(capabilities.begin(), capabilities.end(),
                      lessForeignCapability) ||
      !std::ranges::is_sorted(protocol_identities) ||
      !std::ranges::is_sorted(protocol_edges) ||
      !std::ranges::is_sorted(ports) ||
      !std::ranges::is_sorted(success_lanes) ||
      !std::ranges::is_sorted(failure_lanes) ||
      !std::ranges::is_sorted(out_lanes) ||
      !std::ranges::is_sorted(wake_callback_lanes)) {
    error = "interop foreign operation artifact is not canonical";
    return false;
  }
  if (std::ranges::adjacent_find(out_lanes) != out_lanes.end()) {
    error = "interop out lanes are duplicated";
    return false;
  }
  const auto fingerprint_less = [](const auto &lhs, const auto &rhs) {
    return lhs.hex() < rhs.hex();
  };
  if (!std::ranges::is_sorted(completion_events, fingerprint_less) ||
      !std::ranges::is_sorted(cancel_events, fingerprint_less) ||
      !std::ranges::is_sorted(wake_events, fingerprint_less) ||
      std::ranges::adjacent_find(completion_events) !=
          completion_events.end() ||
      std::ranges::adjacent_find(cancel_events) != cancel_events.end() ||
      std::ranges::adjacent_find(wake_events) != wake_events.end()) {
    error = "interop completion event set is not canonical";
    return false;
  }
  for (const auto &identity : protocol_identities) {
    if (identity.canonical_package.empty() ||
        identity.canonical_module.empty() ||
        identity.canonical_resource.empty() ||
        identity.canonical_name.empty() ||
        (identity.kind == ForeignProtocolIdentityKind::Event &&
         identity.owner_callable.empty()) ||
        !identity.fingerprint.hasValue()) {
      error = "interop protocol identity is incomplete";
      return false;
    }
  }
  for (const auto &edge : protocol_edges) {
    if (!edge.source.hasValue() || !edge.target.hasValue() ||
        edge.owner_callable.empty()) {
      error = "interop protocol edge is incomplete";
      return false;
    }
  }
  for (const auto &event : completion_events) {
    if (!event.hasValue() ||
        !std::ranges::any_of(protocol_identities, [&](const auto &identity) {
          return identity.kind == ForeignProtocolIdentityKind::Event &&
                 identity.fingerprint == event;
        })) {
      error = "interop completion event is not a declared identity";
      return false;
    }
    if (!std::ranges::any_of(protocol_edges, [&](const auto &edge) {
          return edge.kind == ForeignProtocolEdgeKind::Invokes &&
                 edge.source == event;
        })) {
      error = "interop completion event is unreachable";
      return false;
    }
  }
  const auto is_completion_event = [&](const StableFingerprint &event) {
    return std::ranges::find(completion_events, event) !=
           completion_events.end();
  };
  if (!std::ranges::all_of(cancel_events, is_completion_event) ||
      !std::ranges::all_of(wake_events, is_completion_event) ||
      (requires_quiescence && !discharges_quiescence)) {
    error = "interop completion cancel/wake/quiescence composition is open";
    return false;
  }
  return true;
}

bool ArtifactReference::verify(std::string &error) const {
  error.clear();
  if (schema_epoch != CurrentSchemaEpoch || canonical_package.empty() ||
      canonical_module.empty() || canonical_name.empty() ||
      !fingerprint.hasValue()) {
    error = "interop artifact reference has an invalid identity";
    return false;
  }
  return true;
}

void ArtifactBundle::canonicalize() {
  for (auto &record : records)
    record.artifact.canonicalize();
  std::ranges::sort(records, [](const auto &lhs, const auto &rhs) {
    return referenceLess(lhs.reference, rhs.reference);
  });
}

bool ArtifactBundle::verify(std::string &error) const {
  error.clear();
  if (format_version != CurrentFormatVersion || records.size() > MaxItems) {
    error = "interop bundle has an unsupported version or size";
    return false;
  }
  ArtifactReference previous;
  bool has_previous = false;
  for (const auto &record : records) {
    if (!record.reference.verify(error) || !record.artifact.verify(error) ||
        record.reference.fingerprint != record.artifact.fingerprint)
      return false;
    if (has_previous && !referenceLess(previous, record.reference)) {
      error = "interop bundle records are not canonical";
      return false;
    }
    previous = record.reference;
    has_previous = true;
  }
  return true;
}
