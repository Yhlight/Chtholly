// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
void SemanticContext::checkFunctionBody(NodeId node,
                                        std::vector<InstId> &declarations,
                                        bool templates, bool const_functions) {
  const auto found = function_nodes_.find(node.index);
  if (found == function_nodes_.end())
    return;
  const auto function_id = found->second;
  const auto children = tree_.children(node);
  auto body_node = functionChild(node, NodeKind::Block);
  if (!body_node.hasValue())
    body_node = functionChild(node, NodeKind::ValueBlock);
  if (!body_node.hasValue()) {
    const auto binding = functionChild(node, NodeKind::FunctionBindingTarget);
    if (!binding.hasValue())
      return;
    const auto function = sem_ir_.function(function_id);
    if (((function.flags & SemFunctionTemplate) != 0) != templates ||
        (((function.flags & SemFunctionConst) != 0) != const_functions))
      return;
    const auto binding_children = tree_.children(binding);
    if (binding_children.size() != 1) {
      emit(DiagnosticKind::InvalidForeignFunctionReference, binding);
      return;
    }
    std::vector<FunctionRefId> candidates;
    const auto target = binding_children.front();
    if (tree_.kind(target) == NodeKind::QualifiedName)
      candidates = resolveImportedFunctions(target, false);
    else if (tree_.kind(target) == NodeKind::Name) {
      const auto name = nameFor(target);
      if (name.hasValue()) {
        const auto local_functions = function_names_.find(name.index);
        if (local_functions != function_names_.end())
          candidates = local_functions->second;
      }
    }
    FunctionRefId target_ref;
    for (const auto candidate : candidates) {
      const auto &reference = sem_ir_.functionRef(candidate);
      const auto *entity =
          reference.public_entity.hasValue()
              ? sem_ir_.importIRs().tryGetEntity(reference.public_entity)
              : nullptr;
      const auto bindable =
          (reference.local_function.hasValue() &&
           sem_ir_.functionDeclaration(reference.local_function)
               .interop_artifact.has_value()) ||
          (entity && entity->interop_artifact.has_value());
      if (!bindable)
        continue;
      if (target_ref.hasValue()) {
        emit(DiagnosticKind::AmbiguousCall, target);
        return;
      }
      target_ref = candidate;
    }
    if (!target_ref.hasValue()) {
      emit(DiagnosticKind::InvalidForeignFunctionReference, target);
      return;
    }
    const auto &target_type =
        sem_ir_.type(sem_ir_.functionRef(target_ref).local_type);
    if (target_type.kind != SemTypeKind::Function) {
      emit(DiagnosticKind::InvalidForeignFunctionReference, target);
      return;
    }
    const auto parameter_syntax = functionParameters(
        children[(functionChild(node, NodeKind::AsyncModifier).hasValue()
                      ? 1U
                      : 0U) +
                 (hasNodeFlag(tree_.get(node).flags, NodeFlags::IsForeign)
                      ? 1U
                      : 0U) +
                 2U]);
    const auto function_type = sem_ir_.type(function.type);
    const auto parameter_types =
        sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
    const auto target_parameters =
        sem_ir_.typeBlock(TypeBlockId(target_type.arg0));
    const auto target_operation =
        [&]() -> const interop::ForeignOperationArtifact * {
      const auto &reference = sem_ir_.functionRef(target_ref);
      if (reference.local_function.hasValue())
        return sem_ir_.functionDeclaration(reference.local_function)
                       .interop_artifact
                   ? sem_ir_.importIRs().interopRegistry().resolve(
                         *sem_ir_.functionDeclaration(reference.local_function)
                              .interop_artifact)
                   : nullptr;
      const auto *entity =
          sem_ir_.importIRs().tryGetEntity(reference.public_entity);
      return entity && entity->interop_artifact
                 ? sem_ir_.importIRs().interopRegistry().resolve(
                       *entity->interop_artifact)
                 : nullptr;
    }();
    const auto operation_shape_valid = [&] {
      if (!target_operation ||
          target_operation->kind >= interop::ForeignOperationKind::Count)
        return false;
      const auto lane_valid = [&](std::uint32_t lane) {
        return lane <= target_parameters.size();
      };
      for (const auto &capability : target_operation->capabilities) {
        if (std::ranges::any_of(capability.lanes,
                                [&](auto lane) { return !lane_valid(lane); }))
          return false;
      }
      for (const auto &port : target_operation->ports) {
        if (port.kind >= interop::ForeignPortKind::Count ||
            port.effect >= interop::ForeignPortEffect::Count ||
            port.lanes.empty() ||
            std::ranges::any_of(port.lanes, [&](auto lane) {
              if (!lane_valid(lane))
                return true;
              const bool result_lane = lane == target_parameters.size();
              return port.kind == interop::ForeignPortKind::Input &&
                     result_lane;
            }))
          return false;
      }
      const auto projection = target_operation->completion_projection;
      if (projection ==
          interop::ForeignCompletionProjectionKind::ScalarToWake) {
        if (target_operation->completion_carrier_lane >=
                target_parameters.size() ||
            target_operation->wake_callback_lanes.size() != 3 ||
            std::ranges::any_of(
                target_operation->wake_callback_lanes,
                [&](auto lane) { return lane >= target_parameters.size(); }))
          return false;
        std::vector<std::uint32_t> projected_lanes =
            target_operation->wake_callback_lanes;
        projected_lanes.push_back(target_operation->completion_carrier_lane);
        std::ranges::sort(projected_lanes);
        if (std::ranges::adjacent_find(projected_lanes) !=
            projected_lanes.end())
          return false;
        std::size_t completion_count = 0;
        std::size_t adapter_count = 0;
        for (const auto type : parameter_types) {
          completion_count +=
              sem_ir_.type(type).kind == SemTypeKind::CallbackCompletion;
          adapter_count +=
              sem_ir_.type(type).kind == SemTypeKind::CallbackAdapter;
        }
        const auto return_kind = sem_ir_.type(TypeId(function_type.arg1)).kind;
        return completion_count == 1 && adapter_count == 1 &&
               return_kind == SemTypeKind::CallbackWake &&
               parameter_types.size() ==
                   target_parameters.size() - projected_lanes.size() + 2;
      }
      if (projection ==
          interop::ForeignCompletionProjectionKind::ScalarToSubscription) {
        const auto owner =
            sem_ir_.foreignOperationStateOwner(TypeId(function_type.arg1));
        return owner &&
               owner->state == ForeignOperationStateKind::Subscription &&
               parameter_types.size() == target_parameters.size() &&
               target_operation->wake_callback_lanes.size() == 3;
      }
      if (projection ==
          interop::ForeignCompletionProjectionKind::ScalarToCompletion) {
        return target_parameters.size() == parameter_types.size() &&
               sem_ir_.type(TypeId(function_type.arg1)).kind ==
                   SemTypeKind::CallbackCompletion;
      }
      if (projection != interop::ForeignCompletionProjectionKind::None)
        return false;
      if (target_parameters.size() != parameter_types.size())
        return false;
      return true;
    };
    if (!operation_shape_valid()) {
      emit(DiagnosticKind::TypeMismatch, binding);
      return;
    }
    current_function_ = function_id;
    current_generic_ = function.generic;
    current_impl_owner_ = function.semantic_owner;
    current_impl_owner_type_ = TypeId::invalid();
    generic_bindings_.clear();
    lexical_scopes_.push();
    std::vector<LocalId> parameters;
    std::vector<InstId> body;
    for (std::size_t index = 0; index < parameter_syntax.size(); ++index) {
      const auto parameter_name = nameFor(parameter_syntax[index].name);
      const auto local =
          sem_ir_.addLocal({parameter_name, parameter_types[index],
                            parameter_syntax[index].name, 0});
      sem_ir_.addLocalOccurrence(parameter_syntax[index].name,
                                 SemSymbolOccurrenceKind::Declaration, local);
      bind(parameter_name, local, parameter_syntax[index].name);
      parameters.push_back(local);
      (void)appendInst<SemParameter>(body, parameter_syntax[index].name,
                                     parameter_types[index], local);
    }
    current_owner_parameter_ =
        parameters.empty() ? LocalId::invalid() : parameters.front();
    return_type_ = TypeId(function_type.arg1);
    std::vector<InstId> arguments;
    arguments.reserve(target_parameters.size());
    bool valid = true;
    std::vector<InstId> logical_values;
    logical_values.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      auto value =
          appendInst<SemNameRef>(body, parameter_syntax[index].name,
                                 parameter_types[index], parameters[index]);
      if (sem_ir_.type(parameter_types[index]).kind != SemTypeKind::Reference)
        value = appendInst<SemMove>(body, parameter_syntax[index].name,
                                    parameter_types[index], value);
      logical_values.push_back(value);
    }
    const auto projection = target_operation->completion_projection;
    InstId completion_value;
    InstId adapter_value;
    if (projection == interop::ForeignCompletionProjectionKind::ScalarToWake) {
      std::vector<std::size_t> ordinary;
      for (std::size_t index = 0; index < parameter_types.size(); ++index) {
        const auto kind = sem_ir_.type(parameter_types[index]).kind;
        if (kind == SemTypeKind::CallbackCompletion)
          completion_value = logical_values[index];
        else if (kind == SemTypeKind::CallbackAdapter)
          adapter_value = logical_values[index];
        else
          ordinary.push_back(index);
      }
      std::size_t ordinary_index = 0;
      for (std::uint32_t lane = 0; lane < target_parameters.size(); ++lane) {
        InstId source;
        if (projection ==
                interop::ForeignCompletionProjectionKind::ScalarToWake &&
            lane == target_operation->completion_carrier_lane)
          source = completion_value;
        else if (std::ranges::find(target_operation->wake_callback_lanes,
                                   lane) !=
                 target_operation->wake_callback_lanes.end())
          source = adapter_value;
        else if (ordinary_index < ordinary.size()) {
          const auto logical = ordinary[ordinary_index++];
          auto value = logical_values[logical];
          if (!adjustExpression(value, target_parameters[lane],
                                parameter_syntax[logical].name, body)) {
            valid = false;
            break;
          }
          arguments.push_back(value);
          continue;
        } else {
          valid = false;
          break;
        }
        const auto lane_literal = appendInst<SemIntegerLiteral>(
            body, binding, sem_ir_.i32Type(), sem_ir_.addInteger(lane));
        const std::array operands{source, lane_literal};
        arguments.push_back(appendInst<SemForeignOperationPortProject>(
            body, binding, target_parameters[lane], target_ref,
            sem_ir_.addInstBlock(operands)));
      }
      valid = valid && ordinary_index == ordinary.size();
    } else {
      for (std::size_t index = 0; index < parameters.size(); ++index) {
        auto value = logical_values[index];
        const auto owner =
            sem_ir_.foreignOperationStateOwner(parameter_types[index]);
        if (projection ==
                interop::ForeignCompletionProjectionKind::ScalarToCompletion &&
            owner && owner->state == ForeignOperationStateKind::Subscription &&
            target_operation->completion_carrier_lane == index)
          completion_value = value;
        if (!adjustExpression(value, target_parameters[index],
                              parameter_syntax[index].name, body)) {
          auto source_type = parameter_types[index];
          auto projection_source = logical_values[index];
          if (sem_ir_.type(source_type).kind == SemTypeKind::Reference) {
            source_type = TypeId(sem_ir_.type(source_type).arg0);
            projection_source =
                appendInst<SemDereference>(body, parameter_syntax[index].name,
                                           source_type, projection_source);
          }
          const auto source_kind = sem_ir_.type(source_type).kind;
          const auto source_owner =
              sem_ir_.foreignOperationStateOwner(source_type);
          if ((source_kind == SemTypeKind::CallbackCompletion ||
               source_kind == SemTypeKind::CallbackWake ||
               (source_owner && source_owner->state ==
                                    ForeignOperationStateKind::Subscription)) &&
              target_operation->completion_carrier_lane == index) {
            const auto lane_literal = appendInst<SemIntegerLiteral>(
                body, binding, sem_ir_.i32Type(), sem_ir_.addInteger(index));
            const std::array operands{projection_source, lane_literal};
            value = appendInst<SemForeignOperationPortProject>(
                body, binding, target_parameters[index], target_ref,
                sem_ir_.addInstBlock(operands));
            arguments.push_back(value);
            continue;
          }
          valid = false;
          break;
        }
        arguments.push_back(value);
      }
    }
    if (valid) {
      const auto call = target_operation
                            ? appendInst<SemForeignOperationCall>(
                                  body, binding, TypeId(target_type.arg1),
                                  target_ref, sem_ir_.addInstBlock(arguments))
                            : appendInst<SemCall>(
                                  body, binding, TypeId(target_type.arg1),
                                  target_ref, sem_ir_.addInstBlock(arguments));
      auto result = call;
      if (projection ==
          interop::ForeignCompletionProjectionKind::ScalarToWake) {
        const std::array operands{completion_value, adapter_value, call};
        result = appendInst<SemForeignOperationProjectWake>(
            body, binding, TypeId(function_type.arg1), target_ref,
            sem_ir_.addInstBlock(operands));
      } else if (projection == interop::ForeignCompletionProjectionKind::
                                   ScalarToSubscription) {
        const auto lane_literal = appendInst<SemIntegerLiteral>(
            body, binding, sem_ir_.i32Type(),
            sem_ir_.addInteger(target_parameters.size()));
        std::array<InstId, 5> operands;
        for (std::size_t field = 0; field < 3; ++field)
          operands[field] =
              logical_values[target_operation->wake_callback_lanes[field]];
        operands[3] = call;
        operands[4] = lane_literal;
        result = appendInst<SemForeignOperationPortProject>(
            body, binding, TypeId(function_type.arg1), target_ref,
            sem_ir_.addInstBlock(operands));
      } else if (projection ==
                 interop::ForeignCompletionProjectionKind::ScalarToCompletion) {
        const std::array operands{completion_value, call};
        result = appendInst<SemForeignOperationProjectCompletion>(
            body, binding, TypeId(function_type.arg1), target_ref,
            sem_ir_.addInstBlock(operands));
      }
      // A foreign operation returning the success payload is lifted into
      // the caller's canonical Result only after the target shape has been
      // verified.  This keeps scalar calls on the existing fast path while
      // making the structured boundary explicit in SemIR.
      if (target_operation &&
          projection == interop::ForeignCompletionProjectionKind::None) {
        const auto shape =
            sem_ir_.canonicalResultShape(TypeId(function_type.arg1));
        const auto payload_kind = target_operation->success_payload;
        const bool exact = payload_kind == interop::ForeignPayloadKind::Exact &&
                           shape && shape->success == TypeId(target_type.arg1);
        const bool empty = payload_kind == interop::ForeignPayloadKind::Void &&
                           shape && shape->success == sem_ir_.voidType() &&
                           TypeId(target_type.arg1) == sem_ir_.voidType();
        if (exact || empty) {
          const std::array payload{call};
          result =
              appendInst<SemEnumInit>(body, binding, TypeId(function_type.arg1),
                                      sem_ir_.addInstBlock(payload, true),
                                      sem_ir_.addInteger(shape->ok_variant));
        }
      }
      if (!adjustExpression(result, TypeId(function_type.arg1), binding, body))
        valid = false;
      else
        appendReturnTerminal(result, binding, body);
    }
    lexical_scopes_.pop();
    if (!valid) {
      emit(DiagnosticKind::TypeMismatch, binding);
      current_function_ = FunctionId::invalid();
      return;
    }
    const auto body_id = sem_ir_.addInstBlock(body);
    const auto parameters_id = sem_ir_.addLocalBlock(parameters);
    sem_ir_.setFunction(function_id,
                        {function.name, function.type, parameters_id, body_id,
                         function.flags, function.generic, function.specific,
                         function.semantic_owner, function.semantic_role,
                         function.semantic_projector});
    auto declaration = sem_ir_.functionDeclaration(function_id);
    declaration.kind = SemCallableDeclarationKind::Definition;
    sem_ir_.setFunctionDeclaration(function_id, std::move(declaration));
    declarations.push_back(sem_ir_.addInst(
        SemFunctionDecl{sem_ir_.voidType(), function_id, body_id}, node));
    current_function_ = FunctionId::invalid();
    current_impl_owner_ = NominalTypeId::invalid();
    current_impl_owner_type_ = TypeId::invalid();
    return;
  }
  const auto is_foreign =
      hasNodeFlag(tree_.get(node).flags, NodeFlags::IsForeign);
  const auto is_async = functionChild(node, NodeKind::AsyncModifier).hasValue();
  const std::size_t base = (is_async ? 1U : 0U) + (is_foreign ? 1U : 0U);
  const auto parameter_syntax = functionParameters(children[base + 2]);
  const auto function = sem_ir_.function(function_id);
  if (((function.flags & SemFunctionTemplate) != 0) != templates ||
      (((function.flags & SemFunctionConst) != 0) != const_functions))
    return;
  current_function_ = function_id;
  current_generic_ = function.generic;
  current_impl_owner_ = function.semantic_owner;
  current_impl_owner_type_ = TypeId::invalid();
  restoreFunctionGenericEnvironment(function_id);
  const auto &function_type = sem_ir_.type(function.type);
  const auto parameter_types =
      sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
  if (current_impl_owner_.hasValue() && !parameter_types.empty()) {
    current_impl_owner_type_ = parameter_types.front();
    if (sem_ir_.type(current_impl_owner_type_).kind == SemTypeKind::Reference)
      current_impl_owner_type_ =
          sem_ir_.referencePointee(current_impl_owner_type_);
  }
  std::vector<LocalId> parameters;
  std::vector<InstId> body;
  lexical_scopes_.push();
  for (std::size_t index = 0; index < parameter_syntax.size(); ++index) {
    const auto parameter_name =
        parameter_syntax[index].pattern.hasValue()
            ? sem_ir_.addName(values_.internIdentifier("__pattern_parameter_" +
                                                       std::to_string(index)))
            : nameFor(parameter_syntax[index].name);
    const auto parameter_type = parameter_types[index];
    const auto local =
        sem_ir_.addLocal({parameter_name, parameter_type,
                          parameter_syntax[index].pattern.hasValue()
                              ? parameter_syntax[index].pattern
                              : parameter_syntax[index].name,
                          0});
    if (!parameter_syntax[index].pattern.hasValue()) {
      sem_ir_.addLocalOccurrence(parameter_syntax[index].name,
                                 SemSymbolOccurrenceKind::Declaration, local);
      bind(parameter_name, local, parameter_syntax[index].name);
    }
    parameters.push_back(local);
    (void)appendInst<SemParameter>(body, parameter_syntax[index].name,
                                   parameter_type, local);
    if (parameter_syntax[index].pattern.hasValue()) {
      const auto source = appendInst<SemNameRef>(
          body, parameter_syntax[index].pattern, parameter_type, local);
      (void)bindStructuredPattern(parameter_syntax[index].pattern, source, body,
                                  parameter_syntax[index].pattern, false);
    }
  }
  current_owner_parameter_ =
      parameters.empty() ? LocalId::invalid() : parameters.front();
  return_type_ =
      is_async ? checkType(children[base + 3]) : TypeId(function_type.arg1);
  checkCallableBodyInto(body_node, body);
  lexical_scopes_.pop();

  const auto body_id = sem_ir_.addInstBlock(body);
  const auto parameters_id = sem_ir_.addLocalBlock(parameters);
  sem_ir_.setFunction(function_id,
                      {function.name, function.type, parameters_id, body_id,
                       function.flags, function.generic, function.specific,
                       function.semantic_owner, function.semantic_role,
                       function.semantic_projector});
  const auto declaration = sem_ir_.addInst(
      SemFunctionDecl{sem_ir_.voidType(), function_id, body_id}, node);
  declarations.push_back(declaration);
  current_impl_owner_ = NominalTypeId::invalid();
  current_impl_owner_type_ = TypeId::invalid();
  current_function_ = FunctionId::invalid();
  active_resolved_constraints_.clear();
  current_owner_parameter_ = LocalId::invalid();
}
