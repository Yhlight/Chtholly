// Included inside buildCFDLSemIR; this fragment aliases the one SemIR
// builder state and preserves CFDL ABI/resource ordering.
  std::set<std::string> names;
  for (const auto &callable : file.callables) {
    if (types.contains(callable.name) || value_names.contains(callable.name) ||
        !names.insert(callable.name).second) {
      report(CFDLDiagnostic::Kind::DuplicateName, callable.offset);
      continue;
    }
    std::vector<CFDLDiagnostic> validation_diagnostics;
    if (!validateCFDLCallable(callable, validation_diagnostics)) {
      for (const auto &diagnostic : validation_diagnostics)
        report(diagnostic.kind, diagnostic.offset);
      continue;
    }
    std::vector<TypeId> parameters;
    ForeignAbiSignature signature;
    signature.calling_convention =
        normalize_convention(callable.calling_convention);
    const auto convention_valid =
        signature.calling_convention == ForeignCallingConvention::C ||
        (signature.calling_convention == ForeignCallingConvention::Win64 &&
         target_is_windows && target_triple.starts_with("x86_64")) ||
        (signature.calling_convention == ForeignCallingConvention::SysV64 &&
         !target_is_windows && target_triple.starts_with("x86_64"));
    if (!convention_valid)
      report(CFDLDiagnostic::Kind::InvalidCallable, callable.offset);
    bool valid = true;
    const auto classify = [&](TypeId type, bool result) -> ForeignAbiValue {
      if (!type.hasValue())
        return {};
      auto physical = sem_ir.foreignRepresentationType(type);
      if (physical.hasValue())
        type = physical;
      const auto &value = sem_ir.type(type);
      ForeignAbiValue abi;
      if (value.kind == SemTypeKind::Void && result)
        abi.kind = ForeignAbiValueKind::Void;
      else if (value.kind == SemTypeKind::Bool) {
        abi.kind = ForeignAbiValueKind::Bool;
        abi.width = 1;
      } else if (value.kind == SemTypeKind::Integer) {
        abi.kind = value.arg1 ? ForeignAbiValueKind::SignedInteger
                              : ForeignAbiValueKind::UnsignedInteger;
        abi.width = value.arg0;
      } else if (value.kind == SemTypeKind::Float) {
        abi.kind = ForeignAbiValueKind::Float;
        abi.width = value.arg0;
      } else if (value.kind == SemTypeKind::RawPointer) {
        abi.kind = ForeignAbiValueKind::RawPointer;
        abi.pointee_const = sem_ir.rawPointerPointeeConst(type);
      } else if (value.kind == SemTypeKind::CFunctionPointer) {
        abi.kind = ForeignAbiValueKind::FunctionPointer;
      } else if (value.kind == SemTypeKind::Tuple ||
                 value.kind == SemTypeKind::Array) {
        abi.kind = ForeignAbiValueKind::Aggregate;
      }
      return abi;
    };
    for (const auto &parameter : callable.parameters) {
      const auto base_type =
          resolve_type(parameter.type.physical_type, parameter.offset);
      TypeId type = base_type;
      if (base_type.hasValue()) {
        const auto incomplete_foreign = [&] {
          if (sem_ir.type(base_type).kind != SemTypeKind::Nominal)
            return false;
          const auto &nominal =
              sem_ir.nominalType(NominalTypeId(sem_ir.type(base_type).arg0));
          return nominal.kind == NominalKind::ForeignHandle &&
                 !nominal.foreign_representation.hasValue();
        }();
        if (incomplete_foreign &&
            parameter.type.qualifier != CFDLFlowQualifier::Ref &&
            parameter.type.qualifier != CFDLFlowQualifier::RefMut &&
            parameter.type.qualifier != CFDLFlowQualifier::View &&
            parameter.type.qualifier != CFDLFlowQualifier::ViewMut)
          report(CFDLDiagnostic::Kind::IncompleteCarrier, parameter.offset);
        switch (parameter.type.qualifier) {
        case CFDLFlowQualifier::Ref:
          type = sem_ir.addReferenceType(base_type,
                                         SemReferenceMutability::ReadOnly);
          break;
        case CFDLFlowQualifier::RefMut:
          type = sem_ir.addReferenceType(base_type,
                                         SemReferenceMutability::Mutable);
          break;
        case CFDLFlowQualifier::View:
          if (incomplete_foreign)
            type = sem_ir.addReferenceType(base_type,
                                           SemReferenceMutability::ReadOnly);
          break;
        case CFDLFlowQualifier::ViewMut:
          if (incomplete_foreign)
            type = sem_ir.addReferenceType(base_type,
                                           SemReferenceMutability::Mutable);
          break;
        case CFDLFlowQualifier::Out:
        case CFDLFlowQualifier::InOut:
          if (incomplete_foreign) {
            type = TypeId::invalid();
          } else {
            type = sem_ir.addReferenceType(base_type,
                                           SemReferenceMutability::Mutable);
          }
          break;
        default:
          break;
        }
      }
      parameters.push_back(type);
      ForeignAbiValue abi;
      if (type.hasValue() && sem_ir.type(type).kind == SemTypeKind::Reference) {
        abi.kind = ForeignAbiValueKind::Reference;
        abi.pointee_const = sem_ir.referenceMutability(type) ==
                            SemReferenceMutability::ReadOnly;
      } else {
        abi = classify(type, false);
      }
      valid &= abi.kind != ForeignAbiValueKind::Count;
      signature.parameters.push_back(abi);
    }
    const auto result_type =
        resolve_type(callable.result.physical_type, callable.offset);
    if (result_type.hasValue() &&
        sem_ir.type(result_type).kind == SemTypeKind::Nominal) {
      const auto &nominal =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(result_type).arg0));
      if (nominal.kind == NominalKind::ForeignHandle &&
          !nominal.foreign_representation.hasValue())
        report(CFDLDiagnostic::Kind::IncompleteCarrier, callable.offset);
    }
    signature.result = classify(result_type, true);
    valid &= signature.result.kind != ForeignAbiValueKind::Count;
    if (!valid || !result_type.hasValue()) {
      report(CFDLDiagnostic::Kind::InvalidCallable, callable.offset);
      continue;
    }
    using ErrorArtifact = interop::ForeignOperationArtifact;
    auto normalized_error_predicate = ErrorArtifact::ErrorPredicate::None;
    std::vector<ErrorArtifact::ErrorInterval> normalized_error_intervals;
    std::uint32_t normalized_error_width = 0;
    bool normalized_error_signed = false;
    bool normalized_error_inverted = false;
    TypeId outcome_element_type;
    std::uint32_t outcome_buffer_lane = core::AnyId::InvalidIndex;
    std::uint32_t outcome_capacity_lane = core::AnyId::InvalidIndex;
    std::uint32_t outcome_count_lane = core::AnyId::InvalidIndex;
    std::uint32_t outcome_context_lane = core::AnyId::InvalidIndex;
    std::uint32_t outcome_size_lane = core::AnyId::InvalidIndex;
    if (callable.error_contract) {
      auto physical_result = sem_ir.foreignRepresentationType(result_type);
      if (!physical_result.hasValue())
        physical_result = result_type;
      const auto fread_error_accessor =
          callable.outcome_contract &&
          callable.outcome_contract->kind == CFDLOutcomeKind::Fread &&
          callable.error_contract->observed == "ferror";
      if (fread_error_accessor)
        physical_result = sem_ir.i32Type();
      const auto physical_kind = sem_ir.type(physical_result).kind;
      const auto &physical_type = sem_ir.type(physical_result);
      const auto domain = callable.error_contract->domain;
      const auto predicate = callable.error_contract->predicate;
      const bool integer_result = physical_kind == SemTypeKind::Integer;
      const bool pointer_result = physical_kind == SemTypeKind::RawPointer;
      normalized_error_width =
          integer_result ? physical_type.arg0 : pointer_width;
      normalized_error_signed = integer_result && physical_type.arg1 != 0;
      const auto mask = normalized_error_width == 64
                            ? std::numeric_limits<std::uint64_t>::max()
                            : (std::uint64_t{1} << normalized_error_width) - 1;
      const auto sign_bit = normalized_error_signed
                                ? std::uint64_t{1}
                                      << (normalized_error_width - 1)
                                : 0;
      const auto order_key = [&](std::uint64_t bits) {
        return (bits & mask) ^ sign_bit;
      };
      const auto value_bits = [&](const CFDLErrorValueSyntax &syntax)
          -> std::optional<std::uint64_t> {
        auto magnitude = syntax.magnitude;
        auto negative = syntax.negative;
        if (!syntax.name.empty()) {
          const auto found = predicate_constants.find(syntax.name);
          if (found == predicate_constants.end())
            return std::nullopt;
          magnitude = found->second.magnitude;
          negative = found->second.negative;
        }
        if (negative) {
          if (!normalized_error_signed)
            return std::nullopt;
          const auto limit = std::uint64_t{1} << (normalized_error_width - 1);
          if (magnitude > limit)
            return std::nullopt;
          return (std::uint64_t{0} - magnitude) & mask;
        }
        const auto limit = normalized_error_signed ? sign_bit - 1 : mask;
        if (magnitude > limit)
          return std::nullopt;
        return magnitude & mask;
      };
      bool predicate_valid = true;
      if (predicate == CFDLErrorPredicateKind::Null) {
        normalized_error_predicate = ErrorArtifact::ErrorPredicate::Null;
        predicate_valid = pointer_result;
      } else if (predicate == CFDLErrorPredicateKind::Invalid) {
        if (sem_ir.type(result_type).kind != SemTypeKind::Nominal) {
          predicate_valid = false;
        } else {
          const auto &nominal =
              sem_ir.nominalType(NominalTypeId(sem_ir.type(result_type).arg0));
          if (nominal.foreign_invalid_state ==
              ForeignResourceInvalidState::Null) {
            normalized_error_predicate = ErrorArtifact::ErrorPredicate::Null;
          } else if (nominal.foreign_invalid_state ==
                         ForeignResourceInvalidState::Integer ||
                     nominal.foreign_invalid_state ==
                         ForeignResourceInvalidState::PointerBitPattern) {
            normalized_error_predicate =
                ErrorArtifact::ErrorPredicate::InvalidSentinel;
            CFDLErrorValueSyntax value;
            value.negative = nominal.foreign_invalid_integer < 0;
            value.magnitude =
                value.negative
                    ? std::uint64_t{0} - static_cast<std::uint64_t>(
                                             nominal.foreign_invalid_integer)
                    : static_cast<std::uint64_t>(
                          nominal.foreign_invalid_integer);
            const auto bits = pointer_result
                                  ? std::optional<std::uint64_t>(
                                        static_cast<std::uint64_t>(
                                            nominal.foreign_invalid_integer) &
                                        mask)
                                  : value_bits(value);
            if (!bits)
              predicate_valid = false;
            else
              normalized_error_intervals.push_back({*bits, *bits});
          } else {
            predicate_valid = false;
          }
        }
      } else {
        normalized_error_predicate = ErrorArtifact::ErrorPredicate::IntegerSet;
        normalized_error_inverted =
            predicate == CFDLErrorPredicateKind::NotEqual ||
            predicate == CFDLErrorPredicateKind::NotInSet;
        if (!integer_result || callable.error_contract->ranges.empty()) {
          predicate_valid = false;
        } else {
          for (const auto &range : callable.error_contract->ranges) {
            const auto lower = value_bits(range.lower);
            const auto upper = range.upper ? value_bits(*range.upper) : lower;
            if (!lower || !upper || order_key(*lower) > order_key(*upper)) {
              predicate_valid = false;
              break;
            }
            normalized_error_intervals.push_back({*lower, *upper});
          }
          if (predicate_valid && predicate == CFDLErrorPredicateKind::Less) {
            const auto boundary = normalized_error_intervals.front().lower;
            const auto key = order_key(boundary);
            if (key == 0) {
              predicate_valid = false;
            } else {
              normalized_error_intervals.front() = {
                  sign_bit, ((key - 1) ^ sign_bit) & mask};
            }
          }
        }
      }
      if (predicate_valid && !normalized_error_intervals.empty()) {
        std::ranges::sort(
            normalized_error_intervals, [&](const auto &lhs, const auto &rhs) {
              return std::tuple(order_key(lhs.lower), order_key(lhs.upper)) <
                     std::tuple(order_key(rhs.lower), order_key(rhs.upper));
            });
        std::vector<ErrorArtifact::ErrorInterval> merged;
        for (const auto &range : normalized_error_intervals) {
          if (!merged.empty()) {
            const auto current_upper = order_key(merged.back().upper);
            const auto next_lower = order_key(range.lower);
            if (next_lower <= current_upper ||
                (current_upper != mask && next_lower == current_upper + 1)) {
              if (order_key(range.upper) > current_upper)
                merged.back().upper = range.upper;
              continue;
            }
          }
          merged.push_back(range);
        }
        normalized_error_intervals = std::move(merged);
      }
      const auto pointer_predicate =
          normalized_error_predicate == ErrorArtifact::ErrorPredicate::Null ||
          (normalized_error_predicate ==
               ErrorArtifact::ErrorPredicate::InvalidSentinel &&
           pointer_result);
      const bool valid_error_contract =
          predicate_valid &&
          ((domain == CFDLErrorDomainKind::Code && integer_result &&
            !pointer_predicate) ||
           (domain == CFDLErrorDomainKind::Errno &&
            (integer_result || pointer_predicate)) ||
           (domain == CFDLErrorDomainKind::Win32 && target_is_windows &&
            (integer_result || pointer_predicate)));
      if (!valid_error_contract) {
        report(CFDLDiagnostic::Kind::InvalidCallable,
               callable.error_contract->offset);
        continue;
      }
    }
    if (callable.outcome_contract) {
      const auto &outcome = *callable.outcome_contract;
      const auto find_parameter = [&](std::string_view name) {
        const auto found = std::ranges::find(
            callable.parameters, name, &CFDLCallableParameterSyntax::name);
        return found == callable.parameters.end()
                   ? core::AnyId::InvalidIndex
                   : static_cast<std::uint32_t>(
                         std::distance(callable.parameters.begin(), found));
      };
      outcome_buffer_lane = find_parameter(outcome.buffer);
      outcome_capacity_lane = find_parameter(outcome.capacity);
      if (outcome.kind == CFDLOutcomeKind::Win32Read) {
        outcome_count_lane = find_parameter(outcome.count);
        outcome_context_lane = find_parameter(outcome.context);
      } else if (outcome.kind == CFDLOutcomeKind::Fread) {
        outcome_size_lane = find_parameter(outcome.element_size);
        outcome_count_lane = find_parameter(outcome.element_count);
        outcome_capacity_lane = outcome_count_lane;
        outcome_context_lane = find_parameter(outcome.stream);
      }
      outcome_element_type = resolve_type(outcome.element_type, outcome.offset);
      const auto common_lanes =
          outcome_buffer_lane != core::AnyId::InvalidIndex &&
          outcome_capacity_lane != core::AnyId::InvalidIndex &&
          outcome_buffer_lane != outcome_capacity_lane;
      const auto &raw_result = sem_ir.type(result_type);
      const auto &buffer_type =
          common_lanes ? sem_ir.type(parameters[outcome_buffer_lane])
                       : sem_ir.type(sem_ir.voidType());
      const auto &capacity_type =
          common_lanes ? sem_ir.type(parameters[outcome_capacity_lane])
                       : sem_ir.type(sem_ir.voidType());
      const auto &element_type = outcome_element_type.hasValue()
                                     ? sem_ir.type(outcome_element_type)
                                     : sem_ir.type(sem_ir.voidType());
      const auto failure_bits =
          raw_result.kind == SemTypeKind::Integer
              ? (raw_result.arg0 == 64
                     ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << raw_result.arg0) - 1)
              : 0;
      const auto common_outcome =
          usesWideSliceIndices(language_version) && common_lanes &&
          callable.parameters[outcome_buffer_lane].type.qualifier ==
              CFDLFlowQualifier::ViewMut &&
          callable.parameters[outcome_capacity_lane].type.qualifier ==
              CFDLFlowQualifier::Value &&
          buffer_type.kind == SemTypeKind::RawPointer &&
          !sem_ir.rawPointerPointeeConst(parameters[outcome_buffer_lane]) &&
          capacity_type.kind == SemTypeKind::Integer &&
          capacity_type.arg1 == 0 &&
          element_type.kind == SemTypeKind::Integer && element_type.arg0 == 8 &&
          callable.error_contract;
      const auto valid_posix =
          outcome.kind == CFDLOutcomeKind::PosixRead &&
          target_triple.find("linux") != std::string_view::npos &&
          common_outcome && raw_result.kind == SemTypeKind::Integer &&
          raw_result.arg1 != 0 && capacity_type.arg0 == raw_result.arg0 &&
          callable.error_contract->domain == CFDLErrorDomainKind::Errno &&
          normalized_error_predicate ==
              ErrorArtifact::ErrorPredicate::IntegerSet &&
          normalized_error_signed && !normalized_error_inverted &&
          normalized_error_intervals.size() == 1 &&
          normalized_error_intervals.front().lower == failure_bits &&
          normalized_error_intervals.front().upper == failure_bits;
      const auto win_lanes =
          outcome_count_lane != core::AnyId::InvalidIndex &&
          outcome_context_lane != core::AnyId::InvalidIndex &&
          outcome_count_lane != outcome_context_lane &&
          outcome_count_lane != outcome_buffer_lane &&
          outcome_count_lane != outcome_capacity_lane &&
          outcome_context_lane != outcome_buffer_lane &&
          outcome_context_lane != outcome_capacity_lane;
      const auto fread_lanes =
          outcome_count_lane != core::AnyId::InvalidIndex &&
          outcome_context_lane != core::AnyId::InvalidIndex &&
          outcome_count_lane != outcome_buffer_lane &&
          outcome_context_lane != outcome_buffer_lane &&
          outcome_context_lane != outcome_capacity_lane;
      const auto fread_count_type =
          fread_lanes ? parameters[outcome_count_lane] : TypeId::invalid();
      const auto count_value =
          fread_count_type.hasValue() &&
                  sem_ir.type(fread_count_type).kind == SemTypeKind::Reference
              ? sem_ir.referencePointee(fread_count_type)
              : TypeId::invalid();
      const auto valid_win32 =
          outcome.kind == CFDLOutcomeKind::Win32Read && target_is_windows &&
          common_outcome && win_lanes &&
          callable.parameters[outcome_count_lane].type.qualifier ==
              CFDLFlowQualifier::Out &&
          callable.parameters[outcome_context_lane].type.qualifier ==
              CFDLFlowQualifier::Value &&
          count_value.hasValue() &&
          sem_ir.type(count_value).kind == SemTypeKind::Integer &&
          sem_ir.type(count_value).arg0 == 32 &&
          sem_ir.type(count_value).arg1 == 0 && capacity_type.arg0 == 32 &&
          raw_result.kind == SemTypeKind::Integer && raw_result.arg0 == 32 &&
          sem_ir.type(parameters[outcome_context_lane]).kind ==
              SemTypeKind::RawPointer &&
          callable.error_contract->domain == CFDLErrorDomainKind::Win32 &&
          normalized_error_predicate ==
              ErrorArtifact::ErrorPredicate::IntegerSet &&
          normalized_error_signed && !normalized_error_inverted &&
          normalized_error_intervals.size() == 1 &&
          normalized_error_intervals.front().lower == 0 &&
          normalized_error_intervals.front().upper == 0;
      const auto size_type = outcome_size_lane != core::AnyId::InvalidIndex
                                 ? parameters[outcome_size_lane]
                                 : TypeId::invalid();
      const auto stream_type = outcome_context_lane != core::AnyId::InvalidIndex
                                   ? parameters[outcome_context_lane]
                                   : TypeId::invalid();
      const auto valid_fread =
          outcome.kind == CFDLOutcomeKind::Fread &&
          target_triple.find("linux") != std::string_view::npos &&
          common_lanes && outcome_size_lane != core::AnyId::InvalidIndex &&
          outcome_count_lane != core::AnyId::InvalidIndex &&
          outcome_context_lane != core::AnyId::InvalidIndex &&
          fread_lanes &&
          outcome_size_lane != outcome_count_lane &&
          outcome_size_lane != outcome_buffer_lane &&
          outcome_size_lane != outcome_capacity_lane &&
          outcome_count_lane != outcome_buffer_lane &&
          outcome_context_lane != outcome_buffer_lane &&
          outcome_context_lane != outcome_capacity_lane &&
          callable.parameters[outcome_buffer_lane].type.qualifier ==
              CFDLFlowQualifier::ViewMut &&
          sem_ir.type(size_type).kind == SemTypeKind::Integer &&
          sem_ir.type(size_type).arg1 == 0 &&
          sem_ir.type(fread_count_type).kind == SemTypeKind::Integer &&
          sem_ir.type(fread_count_type).arg1 == 0 &&
          sem_ir.type(size_type).arg0 == sem_ir.type(fread_count_type).arg0 &&
          sem_ir.type(stream_type).kind == SemTypeKind::RawPointer &&
          raw_result.kind == SemTypeKind::Integer && raw_result.arg1 == 0 &&
          raw_result.arg0 == sem_ir.type(fread_count_type).arg0 &&
          element_type.kind == SemTypeKind::Integer && element_type.arg0 == 8 &&
          callable.error_contract->domain == CFDLErrorDomainKind::Errno &&
          callable.error_contract->observed == "ferror" &&
          callable.outcome_contract->eof_symbol.size() <= 1024 &&
          callable.outcome_contract->ferror_symbol.size() <= 1024 &&
          normalized_error_predicate ==
              ErrorArtifact::ErrorPredicate::IntegerSet &&
          normalized_error_signed && normalized_error_inverted &&
          normalized_error_intervals.size() == 1 &&
          normalized_error_intervals.front().lower == 0 &&
          normalized_error_intervals.front().upper == 0;
      if (!valid_posix && !valid_win32 && !valid_fread) {
        report(CFDLDiagnostic::Kind::InvalidCallable, outcome.offset);
        continue;
      }
    }
    std::vector<TypeId> public_parameters;
    std::vector<IdentifierId> public_parameter_names;
    for (std::uint32_t index = 0; index < parameters.size(); ++index) {
      if (callable.outcome_contract &&
          callable.outcome_contract->kind == CFDLOutcomeKind::Win32Read &&
          (index == outcome_count_lane || index == outcome_context_lane))
        continue;
      public_parameters.push_back(parameters[index]);
      public_parameter_names.push_back(
          values.internIdentifier(callable.parameters[index].name));
    }
    const auto function_type = sem_ir.addType(
        {SemTypeKind::Function, sem_ir.addTypeBlock(public_parameters).index,
         result_type.index, 0});
    const std::vector<LocalId> no_locals;
    const std::vector<InstId> no_body;
    const auto function = sem_ir.addFunction(
        {sem_ir.addName(values.internIdentifier(callable.name)), function_type,
         sem_ir.addLocalBlock(no_locals), sem_ir.addInstBlock(no_body),
         SemFunctionPublic});
    auto flow_callable = callable;
    if (callable.outcome_contract &&
        callable.outcome_contract->kind == CFDLOutcomeKind::Win32Read) {
      const auto erase = [&](std::uint32_t lane) {
        flow_callable.parameters.erase(flow_callable.parameters.begin() + lane);
      };
      if (outcome_count_lane > outcome_context_lane) {
        erase(outcome_count_lane);
        erase(outcome_context_lane);
      } else {
        erase(outcome_context_lane);
        erase(outcome_count_lane);
      }
    }
    const auto declaration = makeResourceFlowDeclaration(
        flow_callable, sem_ir.canonicalType(function_type));
    NormalizedResourceFlow normalized_flow;
    std::string flow_error;
    if (!normalizeResourceFlow(declaration, normalized_flow, flow_error)) {
      report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
      continue;
    }
    interop::ForeignOperationArtifact interop_artifact;
    interop_artifact.kind =
        normalized_flow.plan.category == ResourceFlowCategory::Resource
            ? interop::ForeignOperationKind::Resource
        : normalized_flow.plan.category == ResourceFlowCategory::Projection
            ? interop::ForeignOperationKind::Memory
            : interop::ForeignOperationKind::Value;
    std::uint32_t public_lane = 0;
    for (std::uint32_t index = 0; index < callable.parameters.size(); ++index) {
      const auto hidden_count =
          callable.outcome_contract &&
          callable.outcome_contract->kind == CFDLOutcomeKind::Win32Read &&
          index == outcome_count_lane;
      const auto hidden_context =
          callable.outcome_contract &&
          callable.outcome_contract->kind == CFDLOutcomeKind::Win32Read &&
          index == outcome_context_lane;
      if (hidden_count) {
        interop_artifact.argument_sources.push_back(
            {ErrorArtifact::ArgumentSourceKind::OutcomeStorage,
             core::AnyId::InvalidIndex});
      } else if (hidden_context) {
        interop_artifact.argument_sources.push_back(
            {ErrorArtifact::ArgumentSourceKind::NullPointer,
             core::AnyId::InvalidIndex});
      } else {
        if (callable.outcome_contract)
          interop_artifact.argument_sources.push_back(
              {ErrorArtifact::ArgumentSourceKind::PublicArgument, public_lane});
        if (callable.parameters[index].type.qualifier == CFDLFlowQualifier::Out)
          interop_artifact.out_lanes.push_back(public_lane);
        ++public_lane;
      }
    }
    if (callable.error_contract) {
      switch (callable.error_contract->domain) {
      case CFDLErrorDomainKind::Code:
        interop_artifact.error_extractor =
            interop::ForeignOperationArtifact::ErrorExtractor::ReturnedCode;
        interop_artifact.error_success_payload =
            interop::ForeignOperationArtifact::ErrorSuccessPayload::Void;
        break;
      case CFDLErrorDomainKind::Errno:
        interop_artifact.error_extractor =
            interop::ForeignOperationArtifact::ErrorExtractor::Errno;
        interop_artifact.error_success_payload =
            interop::ForeignOperationArtifact::ErrorSuccessPayload::Raw;
        break;
      case CFDLErrorDomainKind::Win32:
        interop_artifact.error_extractor =
            interop::ForeignOperationArtifact::ErrorExtractor::Win32LastError;
        interop_artifact.error_success_payload =
            interop::ForeignOperationArtifact::ErrorSuccessPayload::Raw;
        break;
      case CFDLErrorDomainKind::None:
      case CFDLErrorDomainKind::Count:
        break;
      }
      interop_artifact.error_predicate = normalized_error_predicate;
      interop_artifact.error_intervals = normalized_error_intervals;
      interop_artifact.error_predicate_width = normalized_error_width;
      interop_artifact.error_predicate_signed = normalized_error_signed;
      interop_artifact.error_predicate_inverted = normalized_error_inverted;
    }
    if (callable.outcome_contract) {
      interop_artifact.outcome_projection =
          callable.outcome_contract->kind == CFDLOutcomeKind::PosixRead
              ? interop::ForeignOperationArtifact::OutcomeProjection::PosixRead
          : callable.outcome_contract->kind == CFDLOutcomeKind::Fread
              ? interop::ForeignOperationArtifact::OutcomeProjection::Fread
              : interop::ForeignOperationArtifact::OutcomeProjection::Win32Read;
      interop_artifact.outcome_buffer_lane = outcome_buffer_lane;
      interop_artifact.outcome_capacity_lane = outcome_capacity_lane;
      interop_artifact.outcome_count_lane = outcome_count_lane;
      interop_artifact.outcome_context_lane = outcome_context_lane;
      interop_artifact.outcome_size_lane = outcome_size_lane;
      interop_artifact.outcome_eof_symbol =
          callable.outcome_contract->eof_symbol;
      interop_artifact.outcome_ferror_symbol =
          callable.outcome_contract->ferror_symbol;
      interop_artifact.outcome_element_type =
          PublicType::integer(8, sem_ir.type(outcome_element_type).arg1 != 0);
      if (callable.outcome_contract->kind == CFDLOutcomeKind::Win32Read)
        interop_artifact.outcome_count_type = PublicType::integer(32, false);
    }
    // A status-plus-output CFDL signature publishes its outcome convention in
    // addition to the Initialize effect shared by every output lane.
    if (result_type == sem_ir.i32Type() &&
        !interop_artifact.out_lanes.empty()) {
      interop_artifact.status_lane =
          static_cast<std::uint32_t>(parameters.size());
      interop_artifact.status_success_literal = 0;
      interop_artifact.out_initialized_on_failure = true;
    }
    const auto resource_name = protocolResourceName(file, callable);
    std::unordered_map<std::string, StableFingerprint> action_ids;
    std::unordered_map<std::string, StableFingerprint> event_ids;
    struct ImportedProtocolName {
      std::string package;
      std::string module;
      std::string remainder;
    };
    const auto resolve_imported_name =
        [&](std::string_view name) -> std::optional<ImportedProtocolName> {
      for (const auto &import : imports) {
        const auto *interface = public_interfaces.tryGet(import.interface_id);
        if (!interface)
          continue;
        const auto lookup = import.alias.hasValue()
                                ? values.identifier(import.alias)
                                : values.identifier(interface->moduleName());
        if (!name.starts_with(lookup) || name.size() <= lookup.size() + 2 ||
            name.substr(lookup.size(), 2) != "::")
          continue;
        return ImportedProtocolName{
            std::string(values.identifier(interface->packageName())),
            std::string(values.identifier(interface->moduleName())),
            std::string(name.substr(lookup.size() + 2))};
      }
      return std::nullopt;
    };
    bool protocol_identities_valid = true;
    for (const auto &action_name : declaration.action_names) {
      const auto imported = resolve_imported_name(action_name);
      if (action_name.find("::") != std::string::npos &&
          (!imported || imported->remainder.find("::") != std::string::npos)) {
        report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
        protocol_identities_valid = false;
        continue;
      }
      if (imported) {
        std::string lookup_error;
        const auto *identity = interop_registry.findProtocolIdentity(
            interop::ForeignProtocolIdentityKind::Action, imported->package,
            imported->module, {}, imported->remainder, {}, lookup_error);
        if (!identity) {
          report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
          protocol_identities_valid = false;
          continue;
        }
        action_ids.emplace(action_name, identity->fingerprint);
        continue;
      }
      interop::ForeignProtocolIdentity identity;
      identity.kind = interop::ForeignProtocolIdentityKind::Action;
      identity.canonical_package = std::string(canonical_package);
      identity.canonical_module = std::string(values.identifier(module_name));
      identity.canonical_resource = resource_name;
      identity.canonical_name = action_name;
      identity.fingerprint = protocolFingerprint(
          identity.kind, identity.canonical_package, identity.canonical_module,
          identity.canonical_resource, identity.canonical_name,
          identity.owner_callable);
      action_ids.emplace(action_name, identity.fingerprint);
      interop_artifact.protocol_identities.push_back(std::move(identity));
    }
    for (const auto &event_name : declaration.event_names) {
      const auto imported = resolve_imported_name(event_name);
      if (event_name.find("::") != std::string::npos && !imported) {
        report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
        protocol_identities_valid = false;
        continue;
      }
      if (imported) {
        const auto owner_separator = imported->remainder.find("::");
        if (owner_separator == std::string::npos || owner_separator == 0 ||
            owner_separator + 2 == imported->remainder.size() ||
            imported->remainder.find("::", owner_separator + 2) !=
                std::string::npos) {
          report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
          protocol_identities_valid = false;
          continue;
        }
        std::string lookup_error;
        const auto *identity = interop_registry.findProtocolIdentity(
            interop::ForeignProtocolIdentityKind::Event, imported->package,
            imported->module, {},
            imported->remainder.substr(owner_separator + 2),
            imported->remainder.substr(0, owner_separator), lookup_error);
        if (!identity) {
          report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
          protocol_identities_valid = false;
          continue;
        }
        event_ids.emplace(event_name, identity->fingerprint);
        continue;
      }
      interop::ForeignProtocolIdentity identity;
      identity.kind = interop::ForeignProtocolIdentityKind::Event;
      identity.canonical_package = std::string(canonical_package);
      identity.canonical_module = std::string(values.identifier(module_name));
      identity.canonical_resource = resource_name;
      identity.canonical_name = event_name;
      identity.owner_callable = callable.name;
      identity.fingerprint = protocolFingerprint(
          identity.kind, identity.canonical_package, identity.canonical_module,
          identity.canonical_resource, identity.canonical_name,
          identity.owner_callable);
      event_ids.emplace(event_name, identity.fingerprint);
      interop_artifact.protocol_identities.push_back(std::move(identity));
    }
    for (const auto &[event_name, event_id] : event_ids) {
      if (!std::ranges::any_of(
              interop_artifact.protocol_identities, [&](const auto &identity) {
                return identity.kind ==
                           interop::ForeignProtocolIdentityKind::Event &&
                       identity.fingerprint == event_id;
              }))
        continue;
      interop_artifact.completion_events.push_back(event_id);
      const auto is_cancel =
          event_name == "cancel" || event_name == "cancelled";
      const auto is_wake = event_name == "wake" || event_name == "woken";
      if (is_cancel)
        interop_artifact.cancel_events.push_back(event_id);
      if (is_wake)
        interop_artifact.wake_events.push_back(event_id);
    }
    interop_artifact.requires_quiescence =
        std::ranges::any_of(normalized_flow.facts, [](const auto &fact) {
          return fact.predicate &&
                 *fact.predicate == ResourceFlowPredicate::Quiescent;
        });
    interop_artifact.discharges_quiescence =
        std::ranges::any_of(normalized_flow.facts, [](const auto &fact) {
          return fact.relation ==
                 ResourceFlowRelationKind::ObligationDischarges;
        });
    if (!protocol_identities_valid)
      continue;
    for (const auto &fact : normalized_flow.facts) {
      interop::ForeignCapability capability;
      capability.path = std::string(resourceFlowRelationName(fact.relation));
      const auto placeName = [](ResourceFlowPlace place) {
        std::string value;
        switch (place.kind) {
        case ResourceFlowPlaceKind::Parameter:
          value = "parameter";
          break;
        case ResourceFlowPlaceKind::Result:
          value = "result";
          break;
        case ResourceFlowPlaceKind::Count:
          value = "invalid";
          break;
        }
        return value + ":" + std::to_string(place.index);
      };
      const auto endpointName = [&](ResourceFlowEndpoint endpoint) {
        std::string value;
        switch (endpoint.kind) {
        case ResourceFlowEndpointKind::CallReturn:
          value = "call_return";
          break;
        case ResourceFlowEndpointKind::Event:
          value = "event:" + std::to_string(endpoint.index) + ":" +
                  declaration.event_names.at(endpoint.index);
          return value;
          break;
        case ResourceFlowEndpointKind::Count:
          value = "invalid";
          break;
        }
        return value + ":" + std::to_string(endpoint.index);
      };
      const auto actionName = [&](ResourceFlowAction action) {
        return std::to_string(action.index) + ":" +
               declaration.action_names.at(action.index);
      };
      if (fact.subject.kind != ResourceFlowPlaceKind::Count)
        capability.literals.push_back("subject=" + placeName(fact.subject));
      if (fact.target_place)
        capability.literals.push_back("target=" +
                                      placeName(*fact.target_place));
      if (fact.endpoint)
        capability.literals.push_back("endpoint=" +
                                      endpointName(*fact.endpoint));
      if (fact.action)
        capability.literals.push_back("action=" + actionName(*fact.action));
      if (fact.subject_action)
        capability.literals.push_back("subject_action=" +
                                      actionName(*fact.subject_action));
      if (fact.predicate) {
        std::string predicate;
        switch (*fact.predicate) {
        case ResourceFlowPredicate::Valid:
          predicate = "valid";
          break;
        case ResourceFlowPredicate::Initialized:
          predicate = "initialized";
          break;
        case ResourceFlowPredicate::Quiescent:
          predicate = "quiescent";
          break;
        case ResourceFlowPredicate::SameThread:
          predicate = "same_thread";
          break;
        case ResourceFlowPredicate::Count:
          predicate = "invalid";
          break;
        }
        capability.literals.push_back("predicate=" + predicate);
      }
      interop_artifact.capabilities.push_back(std::move(capability));

      interop::ForeignProtocolEdge edge;
      edge.owner_callable = callable.name;
      switch (fact.relation) {
      case ResourceFlowRelationKind::ObligationObliges:
        edge.kind = interop::ForeignProtocolEdgeKind::Obliges;
        break;
      case ResourceFlowRelationKind::ObligationDischarges:
        edge.kind = interop::ForeignProtocolEdgeKind::Discharges;
        break;
      case ResourceFlowRelationKind::Requires:
        edge.kind = interop::ForeignProtocolEdgeKind::Requires;
        break;
      case ResourceFlowRelationKind::EventInvokes:
        edge.kind = interop::ForeignProtocolEdgeKind::Invokes;
        break;
      case ResourceFlowRelationKind::LoanEscapes:
        edge.kind = interop::ForeignProtocolEdgeKind::Escapes;
        break;
      case ResourceFlowRelationKind::ObligationTransfer:
        edge.kind = interop::ForeignProtocolEdgeKind::Transfers;
        break;
      default:
        edge.kind = interop::ForeignProtocolEdgeKind::Count;
        break;
      }
      if (fact.action) {
        const auto name = declaration.action_names.at(fact.action->index);
        edge.source = action_ids.at(name);
        edge.target = edge.source;
      } else if (fact.subject_action) {
        const auto name =
            declaration.action_names.at(fact.subject_action->index);
        edge.source = action_ids.at(name);
        edge.target = edge.source;
      }
      if (fact.endpoint &&
          fact.endpoint->kind == ResourceFlowEndpointKind::Event) {
        const auto name = declaration.event_names.at(fact.endpoint->index);
        edge.source = event_ids.at(name);
        edge.target = edge.source;
      }
      if (fact.predicate) {
        const auto &published_capability = interop_artifact.capabilities.back();
        if (!published_capability.literals.empty())
          edge.predicate = published_capability.literals.back();
        if (!edge.predicate.starts_with("predicate="))
          edge.predicate.clear();
      }
      if (edge.kind != interop::ForeignProtocolEdgeKind::Count &&
          edge.source.hasValue() && edge.target.hasValue())
        interop_artifact.protocol_edges.push_back(std::move(edge));
    }
    std::string artifact_identity = "CFDL-INTEROP-EPOCH-13\n";
    artifact_identity += normalized_flow.fingerprint.hex();
    artifact_identity += '\n';
    artifact_identity += std::to_string(interop_artifact.status_lane);
    artifact_identity += '\n';
    artifact_identity +=
        std::to_string(interop_artifact.status_success_literal);
    artifact_identity += '\n';
    artifact_identity +=
        interop_artifact.out_initialized_on_failure ? "1" : "0";
    artifact_identity += '\n';
    artifact_identity += std::to_string(
        static_cast<std::uint8_t>(interop_artifact.error_extractor));
    artifact_identity += '\n';
    artifact_identity += std::to_string(
        static_cast<std::uint8_t>(interop_artifact.error_predicate));
    artifact_identity += '\n';
    artifact_identity += std::to_string(
        static_cast<std::uint8_t>(interop_artifact.error_success_payload));
    artifact_identity += '\n';
    artifact_identity += std::to_string(interop_artifact.error_predicate_width);
    artifact_identity += '\n';
    artifact_identity += interop_artifact.error_predicate_signed ? "1" : "0";
    artifact_identity += '\n';
    artifact_identity += interop_artifact.error_predicate_inverted ? "1" : "0";
    for (const auto &interval : interop_artifact.error_intervals) {
      artifact_identity += '\n';
      artifact_identity += std::to_string(interval.lower);
      artifact_identity += ':';
      artifact_identity += std::to_string(interval.upper);
    }
    artifact_identity += '\n';
    artifact_identity += std::to_string(
        static_cast<std::uint8_t>(interop_artifact.outcome_projection));
    artifact_identity += '\n';
    artifact_identity += std::to_string(interop_artifact.outcome_buffer_lane);
    artifact_identity += '\n';
    artifact_identity += std::to_string(interop_artifact.outcome_capacity_lane);
    artifact_identity += '\n';
    artifact_identity += std::to_string(interop_artifact.outcome_count_lane);
    artifact_identity += '\n';
    artifact_identity += std::to_string(interop_artifact.outcome_context_lane);
    if (interop_artifact.outcome_element_type) {
      artifact_identity += '\n';
      artifact_identity +=
          std::to_string(interop_artifact.outcome_element_type->scalar_width);
      artifact_identity += interop_artifact.outcome_element_type->integer_signed
                               ? ":signed"
                               : ":unsigned";
    }
    if (interop_artifact.outcome_count_type) {
      artifact_identity += '\n';
      artifact_identity +=
          std::to_string(interop_artifact.outcome_count_type->scalar_width);
      artifact_identity += interop_artifact.outcome_count_type->integer_signed
                               ? ":signed"
                               : ":unsigned";
    }
    for (const auto source : interop_artifact.argument_sources) {
      artifact_identity += '\n';
      artifact_identity +=
          std::to_string(static_cast<std::uint8_t>(source.kind));
      artifact_identity += ':';
      artifact_identity += std::to_string(source.index);
    }
    for (const auto lane : interop_artifact.out_lanes) {
      artifact_identity += '\n';
      artifact_identity += std::to_string(lane);
    }
    interop_artifact.fingerprint =
        StableFingerprint::fromCanonicalBytes(artifact_identity);
    CallableOwnershipSummary ownership;
    ownership.returns_owned =
        sem_ir.typeRepresentation(result_type).ownership !=
        OwnershipReprKind::Borrowed;
    for (const auto lane : interop_artifact.out_lanes) {
      OwnershipRegion root{.parameter_index = lane};
      ownership.effects.push_back({CallableEffectKind::Initialize, root});
      ownership.postconditions.push_back({root, CallableOutcomeInitialize});
    }
    ownership.canonicalize();
    sem_ir.setFunctionOwnership(function, ownership);
    std::string interop_error;
    const auto interop_reference = interop_registry.publish(
        canonical_package, values.identifier(module_name), callable.name,
        std::move(interop_artifact), interop_error);
    if (!interop_reference.verify(interop_error)) {
      report(CFDLDiagnostic::Kind::InvalidProtocol, callable.offset);
      continue;
    }
    sem_ir.setFunctionDeclaration(
        function, {.kind = SemCallableDeclarationKind::Foreign,
                   .is_unsafe = true,
                   .foreign_abi = values.internIdentifier("C"),
                   .external_symbol =
                       values.internIdentifier(callable.external_symbol.empty()
                                                   ? callable.name
                                                   : callable.external_symbol),
                   .foreign_signature = signature,
                   .interop_artifact = interop_reference,
                   .declared_contract = std::move(ownership),
                   .parameter_names = std::move(public_parameter_names)});
  }

  const std::vector<InstId> declarations;
  sem_ir.setTopBlock(sem_ir.addInstBlock(declarations));
  sem_ir.setPlaceStateQuery(PlaceStateQuery{});
  return sem_ir;
