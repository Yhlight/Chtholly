#include "LowerToLowIRInternal.h"

#include <cassert>
#include <algorithm>

namespace chtholly::compiler::internal {

void LoweringIfService::lower(InstId semantic_id, SemIf semantic,
                              LowBlockId &current, LoweringIfState &state) {
  const auto arms = state.session.sem_ir.instBlock(semantic.arg1);
  assert(arms.size() == 1 || arms.size() == 2);
  const auto then_block = state.new_block();
  const auto continuation = state.new_block();
  const auto else_block = arms.size() == 2 ? state.new_block() : continuation;
  const auto result_slot =
      semantic.type == state.session.sem_ir.voidType() ||
              semantic.type == state.session.sem_ir.neverType()
          ? SlotId::invalid()
          : state.session.low_ir.addSlot(
                {semantic.type, LocalId::invalid(), LowSlotSynthetic, 0});
  if (result_slot.hasValue())
    state.function_slots.push_back(result_slot);
  state.emit_branch_if(
      current, semantic_id, state.value_for(semantic.arg0),
      state.session.low_ir.addTargets({then_block, else_block}));
  for (std::size_t index = 0; index < arms.size(); ++index) {
    const auto &arm = state.session.sem_ir.getAs<SemIfArm>(arms[index]);
    auto nested = index == 0 ? then_block : else_block;
    state.lower_block(arm.arg0, nested);
    if (!state.is_terminated(nested)) {
      if (result_slot.hasValue()) {
        const auto body = state.session.sem_ir.instBlock(arm.arg0);
        assert(!body.empty() &&
               state.session.sem_ir.inst(body.back()).kind == SemInstKind::Yield);
        const auto &yield = state.session.sem_ir.getAs<SemYield>(body.back());
        state.emit_initialize(nested, semantic_id, result_slot,
                              state.value_for(yield.arg0));
      }
      nested = state.emit_cleanup(
          nested, semantic_id,
          state.session.sem_ir.placeStates().blockCleanup(arm.arg0));
      state.emit_branch(nested, semantic_id, continuation);
    }
  }
  current = continuation;
  if (result_slot.hasValue())
    state.values[semantic_id.index] =
        state.emit_load(current, semantic.type, semantic_id, result_slot);
  else if (semantic.type == state.session.sem_ir.neverType())
    state.emit_unreachable(current, semantic_id);
}

void LoweringIfService::scopedBlock(InstId semantic_id, SemScopedBlock semantic,
                                    LowBlockId &current,
                                    LoweringIfState &state) {
  const auto nested_entry = state.new_block();
  const auto continuation = state.new_block();
  const auto result_slot =
      semantic.type == state.session.sem_ir.voidType() ||
              semantic.type == state.session.sem_ir.neverType()
          ? SlotId::invalid()
          : state.session.low_ir.addSlot(
                {semantic.type, LocalId::invalid(), LowSlotSynthetic, 0});
  if (result_slot.hasValue())
    state.function_slots.push_back(result_slot);
  state.emit_branch(current, semantic_id, nested_entry);
  auto nested = nested_entry;
  state.lower_block(semantic.arg0, nested);
  if (!state.is_terminated(nested)) {
    if (result_slot.hasValue()) {
      const auto body = state.session.sem_ir.instBlock(semantic.arg0);
      assert(!body.empty() &&
             state.session.sem_ir.inst(body.back()).kind == SemInstKind::Yield);
      const auto &yield = state.session.sem_ir.getAs<SemYield>(body.back());
      state.emit_initialize(nested, semantic_id, result_slot,
                            state.value_for(yield.arg0));
    }
    nested = state.emit_cleanup(
        nested, semantic_id,
        state.session.sem_ir.placeStates().blockCleanup(semantic.arg0));
    state.emit_branch(nested, semantic_id, continuation);
  }
  current = continuation;
  if (result_slot.hasValue())
    state.values[semantic_id.index] =
        state.emit_load(current, semantic.type, semantic_id, result_slot);
  else if (semantic.type == state.session.sem_ir.neverType())
    state.emit_unreachable(current, semantic_id);
  else
    state.values[semantic_id.index] =
        state.emit_void(current, semantic.type, semantic_id);
}

LowBlockId LoweringControlService::newBlock(
    LoweringSessionState &session,
    std::vector<std::vector<LowInstId>> &pending_blocks) {
  const auto index = session.low_ir.blockCount() + pending_blocks.size();
  pending_blocks.emplace_back();
  return LowBlockId(static_cast<std::uint32_t>(index));
}

bool LoweringControlService::isTerminated(
    const LoweringSessionState &session,
    const std::vector<std::vector<LowInstId>> &pending_blocks,
    LowBlockId block) {
  const auto &instructions =
      pending_blocks[block.index - session.low_ir.blockCount()];
  if (instructions.empty())
    return false;
  switch (session.low_ir.inst(instructions.back()).kind) {
  case LowInstKind::Branch:
  case LowInstKind::BranchIf:
  case LowInstKind::Return:
  case LowInstKind::ReturnInPlace:
  case LowInstKind::Unreachable:
  case LowInstKind::FatalFailure:
  case LowInstKind::CoroutineRuntimeFault:
  case LowInstKind::CoroutineReturnSuccess:
  case LowInstKind::CoroutineReturnError:
  case LowInstKind::CoroutineReturnCancelled:
    return true;
  default:
    return false;
  }
}

InstId LoweringControlService::semanticBlockResult(
    const SemIR &sem_ir, std::span<const InstId> instructions) {
  assert(!instructions.empty());
  if (sem_ir.inst(instructions.back()).kind == SemInstKind::EndFullExpression) {
    assert(instructions.size() >= 2);
    return instructions[instructions.size() - 2];
  }
  return instructions.back();
}

bool LoweringControlService::semanticConditionIsAlwaysTrue(
    const SemIR &sem_ir, InstBlockId block) {
  const auto instructions = sem_ir.instBlock(block);
  if (instructions.empty())
    return false;
  const auto last = semanticBlockResult(sem_ir, instructions);
  const auto &inst = sem_ir.inst(last);
  return inst.kind == SemInstKind::BoolLiteral &&
         sem_ir.integer(IntegerId(inst.arg0)) != 0;
}

bool LoweringControlService::semanticBlockContainsLoopBreak(
    const SemIR &sem_ir, InstBlockId block, std::uint32_t nested_loops) {
  for (const auto id : sem_ir.instBlock(block)) {
    const auto &inst = sem_ir.inst(id);
    if (inst.kind == SemInstKind::Break &&
        sem_ir.integer(IntegerId(inst.arg0)) == nested_loops)
      return true;
    if (inst.kind == SemInstKind::If &&
        std::ranges::any_of(sem_ir.instBlock(InstBlockId(inst.arg1)),
                            [&](InstId arm) {
                              return semanticBlockContainsLoopBreak(
                                  sem_ir, InstBlockId(sem_ir.inst(arm).arg0),
                                  nested_loops);
                            }))
      return true;
    if (inst.kind == SemInstKind::Switch &&
        std::ranges::any_of(sem_ir.instBlock(InstBlockId(inst.arg1)),
                            [&](InstId arm) {
                              return semanticBlockContainsLoopBreak(
                                  sem_ir, InstBlockId(sem_ir.inst(arm).arg1),
                                  nested_loops);
                            }))
      return true;
    if (inst.kind == SemInstKind::CoroutineTaskScope &&
        semanticBlockContainsLoopBreak(sem_ir, InstBlockId(inst.arg0),
                                       nested_loops))
      return true;
    if ((inst.kind == SemInstKind::While || inst.kind == SemInstKind::For ||
         inst.kind == SemInstKind::DoWhile) &&
        semanticBlockContainsLoopBreak(sem_ir, InstBlockId(inst.arg1),
                                       nested_loops + 1))
      return true;
  }
  return false;
}

bool LoweringControlService::isDirectPlacementCall(const SemIR &sem_ir,
                                                   InstId call,
                                                   InstId consumer) {
  const auto &call_inst = sem_ir.inst(call);
  const auto &consumer_inst = sem_ir.inst(consumer);
  if ((call_inst.kind != SemInstKind::Call &&
       call_inst.kind != SemInstKind::ForeignOperationCall) ||
      consumer_inst.arg1 != call.index ||
      !LoweringExpressionService::isConstructor(
          sem_ir, FunctionRefId(call_inst.arg0)))
    return false;
  if (consumer_inst.kind == SemInstKind::Assign ||
      consumer_inst.kind == SemInstKind::Placement)
    return true;
  return (consumer_inst.kind == SemInstKind::BindName ||
          consumer_inst.kind == SemInstKind::MaterializeTemporary) &&
         LoweringExpressionService::isInfallibleConstructorFor(
             sem_ir, FunctionRefId(call_inst.arg0), TypeId(call_inst.type));
}

} // namespace chtholly::compiler::internal
