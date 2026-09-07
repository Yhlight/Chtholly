// Included inside the SemToLowContext class; all methods retain access to the
// single context state and previously declared service views.

  void lowerInst(InstId semantic_id, SemCoroutineSuspend semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineScopeService::suspend(
        semantic_id, semantic, current, coroutine_scope_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineCancellationCheck semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineScopeService::cancellationCheck(
        semantic_id, semantic, current, coroutine_scope_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineRuntimeFault semantic,
                 LowBlockId &current) {
    (void)emit<LowCoroutineRuntimeFault>(current, session_state_.sem_ir.voidType(),
                                         semantic_id, semantic.arg0);
  }

  template <typename LowTerminal>
  void lowerCoroutinePayloadTerminal(InstId semantic_id, InstId payload,
                                     LowBlockId &current) {
    current = internal::LoweringCoroutineInstructionService::payloadTerminal(
        semantic_id, payload,
        std::is_same_v<LowTerminal, LowCoroutineReturnError>, current,
        coroutine_instruction_state_,
        [this](const PlaceCleanupPlan &plan, InstId origin,
               LowBlockId successor) {
          return cleanupTail(plan, origin, successor);
        },
        [this](LowBlockId block, std::size_t depth,
               CoroutineTaskGroupExitIntent intent) {
          return emitTaskScopeDrains(block, depth, intent);
        });
  }
  void lowerInst(InstId semantic_id, SemCoroutineReturnSuccess semantic,
                 LowBlockId &current) {
    lowerCoroutinePayloadTerminal<LowCoroutineReturnSuccess>(
        semantic_id, semantic.arg0, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineReturnError semantic,
                 LowBlockId &current) {
    lowerCoroutinePayloadTerminal<LowCoroutineReturnError>(
        semantic_id, semantic.arg0, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineReturnCancelled,
                 LowBlockId &current) {
    current = internal::LoweringCoroutineInstructionService::cancelledTerminal(
        semantic_id, current, coroutine_instruction_state_,
        [this](const PlaceCleanupPlan &plan, InstId origin,
               LowBlockId successor) {
          return cleanupTail(plan, origin, successor);
        },
        [this](LowBlockId block, std::size_t depth,
               CoroutineTaskGroupExitIntent intent) {
          return emitTaskScopeDrains(block, depth, intent);
        });
  }

  void lowerInst(InstId semantic_id, SemCoroutineExecutorSwitch semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowCoroutineExecutorSwitch>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerTaskCreate(InstId semantic_id, FunctionRefId target,
                       InstBlockId operands, CoroutineTaskCreateMode mode,
                       TypeId checked_type, LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::taskCreate(
        semantic_id, target, operands, mode, checked_type, current,
        active_task_scopes_.empty() ? LowInstId::invalid()
                                    : active_task_scopes_.back().group,
        coroutine_instruction_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCreate semantic,
                 LowBlockId &current) {
    lowerTaskCreate(semantic_id, semantic.arg0, semantic.arg1,
                    CoroutineTaskCreateMode::Root, semantic.type, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineChildTaskCreate semantic,
                 LowBlockId &current) {
    lowerTaskCreate(semantic_id, semantic.arg0, semantic.arg1,
                    CoroutineTaskCreateMode::Child, semantic.type, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCompletionArm semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::completionArm(
        semantic_id, semantic, current, coroutine_instruction_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCompletionReady semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::completionReady(
        semantic_id, semantic, current, coroutine_instruction_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCompletionDetach semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::completionDetach(
        semantic_id, semantic, current, coroutine_instruction_state_);
  }

  void lowerInst(InstId semantic_id,
                 SemCoroutineTaskCompletionSetCreate semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::completionSetCreate(
        semantic_id, semantic, current, coroutine_instruction_state_);
  }

  void lowerCompletionCombine(InstId semantic_id, InstId set,
                              CoroutineTaskCompletionCombineKind operation,
                              TypeId result_type, LowBlockId &current) {
    internal::LoweringCoroutineScopeService::completionCombine(
        semantic_id, set, operation, result_type, current,
        coroutine_scope_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCompletionWaitAll semantic,
                 LowBlockId &current) {
    lowerCompletionCombine(semantic_id, semantic.arg0,
                           CoroutineTaskCompletionCombineKind::WaitAll,
                           semantic.type, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCompletionSelect semantic,
                 LowBlockId &current) {
    lowerCompletionCombine(semantic_id, semantic.arg0,
                           CoroutineTaskCompletionCombineKind::Select,
                           semantic.type, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskCompletionRace semantic,
                 LowBlockId &current) {
    lowerCompletionCombine(semantic_id, semantic.arg0,
                           CoroutineTaskCompletionCombineKind::Race,
                           semantic.type, current);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskSelectionWinner semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::unary(
        semantic_id, semantic.type, current, valueFor(semantic.arg0),
        coroutine_instruction_state_, LowInstKind::CoroutineTaskSelectionWinner);
  }

  void lowerInst(InstId semantic_id,
                 SemCoroutineTaskSelectionTakeRemaining semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::unary(
        semantic_id, semantic.type, current, valueFor(semantic.arg0),
        coroutine_instruction_state_,
        LowInstKind::CoroutineTaskSelectionTakeRemaining);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskJoin semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineInstructionService::unary(
        semantic_id, semantic.type, current, valueFor(semantic.arg0),
        coroutine_instruction_state_, LowInstKind::CoroutineTaskJoin);
  }

#define CHTHOLLY_LOWER_COROUTINE_UNARY(SemName, LowName, KindName)             \
  void lowerInst(InstId semantic_id, SemName semantic, LowBlockId &current) {  \
    internal::LoweringCoroutineInstructionService::unary(                       \
        semantic_id, semantic.type, current, valueFor(semantic.arg0),          \
        coroutine_instruction_state_, LowInstKind::KindName);                   \
  }

  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineTaskQuery, LowCoroutineTaskQuery,
                                 CoroutineTaskQuery)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineTaskTakeResult,
                                 LowCoroutineTaskTakeResult,
                                 CoroutineTaskTakeResult)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineTaskTakeError,
                                 LowCoroutineTaskTakeError, CoroutineTaskTakeError)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineCheckedStatus,
                                 LowCoroutineCheckedStatus, CoroutineCheckedStatus)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineCheckedTake,
                                 LowCoroutineCheckedTake, CoroutineCheckedTake)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineOutcomeCompleted,
                                 LowCoroutineOutcomeCompleted, CoroutineOutcomeCompleted)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineOutcomeFailed,
                                 LowCoroutineOutcomeFailed, CoroutineOutcomeFailed)
  CHTHOLLY_LOWER_COROUTINE_UNARY(SemCoroutineOutcomeCancelled,
                                 LowCoroutineOutcomeCancelled, CoroutineOutcomeCancelled)

#undef CHTHOLLY_LOWER_COROUTINE_UNARY

  void lowerInst(InstId semantic_id, SemMakeCallbackWake semantic,
                 LowBlockId &current) {
    std::vector<LowInstId> operands;
    for (const auto operand : session_state_.sem_ir.instBlock(semantic.arg0))
      operands.push_back(valueFor(operand));
    const auto plan = session_state_.low_ir.callbackWakePlanFor(semantic.type);
    values_[semantic_id.index] =
        emit<LowMakeCallbackWake>(current, semantic.type, semantic_id, plan,
                                  session_state_.low_ir.addValueBlock(operands));
  }

  void lowerInst(InstId semantic_id, SemCallbackWakeReady semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowCallbackWakeReady>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemCallbackDetach semantic,
                 LowBlockId &current) {
    const auto operand_type = TypeId(session_state_.sem_ir.inst(semantic.arg0).type);
    const auto plan = session_state_.low_ir.callbackWakePlanFor(operand_type);
    const std::array operands{valueFor(semantic.arg0)};
    values_[semantic_id.index] =
        emit<LowCallbackDetach>(current, semantic.type, semantic_id, plan,
                                session_state_.low_ir.addValueBlock(operands));
  }

