// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
[[nodiscard]] InstId SemanticContext::appendInvalid(std::vector<InstId> &block,
                                                    NodeId node) {
  return appendInst<SemInvalid>(block, node, sem_ir_.voidType());
}

[[nodiscard]] bool SemanticContext::isCurrentAsyncBody() const {
  if (!current_function_.hasValue())
    return false;
  const auto &function = sem_ir_.function(current_function_);
  return (function.flags & SemFunctionAsync) != 0 &&
         sem_ir_.type(function.type).kind == SemTypeKind::AsyncFunction;
}

[[nodiscard]] InstId SemanticContext::appendCoroutineRuntimeFault(
    std::vector<InstId> &block, NodeId node,
    CoroutineRuntimeFaultReason reason) {
  return appendInst<SemCoroutineRuntimeFault>(
      block, node, sem_ir_.neverType(),
      sem_ir_.addInteger(static_cast<std::uint32_t>(reason)));
}

[[nodiscard]] InstId
SemanticContext::takeCoroutineChecked(InstId checked, NodeId node,
                                      CoroutineRuntimeFaultReason fault_reason,
                                      std::vector<InstId> &block) {
  const auto checked_type = instType(checked);
  if (sem_ir_.type(checked_type).kind != SemTypeKind::CoroutineChecked)
    return appendInvalid(block, node);
  const auto payload = sem_ir_.coroutineCheckedPayloadType(checked_type);
  const auto status = appendInst<SemCoroutineCheckedStatus>(
      block, node, sem_ir_.i32Type(), checked);
  const auto zero = appendInst<SemIntegerLiteral>(
      block, node, sem_ir_.i32Type(), sem_ir_.addInteger(0));
  const auto succeeded =
      appendInst<SemEqual>(block, node, sem_ir_.boolType(), status, zero);

  std::vector<InstId> success;
  const auto value =
      appendInst<SemCoroutineCheckedTake>(success, node, payload, checked);
  (void)appendInst<SemYield>(success, node, sem_ir_.voidType(), value);
  std::vector<InstId> failure;
  (void)appendCoroutineRuntimeFault(failure, node, fault_reason);
  const std::array arms{
      sem_ir_.addInst(SemIfArm{payload, sem_ir_.addInstBlock(success)}, node),
      sem_ir_.addInst(SemIfArm{payload, sem_ir_.addInstBlock(failure)}, node)};
  return appendInst<SemIf>(block, node, payload, succeeded,
                           sem_ir_.addInstBlock(arms, true));
}

[[nodiscard]] InstId
SemanticContext::appendAsyncCall(std::vector<InstId> &block, NodeId node,
                                 FunctionRefId target,
                                 std::span<const InstId> arguments) {
  if (!isCurrentAsyncBody()) {
    emit(DiagnosticKind::InvalidAsyncOperation, node);
    return appendInvalid(block, node);
  }
  const auto function_type = sem_ir_.functionRef(target).local_type;
  const auto task_type =
      sem_ir_.addCoroutineTaskType(sem_ir_.asyncSuccessType(function_type),
                                   sem_ir_.asyncErrorType(function_type));
  const auto checked_type = sem_ir_.addCoroutineCheckedType(task_type);
  const auto create = appendInst<SemCoroutineChildTaskCreate>(
      block, node, checked_type, target, sem_ir_.addInstBlock(arguments));
  return takeCoroutineChecked(create, node,
                              CoroutineRuntimeFaultReason::TaskCreate, block);
}

[[nodiscard]] std::optional<SemanticContext::ResultTypeShape>
SemanticContext::canonicalResultTypeShape(TypeId success, TypeId error,
                                          NodeId node,
                                          DiagnosticKind missing_diagnostic) {
  const auto result_module = values_.internIdentifier("std::result");
  if (!sem_ir_.importIRs().findByModule(result_module).hasValue()) {
    emit(missing_diagnostic, node);
    return std::nullopt;
  }
  const auto entity = sem_ir_.importIRs().registry().findEntity(
      "std", "std::result", "Result", PublicEntityKind::NominalType);
  if (!entity.hasValue()) {
    emit(missing_diagnostic, node);
    return std::nullopt;
  }
  const auto nominal = materializeImportedNominal(entity, node);
  if (!nominal.hasValue())
    return std::nullopt;
  const std::array arguments{success, error};
  const auto type = sem_ir_.addNominalType(nominal, arguments);
  const auto shape = sem_ir_.canonicalResultShape(type);
  if (!shape || shape->success != success || shape->error != error) {
    emit(missing_diagnostic, node);
    return std::nullopt;
  }
  return ResultTypeShape{type, *shape};
}

[[nodiscard]] InstId
SemanticContext::makeResultVariant(std::vector<InstId> &block, NodeId node,
                                   TypeId result_type, std::uint32_t variant,
                                   InstId payload) {
  const std::array values{payload};
  return appendInst<SemEnumInit>(block, node, result_type,
                                 sem_ir_.addInstBlock(values, true),
                                 sem_ir_.addInteger(variant));
}

[[nodiscard]] InstId
SemanticContext::appendCancellationTerminal(InstId outcome, NodeId node,
                                            std::vector<InstId> &block) {
  const auto cancelled = appendInst<SemCoroutineOutcomeCancelled>(
      block, node, sem_ir_.boolType(), outcome);
  std::vector<InstId> cancel_body;
  (void)appendInst<SemCoroutineReturnCancelled>(cancel_body, node,
                                                sem_ir_.neverType());
  std::vector<InstId> invalid_body;
  (void)appendCoroutineRuntimeFault(
      invalid_body, node, CoroutineRuntimeFaultReason::UnexpectedTerminal);
  const std::array arms{
      sem_ir_.addInst(
          SemIfArm{sem_ir_.neverType(), sem_ir_.addInstBlock(cancel_body)},
          node),
      sem_ir_.addInst(
          SemIfArm{sem_ir_.neverType(), sem_ir_.addInstBlock(invalid_body)},
          node)};
  return appendInst<SemIf>(block, node, sem_ir_.neverType(), cancelled,
                           sem_ir_.addInstBlock(arms, true));
}

void SemanticContext::appendReturnTerminal(InstId value, NodeId node,
                                           std::vector<InstId> &block) {
  if (!isCurrentAsyncBody()) {
    (void)appendInst<SemReturn>(block, node, sem_ir_.voidType(), value);
    return;
  }
  const auto shape = sem_ir_.canonicalResultShape(return_type_);
  if (!shape) {
    (void)appendInst<SemCoroutineReturnSuccess>(block, node,
                                                sem_ir_.neverType(), value);
    return;
  }

  const auto local = sem_ir_.addLocal(
      {sem_ir_.addName(values_.internIdentifier(
           "__async_return" + std::to_string(temporary_counter_++))),
       return_type_, node, 0U});
  (void)appendInst<SemBindName>(block, node, sem_ir_.voidType(), local, value);
  const auto scrutinee =
      appendInst<SemNameRef>(block, node, return_type_, local);
  const auto payload = [&](std::vector<InstId> &arm, TypeId payload_type,
                           std::uint32_t variant) {
    if (payload_type == sem_ir_.voidType())
      return appendInst<SemVoidValue>(arm, node, payload_type);
    const auto encoded = static_cast<std::uint64_t>(variant) << 32U;
    const auto access = appendInst<SemEnumPayloadAccess>(
        arm, node, payload_type, scrutinee,
        sem_ir_.addInteger(static_cast<std::int64_t>(encoded)));
    return appendInst<SemMove>(arm, node, payload_type, access);
  };

  std::vector<InstId> success_body;
  const auto success = payload(success_body, shape->success, shape->ok_variant);
  (void)appendInst<SemCoroutineReturnSuccess>(success_body, node,
                                              sem_ir_.neverType(), success);
  std::vector<InstId> error_body;
  const auto error = payload(error_body, shape->error, shape->err_variant);
  (void)appendInst<SemCoroutineReturnError>(error_body, node,
                                            sem_ir_.neverType(), error);
  const std::array arms{
      sem_ir_.addInst(SemSwitchArm{sem_ir_.neverType(),
                                   sem_ir_.addInteger(shape->ok_variant),
                                   sem_ir_.addInstBlock(success_body)},
                      node),
      sem_ir_.addInst(SemSwitchArm{sem_ir_.neverType(),
                                   sem_ir_.addInteger(shape->err_variant),
                                   sem_ir_.addInstBlock(error_body)},
                      node)};
  (void)appendInst<SemSwitch>(block, node, sem_ir_.neverType(), scrutinee,
                              sem_ir_.addInstBlock(arms, true));
}

[[nodiscard]] InstId
SemanticContext::takeTaskPayload(LocalId task, bool error, TypeId payload,
                                 NodeId node, std::vector<InstId> &block) {
  const auto task_type = sem_ir_.local(task).type;
  const auto reference = appendInst<SemNameRef>(block, node, task_type, task);
  const auto checked_type = sem_ir_.addCoroutineCheckedType(payload);
  const auto checked = error ? appendInst<SemCoroutineTaskTakeError>(
                                   block, node, checked_type, reference)
                             : appendInst<SemCoroutineTaskTakeResult>(
                                   block, node, checked_type, reference);
  return takeCoroutineChecked(checked, node,
                              error ? CoroutineRuntimeFaultReason::TakeError
                                    : CoroutineRuntimeFaultReason::TakeResult,
                              block);
}

[[nodiscard]] InstId
SemanticContext::buildWaitOutcome(LocalId task, InstId outcome, NodeId node,
                                  std::vector<InstId> &block) {
  const auto task_type = sem_ir_.local(task).type;
  const auto success = sem_ir_.coroutineTaskSuccessType(task_type);
  const auto error = sem_ir_.coroutineTaskErrorType(task_type);
  std::optional<ResultTypeShape> result;
  if (error) {
    result = canonicalResultTypeShape(success, *error, node,
                                      DiagnosticKind::MissingAsyncResultImport);
    if (!result)
      return appendInvalid(block, node);
  }
  const auto result_type = result ? result->type : success;
  const auto completed = appendInst<SemCoroutineOutcomeCompleted>(
      block, node, sem_ir_.boolType(), outcome);

  std::vector<InstId> completed_body;
  InstId completed_value;
  if (success == sem_ir_.voidType()) {
    completed_value = appendInst<SemVoidValue>(completed_body, node, success);
  } else {
    completed_value =
        takeTaskPayload(task, false, success, node, completed_body);
  }
  if (result)
    completed_value =
        makeResultVariant(completed_body, node, result_type,
                          result->shape.ok_variant, completed_value);
  if (result_type != sem_ir_.voidType())
    (void)appendInst<SemYield>(completed_body, node, sem_ir_.voidType(),
                               completed_value);

  std::vector<InstId> other_body;
  if (result) {
    const auto failed = appendInst<SemCoroutineOutcomeFailed>(
        other_body, node, sem_ir_.boolType(), outcome);
    std::vector<InstId> failed_body;
    auto error_value =
        *error == sem_ir_.voidType()
            ? appendInst<SemVoidValue>(failed_body, node, *error)
            : takeTaskPayload(task, true, *error, node, failed_body);
    error_value = makeResultVariant(failed_body, node, result_type,
                                    result->shape.err_variant, error_value);
    (void)appendInst<SemYield>(failed_body, node, sem_ir_.voidType(),
                               error_value);
    std::vector<InstId> cancelled_body;
    (void)appendCancellationTerminal(outcome, node, cancelled_body);
    const std::array failed_arms{
        sem_ir_.addInst(
            SemIfArm{result_type, sem_ir_.addInstBlock(failed_body)}, node),
        sem_ir_.addInst(
            SemIfArm{result_type, sem_ir_.addInstBlock(cancelled_body)}, node)};
    const auto failed_value =
        appendInst<SemIf>(other_body, node, result_type, failed,
                          sem_ir_.addInstBlock(failed_arms, true));
    (void)appendInst<SemYield>(other_body, node, sem_ir_.voidType(),
                               failed_value);
  } else {
    (void)appendCancellationTerminal(outcome, node, other_body);
  }

  const std::array arms{
      sem_ir_.addInst(
          SemIfArm{result_type, sem_ir_.addInstBlock(completed_body)}, node),
      sem_ir_.addInst(SemIfArm{result_type, sem_ir_.addInstBlock(other_body)},
                      node)};
  return appendInst<SemIf>(block, node, result_type, completed,
                           sem_ir_.addInstBlock(arms, true));
}

[[nodiscard]] InstId
