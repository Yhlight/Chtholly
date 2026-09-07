// Included inside the SemToLowContext class; all methods retain access to the
// single context state and previously declared service views.

  void lowerInst(InstId semantic_id, SemForeignOperationCall semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::ForeignOperationCallState state{
        session_state_, values_, [this](InstId id) { return valueFor(id); },
        [this](LowBlockId block, TypeId type, InstId origin,
               ForeignCallOutcomePlanId plan, LowValueBlockId operands) {
          return emit<LowForeignResultCall>(block, type, origin, plan, operands);
        },
        [this](InstId id, FunctionRefId target, InstBlockId operands,
               TypeId type, LowBlockId &block) {
          lowerInst(id, SemCall{type, target, operands}, block);
        }};
    internal::LoweringInteropService::foreignOperationCall(
        semantic_id, semantic, current, state);
  }

  void lowerForeignOperationProjection(InstId semantic_id,
                                       FunctionRefId target,
                                       InstBlockId operands, TypeId result_type,
                                       LowBlockId &current,
                                       internal::LoweringInteropService::ForeignOperationProjectionKind kind) {
    internal::LoweringInteropService::ForeignOperationProjectionState state{
        session_state_, values_, [this](InstId id) { return valueFor(id); },
        [this](LowBlockId block, TypeId type, InstId origin,
               ForeignOperationCompletionPlanId plan, LowValueBlockId values,
               internal::LoweringInteropService::ForeignOperationProjectionKind projection_kind) {
          switch (projection_kind) {
          case internal::LoweringInteropService::ForeignOperationProjectionKind::Completion:
            return emit<LowForeignOperationProjectCompletion>(block, type, origin, plan, values);
          case internal::LoweringInteropService::ForeignOperationProjectionKind::Wake:
            return emit<LowForeignOperationProjectWake>(block, type, origin, plan, values);
          case internal::LoweringInteropService::ForeignOperationProjectionKind::Port:
            return emit<LowForeignOperationPortProject>(block, type, origin, plan, values);
          }
          return LowInstId::invalid();
        }};
    internal::LoweringInteropService::foreignOperationProjection(
        semantic_id, target, operands, result_type, current, kind, state);
  }

  void lowerInst(InstId semantic_id,
                 SemForeignOperationProjectCompletion semantic,
                 LowBlockId &current) {
    lowerForeignOperationProjection(
        semantic_id, semantic.arg0, semantic.arg1, semantic.type, current,
        internal::LoweringInteropService::ForeignOperationProjectionKind::Completion);
  }

  void lowerInst(InstId semantic_id, SemForeignOperationProjectWake semantic,
                 LowBlockId &current) {
    lowerForeignOperationProjection(
        semantic_id, semantic.arg0, semantic.arg1, semantic.type, current,
        internal::LoweringInteropService::ForeignOperationProjectionKind::Wake);
  }

  void lowerInst(InstId semantic_id, SemForeignOperationPortProject semantic,
                 LowBlockId &current) {
    lowerForeignOperationProjection(
        semantic_id, semantic.arg0, semantic.arg1, semantic.type, current,
        internal::LoweringInteropService::ForeignOperationProjectionKind::Port);
  }

  void lowerInst(InstId semantic_id, SemCompilerIntrinsicCall semantic,
                 LowBlockId &current) {
    std::vector<LowInstId> arguments;
    for (const auto argument : session_state_.sem_ir.instBlock(semantic.arg1))
      arguments.push_back(valueFor(argument));
    values_[semantic_id.index] = emit<LowCompilerIntrinsicCall>(
        current, semantic.type, semantic_id, semantic.arg0,
        session_state_.low_ir.addValueBlock(arguments));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelSendPrepare semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelSendPrepare>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0),
        valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelSendCommit semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelSendCommit>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0),
        valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelSendCancel semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelSendCancel>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelReceiveAcquire semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelReceiveAcquire>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelReceiveCommit semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelReceiveCommit>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0),
        valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelReceiveCancel semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelReceiveCancel>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemTypedChannelClose semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowTypedChannelClose>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemFunctionValue semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowFunctionValue>(
        current, semantic.type, semantic_id, semantic.arg0);
  }

  void lowerInst(InstId semantic_id, SemIndirectCall semantic,
                 LowBlockId &current) {
    std::vector<LowInstId> arguments;
    for (const auto argument : session_state_.sem_ir.instBlock(semantic.arg1))
      arguments.push_back(valueFor(argument));
    values_[semantic_id.index] = emit<LowIndirectCall>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0),
        session_state_.low_ir.addValueBlock(arguments));
    if (semantic.type == session_state_.sem_ir.neverType())
      (void)emit<LowUnreachable>(current, session_state_.sem_ir.voidType(), semantic_id);
  }

  void lowerInst(InstId semantic_id, SemForeignResourceWrap semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowForeignResourceWrap>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemForeignResourceUnwrap semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowForeignResourceUnwrap>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemForeignResourceValid semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowForeignResourceValid>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemCallbackAdapter semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackAdapter(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemIndirectForeignCall semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::indirectForeignCall(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemMakeCallbackAdapter semantic,
                 LowBlockId &current) {
    std::vector<LowInstId> fields;
    for (const auto field : session_state_.sem_ir.instBlock(semantic.arg0))
      fields.push_back(valueFor(field));
    internal::LoweringInteropService::makeCallbackAdapter(
        semantic_id, semantic, current, fields, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemForeignFunctionRef semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::foreignFunctionRef(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCallbackRegistrationBinding semantic,
                 LowBlockId &) {
    internal::LoweringInteropService::callbackRegistrationBinding(
        semantic_id, semantic, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemMakeCallbackRegistration semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::makeCallbackRegistration(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCallbackRegistrationActive semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackRegistrationActive(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCallbackUnregister semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackRegistrationFinish(
        semantic_id, semantic.arg0, semantic.type, current, expression_state_,
        false);
  }

  void lowerInst(InstId semantic_id, SemCallbackCancel semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackRegistrationFinish(
        semantic_id, semantic.arg0, semantic.type, current, expression_state_,
        true);
  }

  void lowerInst(InstId semantic_id, SemCallbackCancelAsync semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackRegistrationCancelAsync(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCallbackCompletionPending semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackCompletionPending(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCallbackCompletionPoll semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackCompletionPoll(
        semantic_id, semantic, current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCallbackWait semantic,
                 LowBlockId &current) {
    internal::LoweringInteropService::callbackWait(
        semantic_id, semantic, current, expression_state_);
  }
