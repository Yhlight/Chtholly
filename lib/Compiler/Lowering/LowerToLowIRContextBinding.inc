// Included inside the SemToLowContext class; all methods retain access to the
// single context state and previously declared service views.

  void lowerInst(InstId semantic_id, SemDiscardValue semantic,
                 LowBlockId &current) {
    const auto value_type = TypeId(session_state_.sem_ir.inst(semantic.arg0).type);
    const auto source_kind = session_state_.sem_ir.inst(semantic.arg0).kind;
    const auto is_place_read = source_kind == SemInstKind::NameRef ||
                               source_kind == SemInstKind::StructFieldAccess ||
                               source_kind == SemInstKind::UnionFieldAccess ||
                               source_kind == SemInstKind::Index ||
                               source_kind == SemInstKind::Dereference;
    if (!is_place_read && session_state_.sem_ir.typeRepresentation(value_type).ownership ==
                              OwnershipReprKind::Owned)
      current = emitDestroyValue(current, semantic_id, value_type,
                                 valueFor(semantic.arg0));
    values_[semantic_id.index] =
        emit<LowVoidValue>(current, semantic.type, semantic_id);
  }

  void lowerInst(InstId semantic_id, SemMaterializeTemporary semantic,
                 LowBlockId &current) {
    const auto slot = slotFor(semantic.arg0);
    const auto representation = session_state_.sem_ir.typeRepresentation(semantic.type);
    const auto &initializer = session_state_.sem_ir.inst(semantic.arg1);
    if ((initializer.kind == SemInstKind::Call ||
         initializer.kind == SemInstKind::ForeignOperationCall) &&
        isInfallibleConstructorFor(FunctionRefId(initializer.arg0),
                                   semantic.type) &&
        representation.init_repr == InitReprKind::InPlace) {
      std::vector<LowInstId> arguments;
      for (const auto argument :
           session_state_.sem_ir.instBlock(InstBlockId(initializer.arg1)))
        arguments.push_back(valueFor(argument));
      const auto plan = session_state_.low_ir.addConstructPlan(
          {rootPlaceFor(slot), FunctionRefId(initializer.arg0),
           session_state_.low_ir.addValueBlock(arguments)});
      (void)emit<LowConstruct>(current, session_state_.sem_ir.voidType(), semantic_id, plan);
      values_[semantic_id.index] =
          emit<LowLoad>(current, semantic.type, semantic_id, slot);
      return;
    }
    if (representation.init_repr == InitReprKind::ByConversion)
      (void)emit<LowInitializeFromValue>(current, session_state_.sem_ir.voidType(),
                                         semantic_id, slot,
                                         valueFor(semantic.arg1));
    else
      (void)emit<LowInitialize>(current, session_state_.sem_ir.voidType(), semantic_id, slot,
                                valueFor(semantic.arg1));
    values_[semantic_id.index] =
        emit<LowLoad>(current, semantic.type, semantic_id, slot);
  }

  void lowerInst(InstId semantic_id, SemEndFullExpression,
                 LowBlockId &current) {
    current =
        emitCleanup(current, semantic_id,
                    session_state_.sem_ir.placeStates().fullExpressionCleanup(semantic_id));
    values_[semantic_id.index] =
        emit<LowVoidValue>(current, session_state_.sem_ir.voidType(), semantic_id);
  }

  void lowerInst(InstId, SemExtendTemporary, LowBlockId &) {}

  void lowerInst(InstId semantic_id, SemCallbackAdapterCall semantic,
                 LowBlockId &current) {
    const auto adapter_type = TypeId(session_state_.sem_ir.inst(semantic.arg0).type);
    const auto plan = session_state_.low_ir.callbackAdapterPlanFor(adapter_type);
    std::vector<LowInstId> operands{valueFor(semantic.arg0)};
    for (const auto argument : session_state_.sem_ir.instBlock(semantic.arg1))
      operands.push_back(valueFor(argument));
    values_[semantic_id.index] =
        emit<LowCallbackAdapterCall>(current, semantic.type, semantic_id, plan,
                                     session_state_.low_ir.addValueBlock(operands));
  }

  void lowerInst(InstId semantic_id, SemBindName semantic,
                 LowBlockId &current) {
    internal::LoweringBindingService::bindName(semantic_id, semantic, current,
                                               binding_state_);
  }

  void lowerInst(InstId semantic_id, SemDeclareUninitialized semantic,
                 LowBlockId &current) {
    internal::LoweringBindingService::declareUninitialized(
        semantic_id, semantic, current, binding_state_);
  }

  void lowerInst(InstId semantic_id, SemOutPlace semantic,
                 LowBlockId &current) {
    internal::LoweringBindingService::outPlace(semantic_id, semantic, current,
                                               binding_state_);
  }

  void lowerInst(InstId semantic_id, SemInitializePlace semantic,
                 LowBlockId &current) {
    internal::LoweringBindingService::initializePlace(
        semantic_id, semantic, current, binding_state_);
  }

  void lowerInst(InstId semantic_id, SemAssign semantic, LowBlockId &current) {
    internal::LoweringBindingService::assign(semantic_id, semantic, current,
                                             binding_state_);
  }

  void lowerInst(InstId semantic_id, SemPlacement semantic,
                 LowBlockId &current) {
    internal::LoweringBindingService::placement(semantic_id, semantic, current,
                                                binding_state_);
  }
