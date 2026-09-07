// Included inside SemanticContext::checkCallExpression; this fragment
// aliases the single semantic call-resolution state.
  const auto bypass_overload_resolution = [&] {
    if (free_candidates.size() != 1)
      return false;
    const auto &reference = sem_ir_.functionRef(free_candidates.front());
    const auto kind = sem_ir_.type(reference.local_type).kind;
    if (kind != SemTypeKind::Function && kind != SemTypeKind::AsyncFunction)
      return true;
    if (!reference.local_function.hasValue())
      return false;
    const auto &declaration =
        sem_ir_.functionDeclaration(reference.local_function);
    return declaration.foreign_signature &&
           declaration.foreign_signature->is_variadic;
  }();
  if (!free_candidates.empty() && !bypass_overload_resolution) {
    struct SourceArgument {
      NodeId node;
      IdentifierId name;
      InstId value;
    };
    std::vector<SourceArgument> source_arguments;
    if (method_receiver.hasValue())
      source_arguments.push_back(
          {method_receiver_node, IdentifierId::invalid(), method_receiver});
    bool saw_named = false;
    bool invalid_order = false;
    for (std::size_t index = 1; index < children.size(); ++index) {
      const auto argument_node = children[index];
      if (tree_.kind(argument_node) == NodeKind::NamedArgument) {
        const auto named = tree_.children(argument_node);
        if (named.size() != 2) {
          invalid_order = true;
          continue;
        }
        saw_named = true;
        source_arguments.push_back({named[1], tokenString(named[0]),
                                    checkExpression(named[1], block)});
      } else {
        invalid_order |= saw_named;
        source_arguments.push_back({argument_node, IdentifierId::invalid(),
                                    checkExpression(argument_node, block)});
      }
    }
    if (invalid_order) {
      emit(DiagnosticKind::InvalidNamedArgument, node);
      return appendInvalid(block, node);
    }
    struct CandidatePlan {
      FunctionRefId reference;
      FunctionRefId argument_metadata_reference;
      std::vector<std::int32_t> parameter_sources;
      SemanticCallCandidateRank rank;
    };
    std::vector<CandidatePlan> viable;
    bool named_failure = false;
    for (auto candidate : free_candidates) {
      const auto argument_metadata_reference = candidate;
      auto candidate_reference = sem_ir_.functionRef(candidate);
      const auto template_generic = candidate_reference.generic.hasValue();
      auto candidate_type = sem_ir_.type(candidate_reference.local_type);
      if (candidate_type.kind != SemTypeKind::Function &&
          candidate_type.kind != SemTypeKind::AsyncFunction)
        continue;
      auto candidate_parameters =
          sem_ir_.typeBlock(TypeBlockId(candidate_type.arg0));
      const SemCallableDeclaration *candidate_declaration = nullptr;
      const PublicEntity *candidate_entity = nullptr;
      if (candidate_reference.local_function.hasValue())
        candidate_declaration =
            &sem_ir_.functionDeclaration(candidate_reference.local_function);
      if (candidate_reference.public_entity.hasValue())
        candidate_entity =
            sem_ir_.importIRs().tryGetEntity(candidate_reference.public_entity);
      if (!candidate_declaration && !candidate_entity)
        continue;
      std::vector<std::int32_t> parameter_sources(candidate_parameters.size(),
                                                  -1);
      std::size_t positional = 0;
      bool valid = true;
      for (std::size_t source_index = 0; source_index < source_arguments.size();
           ++source_index) {
        std::size_t parameter = positional;
        if (source_arguments[source_index].name.hasValue()) {
          std::optional<std::size_t> named_parameter;
          if (candidate_entity) {
            const auto requested =
                values_.identifier(source_arguments[source_index].name);
            const auto found_name =
                std::ranges::find(candidate_entity->parameter_names, requested);
            if (found_name != candidate_entity->parameter_names.end())
              named_parameter = static_cast<std::size_t>(std::distance(
                  candidate_entity->parameter_names.begin(), found_name));
          } else {
            const auto found_name =
                std::ranges::find(candidate_declaration->parameter_names,
                                  source_arguments[source_index].name);
            if (found_name != candidate_declaration->parameter_names.end())
              named_parameter = static_cast<std::size_t>(std::distance(
                  candidate_declaration->parameter_names.begin(), found_name));
          }
          if (!named_parameter) {
            named_failure = true;
            valid = false;
            break;
          }
          parameter = *named_parameter;
        } else {
          ++positional;
        }
        if (parameter >= parameter_sources.size() ||
            parameter_sources[parameter] >= 0) {
          named_failure |= source_arguments[source_index].name.hasValue();
          valid = false;
          break;
        }
        parameter_sources[parameter] = static_cast<std::int32_t>(source_index);
      }
      if (!valid)
        continue;
      for (std::size_t parameter = 0; parameter < parameter_sources.size();
           ++parameter) {
        if (parameter_sources[parameter] >= 0)
          continue;
        const auto has_default =
            candidate_entity
                ? parameter < candidate_entity->default_arguments.size() &&
                      candidate_entity->default_arguments[parameter].has_value()
                : parameter < candidate_declaration->default_arguments.size() &&
                      candidate_declaration->default_arguments[parameter]
                          .hasValue();
        if (!has_default) {
          valid = false;
          break;
        }
      }
      if (!valid)
        continue;
      if (template_generic) {
        std::vector<InstId> deduction_arguments;
        deduction_arguments.reserve(candidate_parameters.size());
        for (std::size_t parameter = 0; parameter < candidate_parameters.size();
             ++parameter) {
          const auto source = parameter_sources[parameter];
          if (source >= 0) {
            deduction_arguments.push_back(
                source_arguments[static_cast<std::size_t>(source)].value);
            continue;
          }
          ConstantEntityId default_entity;
          if (candidate_entity) {
            default_entity = materializeImportedDefaultArgument(
                *candidate_entity->default_arguments[parameter],
                candidate_parameters[parameter], node);
          } else {
            default_entity =
                candidate_declaration->default_arguments[parameter];
          }
          if (!default_entity.hasValue()) {
            valid = false;
            break;
          }
          const auto &constant = sem_ir_.constantEntity(default_entity);
          deduction_arguments.push_back(
              constant.value.hasValue()
                  ? constant.value
                  : sem_ir_.addInst(
                        SemConstantRef{constant.type, default_entity}, node));
        }
        if (!valid)
          continue;
        const auto deduction_offset = method_receiver.hasValue() ? 1U : 0U;
        const auto deduction_span = std::span<const InstId>(deduction_arguments)
                                        .subspan(deduction_offset);
        const auto specific =
            specializeFunction(candidate, deduction_span, node, false,
                               explicit_member_arguments, deduction_offset);
        if (!specific.hasValue())
          continue;
        candidate = specific;
        candidate_reference = sem_ir_.functionRef(candidate);
        candidate_type = sem_ir_.type(candidate_reference.local_type);
        candidate_parameters =
            sem_ir_.typeBlock(TypeBlockId(candidate_type.arg0));
      }
      CandidatePlan plan{candidate,
                         argument_metadata_reference,
                         std::move(parameter_sources),
                         {.generic = template_generic}};
      for (std::size_t parameter = 0; parameter < candidate_parameters.size();
           ++parameter) {
        const auto source = plan.parameter_sources[parameter];
        if (source < 0)
          continue;
        if (method_receiver.hasValue() && source == 0) {
          if (!canBindMethodReceiver(method_receiver,
                                     candidate_parameters[parameter],
                                     method_receiver_node)) {
            valid = false;
            break;
          }
          continue;
        }
        if (isInitializationArgument(
                candidate, parameter,
                source_arguments[static_cast<std::size_t>(source)].value,
                candidate_parameters[parameter]))
          continue;
        const auto conversion = queryConversion(
            source_arguments[static_cast<std::size_t>(source)].value,
            candidate_parameters[parameter]);
        if (!conversion.valid()) {
          valid = false;
          break;
        }
        plan.rank.maximum_conversion_rank = std::max<std::uint32_t>(
            plan.rank.maximum_conversion_rank, conversion.rank);
        plan.rank.total_conversion_rank += conversion.rank;
      }
      if (valid)
        viable.push_back(std::move(plan));
    }
    if (viable.empty()) {
      if (method_receiver.hasValue() && free_candidates.size() == 1) {
        const auto &reference = sem_ir_.functionRef(free_candidates.front());
        const auto &type = sem_ir_.type(reference.local_type);
        const auto parameters = sem_ir_.typeBlock(TypeBlockId(type.arg0));
        if (!parameters.empty() &&
            !canBindMethodReceiver(method_receiver, parameters.front(),
                                   method_receiver_node)) {
          auto receiver = method_receiver;
          if (!adjustMethodReceiver(receiver, parameters.front(),
                                    method_receiver_node, block))
            return appendInvalid(block, node);
        }
      }
      if (free_candidates.size() == 1 &&
          sem_ir_.functionRef(free_candidates.front()).generic.hasValue() &&
          std::ranges::none_of(source_arguments, [](const auto &argument) {
            return argument.name.hasValue();
          })) {
        std::vector<InstId> deduction_arguments;
        for (const auto &argument : source_arguments)
          deduction_arguments.push_back(argument.value);
        (void)specializeFunction(free_candidates.front(), deduction_arguments,
                                 node, true);
        return appendInvalid(block, node);
      }
      emit(named_failure ? DiagnosticKind::InvalidNamedArgument
                         : DiagnosticKind::ArgumentCountMismatch,
           node);
      return appendInvalid(block, node);
    }
    std::vector<SemanticCallCandidateRank> ranks;
    ranks.reserve(viable.size());
    for (const auto &candidate : viable)
      ranks.push_back(candidate.rank);
    const auto selection = selectCallCandidate(ranks);
    if (selection.kind == SemanticCallSelectionKind::Ambiguous) {
      emit(DiagnosticKind::AmbiguousCall, node);
      return appendInvalid(block, node);
    }
    assert(selection.kind == SemanticCallSelectionKind::Selected);
    auto selected = std::move(viable[selection.index]);
    function_ref = selected.reference;
    const auto &selected_reference = sem_ir_.functionRef(function_ref);
    const auto &selected_type = sem_ir_.type(selected_reference.local_type);
    const auto selected_parameters =
        sem_ir_.typeBlock(TypeBlockId(selected_type.arg0));
    const auto *selected_declaration =
        selected_reference.local_function.hasValue()
            ? &sem_ir_.functionDeclaration(selected_reference.local_function)
            : nullptr;
    const auto *selected_entity =
        selected_reference.public_entity.hasValue()
            ? sem_ir_.importIRs().tryGetEntity(selected_reference.public_entity)
            : nullptr;
    const auto &argument_metadata_reference =
        sem_ir_.functionRef(selected.argument_metadata_reference);
    const auto *argument_metadata_declaration =
        argument_metadata_reference.local_function.hasValue()
            ? &sem_ir_.functionDeclaration(
                  argument_metadata_reference.local_function)
            : nullptr;
    const auto *argument_metadata_entity =
        argument_metadata_reference.public_entity.hasValue()
            ? sem_ir_.importIRs().tryGetEntity(
                  argument_metadata_reference.public_entity)
            : nullptr;
    if ((selected_declaration && selected_declaration->is_unsafe) ||
        (selected_entity && selected_entity->is_unsafe))
      (void)requireUnsafe(UnsafeOperationKind::UnsafeFunctionCall, callee_node);
    std::vector<InstId> arguments;
    arguments.reserve(selected_parameters.size());
    for (std::size_t parameter = 0; parameter < selected_parameters.size();
         ++parameter) {
      const auto source = selected.parameter_sources[parameter];
      if (source < 0) {
        ConstantEntityId entity;
        if (argument_metadata_entity) {
          entity = materializeImportedDefaultArgument(
              *argument_metadata_entity->default_arguments[parameter],
              selected_parameters[parameter], node);
        } else if (argument_metadata_declaration) {
          entity = argument_metadata_declaration->default_arguments[parameter];
        }
        if (!entity.hasValue()) {
          emit(DiagnosticKind::InvalidDefaultArgument, node);
          return appendInvalid(block, node);
        }
        arguments.push_back(appendInst<SemConstantRef>(
            block, node, selected_parameters[parameter], entity));
        continue;
      }
      auto argument = source_arguments[static_cast<std::size_t>(source)].value;
      const auto source_node =
          source_arguments[static_cast<std::size_t>(source)].node;
      if (prepareInitializationArgument(function_ref, parameter, argument,
                                        selected_parameters[parameter],
                                        source_node, block)) {
        arguments.push_back(argument);
        continue;
      }
      if (method_receiver.hasValue() && source == 0) {
        if (!adjustMethodReceiver(argument, selected_parameters[parameter],
                                  source_node, block))
          return appendInvalid(block, node);
        arguments.push_back(argument);
        continue;
      }
      const auto conversion =
          queryConversion(argument, selected_parameters[parameter]);
      if (!applyConversion(argument, selected_parameters[parameter],
                           source_node, block, conversion)) {
        emit(DiagnosticKind::TypeMismatch, source_node);
        return appendInvalid(block, node);
      }
      arguments.push_back(argument);
    }
    sem_ir_.addFunctionOccurrence(
        occurrence_node, SemSymbolOccurrenceKind::Reference, function_ref);
    if (selected_type.kind == SemTypeKind::AsyncFunction)
      return appendAsyncCall(block, node, function_ref, arguments);
    (void)ensureResultOutcomeType(TypeId(selected_type.arg1));
    return appendResolvedCall(block, node, function_ref,
                              TypeId(selected_type.arg1), arguments);
  }
