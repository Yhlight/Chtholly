// Included inside ModuleLoweringContext; all methods alias its existing LLVM/LowIR state.
  llvm::Value *
  emitForeignCallValues(const ForeignAbiFunctionLayout &layout,
                        const ForeignAbiCallLayout &call_layout,
                        llvm::Value *callee,
                        std::span<llvm::Value *const> semantic_arguments,
                        llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMForeignCallLoweringService::emitValues(
        layout, call_layout, callee, semantic_arguments, builder, function,
        foreign_call_state_);
  }
  llvm::Value *lowerInst(LowForeignCall inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto &call_layout = low_ir_.foreignAbiCallLayout(inst.arg0);
    const auto &layout = low_ir_.foreignAbiLayout(call_layout.function_layout);
    const auto semantic_arguments = low_ir_.valueBlock(inst.arg1);
    llvm::SmallVector<llvm::Value *, 4> arguments;
    for (const auto argument : semantic_arguments)
      arguments.push_back(value(argument));
    return emitForeignCallValues(layout, call_layout,
                                 functions_.at(layout.target.index), arguments,
                                 builder, function);
  }

  llvm::Value *lowerInst(LowForeignResultCall inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMForeignResultLoweringService::lower(
        inst, builder, function, foreign_result_state_);
  }
  llvm::Constant *foreignInvalidSentinel(const SemNominalType &resource,
                                         TypeId value_type) {
    return LLVMForeignValueLoweringService::invalidSentinel(
        resource, value_type, foreign_value_state_);
  }

  llvm::Value *lowerInst(LowForeignResourceValid inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return LLVMForeignValueLoweringService::valid(inst, builder,
                                                  foreign_value_state_);
  }

  llvm::Value *lowerInst(LowForeignResourceWrap inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMForeignValueLoweringService::wrap(inst, builder, function,
                                                 foreign_value_state_);
  }

  llvm::Value *lowerInst(LowForeignResourceUnwrap inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return LLVMForeignValueLoweringService::unwrap(inst, builder,
                                                   foreign_value_state_);
  }

  llvm::Value *finishForeignValue(TypeId source_type, llvm::Value *handle,
                                  llvm::IRBuilder<> &builder) {
    return LLVMForeignValueLoweringService::finish(
        source_type, handle, builder, foreign_value_state_);
  }

  llvm::Value *lowerInst(LowFinishForeignResource inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return finishForeignValue(TypeId(low_ir_.inst(inst.arg0).type),
                              value(inst.arg0), builder);
  }

  llvm::Value *lowerInst(LowFinishForeignCompletion inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return finishForeignValue(TypeId(low_ir_.inst(inst.arg0).type),
                              value(inst.arg0), builder);
  }

  llvm::Value *lowerInst(LowCallbackAdapter inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return LLVMCallbackRegistrationService::callbackAdapter(inst,
                                                             callback_state_);
  }

  llvm::Value *lowerInst(LowForeignFunctionRef inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return LLVMCallbackRegistrationService::foreignFunctionRef(
        inst, callback_state_);
  }

  llvm::Value *lowerInst(LowIndirectForeignCall inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCallbackRegistrationService::indirectCall(
        inst, builder, function, callback_state_);
  }

  llvm::Value *lowerInst(LowMakeCallbackAdapter inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return LLVMCallbackRegistrationService::makeAdapter(inst, builder,
                                                        callback_state_);
  }

  llvm::Value *lowerInst(LowMakeCallbackRegistration inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackRegistrationService::makeRegistration(
        inst, builder, function, callback_state_);
  }
  llvm::Value *lowerInst(LowCallbackRegistrationActive inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return LLVMCallbackRegistrationService::registrationActive(
        inst, builder, callback_state_);
  }

  void emitCallbackAdapterRelease(llvm::Value *adapter, TypeId adapter_type,
                                  llvm::IRBuilder<> &builder) {
    LLVMCallbackRegistrationService::releaseAdapter(
        adapter, adapter_type, builder, callback_state_);
  }

  llvm::Value *finishCallbackRegistration(CallbackRegistrationPlanId plan_id,
                                          LowValueBlockId operands, bool cancel,
                                          llvm::IRBuilder<> &builder,
                                          llvm::Function &function) {
    return LLVMCallbackRegistrationService::finishRegistration(
        plan_id, operands, cancel, builder, function, callback_state_);
  }
  llvm::Value *lowerInst(LowFinishCallbackRegistration inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return finishCallbackRegistration(inst.arg0, inst.arg1, false, builder,
                                      function);
  }

  llvm::Value *lowerInst(LowCallbackRegistrationUnregister inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return finishCallbackRegistration(inst.arg0, inst.arg1, false, builder,
                                      function);
  }

  llvm::Value *lowerInst(LowCallbackRegistrationCancel inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return finishCallbackRegistration(inst.arg0, inst.arg1, true, builder,
                                      function);
  }

  llvm::Value *lowerInst(LowCallbackRegistrationCancelAsync inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackRegistrationService::cancelAsync(
        inst, builder, function, callback_state_);
  }
  llvm::Value *lowerInst(LowCallbackCompletionPending inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return LLVMCallbackCompletionService::pending(inst, builder,
                                                  callback_state_);
  }

  llvm::Value *lowerInst(LowCallbackCompletionPoll inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackCompletionService::poll(
        inst, builder, function, callback_state_);
  }
  llvm::Value *finishCallbackCompletionValue(CallbackCompletionPlanId plan_id,
                                             llvm::Value *completion,
                                             llvm::IRBuilder<> &builder,
                                             llvm::Function &function) {
    return LLVMCallbackCompletionService::finishValue(
        plan_id, completion, builder, function, callback_state_);
  }
  llvm::Value *finishCallbackCompletion(CallbackCompletionPlanId plan_id,
                                        LowValueBlockId operands,
                                        llvm::IRBuilder<> &builder,
                                        llvm::Function &function) {
    return LLVMCallbackCompletionService::finish(
        plan_id, operands, builder, function, callback_state_);
  }

  llvm::Value *lowerInst(LowFinishCallbackCompletion inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return finishCallbackCompletion(inst.arg0, inst.arg1, builder, function);
  }

  llvm::Value *lowerInst(LowCallbackCompletionWait inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return finishCallbackCompletion(inst.arg0, inst.arg1, builder, function);
  }

  void emitWakeAdapterRelease(llvm::Value *adapter,
                              const CallbackWakePlan &plan,
                              llvm::IRBuilder<> &builder) {
    LLVMCallbackWakeService::releaseAdapter(
        adapter, plan, builder, callback_wake_state_);
  }

  llvm::Value *lowerInst(LowMakeCallbackWake inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackWakeService::makeWake(
        inst, builder, function, callback_wake_state_);
  }

  llvm::Value *lowerInst(LowForeignOperationProjectCompletion inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackWakeService::projectCompletion(
        inst, builder, function, callback_wake_state_);
  }

  llvm::Value *lowerInst(LowForeignOperationProjectWake inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackWakeService::projectWake(
        inst, builder, function, callback_wake_state_);
  }

  llvm::Value *lowerInst(LowForeignOperationPortProject inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackWakeService::projectPort(
        inst, builder, function, callback_wake_state_);
  }

  llvm::Value *lowerInst(LowCallbackWakeReady inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackWakeService::ready(
        inst, builder, function, callback_wake_state_);
  }

  llvm::Value *finishCallbackWake(CallbackWakePlanId plan_id,
                                  LowValueBlockId operands,
                                  llvm::IRBuilder<> &builder,
                                  llvm::Function &function) {
    return LLVMCallbackWakeService::finish(
        plan_id, operands, builder, function, callback_wake_state_);
  }

  llvm::Value *lowerInst(LowFinishCallbackWake inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return finishCallbackWake(inst.arg0, inst.arg1, builder, function);
  }

  llvm::Value *lowerInst(LowCallbackWakeWait inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return finishCallbackWake(inst.arg0, inst.arg1, builder, function);
  }

  llvm::Value *lowerInst(LowCallbackDetach inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCallbackWakeService::detach(
        inst, builder, function, callback_wake_state_);
  }
  llvm::BasicBlock *coroutineCancellationTarget(llvm::IRBuilder<> &,
                                                llvm::Function &function) {
    assert(coroutine_state_.cancel && coroutine_state_.plan);
    const auto cleanup = std::ranges::find(
        coroutine_state_.plan->cancellation_cleanups,
        coroutine_state_.low_instruction, &CoroutineCancellationCleanup::instruction);
    if (cleanup == coroutine_state_.plan->cancellation_cleanups.end())
      return coroutine_state_.cancel;
    auto *entry =
        llvm::BasicBlock::Create(context_, "task.explicit.cleanup", &function);
    emitCoroutineCleanupGraph(
        function, coroutine_state_.frame_type, coroutine_state_.frame,
        *coroutine_state_.plan, cleanup->cleanup, entry,
        coroutine_state_.cancelled, "task.explicit.cleanup", false);
    return entry;
  }

