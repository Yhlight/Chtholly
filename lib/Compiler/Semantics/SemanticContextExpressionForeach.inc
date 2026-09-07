// Included by SemanticContextExpressions.cpp; this fragment aliases the
// shared SemanticContext state and preserves expression phase ordering.
void SemanticContext::checkForeachStatement(NodeId node,
                                            std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  const auto labeled =
      !children.empty() && tree_.kind(children.front()) == NodeKind::LoopLabel;
  const auto offset = labeled ? 1U : 0U;
  if (children.size() != offset + 3 ||
      tree_.kind(children[offset]) != NodeKind::ForeachBinding ||
      tree_.kind(children[offset + 2]) != NodeKind::Block) {
    emit(DiagnosticKind::InvalidForeachBinding, node);
    return;
  }
  if (!requireVersion(FrozenV14LanguageVersion,
                      DiagnosticKind::ForeachRequires14, node))
    return;

  const auto binding = children[offset];
  const auto binding_children = tree_.children(binding);
  if (binding_children.empty() || binding_children.size() > 2 ||
      tree_.kind(binding_children.front()) != NodeKind::Name) {
    emit(DiagnosticKind::InvalidForeachBinding, binding);
    return;
  }

  std::vector<InstId> init;
  const auto source_node = children[offset + 1];
  auto iterator = checkExpression(source_node, init);
  if (expressionCategory(sem_ir_, iterator) == SemExprCategory::Diverging)
    return;
  if (expressionCategory(sem_ir_, iterator) == SemExprCategory::Place) {
    emit(DiagnosticKind::ForeachIteratorMoveRequired, source_node);
    return;
  }
  const auto iterator_type = instType(iterator);
  if (sem_ir_.type(iterator_type).kind != SemTypeKind::Nominal) {
    emit(DiagnosticKind::InvalidForeachIterator, source_node);
    return;
  }
  const auto protocol = resolveIteratorProtocol(iterator_type, source_node);
  if (!protocol) {
    emit(DiagnosticKind::InvalidForeachIterator, source_node);
    return;
  }
  if (sem_ir_.languageVersion() < FrozenV15LanguageVersion) {
    const auto role = protocol->intrinsic_role;
    if ((role != CompilerIntrinsicRole::VecIterNext &&
         role != CompilerIntrinsicRole::VecIterMutNext) ||
        sem_ir_.type(protocol->item_type).kind != SemTypeKind::Reference) {
      emit(DiagnosticKind::InvalidForeachIterator, source_node);
      return;
    }
  }
  const auto next_target = protocol->next;
  const auto step_type = protocol->step_type;
  const auto item_type = protocol->item_type;
  const auto continuation_type = iterator_type;
  const auto item_variant = protocol->item_variant;
  const auto done_variant = protocol->done_variant;
  if (binding_children.size() == 2 &&
      sem_ir_.canonicalType(checkType(binding_children[1])) !=
          sem_ir_.canonicalType(item_type)) {
    emit(DiagnosticKind::ForeachItemTypeMismatch, binding_children[1]);
    return;
  }

  const auto iterator_name = sem_ir_.addName(values_.internIdentifier(
      "$foreach$" + std::to_string(temporary_counter_++)));
  const auto iterator_local = sem_ir_.addLocal(
      {iterator_name, iterator_type, source_node, SemLocalMutable});
  (void)appendInst<SemBindName>(init, node, sem_ir_.voidType(), iterator_local,
                                iterator);

  std::vector<InstId> loop_body;
  auto iterator_ref = appendInst<SemNameRef>(loop_body, source_node,
                                             iterator_type, iterator_local);
  auto moved_iterator =
      appendInst<SemMove>(loop_body, source_node, iterator_type, iterator_ref);
  const std::array call_arguments{moved_iterator};
  const auto step_value = appendResolvedCall(
      loop_body, source_node, next_target, step_type, call_arguments);
  const auto step_name = sem_ir_.addName(values_.internIdentifier(
      "$foreach$step$" + std::to_string(temporary_counter_++)));
  const auto step_local =
      sem_ir_.addLocal({step_name, step_type, source_node, 0U});
  (void)appendInst<SemBindName>(loop_body, node, sem_ir_.voidType(), step_local,
                                step_value);
  const auto step =
      appendInst<SemNameRef>(loop_body, source_node, step_type, step_local);

  std::vector<InstId> item_arm;
  const auto item_encoded =
      (static_cast<std::uint64_t>(item_variant) << 32U) | 0U;
  auto item = appendInst<SemEnumPayloadAccess>(
      item_arm, binding, item_type, step,
      sem_ir_.addInteger(static_cast<std::int64_t>(item_encoded)));
  item = sem_ir_.type(item_type).kind == SemTypeKind::Reference
             ? appendInst<SemCopy>(item_arm, binding, item_type, item)
             : appendInst<SemMove>(item_arm, binding, item_type, item);
  const auto continuation_encoded =
      (static_cast<std::uint64_t>(item_variant) << 32U) | 1U;
  auto continuation = appendInst<SemEnumPayloadAccess>(
      item_arm, binding, continuation_type, step,
      sem_ir_.addInteger(static_cast<std::int64_t>(continuation_encoded)));
  continuation =
      appendInst<SemMove>(item_arm, binding, continuation_type, continuation);
  auto iterator_target =
      appendInst<SemNameRef>(item_arm, binding, iterator_type, iterator_local);
  (void)appendInst<SemAssign>(item_arm, binding, sem_ir_.voidType(),
                              iterator_target, continuation);

  lexical_scopes_.push();
  const auto item_name = nameFor(binding_children.front());
  const auto item_local = sem_ir_.addLocal(
      {item_name, item_type, binding_children.front(),
       tree_.tokens().get(tree_.token(binding)).kind == TokenKind::KwVar
           ? static_cast<std::uint32_t>(SemLocalMutable)
           : 0U});
  sem_ir_.addLocalOccurrence(binding_children.front(),
                             SemSymbolOccurrenceKind::Declaration, item_local);
  bind(item_name, item_local, binding_children.front());
  (void)appendInst<SemBindName>(item_arm, binding, sem_ir_.voidType(),
                                item_local, item);
  checkLoopBlockInto(labeled ? children.front() : NodeId::invalid(),
                     children[offset + 2], item_arm);
  lexical_scopes_.pop();
  if (blockFallsThrough(sem_ir_, item_arm)) {
    const auto value = appendInst<SemVoidValue>(item_arm, children[offset + 2],
                                                sem_ir_.voidType());
    (void)appendInst<SemYield>(item_arm, children[offset + 2],
                               sem_ir_.voidType(), value);
  }

  std::vector<InstId> done_arm;
  (void)appendInst<SemBreak>(done_arm, node, sem_ir_.voidType(),
                             sem_ir_.addInteger(0));
  const std::array arms{
      sem_ir_.addInst(SemSwitchArm{sem_ir_.voidType(),
                                   sem_ir_.addInteger(item_variant),
                                   sem_ir_.addInstBlock(item_arm)},
                      node),
      sem_ir_.addInst(SemSwitchArm{sem_ir_.voidType(),
                                   sem_ir_.addInteger(done_variant),
                                   sem_ir_.addInstBlock(done_arm)},
                      node)};
  (void)appendInst<SemSwitch>(loop_body, node, sem_ir_.voidType(), step,
                              sem_ir_.addInstBlock(arms));

  std::vector<InstId> condition;
  (void)appendInst<SemBoolLiteral>(condition, node, sem_ir_.boolType(),
                                   sem_ir_.addInteger(1));
  const std::array clauses{
      sem_ir_.addInst(SemForClause{sem_ir_.voidType(), sem_ir_.addInteger(0),
                                   sem_ir_.addInstBlock(init)},
                      node),
      sem_ir_.addInst(SemForClause{sem_ir_.voidType(), sem_ir_.addInteger(1),
                                   sem_ir_.addInstBlock(condition)},
                      node),
      sem_ir_.addInst(SemForClause{sem_ir_.voidType(), sem_ir_.addInteger(2),
                                   sem_ir_.addInstBlock({})},
                      node)};
  (void)appendInst<SemFor>(block, node, sem_ir_.voidType(),
                           sem_ir_.addInstBlock(clauses, true),
                           sem_ir_.addInstBlock(loop_body));
}

