// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
SemanticContext::checkWaitExpression(NodeId node, std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  if (children.size() != 1)
    return appendInvalid(block, node);
  if (!isCurrentAsyncBody() || defer_depth_ != 0) {
    emit(DiagnosticKind::InvalidAsyncOperation, node);
    return appendInvalid(block, node);
  }
  auto operand = checkExpression(children.front(), block);
  const auto task_type = instType(operand);
  if (sem_ir_.type(task_type).kind != SemTypeKind::CoroutineTask) {
    emit(DiagnosticKind::InvalidWaitOperand, children.front());
    return appendInvalid(block, node);
  }
  if (expressionCategory(sem_ir_, operand) == SemExprCategory::Place)
    operand = appendInst<SemMove>(block, children.front(), task_type, operand);
  else if (expressionCategory(sem_ir_, operand) != SemExprCategory::Temporary &&
           expressionCategory(sem_ir_, operand) != SemExprCategory::Value) {
    emit(DiagnosticKind::InvalidWaitOperand, children.front());
    return appendInvalid(block, node);
  }

  const auto task = sem_ir_.addLocal(
      {sem_ir_.addName(values_.internIdentifier(
           "__wait_task" + std::to_string(temporary_counter_++))),
       task_type, node, 0U});
  (void)appendInst<SemBindName>(block, node, sem_ir_.voidType(), task, operand);
  const auto task_for_arm =
      appendInst<SemNameRef>(block, node, task_type, task);
  const auto completion_type = sem_ir_.coroutineTaskCompletionType();
  const auto checked_completion =
      sem_ir_.addCoroutineCheckedType(completion_type);
  const auto arm = appendInst<SemCoroutineTaskCompletionArm>(
      block, node, checked_completion, task_for_arm);
  const auto completion = takeCoroutineChecked(
      arm, node, CoroutineRuntimeFaultReason::CompletionArm, block);
  const auto completion_local = sem_ir_.addLocal(
      {sem_ir_.addName(values_.internIdentifier(
           "__wait_completion" + std::to_string(temporary_counter_++))),
       completion_type, node, 0U});
  (void)appendInst<SemBindName>(block, node, sem_ir_.voidType(),
                                completion_local, completion);
  const auto completion_ref =
      appendInst<SemNameRef>(block, node, completion_type, completion_local);
  const auto moved_completion =
      appendInst<SemMove>(block, node, completion_type, completion_ref);
  (void)appendInst<SemCoroutineSuspend>(block, node, sem_ir_.voidType(),
                                        moved_completion);

  const auto task_for_join =
      appendInst<SemNameRef>(block, node, task_type, task);
  const auto join = appendInst<SemCoroutineTaskJoin>(
      block, node, sem_ir_.i32Type(), task_for_join);
  const auto zero = appendInst<SemIntegerLiteral>(
      block, node, sem_ir_.i32Type(), sem_ir_.addInteger(0));
  const auto joined =
      appendInst<SemEqual>(block, node, sem_ir_.boolType(), join, zero);
  const auto success = sem_ir_.coroutineTaskSuccessType(task_type);
  const auto error = sem_ir_.coroutineTaskErrorType(task_type);
  auto result_type = success;
  if (error) {
    const auto shape = canonicalResultTypeShape(
        success, *error, node, DiagnosticKind::MissingAsyncResultImport);
    if (!shape)
      return appendInvalid(block, node);
    result_type = shape->type;
  }

  std::vector<InstId> joined_body;
  const auto task_for_query =
      appendInst<SemNameRef>(joined_body, node, task_type, task);
  const auto outcome_type = sem_ir_.addCoroutineTaskOutcomeType(success, error);
  const auto query = appendInst<SemCoroutineTaskQuery>(
      joined_body, node, sem_ir_.addCoroutineCheckedType(outcome_type),
      task_for_query);
  const auto outcome = takeCoroutineChecked(
      query, node, CoroutineRuntimeFaultReason::TaskQuery, joined_body);
  const auto result_value = buildWaitOutcome(task, outcome, node, joined_body);
  if (result_type != sem_ir_.voidType())
    (void)appendInst<SemYield>(joined_body, node, sem_ir_.voidType(),
                               result_value);
  std::vector<InstId> join_failed;
  (void)appendCoroutineRuntimeFault(join_failed, node,
                                    CoroutineRuntimeFaultReason::TaskJoin);
  const std::array join_arms{
      sem_ir_.addInst(SemIfArm{result_type, sem_ir_.addInstBlock(joined_body)},
                      node),
      sem_ir_.addInst(SemIfArm{result_type, sem_ir_.addInstBlock(join_failed)},
                      node)};
  return appendInst<SemIf>(block, node, result_type, joined,
                           sem_ir_.addInstBlock(join_arms, true));
}

[[nodiscard]] InstId
SemanticContext::materializeTemporary(InstId value, NodeId node,
                                      std::vector<InstId> &block) {
  if (full_expression_temporaries_.empty())
    return InstId::invalid();
  const auto type = instType(value);
  const auto name = sem_ir_.addName(values_.internIdentifier(
      "__temporary" + std::to_string(temporary_counter_++)));
  const auto storage = sem_ir_.addLocal(
      {name, type, node, static_cast<std::uint32_t>(SemLocalTemporary)});
  const auto materialized =
      appendInst<SemMaterializeTemporary>(block, node, type, storage, value);
  full_expression_temporaries_.back().push_back(materialized);
  return materialized;
}

[[nodiscard]] InstId
SemanticContext::temporaryBorrowSource(InstId value) const {
  if (!value.hasValue())
    return InstId::invalid();
  const auto &inst = sem_ir_.inst(value);
  if (inst.kind != SemInstKind::BorrowPlace)
    return InstId::invalid();
  auto source = InstId(inst.arg0);
  while (source.hasValue()) {
    const auto &source_inst = sem_ir_.inst(source);
    if (source_inst.kind == SemInstKind::MaterializeTemporary)
      return source;
    if (source_inst.kind != SemInstKind::StructFieldAccess &&
        source_inst.kind != SemInstKind::Index)
      break;
    source = InstId(source_inst.arg0);
  }
  return InstId::invalid();
}

[[nodiscard]] bool
SemanticContext::typeContainsReference(TypeId type, std::uint32_t depth) const {
  if (!type.hasValue() || depth > 128)
    return true;
  const auto &value = sem_ir_.type(type);
  if (value.kind == SemTypeKind::Reference || value.kind == SemTypeKind::Slice)
    return true;
  if (value.kind == SemTypeKind::Array)
    return typeContainsReference(TypeId(value.arg0), depth + 1);
  if (value.kind == SemTypeKind::Tuple) {
    for (const auto element : sem_ir_.typeBlock(TypeBlockId(value.arg0)))
      if (typeContainsReference(element, depth + 1))
        return true;
    return false;
  }
  if (value.kind != SemTypeKind::Nominal)
    return false;
  const auto &fields = sem_ir_.nominalType(NominalTypeId(value.arg0)).fields;
  for (std::uint32_t index = 0; index < fields.size(); ++index)
    if (typeContainsReference(sem_ir_.nominalFieldType(type, index), depth + 1))
      return true;
  return false;
}

[[nodiscard]] InstId
SemanticContext::temporaryBorrowEscapeSource(InstId value) const {
  std::unordered_set<std::uint32_t> visited;
  const auto find = [&](auto &&self, InstId candidate) -> InstId {
    if (!candidate.hasValue() || candidate.index >= sem_ir_.instCount() ||
        !visited.insert(candidate.index).second)
      return InstId::invalid();
    if (const auto direct = temporaryBorrowSource(candidate); direct.hasValue())
      return direct;
    const auto &inst = sem_ir_.inst(candidate);
    const auto find_values = [&](std::span<const InstId> values) {
      for (const auto operand : values)
        if (const auto source = self(self, operand); source.hasValue())
          return source;
      return InstId::invalid();
    };
    if (inst.kind == SemInstKind::Move || inst.kind == SemInstKind::Copy ||
        inst.kind == SemInstKind::UnionInit)
      return self(self, InstId(inst.arg0));
    if (inst.kind == SemInstKind::MaterializeTemporary)
      return self(self, InstId(inst.arg1));
    if (inst.kind == SemInstKind::Closure)
      return find_values(sem_ir_.instBlock(InstBlockId(inst.arg1)));
    if (inst.kind == SemInstKind::AggregateInit ||
        inst.kind == SemInstKind::ArrayLiteral ||
        inst.kind == SemInstKind::TupleLiteral ||
        inst.kind == SemInstKind::EnumInit || inst.kind == SemInstKind::Slice)
      return find_values(sem_ir_.instBlock(InstBlockId(inst.arg0)));
    if ((inst.kind == SemInstKind::Call ||
         inst.kind == SemInstKind::IndirectCall) &&
        typeContainsReference(TypeId(inst.type)))
      return find_values(sem_ir_.instBlock(InstBlockId(inst.arg1)));
    if (inst.kind == SemInstKind::If &&
        typeContainsReference(TypeId(inst.type))) {
      for (const auto arm_id : sem_ir_.instBlock(InstBlockId(inst.arg1))) {
        const auto &arm = sem_ir_.inst(arm_id);
        if (arm.kind != SemInstKind::IfArm)
          continue;
        for (const auto body_id : sem_ir_.instBlock(InstBlockId(arm.arg0))) {
          const auto &body = sem_ir_.inst(body_id);
          if (body.kind == SemInstKind::Yield)
            if (const auto source = self(self, InstId(body.arg0));
                source.hasValue())
              return source;
        }
      }
    }
    if (inst.kind == SemInstKind::Switch &&
        typeContainsReference(TypeId(inst.type))) {
      for (const auto arm_id : sem_ir_.instBlock(InstBlockId(inst.arg1))) {
        const auto &arm = sem_ir_.inst(arm_id);
        if (arm.kind != SemInstKind::SwitchArm)
          continue;
        for (const auto body_id : sem_ir_.instBlock(InstBlockId(arm.arg1))) {
          const auto &body = sem_ir_.inst(body_id);
          if (body.kind == SemInstKind::Yield)
            if (const auto source = self(self, InstId(body.arg0));
                source.hasValue())
              return source;
        }
      }
    }
    return InstId::invalid();
  };
  return find(find, value);
}

[[nodiscard]] TypeId SemanticContext::instType(InstId id) const {
  return TypeId(sem_ir_.inst(id).type);
}

[[nodiscard]] bool SemanticContext::isPlace(InstId value) const {
  const auto kind = sem_ir_.inst(value).kind;
  return kind == SemInstKind::NameRef ||
         kind == SemInstKind::MaterializeTemporary ||
         kind == SemInstKind::StructFieldAccess ||
         kind == SemInstKind::UnionFieldAccess || kind == SemInstKind::Index ||
         kind == SemInstKind::Dereference;
}

[[nodiscard]] bool SemanticContext::isMutablePlace(InstId value) const {
  const auto &inst = sem_ir_.inst(value);
  if (inst.kind == SemInstKind::NameRef) {
    const auto &local = sem_ir_.local(LocalId(inst.arg0));
    return (local.flags & SemLocalMutable) != 0 ||
           (sem_ir_.type(local.type).kind == SemTypeKind::Reference &&
            sem_ir_.referenceMutability(local.type) ==
                SemReferenceMutability::Mutable);
  }
  if (inst.kind == SemInstKind::Dereference) {
    const auto operand_type = instType(InstId(inst.arg0));
    if (sem_ir_.type(operand_type).kind == SemTypeKind::RawPointer)
      return !sem_ir_.rawPointerPointeeConst(operand_type);
    return isMutablePlace(InstId(inst.arg0));
  }
  if (inst.kind == SemInstKind::StructFieldAccess ||
      inst.kind == SemInstKind::UnionFieldAccess ||
      inst.kind == SemInstKind::Index)
    return isMutablePlace(InstId(inst.arg0));
  return false;
}

[[nodiscard]] bool
SemanticContext::containsRawPointerDereference(InstId value) const {
  const auto &inst = sem_ir_.inst(value);
  if (inst.kind == SemInstKind::Dereference) {
    if (sem_ir_.type(instType(InstId(inst.arg0))).kind ==
        SemTypeKind::RawPointer)
      return true;
    return containsRawPointerDereference(InstId(inst.arg0));
  }
  if (inst.kind == SemInstKind::StructFieldAccess ||
      inst.kind == SemInstKind::UnionFieldAccess ||
      inst.kind == SemInstKind::Index)
    return containsRawPointerDereference(InstId(inst.arg0));
  return false;
}

[[nodiscard]] InstId
SemanticContext::acquireCheckedReference(InstId value, NodeId node,
                                         std::vector<InstId> &block) {
  const auto source_type = instType(value);
  if (sem_ir_.type(source_type).kind != SemTypeKind::Reference)
    return value;
  const auto pointee = sem_ir_.referencePointee(source_type);
  const auto transfer = sem_ir_.inst(value);
  if ((transfer.kind == SemInstKind::Move ||
       transfer.kind == SemInstKind::Copy) &&
      !block.empty() && block.back() == value) {
    block.pop_back();
    const auto place =
        appendInst<SemDereference>(block, node, pointee, InstId(transfer.arg0));
    if (transfer.kind == SemInstKind::Move)
      sem_ir_.replaceInst(value, SemMove{pointee, place});
    else
      sem_ir_.replaceInst(value, SemCopy{pointee, place});
    block.push_back(value);
    return value;
  }
  return appendInst<SemDereference>(block, node, pointee, value);
}

[[nodiscard]] bool
SemanticContext::adjustMethodReceiver(InstId &receiver, TypeId expected,
                                      NodeId receiver_node,
                                      std::vector<InstId> &block) {
  if (containsRawPointerDereference(receiver)) {
    emit(DiagnosticKind::RawPointerCapabilityUnavailable, receiver_node);
    return false;
  }
  const auto &expected_type = sem_ir_.type(expected);
  const auto actual = instType(receiver);
  if (expected_type.kind == SemTypeKind::Reference) {
    const auto expected_mutability = sem_ir_.referenceMutability(expected);
    if (sem_ir_.type(actual).kind == SemTypeKind::Reference) {
      const auto actual_mutability = sem_ir_.referenceMutability(actual);
      if (expected_mutability == SemReferenceMutability::Mutable &&
          actual_mutability != SemReferenceMutability::Mutable) {
        emit(DiagnosticKind::ImmutableMethodReceiver, receiver_node);
        return false;
      }
      auto place = receiver;
      auto place_type = actual;
      while (sem_ir_.type(place_type).kind == SemTypeKind::Reference) {
        const auto pointee = sem_ir_.referencePointee(place_type);
        place = appendInst<SemDereference>(block, receiver_node, pointee,
                                           place);
        place_type = pointee;
      }
      if (sem_ir_.canonicalType(place_type) !=
          sem_ir_.canonicalType(sem_ir_.referencePointee(expected))) {
        emit(DiagnosticKind::InvalidMethodReceiver, receiver_node);
        return false;
      }
      if (expected_mutability == SemReferenceMutability::Mutable &&
          !isMutablePlace(place)) {
        emit(DiagnosticKind::ImmutableMethodReceiver, receiver_node);
        return false;
      }
      receiver = appendInst<SemBorrowPlace>(block, receiver_node, expected,
                                             place);
      return true;
    }
    if (!isPlace(receiver)) {
      emit(DiagnosticKind::InvalidMethodReceiver, receiver_node);
      return false;
    }
    if (expected_mutability == SemReferenceMutability::Mutable &&
        !isMutablePlace(receiver)) {
      emit(DiagnosticKind::ImmutableMethodReceiver, receiver_node);
      return false;
    }
    receiver = appendInst<SemBorrowPlace>(
        block, receiver_node,
        sem_ir_.addReferenceType(actual, expected_mutability), receiver);
    return true;
  }
  if (isPlace(receiver) && tree_.kind(receiver_node) != NodeKind::MoveExpr) {
    emit(DiagnosticKind::ExplicitMethodReceiverMoveRequired, receiver_node);
    return false;
  }
  return true;
}

[[nodiscard]] bool
SemanticContext::canBindMethodReceiver(InstId receiver, TypeId expected,
                                       NodeId receiver_node) const {
  if (containsRawPointerDereference(receiver))
    return false;
  const auto &expected_type = sem_ir_.type(expected);
  const auto actual = instType(receiver);
  if (expected_type.kind == SemTypeKind::Reference) {
    const auto expected_mutability = sem_ir_.referenceMutability(expected);
    if (sem_ir_.type(actual).kind == SemTypeKind::Reference) {
      auto pointee = actual;
      while (sem_ir_.type(pointee).kind == SemTypeKind::Reference)
        pointee = sem_ir_.referencePointee(pointee);
      return sem_ir_.canonicalType(pointee) ==
                 sem_ir_.canonicalType(sem_ir_.referencePointee(expected)) &&
             (expected_mutability != SemReferenceMutability::Mutable ||
              (sem_ir_.referenceMutability(actual) ==
                   SemReferenceMutability::Mutable &&
               isMutablePlace(receiver)));
    }
    return isPlace(receiver) &&
           (expected_mutability != SemReferenceMutability::Mutable ||
            isMutablePlace(receiver));
  }
  return !isPlace(receiver) || tree_.kind(receiver_node) == NodeKind::MoveExpr;
}
