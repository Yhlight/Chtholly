// Included inside the SemToLowContext class; all methods retain access to the
// single context state and previously declared service views.

  void lowerInst(InstId semantic_id, SemIntegerLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::integer(semantic_id, semantic,
                                                 current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemFloatLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::floating(semantic_id, semantic,
                                                  current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemBoolLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::boolean(semantic_id, semantic,
                                                 current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemCharLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::character(semantic_id, semantic,
                                                   current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemVoidValue semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::voidValue(semantic_id, semantic,
                                                   current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemStringLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::string(semantic_id, semantic,
                                                current, expression_state_);
  }

  void lowerInst(InstId semantic_id, SemNullPointer semantic,
                 LowBlockId &current) {
    internal::LoweringExpressionService::nullPointer(semantic_id, semantic,
                                                     current, expression_state_);
  }

  [[nodiscard]] LowInstId lowerConstant(ConstantId constant, InstId semantic_id,
                                        LowBlockId &current);

  void lowerInst(InstId semantic_id, SemConstantRef semantic,
                 LowBlockId &current) {
    const auto &entity = session_state_.sem_ir.constantEntity(semantic.arg0);
    assert(entity.result.isConcrete());
    values_[semantic_id.index] =
        lowerConstant(entity.result.value, semantic_id, current);
  }

  void lowerInst(InstId semantic_id, SemStaticRef semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] =
        emit<LowStaticLoad>(current, semantic.type, semantic_id, semantic.arg0);
  }

  void lowerInst(InstId, SemConstantDecl, LowBlockId &) {}

  void lowerInst(InstId, SemSizeOf, LowBlockId &) {
    assert(false && "unfolded sizeof reached LowIR lowering");
  }

  void lowerInst(InstId, SemAlignOf, LowBlockId &) {
    assert(false && "unfolded alignof reached LowIR lowering");
  }

  void lowerInst(InstId semantic_id, SemArrayLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::arrayLiteral(
        semantic_id, semantic, current, aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemTupleLiteral semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::tupleLiteral(
        semantic_id, semantic, current, aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemSlice semantic, LowBlockId &current) {
    internal::LoweringAggregateService::slice(semantic_id, semantic, current,
                                               aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemAggregateInit semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::aggregateInit(
        semantic_id, semantic, current, aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemClosure semantic, LowBlockId &current) {
    internal::LoweringAggregateService::closure(semantic_id, semantic, current,
                                                 aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemBoundMethod semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::boundMethod(
        semantic_id, semantic, current, aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemUnionInit semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::unionInit(semantic_id, semantic,
                                                  current, aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemEnumInit semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::enumInit(semantic_id, semantic, current,
                                                 aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemEnumTag semantic, LowBlockId &current) {
    internal::LoweringAggregateService::enumTag(semantic_id, semantic, current,
                                                aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemEnumPayloadAccess semantic,
                 LowBlockId &current) {
    internal::LoweringAggregateService::enumPayloadAccess(
        semantic_id, semantic, current, aggregate_state_);
  }

  void lowerInst(InstId semantic_id, SemNameRef semantic, LowBlockId &current) {
    internal::LoweringPlaceService::nameRef(semantic_id, semantic, current,
                                            place_state_);
  }

  void lowerInst(InstId semantic_id, SemBorrowLocal semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::borrowLocal(semantic_id, semantic, current,
                                                place_state_);
  }

  void lowerInst(InstId semantic_id, SemBorrowPlace semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::borrowPlace(semantic_id, semantic, current,
                                                place_state_);
  }

  void lowerInst(InstId semantic_id, SemCarrierView semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::carrierView(semantic_id, semantic, current,
                                                place_state_);
  }

  void lowerInst(InstId semantic_id, SemDereference semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::dereference(semantic_id, semantic, current,
                                                place_state_);
  }

  void lowerInst(InstId semantic_id, SemMove semantic, LowBlockId &current) {
    internal::LoweringPlaceService::move(semantic_id, semantic, current,
                                          place_state_);
  }

  void lowerInst(InstId semantic_id, SemCopy semantic, LowBlockId &current) {
    internal::LoweringPlaceService::copy(semantic_id, semantic, current,
                                          place_state_);
  }

  void lowerInst(InstId semantic_id, SemMemberAccess semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::memberAccess(semantic_id, semantic, current,
                                                 place_state_);
  }

  void lowerInst(InstId semantic_id, SemStructFieldAccess semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::structFieldAccess(
        semantic_id, semantic, current, place_state_);
  }

  void lowerInst(InstId semantic_id, SemUnionFieldAccess semantic,
                 LowBlockId &current) {
    internal::LoweringPlaceService::unionFieldAccess(
        semantic_id, semantic, current, place_state_);
  }

  void lowerInst(InstId semantic_id, SemIndex semantic, LowBlockId &current) {
    internal::LoweringPlaceService::index(semantic_id, semantic, current,
                                          place_state_);
  }

  void lowerInst(InstId semantic_id, SemBuiltinUnary semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] =
        emit<LowBuiltinUnary>(current, semantic.type, semantic_id,
                              valueFor(semantic.arg0), semantic.arg1);
  }

  void lowerInst(InstId semantic_id, SemBuiltinBinary semantic,
                 LowBlockId &current) {
    std::vector<LowInstId> operands;
    for (const auto operand : session_state_.sem_ir.instBlock(semantic.arg0))
      operands.push_back(valueFor(operand));
    values_[semantic_id.index] =
        emit<LowBuiltinBinary>(current, semantic.type, semantic_id,
                               session_state_.low_ir.addValueBlock(operands), semantic.arg1);
  }

  void lowerInst(InstId semantic_id, SemAdd semantic, LowBlockId &current) {
    values_[semantic_id.index] =
        emit<LowAdd>(current, semantic.type, semantic_id,
                     valueFor(semantic.arg0), valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemNumericConvert semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowNumericConvert>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemCheckedNumericCast semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowCheckedNumericCast>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemEqual semantic, LowBlockId &current) {
    values_[semantic_id.index] =
        emit<LowEqual>(current, semantic.type, semantic_id,
                       valueFor(semantic.arg0), valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemLogicalNot semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] = emit<LowLogicalNot>(
        current, semantic.type, semantic_id, valueFor(semantic.arg0));
  }

  void lowerInst(InstId semantic_id, SemLogicalAnd semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] =
        emit<LowLogicalAnd>(current, semantic.type, semantic_id,
                            valueFor(semantic.arg0), valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemLogicalOr semantic,
                 LowBlockId &current) {
    values_[semantic_id.index] =
        emit<LowLogicalOr>(current, semantic.type, semantic_id,
                           valueFor(semantic.arg0), valueFor(semantic.arg1));
  }

  void lowerInst(InstId semantic_id, SemCall semantic, LowBlockId &current) {
    internal::LoweringExpressionService::call(
        semantic_id, semantic, current, expression_state_);
  }
