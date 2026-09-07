#include "LowerToLowIRInternal.h"

#include <array>
#include <cassert>
#include <ranges>
#include <vector>

namespace chtholly::compiler::internal {

void LoweringBindingService::bindName(InstId semantic_id,
                                      SemBindName semantic,
                                      LowBlockId current,
                                      LoweringBindingState &state) {
  const auto &sem_ir = state.session.sem_ir;
  const auto local_type = sem_ir.local(semantic.arg0).type;
  if (local_type == sem_ir.voidType()) {
    state.values[semantic_id.index] =
        state.emit_void(current, sem_ir.voidType(), semantic_id);
    return;
  }
  const auto slot = state.slot_for(semantic.arg0);
  const auto representation = sem_ir.typeRepresentation(local_type);
  const auto transfers_temporary =
      state.expression_category(InstId(semantic.arg1)) ==
      SemExprCategory::Temporary;
  const auto &initializer = sem_ir.inst(semantic.arg1);
  if ((initializer.kind == SemInstKind::Call ||
       initializer.kind == SemInstKind::ForeignOperationCall) &&
      state.is_infallible_constructor(FunctionRefId(initializer.arg0),
                                      local_type) &&
      representation.init_repr == InitReprKind::InPlace) {
    std::vector<LowInstId> arguments;
    for (const auto argument : sem_ir.instBlock(InstBlockId(initializer.arg1)))
      arguments.push_back(state.value_for(argument));
    const auto plan = state.session.low_ir.addConstructPlan(
        {state.root_place_for(slot), FunctionRefId(initializer.arg0),
         state.add_value_block(arguments)});
    (void)state.emit_construct(current, semantic.type, semantic_id, plan);
    return;
  }
  if (representation.init_repr == InitReprKind::ByConversion)
    (void)state.emit_initialize_from_value(
        current, semantic.type, semantic_id, slot,
        state.value_for(semantic.arg1));
  else if (representation.ownership == OwnershipReprKind::Owned &&
           transfers_temporary)
    (void)state.emit_transfer(current, semantic.type, semantic_id, slot,
                              state.value_for(semantic.arg1));
  else
    (void)state.emit_initialize(current, semantic.type, semantic_id, slot,
                                state.value_for(semantic.arg1));
}

void LoweringBindingService::declareUninitialized(
    InstId semantic_id, SemDeclareUninitialized, LowBlockId current,
    LoweringBindingState &state) {
  state.values[semantic_id.index] = state.emit_void(
      current, state.session.sem_ir.voidType(), semantic_id);
}

void LoweringBindingService::outPlace(InstId semantic_id, SemOutPlace semantic,
                                      LowBlockId current,
                                      LoweringBindingState &state) {
  state.values[semantic_id.index] =
      state.emit_borrow(current, semantic.type, semantic_id,
                        state.slot_for(semantic.arg0));
}

void LoweringBindingService::initializePlace(
    InstId semantic_id, SemInitializePlace semantic, LowBlockId current,
    LoweringBindingState &state) {
  state.values[semantic_id.index] =
      state.emit_borrow(current, semantic.type, semantic_id,
                        state.slot_for(semantic.arg0));
}

void LoweringBindingService::assign(InstId semantic_id, SemAssign semantic,
                                    LowBlockId &current,
                                    LoweringBindingState &state) {
  const auto &sem_ir = state.session.sem_ir;
  const auto &plan = sem_ir.placeStates().reinitialization(semantic_id);
  assert(plan.target.hasValue());
  const auto &indexed = sem_ir.inst(semantic.arg0);
  if (indexed.kind == SemInstKind::Index &&
      sem_ir.type(TypeId(sem_ir.inst(InstId(indexed.arg0)).type)).kind == SemTypeKind::Array &&
      sem_ir.inst(InstId(indexed.arg1)).kind != SemInstKind::IntegerLiteral) {
    auto base = sem_ir.placeStates().place(plan.target);
    base.projections.pop_back();
    base.type = TypeId(sem_ir.inst(InstId(indexed.arg0)).type);
    const std::array operands{state.value_for(InstId(indexed.arg1)), state.value_for(semantic.arg1)};
    (void)state.emit_index_store(current, semantic.type, semantic_id, state.add_value_block(operands), base);
    return;
  }
  current = state.emit_cleanup_actions(current, semantic_id,
                                       plan.old_value_cleanups);
  const auto &source = sem_ir.inst(semantic.arg1);
  const auto destination = state.place_for(plan.target);
  if ((source.kind == SemInstKind::Call ||
       source.kind == SemInstKind::ForeignOperationCall) &&
      state.is_constructor(FunctionRefId(source.arg0)) &&
      sem_ir.typeRepresentation(TypeId(source.type)).init_repr ==
          InitReprKind::InPlace &&
      sem_ir.placeStates().place(plan.target).type == TypeId(source.type) &&
      (state.session.low_ir.place(destination).flags & LowPlaceAddressable) !=
          0) {
    std::vector<LowInstId> arguments;
    for (const auto argument : sem_ir.instBlock(InstBlockId(source.arg1)))
      arguments.push_back(state.value_for(argument));
    const auto construct_plan = state.session.low_ir.addConstructPlan(
        {destination, FunctionRefId(source.arg0),
         state.add_value_block(arguments)});
    (void)state.emit_construct(current, semantic.type, semantic_id,
                               construct_plan);
    return;
  }
  const auto &target_inst = sem_ir.inst(semantic.arg0);
  if (target_inst.kind == SemInstKind::StructFieldAccess) {
    const auto base_type =
        TypeId(sem_ir.inst(InstId(target_inst.arg0)).type);
    const auto field = static_cast<std::uint32_t>(
        sem_ir.integer(IntegerId(target_inst.arg1)));
    const auto &projection =
        state.session.low_ir.typeRepresentation(base_type)
            .field_projections[field];
    if (projection.kind != ObjectFieldProjectionKind::StableAddress) {
      const std::array operands{state.value_for(InstId(target_inst.arg0)),
                                state.value_for(semantic.arg1)};
      auto before = PlaceStateKind::Initialized;
      for (const auto &observation : sem_ir.placeStates().observations())
        if (observation.instruction == semantic_id &&
            observation.kind == PlaceObservationKind::Reinitialize) {
          before = observation.before;
          break;
        }
      const auto operand_block = state.add_value_block(operands);
      if (before == PlaceStateKind::Initialized &&
          plan.old_value_cleanups.empty())
        (void)state.emit_projection_store(current, semantic.type, semantic_id,
                                          operand_block, FieldIndex(field));
      else
        (void)state.emit_projection_init(current, semantic.type, semantic_id,
                                         operand_block, FieldIndex(field));
      (void)state.emit_mark_initialized(
          current, sem_ir.voidType(), semantic_id, destination);
      return;
    }
  }
  const auto target_type = sem_ir.placeStates().place(plan.target).type;
  if (sem_ir.typeRepresentation(target_type).init_repr ==
      InitReprKind::ByConversion)
    (void)state.emit_initialize_place_from_value(
        current, semantic.type, semantic_id, destination,
        state.value_for(semantic.arg1));
  else
    (void)state.emit_initialize_place(current, semantic.type, semantic_id,
                                      destination,
                                      state.value_for(semantic.arg1));
}

void LoweringBindingService::placement(InstId semantic_id,
                                       SemPlacement semantic,
                                       LowBlockId &current,
                                       LoweringBindingState &state) {
  const auto &sem_ir = state.session.sem_ir;
  const auto &reinitialization =
      sem_ir.placeStates().reinitialization(semantic_id);
  assert(reinitialization.target.hasValue());
  current = state.emit_cleanup_actions(
      current, semantic_id, reinitialization.old_value_cleanups);
  const auto &call = sem_ir.getAs<SemCall>(semantic.arg1);
  const auto destination = state.place_for(reinitialization.target);
  std::vector<LowInstId> arguments;
  for (const auto argument : sem_ir.instBlock(call.arg1))
    arguments.push_back(state.value_for(argument));
  const auto construct_plan = state.session.low_ir.addConstructPlan(
      {destination, call.arg0, state.add_value_block(arguments)});
  const auto result =
      state.emit_construct(current, semantic.type, semantic_id, construct_plan);
  if (TypeId(semantic.type) != sem_ir.voidType())
    state.values[semantic_id.index] = result;
}

} // namespace chtholly::compiler::internal
