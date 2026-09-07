#include "LowerToLowIRInternal.h"

#include <array>
#include <cassert>

namespace chtholly::compiler::internal {

namespace {

template <typename InstT>
LowInstId emit(LoweringCoroutineInstructionState &state, LowBlockId block,
               InstT inst, InstId origin) {
  const auto id = state.session.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.session.low_ir.blockCount()]
      .push_back(id);
  return id;
}

LowInstId valueFor(const LoweringCoroutineInstructionState &state, InstId id) {
  return state.values.at(id.index);
}

LowBlockId newBlock(LoweringCoroutineInstructionState &state) {
  const auto id = LowBlockId(static_cast<std::uint32_t>(
      state.session.low_ir.blockCount() + state.pending_blocks.size()));
  state.pending_blocks.emplace_back();
  return id;
}

void emitPayloadTerminal(LoweringCoroutineInstructionState &state,
                         bool selected_error, LowBlockId block,
                         InstId semantic_id, LowInstId value) {
  const auto void_type = state.session.sem_ir.voidType();
  if (selected_error)
    (void)emit(state, block,
               LowCoroutineReturnError{void_type, value, {}}, semantic_id);
  else
    (void)emit(state, block,
               LowCoroutineReturnSuccess{void_type, value, {}}, semantic_id);
}

} // namespace

void LoweringCoroutineInstructionService::unary(
    InstId semantic_id, TypeId type, LowBlockId current, LowInstId operand,
    LoweringCoroutineInstructionState &state, LowInstKind kind) {
  state.values[semantic_id.index] =
      [&]() -> LowInstId {
    switch (kind) {
    case LowInstKind::CoroutineTaskQuery:
      return emit(state, current,
                  LowCoroutineTaskQuery{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineTaskTakeResult:
      return emit(state, current,
                  LowCoroutineTaskTakeResult{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineTaskTakeError:
      return emit(state, current,
                  LowCoroutineTaskTakeError{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineCheckedStatus:
      return emit(state, current,
                  LowCoroutineCheckedStatus{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineCheckedTake:
      return emit(state, current,
                  LowCoroutineCheckedTake{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineOutcomeCompleted:
      return emit(state, current,
                  LowCoroutineOutcomeCompleted{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineOutcomeFailed:
      return emit(state, current,
                  LowCoroutineOutcomeFailed{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineOutcomeCancelled:
      return emit(state, current,
                  LowCoroutineOutcomeCancelled{type, operand, {}}, semantic_id);
    case LowInstKind::CoroutineTaskSelectionWinner:
      return emit(state, current,
                  LowCoroutineTaskSelectionWinner{type, operand, {}},
                  semantic_id);
    case LowInstKind::CoroutineTaskSelectionTakeRemaining:
      return emit(state, current,
                  LowCoroutineTaskSelectionTakeRemaining{type, operand, {}},
                  semantic_id);
    case LowInstKind::CoroutineTaskJoin:
      return emit(state, current,
                  LowCoroutineTaskJoin{type, operand, {}}, semantic_id);
    default:
      assert(false && "invalid coroutine unary lowering kind");
      return LowInstId::invalid();
    }
  }();
}

void LoweringCoroutineInstructionService::taskCreate(
    InstId semantic_id, FunctionRefId target, InstBlockId operands,
    CoroutineTaskCreateMode mode, TypeId checked_type, LowBlockId current,
    LowInstId active_task_group, LoweringCoroutineInstructionState &state) {
  const auto plan = LoweringCoroutineService::buildTaskCreatePlan(
      state.session, target, mode, checked_type);
  std::vector<LowInstId> lowered;
  for (const auto operand : state.session.sem_ir.instBlock(operands))
    lowered.push_back(valueFor(state, operand));
  auto result = emit(
      state, current,
      LowCoroutineTaskCreate{
          checked_type, plan, state.session.low_ir.addValueBlock(lowered)},
      semantic_id);
  if (mode == CoroutineTaskCreateMode::Child && active_task_group.hasValue()) {
    const std::array attach_operands{active_task_group, result};
    result = emit(state, current,
                  LowCoroutineTaskGroupAttach{
                      checked_type,
                      state.session.low_ir.addValueBlock(attach_operands), {}},
                  semantic_id);
  }
  state.values[semantic_id.index] = result;
}

void LoweringCoroutineInstructionService::completionArm(
    InstId semantic_id, SemCoroutineTaskCompletionArm semantic,
    LowBlockId current, LoweringCoroutineInstructionState &state) {
  const auto task_type =
      TypeId(state.session.sem_ir.inst(semantic.arg0).type);
  const auto plan = state.session.low_ir.addCoroutineTaskCompletionArmPlan(
      {.scaffold = state.function.current_function,
       .task_type = task_type,
       .completion_type =
           state.session.sem_ir.coroutineTaskCompletionType()});
  state.values[semantic_id.index] = emit(
      state, current,
      LowCoroutineTaskCompletionArm{
          semantic.type, plan, valueFor(state, semantic.arg0)},
      semantic_id);
}

void LoweringCoroutineInstructionService::completionReady(
    InstId semantic_id, SemCoroutineTaskCompletionReady semantic,
    LowBlockId current, LoweringCoroutineInstructionState &state) {
  state.values[semantic_id.index] = emit(
      state, current,
      LowCoroutineTaskCompletionReady{semantic.type,
                                      valueFor(state, semantic.arg0), {}},
      semantic_id);
}

void LoweringCoroutineInstructionService::completionDetach(
    InstId semantic_id, SemCoroutineTaskCompletionDetach semantic,
    LowBlockId current, LoweringCoroutineInstructionState &state) {
  state.values[semantic_id.index] = emit(
      state, current,
      LowFinishCoroutineTaskCompletion{semantic.type,
                                       valueFor(state, semantic.arg0), {}},
      semantic_id);
}

void LoweringCoroutineInstructionService::completionSetCreate(
    InstId semantic_id, SemCoroutineTaskCompletionSetCreate semantic,
    LowBlockId current, LoweringCoroutineInstructionState &state) {
  const auto operands = state.session.sem_ir.instBlock(semantic.arg0);
  std::vector<LowInstId> lowered;
  lowered.reserve(operands.size());
  for (const auto operand : operands)
    lowered.push_back(valueFor(state, operand));
  const auto set_type =
      state.session.sem_ir.coroutineCheckedPayloadType(semantic.type);
  const auto plan = LoweringCoroutineService::buildCompletionSetPlan(
      state.session, state.function.current_function, operands, set_type);
  state.values[semantic_id.index] = emit(
      state, current,
      LowCoroutineTaskCompletionSetCreate{
          semantic.type, plan, state.session.low_ir.addValueBlock(lowered)},
      semantic_id);
}

LowBlockId LoweringCoroutineInstructionService::payloadTerminal(
    InstId semantic_id, InstId payload, bool selected_error,
    LowBlockId current, LoweringCoroutineInstructionState &state,
    const CleanupTailFn &cleanup_tail,
    const TaskScopeDrainFn &task_scope_drains) {
  const auto exit_intent =
      selected_error ? CoroutineTaskGroupExitIntent::SelectedError
                     : CoroutineTaskGroupExitIntent::Normal;
  const auto payload_type = TypeId(state.session.sem_ir.inst(payload).type);
  if (payload_type == state.session.sem_ir.voidType()) {
    const auto terminal = newBlock(state);
    const auto stored = emit(
        state, terminal, LowVoidValue{payload_type, {}, {}}, semantic_id);
    emitPayloadTerminal(state, selected_error, terminal, semantic_id, stored);
    const auto tail = cleanup_tail(
        state.session.sem_ir.placeStates().returnCleanup(semantic_id),
        semantic_id, terminal);
    current = task_scope_drains(current, 0, exit_intent);
    (void)emit(state, current,
               LowBranch{state.session.sem_ir.voidType(), tail, {}},
               semantic_id);
    return terminal;
  }

  const auto slot = state.session.low_ir.addSlot(
      {payload_type, LocalId::invalid(), LowSlotSynthetic, 0});
  state.function_slots.push_back(slot);
  const auto representation =
      state.session.sem_ir.typeRepresentation(payload_type);
  const auto value = valueFor(state, payload);
  if (representation.init_repr == InitReprKind::ByConversion)
    (void)emit(state, current,
               LowInitializeFromValue{state.session.sem_ir.voidType(), slot,
                                      value},
               semantic_id);
  else if (representation.ownership == OwnershipReprKind::Owned)
    (void)emit(state, current,
               LowTransfer{state.session.sem_ir.voidType(), slot, value},
               semantic_id);
  else
    (void)emit(state, current,
               LowInitialize{state.session.sem_ir.voidType(), slot, value},
               semantic_id);

  const auto terminal = newBlock(state);
  const auto stored = emit(
      state, terminal, LowLoad{payload_type, slot, {}}, semantic_id);
  emitPayloadTerminal(state, selected_error, terminal, semantic_id, stored);
  const auto tail = cleanup_tail(
      state.session.sem_ir.placeStates().returnCleanup(semantic_id), semantic_id,
      terminal);
  current = task_scope_drains(current, 0, exit_intent);
  (void)emit(state, current,
             LowBranch{state.session.sem_ir.voidType(), tail, {}}, semantic_id);
  return terminal;
}

LowBlockId LoweringCoroutineInstructionService::cancelledTerminal(
    InstId semantic_id, LowBlockId current,
    LoweringCoroutineInstructionState &state,
    const CleanupTailFn &cleanup_tail,
    const TaskScopeDrainFn &task_scope_drains) {
  const auto terminal = newBlock(state);
  (void)emit(state, terminal,
             LowCoroutineReturnCancelled{state.session.sem_ir.voidType(), {},
                                         {}},
             semantic_id);
  const auto tail = cleanup_tail(
      state.session.sem_ir.placeStates().returnCleanup(semantic_id), semantic_id,
      terminal);
  current = task_scope_drains(
      current, 0, CoroutineTaskGroupExitIntent::SelectedCancellation);
  (void)emit(state, current,
             LowBranch{state.session.sem_ir.voidType(), tail, {}}, semantic_id);
  return terminal;
}

LowBlockId LoweringCoroutineScopeService::drain(
    LowBlockId current, InstId origin, LowInstId group,
    CoroutineTaskGroupExitIntent exit_intent,
    LoweringCoroutineScopeState &state) {
  auto &instruction = state.instruction;
  const auto void_type = instruction.session.sem_ir.voidType();
  if (exit_intent != CoroutineTaskGroupExitIntent::Normal)
    (void)emit(instruction, current,
               LowCoroutineTaskGroupRequestCancel{void_type, group, {}},
               origin);
  (void)emit(instruction, current,
             LowCoroutineTaskGroupClose{void_type, group, {}}, origin);
  const auto completion = emit(
      instruction, current,
      LowCoroutineTaskGroupCompletionArm{
          instruction.session.sem_ir.coroutineTaskCompletionType(), group, {}},
      origin);
  const auto wake_slot = instruction.session.low_ir.addSlot(
      {instruction.session.sem_ir.coroutineTaskCompletionType(),
       LocalId::invalid(), LowSlotSynthetic, 0});
  instruction.function_slots.push_back(wake_slot);
  const auto empty = instruction.session.low_ir.addPlaceProjectionBlock({});
  (void)instruction.session.low_ir.addPlace(
      {wake_slot, empty, empty,
       instruction.session.sem_ir.coroutineTaskCompletionType(),
       LowPlaceAddressable});
  (void)emit(instruction, current,
             LowInitialize{void_type, wake_slot, completion}, origin);
  const auto wake = emit(
      instruction, current,
      LowLoad{instruction.session.sem_ir.coroutineTaskCompletionType(),
              wake_slot, {}},
      origin);
  const std::array operands{wake, group};
  const auto lowered_operands =
      instruction.session.low_ir.addValueBlock(operands);
  if (exit_intent == CoroutineTaskGroupExitIntent::SelectedError)
    (void)emit(instruction, current,
               LowCoroutineTaskGroupErrorDrain{void_type, lowered_operands,
                                               {}},
               origin);
  else if (exit_intent ==
           CoroutineTaskGroupExitIntent::SelectedCancellation)
    (void)emit(instruction, current,
               LowCoroutineTaskGroupCancelDrain{void_type, lowered_operands,
                                                {}},
               origin);
  else
    (void)emit(instruction, current,
               LowCoroutineTaskGroupDrain{void_type, lowered_operands, {}},
               origin);
  return current;
}

LowBlockId LoweringCoroutineScopeService::drains(
    LowBlockId current, std::size_t target_depth,
    CoroutineTaskGroupExitIntent exit_intent,
    LoweringCoroutineScopeState &state) {
  assert(target_depth <= state.active_scopes.size());
  for (std::size_t depth = state.active_scopes.size(); depth > target_depth;
       --depth) {
    const auto &scope = state.active_scopes[depth - 1];
    current = drain(current, scope.origin, scope.group, exit_intent, state);
  }
  return current;
}

void LoweringCoroutineScopeService::cancellationCheck(
    InstId origin, const PlaceCleanupPlan &cleanup, LowBlockId &current,
    LoweringCoroutineScopeState &state) {
  auto &instruction = state.instruction;
  const auto cancelled = emit(
      instruction, current,
      LowCoroutineCancellationRequested{instruction.session.sem_ir.boolType(),
                                        {}, {}},
      origin);
  const auto cancel = newBlock(instruction);
  const auto continuation = newBlock(instruction);
  (void)emit(instruction, current,
             LowBranchIf{instruction.session.sem_ir.voidType(), cancelled,
                         instruction.session.low_ir.addTargets(
                             {cancel, continuation})},
             origin);
  auto cancel_tail = drains(
      cancel, 0, CoroutineTaskGroupExitIntent::SelectedCancellation, state);
  cancel_tail = state.emit_cleanup(cancel_tail, origin, cleanup);
  (void)emit(instruction, cancel_tail,
             LowCoroutineReturnCancelled{
                 instruction.session.sem_ir.voidType(), {}, {}},
             origin);
  current = continuation;
}

void LoweringCoroutineScopeService::taskScope(
    InstId semantic_id, SemCoroutineTaskScope semantic, LowBlockId &current,
    LoweringCoroutineScopeState &state) {
  auto &instruction = state.instruction;
  const auto group = emit(
      instruction, current,
      LowCoroutineTaskGroupCreate{
          instruction.session.sem_ir.coroutineScopeType(), {}, {}},
      semantic_id);
  state.active_scopes.push_back({group, semantic_id});
  state.lower_block(semantic.arg0, current);
  state.active_scopes.pop_back();
  if (state.is_terminated(current))
    return;
  current = drain(current, semantic_id, group,
                  CoroutineTaskGroupExitIntent::Normal, state);
  const auto &cleanup =
      instruction.session.sem_ir.placeStates().blockCleanup(semantic.arg0);
  if (!state.active_scopes.empty())
    cancellationCheck(semantic_id, cleanup, current, state);
  current = state.emit_cleanup(current, semantic_id, cleanup);
}

void LoweringCoroutineScopeService::suspend(
    InstId semantic_id, SemCoroutineSuspend semantic, LowBlockId &current,
    LoweringCoroutineScopeState &state) {
  auto &instruction = state.instruction;
  const auto operand_type =
      TypeId(instruction.session.sem_ir.inst(semantic.arg0).type);
  const std::array operands{valueFor(instruction, semantic.arg0)};
  const auto lowered_operands =
      instruction.session.low_ir.addValueBlock(operands);
  if (instruction.session.sem_ir.type(operand_type).kind ==
      SemTypeKind::CoroutineTaskCompletion) {
    instruction.values[semantic_id.index] = emit(
        instruction, current,
        LowCoroutineTaskCompletionWait{semantic.type, lowered_operands, {}},
        semantic_id);
  } else {
    const auto plan =
        instruction.session.low_ir.callbackWakePlanFor(operand_type);
    instruction.values[semantic_id.index] = emit(
        instruction, current,
        LowCallbackWakeWait{semantic.type, plan, lowered_operands}, semantic_id);
  }
  if (state.active_scopes.empty())
    return;
  state.protected_suspensions.insert(
      instruction.values[semantic_id.index].index);
  cancellationCheck(
      semantic_id,
      instruction.session.sem_ir.placeStates()
          .suspensionCleanup(semantic_id)
          .transferred,
      current, state);
}

void LoweringCoroutineScopeService::cancellationCheck(
    InstId semantic_id, SemCoroutineCancellationCheck semantic,
    LowBlockId &current, LoweringCoroutineScopeState &state) {
  auto &instruction = state.instruction;
  if (!state.active_scopes.empty()) {
    cancellationCheck(
        semantic_id,
        instruction.session.sem_ir.placeStates().cancellationCleanup(
            semantic_id),
        current, state);
    instruction.values[semantic_id.index] = emit(
        instruction, current,
        LowVoidValue{semantic.type, {}, {}}, semantic_id);
    return;
  }
  instruction.values[semantic_id.index] = emit(
      instruction, current,
      LowCoroutineCancellationCheck{semantic.type, {}, {}}, semantic_id);
}

void LoweringCoroutineScopeService::completionCombine(
    InstId semantic_id, InstId set,
    CoroutineTaskCompletionCombineKind operation, TypeId result_type,
    LowBlockId &current, LoweringCoroutineScopeState &state) {
  auto &instruction = state.instruction;
  const auto set_type = TypeId(instruction.session.sem_ir.inst(set).type);
  const auto count =
      instruction.session.sem_ir.coroutineTaskCompletionCapacity(set_type);
  const auto plan = LoweringCoroutineService::buildCompletionCombinePlan(
      instruction.session, instruction.function.current_function, semantic_id,
      set, operation, result_type);
  instruction.values[semantic_id.index] = emit(
      instruction, current,
      LowCoroutineTaskCompletionCombine{
          result_type, plan, valueFor(instruction, set)},
      semantic_id);
  if (count == 0) {
    current = state.emit_cleanup(
        current, semantic_id,
        instruction.session.sem_ir.placeStates()
            .suspensionCleanup(semantic_id)
            .pre_commit);
    return;
  }
  if (state.active_scopes.empty())
    return;
  state.protected_suspensions.insert(
      instruction.values[semantic_id.index].index);
  cancellationCheck(
      semantic_id,
      instruction.session.sem_ir.placeStates()
          .suspensionCleanup(semantic_id)
          .transferred,
      current, state);
}

} // namespace chtholly::compiler::internal
