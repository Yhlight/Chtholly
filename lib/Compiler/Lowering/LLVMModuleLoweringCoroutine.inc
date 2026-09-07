// Included inside ModuleLoweringContext; all methods alias its existing LLVM/LowIR state.
  llvm::Value *lowerInst(LowCoroutineCancellationCheck,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerCancellationCheck(
        builder, function, coroutine_instruction_state_);
  }

  llvm::Value *lowerInst(LowCoroutineCancellationRequested,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerCancellationRequested(
        builder, function, coroutine_instruction_state_);
  }

  llvm::Value *lowerInst(LowCoroutineRuntimeFault inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerRuntimeFault(
        inst, builder, function, sem_ir_, coroutine_instruction_state_);
  }

  llvm::Value *lowerInst(LowCoroutineReturnSuccess,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerTerminalError(
        "coroutine success terminal escaped coroutine frame lowering", builder,
        function, coroutine_instruction_state_);
  }

  llvm::Value *lowerInst(LowCoroutineReturnError,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerTerminalError(
        "coroutine error terminal escaped coroutine frame lowering", builder,
        function, coroutine_instruction_state_);
  }

  llvm::Value *lowerInst(LowCoroutineReturnCancelled,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerTerminalError(
        "coroutine cancellation terminal escaped coroutine frame lowering",
        builder, function, coroutine_instruction_state_);
  }

  llvm::Value *lowerInst(LowCoroutineExecutorSwitch inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineInstructionService::lowerExecutorSwitch(
        inst, builder, function, coroutine_instruction_state_);
  }

  llvm::Value *makeCoroutineChecked(llvm::Value *status, llvm::Value *storage,
                                    TypeId checked_type,
                                    llvm::IRBuilder<> &builder) {
    return LLVMCoroutineLoweringService::makeChecked(
        status, storage, checked_type, builder,
        [this](TypeId type) { return lowerValueType(type); });
  }

  llvm::Value *lowerInst(LowCoroutineTaskCreate inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCoroutineTaskCreateService::lower(
        inst, builder, function, coroutine_task_create_state_);
  }

  void emitCoroutineProtocolTrap(std::uint32_t reason,
                                 llvm::IRBuilder<> &builder) {
    LLVMCoroutineLoweringService::emitProtocolTrap(reason, module_, builder);
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupCreate,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineTaskGroupService::create(
        builder, function, coroutine_task_group_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupAttach inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineTaskGroupService::attach(
        inst, builder, function, coroutine_task_group_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupRequestCancel inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineTaskGroupService::requestCancel(
        inst, builder, function, coroutine_task_group_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupClose inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineTaskGroupService::close(
        inst, builder, function, coroutine_task_group_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupCompletionArm inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineTaskGroupService::completionArm(
        inst, builder, function, coroutine_task_group_state_);
  }
  llvm::Value *lowerInst(LowCoroutineTaskGroupDrain, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return nullptr;
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupErrorDrain, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return nullptr;
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupCancelDrain, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return nullptr;
  }

  llvm::Value *lowerInst(LowCoroutineTaskCompletionArm inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineCompletionService::arm(
        inst, builder, function, coroutine_completion_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskCompletionReady inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return LLVMCoroutineCompletionService::ready(
        inst, builder, coroutine_completion_state_);
  }
  llvm::Value *lowerInst(LowCoroutineTaskCompletionWait, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return nullptr;
  }

  llvm::Value *lowerInst(LowFinishCoroutineTaskCompletion inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    LLVMCoroutineCompletionService::finish(inst, builder,
                                           coroutine_completion_state_);
    return nullptr;
  }

  void armCompletionSet(const CompletionProviderPlan &provider,
                        llvm::Value *set, std::uint32_t count,
                        llvm::IRBuilder<> &builder,
                        llvm::Function &function) {
    LLVMCoroutineCompletionSetService::arm(
        provider, set, count, builder, function,
        coroutine_completion_set_state_);
  }

  void detachCompletionSet(const CompletionProviderPlan &provider,
                           llvm::Value *set, std::uint32_t count,
                           llvm::IRBuilder<> &builder,
                           llvm::Function &function) {
    LLVMCoroutineCompletionSetService::detach(
        provider, set, count, builder, function,
        coroutine_completion_set_state_);
  }

  void releaseCompletionSet(const CompletionProviderPlan &provider,
                            llvm::Value *set, std::uint32_t count,
                            llvm::IRBuilder<> &builder,
                            llvm::Function &function) {
    LLVMCoroutineCompletionSetService::release(
        provider, set, count, builder, function,
        coroutine_completion_set_state_);
  }

  using CompletionCombineProbe = LLVMCompletionCombineProbe;

  CompletionCombineProbe
  probeCompletionSet(const CoroutineTaskCompletionCombinePlan &plan,
                     llvm::Value *set, llvm::IRBuilder<> &builder,
                     llvm::Function &function) {
    return LLVMCoroutineCompletionSetService::probe(
        plan, set, builder, function, coroutine_completion_set_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskCompletionSetCreate inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineCompletionSetService::create(
        inst, builder, function, coroutine_completion_set_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskCompletionCombine inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineCompletionSetService::combine(
        inst, builder, function, coroutine_completion_set_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskSelectionWinner inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return LLVMCoroutineCompletionSetService::selectionWinner(
        inst, builder, coroutine_completion_set_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskSelectionTakeRemaining inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return LLVMCoroutineCompletionSetService::selectionRemaining(
        inst, builder, coroutine_completion_set_state_);
  }

  llvm::Value *lowerInst(LowFinishCoroutineTaskCompletionSet inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    LLVMCoroutineCompletionSetService::finishSet(
        inst, builder, function, coroutine_completion_set_state_);
    return nullptr;
  }

  llvm::Value *lowerInst(LowFinishCoroutineTaskSelection inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    LLVMCoroutineCompletionSetService::finishSelection(
        inst, builder, function, coroutine_completion_set_state_);
    return nullptr;
  }
  llvm::Value *lowerInst(LowCoroutineTaskJoin inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return LLVMCoroutineTaskResultService::join(
        inst, builder, coroutine_task_result_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskQuery inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCoroutineTaskResultService::query(
        inst, builder, function, coroutine_task_result_state_);
  }

  llvm::Value *lowerTaskTake(LowInstId task, TypeId checked_type, bool error,
                             llvm::IRBuilder<> &builder,
                             llvm::Function &function) {
    return LLVMCoroutineTaskResultService::take(
        task, checked_type, error, builder, function,
        coroutine_task_result_state_);
  }

  llvm::Value *lowerInst(LowCoroutineTaskTakeResult inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return lowerTaskTake(inst.arg0, inst.type, false, builder, function);
  }

  llvm::Value *lowerInst(LowCoroutineTaskTakeError inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return lowerTaskTake(inst.arg0, inst.type, true, builder, function);
  }

  llvm::Value *lowerInst(LowCoroutineCheckedStatus inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return LLVMCoroutineTaskResultService::checkedStatus(
        inst, builder, coroutine_task_result_state_);
  }

  llvm::Value *lowerInst(LowCoroutineCheckedTake inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return LLVMCoroutineTaskResultService::checkedTake(
        inst, builder, coroutine_task_result_state_);
  }

  llvm::Value *lowerOutcomePredicate(LowInstId outcome, std::uint32_t state,
                                     llvm::IRBuilder<> &builder) {
    return LLVMCoroutineTaskResultService::outcome(
        outcome, state, builder, coroutine_task_result_state_);
  }

  llvm::Value *lowerInst(LowCoroutineOutcomeCompleted inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return lowerOutcomePredicate(inst.arg0, 4, builder);
  }

  llvm::Value *lowerInst(LowCoroutineOutcomeFailed inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return lowerOutcomePredicate(inst.arg0, 5, builder);
  }

  llvm::Value *lowerInst(LowCoroutineOutcomeCancelled inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return lowerOutcomePredicate(inst.arg0, 3, builder);
  }

  llvm::Value *lowerInst(LowFinishCoroutineTask inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    LLVMCoroutineTaskResultService::finishTask(
        inst, builder, coroutine_task_result_state_);
    return nullptr;
  }

  llvm::Value *lowerInst(LowFinishCoroutineChecked, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return nullptr;
  }
