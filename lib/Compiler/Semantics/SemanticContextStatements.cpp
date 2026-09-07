#include "SemanticContext.h"

namespace chtholly::compiler::semantics_internal {

InstBlockId SemanticContext::checkLoopBlock(NodeId label, NodeId block) {
  pushLoop(label);
  const auto result = checkBlock(block);
  active_loops_.pop_back();
  return result;
}

void SemanticContext::checkLoopBlockInto(NodeId label, NodeId block,
                                         std::vector<InstId> &instructions) {
  pushLoop(label);
  checkBlockInto(block, instructions);
  active_loops_.pop_back();
}

void SemanticContext::checkStatementImpl(NodeId node,
                                         std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  switch (tree_.kind(node)) {
  case NodeKind::BlockStmt: {
    if (children.size() != 1 ||
        tree_.kind(children.front()) != NodeKind::Block) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    const auto body = checkBlock(children.front());
    (void)appendInst<SemScopedBlock>(block, node, sem_ir_.voidType(), body);
    return;
  }
  case NodeKind::SwitchStmt:
    (void)checkSwitchExpression(node, block, sem_ir_.voidType());
    return;
  case NodeKind::UnsafeStmt:
    if (children.size() != 1 ||
        tree_.kind(children.front()) != NodeKind::Block) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    ++unsafe_depth_;
    checkBlockInto(children.front(), block);
    --unsafe_depth_;
    return;
  case NodeKind::CheckCancelStmt:
    if (!children.empty()) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
    } else if (!isCurrentAsyncBody() || defer_depth_ != 0) {
      emit(DiagnosticKind::InvalidAsyncOperation, node);
    } else {
      (void)appendInst<SemCoroutineCancellationCheck>(block, node,
                                                      sem_ir_.voidType());
    }
    return;
  case NodeKind::TaskScopeStmt:
    if (children.size() != 1 ||
        tree_.kind(children.front()) != NodeKind::Block) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
    } else if (!isCurrentAsyncBody() || defer_depth_ != 0) {
      emit(DiagnosticKind::InvalidAsyncOperation, node);
    } else {
      ++task_scope_depth_;
      const auto body = checkBlock(children.front());
      --task_scope_depth_;
      (void)appendInst<SemCoroutineTaskScope>(block, node, sem_ir_.voidType(),
                                              body);
    }
    return;
  case NodeKind::ConstDecl: {
    if (children.size() != 2 && children.size() != 3) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    const auto name = nameFor(children[0]);
    const auto initializer_node = children.back();
    const auto annotated_type =
        children.size() == 3 ? checkType(children[1]) : TypeId::invalid();
    std::vector<InstId> initializer_block;
    auto initializer =
        checkExpression(initializer_node, initializer_block, annotated_type);
    const auto declared_type =
        annotated_type.hasValue() ? annotated_type : instType(initializer);
    if (annotated_type.hasValue() &&
        !adjustExpression(initializer, declared_type, initializer_node,
                          initializer_block))
      emit(DiagnosticKind::TypeMismatch, initializer_node);
    const auto initializer_id = sem_ir_.addInstBlock(initializer_block);
    const auto constant = sem_ir_.addConstantEntity({name,
                                                     declared_type,
                                                     initializer_id,
                                                     initializer,
                                                     children[0],
                                                     0U,
                                                     {}});
    sem_ir_.addConstantOccurrence(
        children[0], SemSymbolOccurrenceKind::Declaration, constant);
    bindConstant(name, constant, children[0]);
    (void)appendInst<SemConstantDecl>(block, node, sem_ir_.voidType(), constant,
                                      initializer_id);
    return;
  }
  case NodeKind::LetStmt: {
    if (children.size() != 2 && children.size() != 3) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    if (tree_.kind(children.front()) == NodeKind::StructuredPattern) {
      const auto pattern = children.front();
      const auto initializer_node = children.back();
      std::vector<InstId> initializer_block;
      auto source = checkExpression(initializer_node, initializer_block);
      if (expressionCategory(sem_ir_, source) == SemExprCategory::Diverging)
        return;
      (void)bindStructuredPattern(pattern, source, initializer_block, node,
                                  tree_.tokens().get(tree_.token(node)).kind ==
                                      TokenKind::KwVar);
      block.insert(block.end(), initializer_block.begin(),
                   initializer_block.end());
      return;
    }
    const auto name = nameFor(children[0]);
    const auto initializer_node = children.back();
    const auto annotated_type =
        children.size() == 3 ? checkType(children[1]) : TypeId::invalid();
    auto initializer = checkExpression(initializer_node, block, annotated_type);
    const auto declared_type =
        annotated_type.hasValue() ? annotated_type : instType(initializer);
    if (children.size() == 3 &&
        !adjustExpression(initializer, declared_type, initializer_node, block))
      emit(DiagnosticKind::TypeMismatch, initializer_node);
    const auto local = sem_ir_.addLocal(
        {name, declared_type, children[0],
         tree_.tokens().get(tree_.token(node)).kind == TokenKind::KwVar
             ? static_cast<std::uint32_t>(SemLocalMutable)
             : 0U});
    sem_ir_.addLocalOccurrence(children[0],
                               SemSymbolOccurrenceKind::Declaration, local);
    bind(name, local, children[0]);
    if (expressionCategory(sem_ir_, initializer) == SemExprCategory::Diverging)
      return;
    if (const auto temporary = temporaryBorrowSource(initializer);
        temporary.hasValue()) {
      if (sem_ir_.type(declared_type).kind != SemTypeKind::Reference ||
          sem_ir_.referenceMutability(declared_type) !=
              SemReferenceMutability::ReadOnly) {
        emit(DiagnosticKind::TypeMismatch, initializer_node);
      } else {
        (void)appendInst<SemExtendTemporary>(block, node, sem_ir_.voidType(),
                                             local, temporary);
      }
    } else if (temporaryBorrowEscapeSource(initializer).hasValue()) {
      emit(DiagnosticKind::TemporaryReferenceEscape, initializer_node);
    }
    (void)appendInst<SemBindName>(block, node, sem_ir_.voidType(), local,
                                  initializer);
    return;
  }
  case NodeKind::UninitializedVarDecl:
  case NodeKind::UninitializedLetDecl: {
    if (children.size() != 2) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    const auto name = nameFor(children[0]);
    const auto type = checkType(children[1]);
    if (!name.hasValue() || !type.hasValue())
      return;
    const auto mutable_storage =
        tree_.kind(node) == NodeKind::UninitializedVarDecl;
    const auto local = sem_ir_.addLocal(
        {name, type, children[0],
         static_cast<std::uint32_t>((mutable_storage ? SemLocalMutable : 0U) |
                                    SemLocalUninitialized)});
    sem_ir_.addLocalOccurrence(children[0],
                               SemSymbolOccurrenceKind::Declaration, local);
    bind(name, local, children[0]);
    (void)appendInst<SemDeclareUninitialized>(block, node, sem_ir_.voidType(),
                                              local);
    return;
  }
  case NodeKind::ReturnStmt: {
    if (children.size() > 1) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    if (defer_depth_ != 0)
      emit(DiagnosticKind::DeferControlFlowEscape, node);
    auto value = children.empty()
                     ? appendInst<SemVoidValue>(block, node, sem_ir_.voidType())
                     : checkExpression(children[0], block, return_type_);
    if (expressionCategory(sem_ir_, value) == SemExprCategory::Diverging)
      return;
    if (!adjustExpression(value, return_type_,
                          children.empty() ? node : children[0], block))
      emit(DiagnosticKind::TypeMismatch, node);
    if (temporaryBorrowEscapeSource(value).hasValue())
      emit(DiagnosticKind::TemporaryReferenceEscape, node);
    if (defer_depth_ != 0)
      return;
    appendReturnTerminal(value, node, block);
    return;
  }
  case NodeKind::DeferStmt: {
    if (children.size() != 1 || tree_.kind(children[0]) != NodeKind::Block) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    const auto nested = defer_depth_ != 0;
    if (nested)
      emit(DiagnosticKind::NestedDefer, node);
    const auto saved_loop_base = defer_loop_base_;
    defer_loop_base_ = static_cast<std::uint32_t>(active_loops_.size());
    ++defer_depth_;
    const auto body = checkBlock(children[0]);
    --defer_depth_;
    defer_loop_base_ = saved_loop_base;
    if (!blockFallsThrough(sem_ir_, sem_ir_.instBlock(body)))
      emit(DiagnosticKind::DeferMustFallThrough, node);
    if (!nested)
      (void)appendInst<SemDefer>(block, node, sem_ir_.voidType(), body);
    return;
  }
  case NodeKind::AssertStmt: {
    if (children.size() != 1) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    auto condition = checkExpression(children.front(), block);
    if (expressionCategory(sem_ir_, condition) == SemExprCategory::Diverging)
      return;
    if (!adjustExpression(condition, sem_ir_.boolType(), children.front(),
                          block))
      emit(DiagnosticKind::TypeMismatch, children.front());
    (void)appendInst<SemAssert>(block, node, sem_ir_.voidType(), condition,
                                sem_ir_.addInteger(static_cast<std::uint32_t>(
                                    UnrecoverableFailureReason::Assertion)));
    return;
  }
  case NodeKind::UnreachableStmt:
    if (!children.empty()) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    (void)appendInst<SemUnrecoverableFailure>(
        block, node, sem_ir_.neverType(),
        sem_ir_.addInteger(static_cast<std::uint32_t>(
            UnrecoverableFailureReason::ReachedUnreachable)));
    return;
  case NodeKind::IfStmt: {
    if (children.size() != 2 && children.size() != 3) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    full_expression_temporaries_.emplace_back();
    auto condition = checkExpression(children[0], block);
    if (expressionCategory(sem_ir_, condition) == SemExprCategory::Diverging) {
      endFullExpression(children[0], block);
      (void)checkBlock(children[1]);
      if (children.size() == 3 && tree_.kind(children[2]) == NodeKind::Block)
        (void)checkBlock(children[2]);
      return;
    }
    if (!adjustExpression(condition, sem_ir_.boolType(), children[0], block))
      emit(DiagnosticKind::TypeMismatch, children[0]);
    endFullExpression(children[0], block);
    std::vector<InstId> arms;
    const auto then_body = checkBlock(children[1]);
    arms.push_back(
        sem_ir_.addInst(SemIfArm{sem_ir_.voidType(), then_body}, children[1]));
    if (children.size() == 3) {
      InstBlockId else_body;
      if (tree_.kind(children[2]) == NodeKind::Block) {
        else_body = checkBlock(children[2]);
      } else {
        std::vector<InstId> nested;
        lexical_scopes_.push();
        checkStatement(children[2], nested);
        lexical_scopes_.pop();
        else_body = sem_ir_.addInstBlock(nested);
      }
      arms.push_back(sem_ir_.addInst(SemIfArm{sem_ir_.voidType(), else_body},
                                     children[2]));
    }
    (void)appendInst<SemIf>(block, node, sem_ir_.voidType(), condition,
                            sem_ir_.addInstBlock(arms, true));
    return;
  }
  case NodeKind::WhileStmt: {
    const auto labeled = !children.empty() &&
                         tree_.kind(children.front()) == NodeKind::LoopLabel;
    const auto offset = labeled ? 1U : 0U;
    if (children.size() != offset + 2) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    std::vector<InstId> condition_insts;
    full_expression_temporaries_.emplace_back();
    auto condition = checkExpression(children[offset], condition_insts);
    if (!adjustExpression(condition, sem_ir_.boolType(), children[offset],
                          condition_insts))
      emit(DiagnosticKind::TypeMismatch, children[offset]);
    endFullExpression(children[offset], condition_insts);
    const auto condition_block = sem_ir_.addInstBlock(condition_insts);
    const auto body = checkLoopBlock(
        labeled ? children.front() : NodeId::invalid(), children[offset + 1]);
    (void)appendInst<SemWhile>(block, node, sem_ir_.voidType(), condition_block,
                               body);
    return;
  }
  case NodeKind::ForStmt: {
    const auto labeled = !children.empty() &&
                         tree_.kind(children.front()) == NodeKind::LoopLabel;
    const auto offset = labeled ? 1U : 0U;
    if (children.size() != offset + 4 ||
        tree_.kind(children[offset]) != NodeKind::ForInit ||
        tree_.kind(children[offset + 1]) != NodeKind::ForCondition ||
        tree_.kind(children[offset + 2]) != NodeKind::ForStep ||
        tree_.kind(children[offset + 3]) != NodeKind::Block) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    lexical_scopes_.push();
    std::vector<InstId> init;
    const auto init_children = tree_.children(children[offset]);
    if (!init_children.empty())
      checkStatement(init_children.front(), init);

    std::vector<InstId> condition_insts;
    full_expression_temporaries_.emplace_back();
    const auto condition_children = tree_.children(children[offset + 1]);
    auto condition =
        condition_children.empty()
            ? appendInst<SemBoolLiteral>(condition_insts, children[offset + 1],
                                         sem_ir_.boolType(),
                                         sem_ir_.addInteger(1))
            : checkExpression(condition_children.front(), condition_insts);
    if (!adjustExpression(condition, sem_ir_.boolType(), children[offset + 1],
                          condition_insts))
      emit(DiagnosticKind::TypeMismatch, children[offset + 1]);
    endFullExpression(children[offset + 1], condition_insts);

    std::vector<InstId> step;
    const auto step_children = tree_.children(children[offset + 2]);
    if (!step_children.empty())
      checkStatement(step_children.front(), step);

    const auto body = checkLoopBlock(
        labeled ? children.front() : NodeId::invalid(), children[offset + 3]);
    lexical_scopes_.pop();

    const std::array clauses{
        sem_ir_.addInst(SemForClause{sem_ir_.voidType(), sem_ir_.addInteger(0),
                                     sem_ir_.addInstBlock(init)},
                        children[offset]),
        sem_ir_.addInst(SemForClause{sem_ir_.voidType(), sem_ir_.addInteger(1),
                                     sem_ir_.addInstBlock(condition_insts)},
                        children[offset + 1]),
        sem_ir_.addInst(SemForClause{sem_ir_.voidType(), sem_ir_.addInteger(2),
                                     sem_ir_.addInstBlock(step)},
                        children[offset + 2])};
    (void)appendInst<SemFor>(block, node, sem_ir_.voidType(),
                             sem_ir_.addInstBlock(clauses, true), body);
    return;
  }
  case NodeKind::ForeachStmt:
    checkForeachStatement(node, block);
    return;
  case NodeKind::DoWhileStmt: {
    const auto labeled = !children.empty() &&
                         tree_.kind(children.front()) == NodeKind::LoopLabel;
    const auto offset = labeled ? 1U : 0U;
    if (children.size() != offset + 2 ||
        tree_.kind(children[offset]) != NodeKind::Block) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    const auto body = checkLoopBlock(
        labeled ? children.front() : NodeId::invalid(), children[offset]);
    std::vector<InstId> condition_insts;
    full_expression_temporaries_.emplace_back();
    auto condition = checkExpression(children[offset + 1], condition_insts);
    if (!adjustExpression(condition, sem_ir_.boolType(), children[offset + 1],
                          condition_insts))
      emit(DiagnosticKind::TypeMismatch, children[offset + 1]);
    endFullExpression(children[offset + 1], condition_insts);
    (void)appendInst<SemDoWhile>(block, node, sem_ir_.voidType(),
                                 sem_ir_.addInstBlock(condition_insts), body);
    return;
  }
  case NodeKind::BreakStmt: {
    const auto target = resolveLoopTarget(children, node, true);
    if (!target)
      return;
    if (defer_depth_ != 0 && *target < defer_loop_base_)
      emit(DiagnosticKind::DeferControlFlowEscape, node);
    else
      (void)appendInst<SemBreak>(
          block, node, sem_ir_.voidType(),
          sem_ir_.addInteger(active_loops_.size() - 1 - *target));
    return;
  }
  case NodeKind::ContinueStmt: {
    const auto target = resolveLoopTarget(children, node, false);
    if (!target)
      return;
    if (defer_depth_ != 0 && *target < defer_loop_base_)
      emit(DiagnosticKind::DeferControlFlowEscape, node);
    else
      (void)appendInst<SemContinue>(
          block, node, sem_ir_.voidType(),
          sem_ir_.addInteger(active_loops_.size() - 1 - *target));
    return;
  }
  case NodeKind::AssignmentStmt: {
    if (children.size() != 2) {
      emit(DiagnosticKind::InvalidSemanticShape, node);
      return;
    }
    if (tree_.kind(children.front()) == NodeKind::StructuredPattern) {
      auto source = checkExpression(children.back(), block);
      if (expressionCategory(sem_ir_, source) == SemExprCategory::Diverging)
        return;
      const auto source_type = instType(source);
      struct PendingAssignment {
        InstId target;
        const CheckedPatternBinding *binding;
      };
      std::vector<PendingAssignment> pending;
      std::unordered_set<std::uint32_t> assigned_targets;
      const auto checked =
          checkPatternBindings(children.front(), 0, source_type);
      bool invalid_assignment = !checked.valid;
      for (const auto &binding : checked.bindings) {
        const auto target_name = nameFor(binding.name);
        const auto target_local =
            target_name.hasValue() ? lookup(target_name) : LocalId::invalid();
        if (!target_local.hasValue()) {
          emit(DiagnosticKind::UnknownName, binding.name);
          invalid_assignment = true;
          continue;
        }
        if (!assigned_targets.insert(target_local.index).second) {
          emit(DiagnosticKind::DuplicateName, binding.name);
          invalid_assignment = true;
          continue;
        }
        if ((sem_ir_.local(target_local).flags & SemLocalMutable) == 0) {
          emit(DiagnosticKind::AssignmentToImmutablePlace, binding.name);
          invalid_assignment = true;
          continue;
        }
        auto target = appendInst<SemNameRef>(block, binding.name,
                                             sem_ir_.local(target_local).type,
                                             target_local);
        target = acquireCheckedReference(target, binding.name, block);
        pending.push_back({target, &binding});
      }
      struct CommittedAssignment {
        InstId target;
        InstId value;
        NodeId location;
      };
      std::vector<CommittedAssignment> committed;
      if (!invalid_assignment) {
        for (const auto &assignment : pending) {
          auto value =
              emitPatternProjection(source, 0, *assignment.binding, block);
          value = applyPatternTransfer(value, *assignment.binding, block);
          if (!value.hasValue() ||
              !adjustExpression(value, instType(assignment.target),
                                assignment.binding->node, block)) {
            emit(DiagnosticKind::TypeMismatch, assignment.binding->node);
            invalid_assignment = true;
            break;
          }
          committed.push_back(
              {assignment.target, value, assignment.binding->node});
        }
      }
      if (!invalid_assignment)
        for (const auto &assignment : committed)
          (void)appendInst<SemAssign>(block, assignment.location,
                                      sem_ir_.voidType(), assignment.target,
                                      assignment.value);
      return;
    }
    auto target = checkExpression(children[0], block);
    const auto initializes_reference = isReferenceFieldInitialization(target);
    if (!initializes_reference)
      target = acquireCheckedReference(target, children[0], block);
    if (containsRawPointerDereference(target) && !isMutablePlace(target))
      emit(DiagnosticKind::AssignmentToImmutablePlace, children[0]);
    const auto assignment_kind = tree_.tokens().get(tree_.token(node)).kind;
    const auto operation = compoundOperator(assignment_kind);
    const auto protocol = compoundAssignmentOperatorProtocol(assignment_kind);
    const auto target_kind = sem_ir_.type(instType(target)).kind;
    const auto protocol_dispatch =
        protocol && (target_kind == SemTypeKind::Nominal ||
                     target_kind == SemTypeKind::TypeParameter);
    const auto old_value =
        operation && !protocol_dispatch
            ? appendInst<SemCopy>(block, children[0], instType(target), target)
            : InstId::invalid();
    auto value = checkExpression(children[1], block);
    if (expressionCategory(sem_ir_, value) == SemExprCategory::Diverging)
      return;
    if (protocol_dispatch) {
      (void)checkOperatorProtocol(node, *protocol, target, value, children[0],
                                  children[1], block);
      return;
    }
    if (operation) {
      if (sem_ir_.type(instType(value)).kind == SemTypeKind::Reference)
        value = acquireCheckedReference(value, children[1], block);
      value =
          checkBuiltinBinary(node, *operation, old_value, value, block, true);
    } else if (!adjustExpression(value, instType(target), children[1], block)) {
      emit(DiagnosticKind::TypeMismatch, children[1]);
    }
    if (temporaryBorrowEscapeSource(value).hasValue())
      emit(DiagnosticKind::TemporaryReferenceEscape, children[1]);
    (void)appendInst<SemAssign>(block, node, sem_ir_.voidType(), target, value);
    return;
  }
  case NodeKind::ExprStmt:
    if (children.size() == 1) {
      auto value = checkExpression(children[0], block);
      if (expressionCategory(sem_ir_, value) != SemExprCategory::Diverging &&
          instType(value) != sem_ir_.voidType()) {
        if (sem_ir_.type(instType(value)).kind == SemTypeKind::CoroutineTask) {
          if (task_scope_depth_ == 0 ||
              sem_ir_.coroutineTaskErrorType(instType(value))) {
            emit(DiagnosticKind::TaskDiscard, children[0]);
            return;
          }
          (void)appendInst<SemDiscardValue>(block, node, sem_ir_.voidType(),
                                            value);
          return;
        }
        const auto representation = sem_ir_.typeRepresentation(instType(value));
        if (expressionCategory(sem_ir_, value) == SemExprCategory::Temporary &&
            representation.ownership == OwnershipReprKind::Owned &&
            representation.destroy != DestroyReprKind::None)
          value = materializeTemporary(value, children[0], block);
        else
          (void)appendInst<SemDiscardValue>(block, node, sem_ir_.voidType(),
                                            value);
      }
    } else {
      emit(DiagnosticKind::InvalidSemanticShape, node);
    }
    return;
  default:
    emit(DiagnosticKind::InvalidSemanticShape, node);
    return;
  }
}

void SemanticContext::endFullExpression(NodeId node,
                                        std::vector<InstId> &block) {
  assert(!full_expression_temporaries_.empty());
  auto temporaries = std::move(full_expression_temporaries_.back());
  full_expression_temporaries_.pop_back();
  if (temporaries.empty())
    return;
  InstId terminator;
  if (!block.empty()) {
    const auto kind = sem_ir_.inst(block.back()).kind;
    if (kind == SemInstKind::Return ||
        kind == SemInstKind::UnrecoverableFailure ||
        kind == SemInstKind::Break || kind == SemInstKind::Continue) {
      terminator = block.back();
      block.pop_back();
    }
  }
  (void)appendInst<SemEndFullExpression>(
      block, node, sem_ir_.voidType(), sem_ir_.addInstBlock(temporaries, true));
  if (terminator.hasValue())
    block.push_back(terminator);
}

void SemanticContext::checkStatement(NodeId node, std::vector<InstId> &block) {
  full_expression_temporaries_.emplace_back();
  checkStatementImpl(node, block);
  endFullExpression(node, block);
}

void SemanticContext::checkBlockInto(NodeId node, std::vector<InstId> &block) {
  lexical_scopes_.push();
  auto reachable = blockFallsThrough(sem_ir_, block);
  for (const auto statement : tree_.children(node)) {
    if (reachable) {
      checkStatement(statement, block);
      reachable = blockFallsThrough(sem_ir_, block);
    } else {
      emit(DiagnosticKind::UnreachableCode, statement);
      std::vector<InstId> detached;
      checkStatement(statement, detached);
    }
  }
  lexical_scopes_.pop();
}

void SemanticContext::checkCallableBodyInto(NodeId node,
                                            std::vector<InstId> &block) {
  if (tree_.kind(node) == NodeKind::Block) {
    checkBlockInto(node, block);
    return;
  }
  if (tree_.kind(node) != NodeKind::ValueBlock) {
    emit(DiagnosticKind::InvalidSemanticShape, node);
    return;
  }
  auto checked = checkValueBlock(node, return_type_);
  block.insert(block.end(), checked.instructions.begin(),
               checked.instructions.end());
  if (!checked.falls_through)
    return;
  auto value = checked.value;
  if (!value.hasValue()) {
    if (return_type_ != sem_ir_.voidType()) {
      emit(DiagnosticKind::MissingReturnValue, node);
      return;
    }
    value = appendInst<SemVoidValue>(block, node, sem_ir_.voidType());
  } else if (!adjustExpression(value, return_type_, checked.value_node,
                               block)) {
    emit(DiagnosticKind::TypeMismatch, checked.value_node);
    return;
  }
  if (temporaryBorrowEscapeSource(value).hasValue()) {
    emit(DiagnosticKind::TemporaryReferenceEscape,
         checked.value_node.hasValue() ? checked.value_node : node);
    return;
  }
  appendReturnTerminal(
      value, checked.value_node.hasValue() ? checked.value_node : node, block);
}

[[nodiscard]] InstId
SemanticContext::checkScopedBlockExpression(NodeId node, NodeId value_block,
                                            std::vector<InstId> &block,
                                            TypeId expected_type) {
  auto checked = checkValueBlock(value_block, expected_type);
  if (!checked.falls_through)
    return appendInst<SemScopedBlock>(
        block, node, sem_ir_.neverType(),
        sem_ir_.addInstBlock(checked.instructions));
  auto value = checked.value;
  if (!value.hasValue())
    value = appendInst<SemVoidValue>(checked.instructions, value_block,
                                     sem_ir_.voidType());
  if (expected_type.hasValue() &&
      !adjustExpression(value, expected_type,
                        checked.value_node.hasValue() ? checked.value_node
                                                      : value_block,
                        checked.instructions))
    emit(DiagnosticKind::TypeMismatch,
         checked.value_node.hasValue() ? checked.value_node : value_block);
  (void)appendInst<SemYield>(checked.instructions, value_block,
                             sem_ir_.voidType(), value);
  return appendInst<SemScopedBlock>(block, node, instType(value),
                                    sem_ir_.addInstBlock(checked.instructions));
}

[[nodiscard]] InstBlockId SemanticContext::checkBlock(NodeId node) {
  std::vector<InstId> block;
  checkBlockInto(node, block);
  return sem_ir_.addInstBlock(block);
}

[[nodiscard]] NodeId SemanticContext::functionChild(NodeId node,
                                                    NodeKind kind) const {
  for (const auto child : tree_.children(node))
    if (tree_.kind(child) == kind)
      return child;
  return NodeId::invalid();
}

[[nodiscard]] std::vector<SemanticContext::FunctionParameterSyntax>
SemanticContext::functionParameters(NodeId parameter_list) const {
  std::vector<FunctionParameterSyntax> result;
  const auto children = tree_.children(parameter_list);
  for (std::size_t index = 0; index < children.size();) {
    if (tree_.kind(children[index]) == NodeKind::VariadicParameter) {
      ++index;
      continue;
    }
    if (tree_.kind(children[index]) == NodeKind::PatternParameter) {
      const auto parameter = tree_.children(children[index++]);
      if (parameter.size() == 3 &&
          tree_.kind(parameter[0]) == NodeKind::StructuredPattern)
        result.push_back(
            {parameter[1], parameter[2], NodeId::invalid(), parameter[0]});
      continue;
    }
    if (tree_.kind(children[index]) == NodeKind::DefaultParameter) {
      const auto parameter = tree_.children(children[index++]);
      if (parameter.size() == 3)
        result.push_back({parameter[0], parameter[1], parameter[2]});
      continue;
    }
    if (index + 1 >= children.size())
      break;
    result.push_back({children[index], children[index + 1], NodeId::invalid()});
    index += 2;
  }
  return result;
}

[[nodiscard]] std::vector<NodeId> SemanticContext::flattenedFunctionParameters(
    std::span<const SemanticContext::FunctionParameterSyntax> parameters)
    const {
  std::vector<NodeId> result;
  result.reserve(parameters.size() * 2);
  for (const auto &parameter : parameters) {
    result.push_back(parameter.name);
    result.push_back(parameter.type);
  }
  return result;
}

[[nodiscard]] std::optional<OwnershipRegion>
SemanticContext::contractRegion(NodeId node,
                                std::span<const NodeId> parameter_nodes,
                                std::span<const TypeId> parameter_types) {
  const auto resolve = [&](auto &&self, NodeId current, TypeId &current_type,
                           OwnershipRegion &region) -> bool {
    const auto children = tree_.children(current);
    if (tree_.kind(current) == NodeKind::Name) {
      const auto name = tokenString(current);
      for (std::size_t index = 0; index + 1 < parameter_nodes.size();
           index += 2) {
        if (tokenString(parameter_nodes[index]) == name) {
          region.parameter_index = static_cast<std::uint32_t>(index / 2);
          current_type = parameter_types[index / 2];
          if (sem_ir_.type(current_type).kind == SemTypeKind::Reference)
            current_type = sem_ir_.referencePointee(current_type);
          return true;
        }
      }
      return false;
    }
    if (tree_.kind(current) == NodeKind::UnaryExpr && children.size() == 1 &&
        tree_.tokens().get(tree_.token(current)).kind == TokenKind::Star) {
      if (!self(self, children.front(), current_type, region) ||
          sem_ir_.type(current_type).kind != SemTypeKind::Reference)
        return false;
      region.path.push_back({OwnershipRegionStepKind::Dereference, 0});
      current_type = sem_ir_.referencePointee(current_type);
      return true;
    }
    if (tree_.kind(current) == NodeKind::MemberExpr && children.size() == 2) {
      if (!self(self, children.front(), current_type, region))
        return false;
      while (sem_ir_.type(current_type).kind == SemTypeKind::Reference) {
        region.path.push_back({OwnershipRegionStepKind::Dereference, 0});
        current_type = sem_ir_.referencePointee(current_type);
      }
      if (sem_ir_.type(current_type).kind != SemTypeKind::Nominal)
        return false;
      const auto &nominal =
          sem_ir_.nominalType(NominalTypeId(sem_ir_.type(current_type).arg0));
      const auto member = tokenString(children.back());
      for (std::uint32_t index = 0; index < nominal.fields.size(); ++index) {
        if (sem_ir_.name(nominal.fields[index].name).text != member)
          continue;
        region.path.push_back({OwnershipRegionStepKind::Field, index});
        current_type = nominalFieldType(current_type, nominal.fields[index]);
        if (nominal.fields[index].projection_kind ==
            PublicObjectProjectionKind::BitPacked) {
          region.has_bit_range = true;
          region.bit_begin = nominal.fields[index].bit_begin;
          region.bit_end = nominal.fields[index].bit_end;
        }
        return true;
      }
      return false;
    }
    if (tree_.kind(current) == NodeKind::IndexExpr && children.size() == 2) {
      if (!self(self, children.front(), current_type, region))
        return false;
      while (sem_ir_.type(current_type).kind == SemTypeKind::Reference) {
        region.path.push_back({OwnershipRegionStepKind::Dereference, 0});
        current_type = sem_ir_.referencePointee(current_type);
      }
      if (sem_ir_.type(current_type).kind != SemTypeKind::Array ||
          tree_.kind(children.back()) != NodeKind::IntegerLiteral)
        return false;
      std::uint32_t index = 0;
      const auto spelling = tree_.tokens().text(tree_.token(children.back()));
      const auto [end, error] = std::from_chars(
          spelling.data(), spelling.data() + spelling.size(), index);
      if (error != std::errc{} || end != spelling.data() + spelling.size() ||
          index >= sem_ir_.type(current_type).arg1)
        return false;
      region.path.push_back({OwnershipRegionStepKind::StaticElement, index});
      current_type = TypeId(sem_ir_.type(current_type).arg0);
      return true;
    }
    return false;
  };
  OwnershipRegion region;
  TypeId type;
  if (!resolve(resolve, node, type, region)) {
    emit(DiagnosticKind::InvalidCallableContract, node);
    return std::nullopt;
  }
  return region;
}

[[nodiscard]] std::optional<CallableOwnershipSummary>
SemanticContext::declaredContract(NodeId node,
                                  std::span<const NodeId> parameter_nodes,
                                  std::span<const TypeId> parameter_types,
                                  TypeId return_type) {
  const auto contract = functionChild(node, NodeKind::CallableContract);
  if (!contract.hasValue())
    return std::nullopt;
  CallableOwnershipSummary summary;
  summary.returns_owned = sem_ir_.typeRepresentation(return_type).ownership !=
                          OwnershipReprKind::Borrowed;
  const auto parse_condition =
      [&](auto &&self, NodeId guard,
          bool negated) -> std::optional<CallableConditionDescriptor> {
    const auto children = tree_.children(guard);
    if (tree_.kind(guard) == NodeKind::BoolLiteral) {
      const auto value =
          tree_.tokens().get(tree_.token(guard)).kind == TokenKind::KwTrue;
      auto result = value ? CallableConditionDescriptor::always()
                          : CallableConditionDescriptor::never();
      return negated ? conditionNot(result) : result;
    }
    if (tree_.kind(guard) == NodeKind::Name) {
      const auto name = tokenString(guard);
      for (std::size_t index = 0; index + 1 < parameter_nodes.size();
           index += 2) {
        if (tokenString(parameter_nodes[index]) == name &&
            parameter_types[index / 2] == sem_ir_.boolType()) {
          return CallableConditionDescriptor::atom(
              static_cast<std::uint32_t>(index / 2), !negated);
        }
      }
      return std::nullopt;
    }
    if (tree_.kind(guard) == NodeKind::UnaryExpr && children.size() == 1 &&
        tree_.tokens().get(tree_.token(guard)).kind == TokenKind::Bang)
      return self(self, children.front(), !negated);
    if (tree_.kind(guard) == NodeKind::BinaryExpr && children.size() == 2) {
      auto lhs = self(self, children.front(), negated);
      auto rhs = self(self, children.back(), negated);
      if (!lhs || !rhs)
        return std::nullopt;
      const auto op = tree_.tokens().get(tree_.token(guard)).kind;
      if ((!negated && op == TokenKind::AmpAmp) ||
          (negated && op == TokenKind::PipePipe))
        return conditionAnd(std::move(*lhs), *rhs);
      if ((!negated && op == TokenKind::PipePipe) ||
          (negated && op == TokenKind::AmpAmp))
        return conditionOr(std::move(*lhs), *rhs);
    }
    return std::nullopt;
  };
  auto covered_returns = CallableConditionDescriptor::never();
  bool saw_otherwise = false;
  const auto entries = tree_.children(contract);
  for (std::size_t entry_index = 0; entry_index < entries.size();
       ++entry_index) {
    const auto entry = entries[entry_index];
    const auto children = tree_.children(entry);
    if (tree_.kind(entry) == NodeKind::ContractEffectEntry) {
      const auto spelling = tree_.tokens().text(tree_.token(entry));
      auto kind = CallableEffectKind::Count;
      std::size_t first_region = 0;
      if (spelling == "reads")
        kind = CallableEffectKind::Read;
      else if (spelling == "writes")
        kind = CallableEffectKind::Write;
      else if (spelling == "takes")
        kind = CallableEffectKind::Take;
      else if (spelling == "initializes")
        kind = CallableEffectKind::Initialize;
      else if (spelling == "borrows" && !children.empty() &&
               tree_.kind(children.front()) == NodeKind::Name) {
        const auto mode = values_.identifier(tokenString(children.front()));
        kind = mode == "shared"    ? CallableEffectKind::BorrowShared
               : mode == "mutable" ? CallableEffectKind::BorrowMutable
                                   : CallableEffectKind::Count;
        first_region = 1;
      }
      if (kind == CallableEffectKind::Count ||
          first_region == children.size()) {
        emit(DiagnosticKind::InvalidCallableContract, entry);
        continue;
      }
      for (std::size_t index = first_region; index < children.size(); ++index)
        if (auto region = contractRegion(children[index], parameter_nodes,
                                         parameter_types))
          summary.effects.push_back({kind, std::move(*region)});
      continue;
    }
    if (tree_.kind(entry) == NodeKind::ContractPostconditionEntry) {
      if (children.size() != 2 ||
          tree_.kind(children.front()) != NodeKind::Name) {
        emit(DiagnosticKind::InvalidCallableContract, entry);
        continue;
      }
      const auto spelling = values_.identifier(tokenString(children.front()));
      std::uint8_t outcome = 0;
      if (spelling == "initialized")
        outcome = CallableOutcomeInitialize;
      else if (spelling == "invalidated")
        outcome = CallableOutcomeInvalidate;
      const auto region =
          contractRegion(children.back(), parameter_nodes, parameter_types);
      if (outcome == 0 || !region) {
        emit(DiagnosticKind::InvalidCallableContract, entry);
        continue;
      }
      summary.postconditions.push_back({*region, outcome});
      continue;
    }
    if (tree_.kind(entry) != NodeKind::ContractReturnEntry ||
        children.empty() || children.size() > 2) {
      emit(DiagnosticKind::InvalidCallableContract, entry);
      continue;
    }
    const auto region =
        contractRegion(children.front(), parameter_nodes, parameter_types);
    if (!region)
      continue;
    CallableConditionDescriptor condition =
        CallableConditionDescriptor::always();
    if (children.size() == 2 &&
        tree_.kind(children.back()) == NodeKind::ContractOtherwise) {
      if (saw_otherwise || entry_index + 1 != entries.size()) {
        emit(DiagnosticKind::InvalidCallableContract, entry);
        continue;
      }
      saw_otherwise = true;
      condition = conditionNot(covered_returns);
    } else if (children.size() == 2) {
      auto parsed = parse_condition(parse_condition, children.back(), false);
      if (!parsed) {
        emit(DiagnosticKind::InvalidCallableContract, children.back());
        continue;
      }
      condition = std::move(*parsed);
      covered_returns = conditionOr(std::move(covered_returns), condition);
    } else {
      covered_returns = CallableConditionDescriptor::always();
    }
    if (!condition.isNever())
      summary.return_provenance.push_back(
          {.region = *region, .condition = std::move(condition)});
  }
  for (const auto &effect : summary.effects) {
    if (effect.kind != CallableEffectKind::Write &&
        effect.kind != CallableEffectKind::Take)
      continue;
    const auto partially_covered =
        std::ranges::any_of(summary.postconditions, [&](const auto &entry) {
          return ownershipRegionCovers(effect.region, entry.region) &&
                 !ownershipRegionCovers(entry.region, effect.region);
        });
    if (partially_covered) {
      emit(DiagnosticKind::InvalidCallableContract, node);
      return std::nullopt;
    }
  }
  const auto carrier_paths = sem_ir_.loanCarrierPaths(return_type);
  if (!carrier_paths.empty() &&
      !(carrier_paths.size() == 1 && carrier_paths.front().empty())) {
    const auto sources = std::move(summary.return_provenance);
    summary.return_provenance.clear();
    for (const auto &source : sources)
      for (const auto &path : carrier_paths) {
        auto expanded = source;
        expanded.carrier_path = path;
        summary.return_provenance.push_back(std::move(expanded));
      }
  }
  summary.canonicalize();
  std::string error;
  if (!summary.verify(static_cast<std::uint32_t>(parameter_types.size()),
                      error)) {
    emit(DiagnosticKind::InvalidCallableContract, node);
    return std::nullopt;
  }
  return summary;
}

void SemanticContext::predeclareModuleConstant(NodeId node) {
  if (tree_.kind(node) != NodeKind::ConstDecl &&
      tree_.kind(node) != NodeKind::StaticDecl)
    return;
  const auto children = tree_.children(node);
  if (children.size() != 3 || tree_.kind(children[0]) != NodeKind::Name) {
    emit(DiagnosticKind::InvalidSemanticShape, node);
    return;
  }
  const auto name = nameFor(children[0]);
  const auto type = checkType(children[1]);
  const auto empty = sem_ir_.addInstBlock({});
  std::uint32_t flags = SemConstantModule;
  if (tree_.kind(node) == NodeKind::StaticDecl)
    flags |= SemConstantStatic;
  if (hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic))
    flags |= SemConstantPublic;
  const auto constant = sem_ir_.addConstantEntity(
      {name, type, empty, InstId::invalid(), children[0], flags, {}});
  sem_ir_.addConstantOccurrence(children[0],
                                SemSymbolOccurrenceKind::Declaration, constant);
  const auto [unused, inserted] =
      module_constant_names_.emplace(name.index, constant);
  (void)unused;
  if (!inserted || function_names_.contains(name.index))
    emit(DiagnosticKind::DuplicateName, children[0]);
  constant_nodes_.emplace(node.index, constant);
}

void SemanticContext::checkModuleConstant(NodeId node,
                                          std::vector<InstId> &declarations) {
  if (tree_.kind(node) != NodeKind::ConstDecl &&
      tree_.kind(node) != NodeKind::StaticDecl)
    return;
  const auto found = constant_nodes_.find(node.index);
  if (found == constant_nodes_.end())
    return;
  const auto children = tree_.children(node);
  auto entity = sem_ir_.constantEntity(found->second);
  std::vector<InstId> initializer;
  auto value = checkExpression(children[2], initializer, entity.type);
  if (!adjustExpression(value, entity.type, children[2], initializer))
    emit(DiagnosticKind::TypeMismatch, children[2]);
  entity.initializer = sem_ir_.addInstBlock(initializer);
  entity.value = value;
  sem_ir_.setConstantEntity(found->second, entity);
  declarations.push_back(sem_ir_.addInst(
      SemConstantDecl{sem_ir_.voidType(), found->second, entity.initializer},
      node));
}

void SemanticContext::validateConstants(bool module_constants) {
  ConstantEvaluator evaluator(sem_ir_);
  const auto valid_static_value = [&](auto &&self, ConstantId id) -> bool {
    if (!id.hasValue())
      return false;
    const auto &value = sem_ir_.constantValue(id);
    switch (value.kind) {
    case ConstantValueKind::Integer:
    case ConstantValueKind::Float:
    case ConstantValueKind::Bool:
    case ConstantValueKind::String:
    case ConstantValueKind::Null:
      return true;
    case ConstantValueKind::Array:
      return std::ranges::all_of(
          sem_ir_.constantBlock(value.elements),
          [&](ConstantId element) { return self(self, element); });
    case ConstantValueKind::Aggregate: {
      const auto &type = sem_ir_.type(value.type);
      if (type.kind != SemTypeKind::Nominal ||
          sem_ir_.objectRepresentationType(value.type) != value.type ||
          sem_ir_.nominalType(NominalTypeId(type.arg0)).kind !=
              NominalKind::Struct)
        return false;
      return std::ranges::all_of(
          sem_ir_.constantBlock(value.elements),
          [&](ConstantId field) { return self(self, field); });
    }
    case ConstantValueKind::Union:
    case ConstantValueKind::Enum:
      return false;
    case ConstantValueKind::ForeignEnum:
      return true;
    }
    return false;
  };
  for (std::uint32_t index = 0; index < sem_ir_.constantEntityCount();
       ++index) {
    const auto entity = ConstantEntityId(index);
    const auto &constant = sem_ir_.constantEntity(entity);
    if (!constant.value.hasValue() ||
        (((constant.flags & SemConstantModule) != 0) != module_constants))
      continue;
    const auto outcome = evaluator.evaluateEntity(entity);
    if (outcome.result.isConcrete() &&
        ((constant.flags & SemConstantStatic) == 0 ||
         valid_static_value(valid_static_value, outcome.result.value)))
      continue;
    if (outcome.result.isConcrete()) {
      emit(DiagnosticKind::InvalidStaticInitializer, constant.declaration);
      continue;
    }
    const auto location = outcome.location.hasValue()
                              ? outcome.location
                              : sem_ir_.constantEntity(entity).declaration;
    if (outcome.failure == ConstantEvaluationFailure::StepLimit ||
        outcome.failure == ConstantEvaluationFailure::CallDepthLimit)
      emit(DiagnosticKind::ConstantEvaluationLimit, location);
    else if (outcome.failure == ConstantEvaluationFailure::Cycle)
      emit(DiagnosticKind::ConstantEvaluationCycle, location);
    else if (outcome.failure == ConstantEvaluationFailure::FatalFailure)
      emit(DiagnosticKind::ConstantEvaluationFatalFailure, location);
    else if (outcome.failure == ConstantEvaluationFailure::Overflow)
      emit(DiagnosticKind::IntegerOverflow, location);
    else if (outcome.failure == ConstantEvaluationFailure::DivisionByZero)
      emit(DiagnosticKind::DivisionByZero, location);
    else if (outcome.failure == ConstantEvaluationFailure::RemainderByZero)
      emit(DiagnosticKind::RemainderByZero, location);
    else if (outcome.failure == ConstantEvaluationFailure::ShiftOutOfRange)
      emit(DiagnosticKind::ShiftOutOfRange, location);
    else
      emit(DiagnosticKind::InvalidConstantInitializer, location);
  }
}

[[nodiscard]] bool
SemanticContext::sameSourceTypePattern(CanonicalTypeId lhs,
                                       CanonicalTypeId rhs) const {
  if (lhs == rhs)
    return true;
  const auto &left = values_.generics().type(lhs);
  const auto &right = values_.generics().type(rhs);
  if (left.kind != right.kind || left.arg1 != right.arg1 ||
      left.nominal_key != right.nominal_key ||
      left.callable_contract != right.callable_contract ||
      left.callable_context_parameter != right.callable_context_parameter ||
      left.registration_authority != right.registration_authority ||
      left.registration_entry_parameter != right.registration_entry_parameter ||
      left.registration_userdata_parameter !=
          right.registration_userdata_parameter ||
      left.registration_release_parameter !=
          right.registration_release_parameter ||
      left.registration_bindings != right.registration_bindings ||
      left.registration_arm_parameters != right.registration_arm_parameters ||
      left.registration_detach_parameters !=
          right.registration_detach_parameters ||
      left.elements.size() != right.elements.size() ||
      left.foreign_resource_protocol.hasValue() !=
          right.foreign_resource_protocol.hasValue())
    return false;
  if (left.kind == CanonicalTypeKind::Array) {
    if (!sameSourceTypePattern(CanonicalTypeId(left.arg0),
                               CanonicalTypeId(right.arg0)))
      return false;
  } else if (left.kind != CanonicalTypeKind::TypeParameter &&
             left.arg0 != right.arg0) {
    return false;
  }
  for (std::size_t index = 0; index < left.elements.size(); ++index)
    if (!sameSourceTypePattern(left.elements[index], right.elements[index]))
      return false;
  if (left.foreign_resource_protocol.hasValue()) {
    const auto &left_protocol = values_.generics().foreignResourceProtocol(
        left.foreign_resource_protocol);
    const auto &right_protocol = values_.generics().foreignResourceProtocol(
        right.foreign_resource_protocol);
    if (left_protocol.facts != right_protocol.facts ||
        left_protocol.types.size() != right_protocol.types.size())
      return false;
    for (std::size_t index = 0; index < left_protocol.types.size(); ++index)
      if (!sameSourceTypePattern(left_protocol.types[index],
                                 right_protocol.types[index]))
        return false;
  }
  return true;
}

} // namespace chtholly::compiler::semantics_internal
