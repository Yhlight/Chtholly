#include "LowerToLowIRInternal.h"

#include <cassert>
#include <cstddef>
#include <vector>

namespace chtholly::compiler::internal {
namespace {

template <typename InstT>
LowInstId emit(LoweringLoopState &state, LowBlockId block, InstT inst,
               InstId origin) {
  const auto id = state.session.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.session.low_ir.blockCount()]
      .push_back(id);
  return id;
}

LowBlockId emitCleanup(LoweringLoopState &state, LowBlockId block,
                       InstId origin, InstBlockId semantic_block) {
  return state.emit_cleanup(
      block, origin, state.session.sem_ir.placeStates().blockCleanup(
                        semantic_block));
}

} // namespace

void LoweringLoopService::whileLoop(InstId semantic_id, SemWhile semantic,
                                    LowBlockId &current,
                                    LoweringLoopState &state) {
  const auto diverges =
      LoweringControlService::semanticConditionIsAlwaysTrue(
          state.session.sem_ir, semantic.arg0) &&
      !LoweringControlService::semanticBlockContainsLoopBreak(
          state.session.sem_ir, semantic.arg1, 0);
  const auto header = state.new_block();
  const auto body = state.new_block();
  const auto continuation = state.new_block();
  emit(state, current,
       LowBranch{state.session.sem_ir.voidType(), header, {}}, semantic_id);

  auto condition_current = header;
  state.lower_block(semantic.arg0, condition_current);
  const auto condition_insts = state.session.sem_ir.instBlock(semantic.arg0);
  assert(!condition_insts.empty());
  const auto targets = state.session.low_ir.addTargets({body, continuation});
  emit(state, condition_current,
       LowBranchIf{state.session.sem_ir.voidType(),
                   state.value_for(LoweringControlService::semanticBlockResult(
                       state.session.sem_ir, condition_insts)),
                   targets},
       semantic_id);

  auto body_current = body;
  state.loop_targets.push_back(
      {continuation, header, state.active_task_scope_depth()});
  state.lower_block(semantic.arg1, body_current);
  state.loop_targets.pop_back();
  if (!state.is_terminated(body_current)) {
    const auto &cleanup =
        state.session.sem_ir.placeStates().blockCleanup(semantic.arg1);
    const auto successor = state.has_custom_cleanup(cleanup)
                               ? state.cleanup_tail(cleanup, semantic_id, header)
                               : header;
    if (!state.has_custom_cleanup(cleanup))
      body_current = state.emit_cleanup(body_current, semantic_id, cleanup);
    emit(state, body_current,
         LowBranch{state.session.sem_ir.voidType(), successor, {}}, semantic_id);
  }
  current = continuation;
  if (diverges)
    emit(state, current,
         LowUnreachable{state.session.sem_ir.voidType(), {}}, semantic_id);
}

void LoweringLoopService::forLoop(InstId semantic_id, SemFor semantic,
                                  LowBlockId &current,
                                  LoweringLoopState &state) {
  const auto clauses = state.session.sem_ir.instBlock(semantic.arg0);
  assert(clauses.size() == 3);
  const auto init = state.session.sem_ir.getAs<SemForClause>(clauses[0]).arg1;
  const auto condition =
      state.session.sem_ir.getAs<SemForClause>(clauses[1]).arg1;
  const auto step = state.session.sem_ir.getAs<SemForClause>(clauses[2]).arg1;
  const auto diverges =
      LoweringControlService::semanticConditionIsAlwaysTrue(
          state.session.sem_ir, condition) &&
      !LoweringControlService::semanticBlockContainsLoopBreak(
          state.session.sem_ir, semantic.arg1, 0);
  state.lower_block(init, current);

  const auto header = state.new_block();
  const auto body = state.new_block();
  const auto step_block = state.new_block();
  const auto natural_exit = state.new_block();
  const auto continuation = state.new_block();
  emit(state, current,
       LowBranch{state.session.sem_ir.voidType(), header, {}}, semantic_id);

  auto condition_current = header;
  state.lower_block(condition, condition_current);
  const auto condition_insts = state.session.sem_ir.instBlock(condition);
  assert(!condition_insts.empty());
  emit(state, condition_current,
       LowBranchIf{
           state.session.sem_ir.voidType(),
           state.value_for(LoweringControlService::semanticBlockResult(
               state.session.sem_ir, condition_insts)),
           state.session.low_ir.addTargets({body, natural_exit})},
       semantic_id);

  auto body_current = body;
  state.loop_targets.push_back(
      {continuation, step_block, state.active_task_scope_depth()});
  state.lower_block(semantic.arg1, body_current);
  state.loop_targets.pop_back();
  if (!state.is_terminated(body_current)) {
    body_current = emitCleanup(state, body_current, semantic_id, semantic.arg1);
    emit(state, body_current,
         LowBranch{state.session.sem_ir.voidType(), step_block, {}},
         semantic_id);
  }

  auto step_current = step_block;
  state.lower_block(step, step_current);
  if (!state.is_terminated(step_current))
    emit(state, step_current,
         LowBranch{state.session.sem_ir.voidType(), header, {}}, semantic_id);

  auto exit_current = emitCleanup(
      state, natural_exit, semantic_id, init);
  emit(state, exit_current,
       LowBranch{state.session.sem_ir.voidType(), continuation, {}},
       semantic_id);
  current = continuation;
  if (diverges)
    emit(state, current,
         LowUnreachable{state.session.sem_ir.voidType(), {}}, semantic_id);
}

void LoweringLoopService::doWhile(InstId semantic_id, SemDoWhile semantic,
                                  LowBlockId &current,
                                  LoweringLoopState &state) {
  const auto diverges =
      LoweringControlService::semanticConditionIsAlwaysTrue(
          state.session.sem_ir, semantic.arg0) &&
      !LoweringControlService::semanticBlockContainsLoopBreak(
          state.session.sem_ir, semantic.arg1, 0);
  const auto body = state.new_block();
  const auto condition = state.new_block();
  const auto continuation = state.new_block();
  emit(state, current,
       LowBranch{state.session.sem_ir.voidType(), body, {}}, semantic_id);
  auto body_current = body;
  state.loop_targets.push_back(
      {continuation, condition, state.active_task_scope_depth()});
  state.lower_block(semantic.arg1, body_current);
  state.loop_targets.pop_back();
  if (!state.is_terminated(body_current)) {
    body_current = emitCleanup(state, body_current, semantic_id, semantic.arg1);
    emit(state, body_current,
         LowBranch{state.session.sem_ir.voidType(), condition, {}},
         semantic_id);
  }
  auto condition_current = condition;
  state.lower_block(semantic.arg0, condition_current);
  const auto condition_insts = state.session.sem_ir.instBlock(semantic.arg0);
  assert(!condition_insts.empty());
  emit(state, condition_current,
       LowBranchIf{
           state.session.sem_ir.voidType(),
           state.value_for(LoweringControlService::semanticBlockResult(
               state.session.sem_ir, condition_insts)),
           state.session.low_ir.addTargets({body, continuation})},
       semantic_id);
  current = continuation;
  if (diverges)
    emit(state, current,
         LowUnreachable{state.session.sem_ir.voidType(), {}}, semantic_id);
}

void LoweringLoopService::breakLoop(InstId semantic_id, LowBlockId &current,
                                    LoweringLoopState &state) {
  assert(!state.loop_targets.empty());
  const auto distance = static_cast<std::size_t>(state.session.sem_ir.integer(
      IntegerId(state.session.sem_ir.inst(semantic_id).arg0)));
  assert(distance < state.loop_targets.size());
  const auto &target = state.loop_targets[state.loop_targets.size() - 1 - distance];
  current = state.task_scope_drains(
      current, target.task_scope_depth, CoroutineTaskGroupExitIntent::Normal);
  current = state.emit_cleanup(
      current, semantic_id,
      state.session.sem_ir.placeStates().edgeCleanup(semantic_id));
  emit(state, current,
       LowBranch{state.session.sem_ir.voidType(), target.break_target, {}},
       semantic_id);
}

void LoweringLoopService::continueLoop(InstId semantic_id,
                                       LowBlockId &current,
                                       LoweringLoopState &state) {
  assert(!state.loop_targets.empty());
  const auto distance = static_cast<std::size_t>(state.session.sem_ir.integer(
      IntegerId(state.session.sem_ir.inst(semantic_id).arg0)));
  assert(distance < state.loop_targets.size());
  const auto &target = state.loop_targets[state.loop_targets.size() - 1 - distance];
  current = state.task_scope_drains(
      current, target.task_scope_depth, CoroutineTaskGroupExitIntent::Normal);
  current = state.emit_cleanup(
      current, semantic_id,
      state.session.sem_ir.placeStates().edgeCleanup(semantic_id));
  emit(state, current,
       LowBranch{state.session.sem_ir.voidType(), target.continue_target, {}},
       semantic_id);
}

void LoweringLoopService::switchValue(InstId semantic_id, SemSwitch semantic,
                                      LowBlockId &current,
                                      LoweringLoopState &state) {
  const auto arms = state.session.sem_ir.instBlock(semantic.arg1);
  assert(!arms.empty());
  const auto result_slot =
      semantic.type == state.session.sem_ir.voidType() ||
              semantic.type == state.session.sem_ir.neverType()
          ? SlotId::invalid()
          : state.session.low_ir.addSlot(
                {semantic.type, LocalId::invalid(), LowSlotSynthetic, 0});
  if (result_slot.hasValue())
    state.function_slots.push_back(result_slot);
  const auto continuation = state.new_block();
  auto dispatch = current;
  const auto tag = emit(
      state, dispatch,
      LowEnumTag{state.session.sem_ir.i32Type(), state.value_for(semantic.arg0)},
      semantic_id);
  for (std::size_t index = 0; index < arms.size(); ++index) {
    const auto &arm = state.session.sem_ir.getAs<SemSwitchArm>(arms[index]);
    const auto arm_block = state.new_block();
    const auto variant = state.session.sem_ir.integer(arm.arg0);
    const auto is_last = index + 1 == arms.size();
    if (variant < 0 || is_last) {
      emit(state, dispatch,
           LowBranch{state.session.sem_ir.voidType(), arm_block, {}},
           semantic_id);
    } else {
      const auto next_dispatch = state.new_block();
      const auto scrutinee_type =
          TypeId(state.session.sem_ir.inst(semantic.arg0).type);
      const auto &nominal = state.session.sem_ir.nominalType(
          NominalTypeId(state.session.sem_ir.type(scrutinee_type).arg0));
      const auto runtime_discriminant = state.session.sem_ir.addInteger(
          nominal.variants[static_cast<std::size_t>(variant)].discriminant);
      const auto variant_value = emit(
          state, dispatch,
          LowIntegerConstant{state.session.sem_ir.i32Type(),
                             runtime_discriminant},
          semantic_id);
      const auto matches = emit(
          state, dispatch,
          LowEqual{state.session.sem_ir.boolType(), tag, variant_value},
          semantic_id);
      emit(state, dispatch,
           LowBranchIf{state.session.sem_ir.voidType(), matches,
                       state.session.low_ir.addTargets({arm_block,
                                                        next_dispatch})},
           semantic_id);
      dispatch = next_dispatch;
    }
    auto arm_current = arm_block;
    state.lower_block(arm.arg1, arm_current);
    if (!state.is_terminated(arm_current)) {
      if (result_slot.hasValue()) {
        const auto body = state.session.sem_ir.instBlock(arm.arg1);
        assert(!body.empty() &&
               state.session.sem_ir.inst(body.back()).kind == SemInstKind::Yield);
        const auto &yield = state.session.sem_ir.getAs<SemYield>(body.back());
        emit(state, arm_current,
             LowInitialize{state.session.sem_ir.voidType(), result_slot,
                           state.value_for(yield.arg0)},
             semantic_id);
      }
      arm_current = emitCleanup(state, arm_current, semantic_id, arm.arg1);
      emit(state, arm_current,
           LowBranch{state.session.sem_ir.voidType(), continuation, {}},
           semantic_id);
    }
  }
  current = continuation;
  if (result_slot.hasValue())
    state.values[semantic_id.index] = emit(
        state, current,
        LowLoad{semantic.type, result_slot}, semantic_id);
  else if (semantic.type == state.session.sem_ir.neverType())
    emit(state, current,
         LowUnreachable{state.session.sem_ir.voidType(), {}}, semantic_id);
  else
    state.values[semantic_id.index] = emit(
        state, current,
        LowVoidValue{semantic.type, {}}, semantic_id);
}

} // namespace chtholly::compiler::internal
