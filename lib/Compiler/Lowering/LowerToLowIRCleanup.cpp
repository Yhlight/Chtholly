#include "LowerToLowIRInternal.h"

#include <algorithm>

namespace chtholly::compiler::internal {

LowBlockId LoweringCleanupService::action(const PlaceCleanupAction &action,
                                         LowBlockId block_id, InstId origin,
                                         State &state) {
  const auto target = state.place_for(action.place);
  if (const auto computed = state.computed_projection_cleanup(action.place)) {
    const auto owner = state.emit_place_address(block_id, computed->owner_type,
                                                origin, computed->owner);
    const auto taken = state.emit_projection_take(
        block_id, state.place_type(target), origin, owner,
        FieldIndex(computed->field));
    state.emit_mark_moved(block_id, origin, target);
    return state.emit_destroy_value(
        block_id, origin, state.place_type(target), taken);
  }
  block_id = state.emit_destroy(block_id, origin, target);
  state.emit_mark_moved(block_id, origin, target);
  return block_id;
}

LowBlockId LoweringCleanupService::emit(const PlaceCleanupPlan &plan,
                                        LowBlockId block_id, InstId origin,
                                        State &state) {
  for (const auto &action : plan.actions) {
    if (action.kind == PlaceCleanupKind::RunDefer) {
      state.lower_block(action.block, block_id);
      if (!state.is_terminated(block_id))
        block_id = emit(state.sem_ir.placeStates().blockCleanup(action.block),
                        block_id, origin, state);
      continue;
    }
    if (action.kind == PlaceCleanupKind::EndLifetime) {
      state.emit_end_lifetime(block_id, origin, state.slot_for(action.local));
      continue;
    }
    if (action.kind == PlaceCleanupKind::DestroyIfInitialized) {
      const auto destroy = state.new_block();
      const auto continuation = state.new_block();
      const auto place = state.place_for(action.place);
      const auto condition = state.emit_is_initialized(
          block_id, state.sem_ir.boolType(), origin, place);
      state.emit_branch_if(block_id, origin, condition, destroy, continuation);
      const auto destroy_tail =
          state.emit_cleanup_action(destroy, origin, action);
      state.emit_branch(destroy_tail, origin, continuation);
      block_id = continuation;
      continue;
    }
    block_id = state.emit_cleanup_action(block_id, origin, action);
  }
  return block_id;
}

bool LoweringCleanupService::hasCustomCleanup(const SemIR &sem_ir,
                                              const PlaceCleanupPlan &plan) {
  return std::ranges::any_of(plan.actions, [&](const auto &action) {
    if (action.kind == PlaceCleanupKind::RunDefer)
      return true;
    if (action.kind == PlaceCleanupKind::EndLifetime)
      return false;
    return sem_ir.typeRepresentation(sem_ir.placeStates().place(action.place).type)
               .destroy == DestroyReprKind::Custom;
  });
}

} // namespace chtholly::compiler::internal
