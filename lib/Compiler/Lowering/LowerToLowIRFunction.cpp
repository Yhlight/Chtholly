#include "LowerToLowIRInternal.h"

#include <cassert>
#include <ranges>
#include <vector>

namespace chtholly::compiler::internal {

LowFunctionId LoweringFunctionService::lower(FunctionId function_id,
                                             State &state) {
  state.function.current_function = function_id;
  state.values.clear();
  state.slots.clear();
  state.places.clear();
  state.write_only_values.clear();
  state.take_values.clear();
  state.borrow_values.clear();
  state.borrow_places.clear();
  state.function_slots.clear();
  state.pending_blocks.clear();
  state.cleanup_tails.clear();
  state.pending_cleanup_graphs.clear();
  state.suspension_pre_cleanup.clear();
  state.suspension_transferred_cleanup.clear();
  state.cancellation_cleanup.clear();
  state.loop_targets.clear();
  state.active_task_scopes.clear();
  state.scope_protected_suspensions.clear();

  const auto entry = state.new_block();
  const auto &function = state.session.sem_ir.function(function_id);
  state.function.unified_return =
      (function.flags & (SemFunctionCoroutineScaffold |
                         SemFunctionCoroutineTaskDriver)) == 0;
  state.function.return_block = state.function.unified_return
                                    ? state.new_block()
                                    : LowBlockId::invalid();
  state.function.current_return_type =
      state.session.sem_ir.type(function.type).kind == SemTypeKind::AsyncFunction
          ? state.session.sem_ir.asyncSuccessType(function.type)
          : TypeId(state.session.sem_ir.type(function.type).arg1);
  state.function.return_slot =
      !state.function.unified_return ||
              state.function.current_return_type ==
                  state.session.sem_ir.voidType() ||
              state.function.current_return_type ==
                  state.session.sem_ir.neverType() ||
              (!state.is_representation_pack() &&
               state.session.sem_ir
                       .typeRepresentation(state.function.current_return_type)
                       .init_repr == InitReprKind::InPlace)
          ? SlotId::invalid()
          : state.session.low_ir.addSlot(
                {state.function.current_return_type, LocalId::invalid(),
                 LowSlotSynthetic, 0});
  if (state.function.return_slot.hasValue())
    state.function_slots.push_back(state.function.return_slot);

  auto current = entry;
  state.next_parameter = 0;
  state.collect_write_only_values(function.body);
  state.lower_block(function.body, current);

  const auto body = state.session.sem_ir.instBlock(function.body);
  assert(!body.empty());
  const auto return_origin = body.back();
  if (!state.function.unified_return) {
    // Coroutine frame lowering consumes the original per-edge return value.
  } else if (state.function.return_slot.hasValue()) {
    const auto return_value = state.emit_load(
        state.function.return_block, state.function.current_return_type,
        return_origin, state.function.return_slot);
    (void)state.emit_return(state.function.return_block,
                            state.session.sem_ir.voidType(), return_origin,
                            return_value);
  } else if (state.function.current_return_type ==
             state.session.sem_ir.neverType()) {
    (void)state.emit_unreachable(state.function.return_block,
                                 state.session.sem_ir.voidType(),
                                 return_origin);
  } else if (state.function.current_return_type ==
             state.session.sem_ir.voidType()) {
    const auto return_value = state.emit_void(
        state.function.return_block, state.function.current_return_type,
        return_origin);
    (void)state.emit_return(state.function.return_block,
                            state.session.sem_ir.voidType(), return_origin,
                            return_value);
  } else {
    (void)state.emit_return_in_place(state.function.return_block,
                                     state.session.sem_ir.voidType(),
                                     return_origin);
  }

  const auto main_block_count = state.pending_blocks.size();
  if ((function.flags & SemFunctionCoroutineScaffold) != 0) {
    std::vector<InstId> cleanup_origins;
    for (std::size_t block_index = 0; block_index < main_block_count;
         ++block_index)
      for (const auto low_instruction : state.pending_blocks[block_index]) {
        const auto origin = state.session.low_ir.origin(low_instruction);
        if (!origin.hasValue() ||
            std::ranges::find(cleanup_origins, origin) !=
                cleanup_origins.end())
          continue;
        const auto empty_combination =
            state.session.low_ir.inst(low_instruction).kind ==
                LowInstKind::CoroutineTaskCompletionCombine &&
            state.session.low_ir.coroutineTaskCompletionCombinePlan(
                state.session.low_ir
                    .getAs<LowCoroutineTaskCompletionCombine>(low_instruction)
                    .arg0)
                    .operand_count == 0;
        const auto kind = state.session.sem_ir.inst(origin).kind;
        if (!empty_combination &&
            (kind == SemInstKind::CoroutineSuspend ||
             kind == SemInstKind::CoroutineTaskCompletionWaitAll ||
             kind == SemInstKind::CoroutineTaskCompletionSelect ||
             kind == SemInstKind::CoroutineTaskCompletionRace ||
             kind == SemInstKind::CoroutineCancellationCheck ||
             kind == SemInstKind::CoroutineExecutorSwitch))
          cleanup_origins.push_back(origin);
      }
    for (const auto origin : cleanup_origins) {
      const auto kind = state.session.sem_ir.inst(origin).kind;
      if (kind == SemInstKind::CoroutineCancellationCheck ||
          kind == SemInstKind::CoroutineExecutorSwitch) {
        state.add_pending_cleanup_graph(
            state.session.sem_ir.placeStates().cancellationCleanup(origin),
            origin, LoweringPendingCleanupGraph::Role::Cancellation);
        continue;
      }
      const auto &partition =
          state.session.sem_ir.placeStates().suspensionCleanup(origin);
      state.add_pending_cleanup_graph(
          partition.pre_commit, origin,
          LoweringPendingCleanupGraph::Role::PreCommit);
      state.add_pending_cleanup_graph(
          partition.transferred, origin,
          LoweringPendingCleanupGraph::Role::Transferred);
    }
  }

  std::vector<LowBlockId> blocks;
  blocks.reserve(state.pending_blocks.size());
  for (const auto &instructions : state.pending_blocks) {
    const auto id = state.session.low_ir.addBlock(instructions);
    assert(id.index == entry.index + blocks.size());
    blocks.push_back(id);
  }
  for (const auto &graph : state.pending_cleanup_graphs) {
    const auto graph_blocks = std::span<const LowBlockId>(blocks).subspan(
        graph.begin, graph.end - graph.begin);
    const auto id = state.session.low_ir.addCoroutineCleanupGraph(
        {function_id, graph.origin, graph.entry,
         state.session.low_ir.addBlockList(graph_blocks), graph.local_slots});
    auto &mapping =
        graph.role == LoweringPendingCleanupGraph::Role::PreCommit
            ? state.suspension_pre_cleanup
            : graph.role == LoweringPendingCleanupGraph::Role::Transferred
                ? state.suspension_transferred_cleanup
                : state.cancellation_cleanup;
    mapping.emplace(graph.origin.index, id);
  }
  const auto block_list = state.session.low_ir.addBlockList(
      std::span<const LowBlockId>(blocks).first(main_block_count));
  const auto slot_block =
      state.session.low_ir.addSlotBlock(state.function_slots);
  return state.session.low_ir.addFunction(
      {function_id, entry, block_list, slot_block});
}

} // namespace chtholly::compiler::internal
