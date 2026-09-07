// Included inside the SemToLowContext class; all methods retain access to the
// single context state and previously declared service views.

  void lowerInst(InstId semantic_id, SemIf semantic, LowBlockId &current) {
    internal::LoweringIfState state{
        session_state_, function_slots_, values_,
        [this]() { return newBlock(); },
        [this](InstBlockId block, LowBlockId &entry) { lowerBlock(block, entry); },
        [this](LowBlockId block) { return isTerminated(block); },
        [this](InstId id) { return valueFor(id); },
        [this](LowBlockId block, InstId origin, const PlaceCleanupPlan &cleanup) {
          return emitCleanup(block, origin, cleanup);
        },
        [this](LowBlockId block, InstId origin, LowBlockId target) {
          (void)emit<LowBranch>(block, session_state_.sem_ir.voidType(), origin,
                                target);
        },
        [this](LowBlockId block, InstId origin, LowInstId condition,
               TargetPairId targets) {
          (void)emit<LowBranchIf>(block, session_state_.sem_ir.voidType(),
                                  origin, condition, targets);
        },
        [this](LowBlockId block, InstId origin, SlotId slot, LowInstId value) {
          (void)emit<LowInitialize>(block, session_state_.sem_ir.voidType(),
                                    origin, slot, value);
        },
        [this](LowBlockId block, TypeId type, InstId origin, SlotId slot) {
          return emit<LowLoad>(block, type, origin, slot);
        },
        [this](LowBlockId block, TypeId type, InstId origin) {
          return emit<LowVoidValue>(block, type, origin);
        },
        [this](LowBlockId block, InstId origin) {
          (void)emit<LowUnreachable>(block, session_state_.sem_ir.voidType(),
                                     origin);
        }};
    internal::LoweringIfService::lower(semantic_id, semantic, current, state);
  }

  void lowerInst(InstId semantic_id, SemScopedBlock semantic,
                 LowBlockId &current) {
    internal::LoweringIfState state{
        session_state_, function_slots_, values_,
        [this]() { return newBlock(); },
        [this](InstBlockId block, LowBlockId &entry) { lowerBlock(block, entry); },
        [this](LowBlockId block) { return isTerminated(block); },
        [this](InstId id) { return valueFor(id); },
        [this](LowBlockId block, InstId origin, const PlaceCleanupPlan &cleanup) {
          return emitCleanup(block, origin, cleanup);
        },
        [this](LowBlockId block, InstId origin, LowBlockId target) {
          (void)emit<LowBranch>(block, session_state_.sem_ir.voidType(), origin,
                                target);
        },
        [this](LowBlockId block, InstId origin, LowInstId condition,
               TargetPairId targets) {
          (void)emit<LowBranchIf>(block, session_state_.sem_ir.voidType(),
                                  origin, condition, targets);
        },
        [this](LowBlockId block, InstId origin, SlotId slot, LowInstId value) {
          (void)emit<LowInitialize>(block, session_state_.sem_ir.voidType(),
                                    origin, slot, value);
        },
        [this](LowBlockId block, TypeId type, InstId origin, SlotId slot) {
          return emit<LowLoad>(block, type, origin, slot);
        },
        [this](LowBlockId block, TypeId type, InstId origin) {
          return emit<LowVoidValue>(block, type, origin);
        },
        [this](LowBlockId block, InstId origin) {
          (void)emit<LowUnreachable>(block, session_state_.sem_ir.voidType(),
                                     origin);
        }};
    internal::LoweringIfService::scopedBlock(semantic_id, semantic, current,
                                              state);
  }

  LowBlockId emitTaskScopeDrain(LowBlockId current, InstId origin,
                                LowInstId group,
                                CoroutineTaskGroupExitIntent exit_intent) {
    return internal::LoweringCoroutineScopeService::drain(
        current, origin, group, exit_intent, coroutine_scope_state_);
  }

  LowBlockId emitTaskScopeDrains(LowBlockId current, std::size_t target_depth,
                                 CoroutineTaskGroupExitIntent exit_intent) {
    return internal::LoweringCoroutineScopeService::drains(
        current, target_depth, exit_intent, coroutine_scope_state_);
  }

  void emitScopedCancellationCheck(InstId origin,
                                   const PlaceCleanupPlan &cleanup,
                                   LowBlockId &current) {
    internal::LoweringCoroutineScopeService::cancellationCheck(
        origin, cleanup, current, coroutine_scope_state_);
  }

  void lowerInst(InstId semantic_id, SemCoroutineTaskScope semantic,
                 LowBlockId &current) {
    internal::LoweringCoroutineScopeService::taskScope(
        semantic_id, semantic, current, coroutine_scope_state_);
  }

  void lowerInst(InstId, SemIfArm, LowBlockId &) {
    assert(false && "if arm records are lowered by their if");
  }

  void lowerInst(InstId semantic_id, SemWhile semantic, LowBlockId &current) {
    internal::LoweringLoopService::whileLoop(semantic_id, semantic, current,
                                              loop_state_);
  }

  void lowerInst(InstId semantic_id, SemFor semantic, LowBlockId &current) {
    internal::LoweringLoopService::forLoop(semantic_id, semantic, current,
                                            loop_state_);
  }

  void lowerInst(InstId, SemForClause, LowBlockId &) {
    assert(false && "for clause records are lowered by their for");
  }

  void lowerInst(InstId semantic_id, SemDoWhile semantic, LowBlockId &current) {
    internal::LoweringLoopService::doWhile(semantic_id, semantic, current,
                                            loop_state_);
  }

  void lowerInst(InstId semantic_id, SemBreak, LowBlockId &current) {
    internal::LoweringLoopService::breakLoop(semantic_id, current,
                                              loop_state_);
  }

  void lowerInst(InstId semantic_id, SemContinue, LowBlockId &current) {
    internal::LoweringLoopService::continueLoop(semantic_id, current,
                                                 loop_state_);
  }

  void lowerInst(InstId semantic_id, SemSwitch semantic, LowBlockId &current) {
    internal::LoweringLoopService::switchValue(semantic_id, semantic, current,
                                                 loop_state_);
  }

  void lowerInst(InstId, SemSwitchArm, LowBlockId &) {
    assert(false && "switch arm records are lowered by their switch");
  }

  void lowerInst(InstId, SemYield, LowBlockId &) {
    assert(false && "yield records are lowered by their control expression");
  }

  void lowerInst(InstId, SemDefer, LowBlockId &) {}

  void lowerInst(InstId semantic_id, SemAssert semantic, LowBlockId &current) {
    const auto continuation = newBlock();
    const auto failure = newBlock();
    const auto targets = session_state_.low_ir.addTargets({continuation, failure});
    (void)emit<LowBranchIf>(current, session_state_.sem_ir.voidType(), semantic_id,
                            valueFor(semantic.arg0), targets);
    (void)emit<LowFatalFailure>(failure, session_state_.sem_ir.voidType(), semantic_id,
                                semantic.arg1);
    current = continuation;
  }

  void lowerInst(InstId semantic_id, SemUnrecoverableFailure semantic,
                 LowBlockId &current) {
    (void)emit<LowFatalFailure>(current, session_state_.sem_ir.voidType(), semantic_id,
                                semantic.arg0);
  }

  void lowerInst(InstId semantic_id, SemReturn semantic, LowBlockId &current) {
    const auto return_type = TypeId(session_state_.sem_ir.inst(semantic.arg0).type);
    if (!isRepresentationPack() &&
        session_state_.sem_ir.typeRepresentation(return_type).init_repr ==
            InitReprKind::InPlace)
      (void)emit<LowTransferReturn>(current, session_state_.sem_ir.voidType(), semantic_id,
                                    valueFor(semantic.arg0));
    else if (function_state_.return_slot.hasValue())
      (void)emit<LowInitialize>(current, session_state_.sem_ir.voidType(), semantic_id,
                                function_state_.return_slot, valueFor(semantic.arg0));
    const auto &cleanup = session_state_.sem_ir.placeStates().returnCleanup(semantic_id);
    if (!function_state_.unified_return) {
      if (hasCustomCleanup(cleanup)) {
        const auto return_block = newBlock();
        (void)emit<LowReturn>(return_block, semantic.type, semantic_id,
                              valueFor(semantic.arg0));
        const auto tail = cleanupTail(cleanup, semantic_id, return_block);
        (void)emit<LowBranch>(current, session_state_.sem_ir.voidType(), semantic_id, tail);
        current = return_block;
      } else {
        current = emitCleanup(current, semantic_id, cleanup);
        (void)emit<LowReturn>(current, semantic.type, semantic_id,
                              valueFor(semantic.arg0));
      }
      return;
    }
    const auto tail = cleanupTail(cleanup, semantic_id, function_state_.return_block);
    (void)emit<LowBranch>(current, session_state_.sem_ir.voidType(), semantic_id, tail);
  }

  void lowerInst(InstId semantic_id, SemParameter semantic,
                 LowBlockId &current) {
    const auto slot = slotFor(semantic.arg0);
    const auto parameter = ParameterIndex(next_parameter_++);
    if (hasConversion(session_state_.sem_ir.local(semantic.arg0).type)) {
      const auto value = emit<LowParameterValue>(
          current, session_state_.sem_ir.local(semantic.arg0).type, semantic_id, parameter);
      (void)emit<LowInitializeFromValue>(current, session_state_.sem_ir.voidType(),
                                         semantic_id, slot, value);
    } else {
      (void)emit<LowParameter>(current, session_state_.sem_ir.voidType(), semantic_id, slot,
                               parameter);
    }
  }

  void lowerInst(InstId, SemInvalid, LowBlockId &) {
    assert(false && "invalid semantic instruction in function body");
  }

  void lowerInst(InstId, SemFunctionDecl, LowBlockId &) {
    assert(false && "invalid semantic instruction in function body");
  }

  void lowerInst(InstId, SemInterfaceCall, LowBlockId &) {
    assert(false &&
           "unresolved interface call reached concrete LowIR lowering");
  }

  void lowerInst(InstId, SemTypeQuery, LowBlockId &) {
    assert(false && "unresolved type query reached concrete LowIR lowering");
  }

  void lowerBlock(InstBlockId semantic_block, LowBlockId &current);

