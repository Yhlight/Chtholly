#include "LowerToLowIRInternal.h"

#include <cassert>
#include <numeric>

namespace chtholly::compiler::internal {

SlotId LoweringOwnershipService::slotFor(LoweringSessionState &session,
                                         LocalId local,
                                         LoweringOwnershipState &state) {
  if (const auto found = state.slots.find(local.index);
      found != state.slots.end())
    return found->second;
  const auto &semantic = session.sem_ir.local(local);
  const auto slot = session.low_ir.addSlot(
      {semantic.type, local, semantic.flags, 0});
  state.slots.emplace(local.index, slot);
  state.function_slots.push_back(slot);
  return slot;
}

LowPlaceId LoweringOwnershipService::rootPlaceFor(
    LoweringSessionState &session, SlotId slot, LoweringOwnershipState &state) {
  if (const auto found = state.root_places.find(slot.index);
      found != state.root_places.end())
    return found->second;
  const auto empty = session.low_ir.addPlaceProjectionBlock({});
  const auto place = session.low_ir.addPlace(
      {slot, empty, empty, session.low_ir.slot(slot).type,
       LowPlaceAddressable});
  state.root_places.emplace(slot.index, place);
  return place;
}

LowPlaceId LoweringOwnershipService::lowerPlace(
    const SemPlace &place, LoweringSessionState &session,
    LoweringOwnershipState &state,
    const std::function<const SemType &(TypeId)> &semantic_type) {
  std::vector<LowPlaceProjection> projections;
  std::vector<LowPlaceProjection> logical_projections;
  auto current_type = session.sem_ir.local(place.root).type;
  bool addressable = true;
  for (const auto projection : place.projections) {
    if (projection.kind == PlaceProjectionKind::CarrierView) {
      current_type = TypeId(projection.index);
    } else if (projection.kind == PlaceProjectionKind::Dereference) {
      logical_projections.push_back(
          {LowPlaceProjectionKind::Dereference, current_type, 0});
      if (addressable)
        projections.push_back(
            {LowPlaceProjectionKind::Dereference, current_type, 0});
      current_type = semantic_type(current_type).kind == SemTypeKind::Reference
                         ? session.sem_ir.referencePointee(current_type)
                         : session.sem_ir.rawPointerPointee(current_type);
    } else if (projection.kind == PlaceProjectionKind::Field) {
      const auto &representation = session.low_ir.typeRepresentation(current_type);
      assert(projection.index < representation.field_projections.size());
      const auto &field_projection =
          representation.field_projections[projection.index];
      logical_projections.push_back({LowPlaceProjectionKind::StructField,
                                     current_type, projection.index});
      if (addressable &&
          field_projection.kind == ObjectFieldProjectionKind::StableAddress) {
        for (const auto physical : field_projection.physical_steps)
          projections.push_back({LowPlaceProjectionKind::StructField,
                                 physical.aggregate_type,
                                 physical.field_index});
      } else {
        addressable = false;
      }
      current_type = session.sem_ir.nominalFieldType(current_type,
                                                      projection.index);
    } else if (projection.kind == PlaceProjectionKind::EnumPayload) {
      logical_projections.push_back({LowPlaceProjectionKind::EnumPayload,
                                     current_type, projection.index,
                                     projection.variant});
      if (addressable)
        projections.push_back({LowPlaceProjectionKind::EnumPayload,
                               current_type, projection.index,
                               projection.variant});
      current_type = session.sem_ir.enumPayloadFieldType(
          current_type, projection.variant, projection.index);
    } else if (projection.kind == PlaceProjectionKind::Element) {
      logical_projections.push_back({LowPlaceProjectionKind::ArrayElement,
                                     current_type, projection.index});
      if (addressable)
        projections.push_back({LowPlaceProjectionKind::ArrayElement,
                               current_type, projection.index});
      current_type = TypeId(semantic_type(current_type).arg0);
    } else {
      logical_projections.push_back(
          {LowPlaceProjectionKind::ArrayElement, current_type, 0});
      addressable = false;
      current_type = TypeId(semantic_type(current_type).arg0);
    }
  }
  const auto projection_block = session.low_ir.addPlaceProjectionBlock(projections);
  const auto logical_block =
      session.low_ir.addPlaceProjectionBlock(logical_projections);
  const auto slot = slotFor(session, place.root, state);
  return session.low_ir.addPlace({slot, projection_block, logical_block,
                                  place.type,
                                  addressable ? LowPlaceAddressable : 0U});
}

std::optional<LoweringOwnershipService::ComputedProjectionCleanup>
LoweringOwnershipService::computedProjectionCleanup(
    SemPlaceId semantic_place, LoweringSessionState &session,
    const std::function<const SemType &(TypeId)> &semantic_type,
    const std::function<LowPlaceId(const SemPlace &)> &lower_place) {
  const auto &place = session.sem_ir.placeStates().place(semantic_place);
  auto current_type = session.sem_ir.local(place.root).type;
  std::vector<PlaceProjection> prefix;
  for (const auto projection : place.projections) {
    if (projection.kind == PlaceProjectionKind::CarrierView) {
      prefix.push_back(projection);
      current_type = TypeId(projection.index);
      continue;
    }
    if (projection.kind == PlaceProjectionKind::Dereference) {
      prefix.push_back(projection);
      current_type = semantic_type(current_type).kind == SemTypeKind::Reference
                         ? session.sem_ir.referencePointee(current_type)
                         : session.sem_ir.rawPointerPointee(current_type);
      continue;
    }
    if (projection.kind == PlaceProjectionKind::Element) {
      prefix.push_back(projection);
      current_type = TypeId(semantic_type(current_type).arg0);
      continue;
    }
    if (projection.kind == PlaceProjectionKind::EnumPayload) {
      prefix.push_back(projection);
      current_type = session.sem_ir.enumPayloadFieldType(
          current_type, projection.variant, projection.index);
      continue;
    }
    const auto &field_projection =
        session.low_ir.typeRepresentation(current_type)
            .field_projections[projection.index];
    if (field_projection.kind != ObjectFieldProjectionKind::StableAddress) {
      SemPlace owner{place.root, current_type, prefix};
      return ComputedProjectionCleanup{lower_place(owner), current_type,
                                        projection.index};
    }
    prefix.push_back(projection);
    current_type =
        session.sem_ir.nominalFieldType(current_type, projection.index);
  }
  return std::nullopt;
}

FunctionRefId LoweringOwnershipService::lifecycleTarget(
    const LoweringSessionState &session, TypeId type,
    SemCanonicalFunctionRole role) {
  const auto &semantic_type = session.sem_ir.type(type);
  if (semantic_type.kind != SemTypeKind::Nominal)
    return FunctionRefId::invalid();
  const auto nominal = NominalTypeId(semantic_type.arg0);
  for (std::uint32_t index = 0; index < session.sem_ir.functionRefCount();
       ++index) {
    const auto id = FunctionRefId(index);
    const auto &reference = session.sem_ir.functionRef(id);
    if (role == SemCanonicalFunctionRole::Drop &&
        session.sem_ir.functionIntrinsicRole(id) == CompilerIntrinsicRole::VecDrop)
      return id;
    if (!reference.local_function.hasValue())
      continue;
    const auto &contract =
        session.sem_ir.functionSemanticContract(reference.local_function);
    if (contract.domain == CallableSemanticDomain::Lifecycle &&
        contract.owner == nominal && contract.role == role)
      return id;
  }
  const auto *witness = session.sem_ir.nominalSemanticWitness(type);
  const auto *target = witness && role == SemCanonicalFunctionRole::Copy &&
                               witness->copy_target
                           ? &*witness->copy_target
                       : witness && role == SemCanonicalFunctionRole::Drop &&
                               witness->destroy_target
                           ? &*witness->destroy_target
                           : nullptr;
  if (!target)
    return FunctionRefId::invalid();
  const auto canonical = session.sem_ir.importIRs().registry().findEntity(
      target->canonical_package, target->canonical_module,
      target->canonical_name, PublicEntityKind::Function,
      target->expected_fingerprint);
  const auto *entity = session.sem_ir.importIRs().tryGetEntity(canonical);
  if (!entity || entity->fingerprint != target->expected_fingerprint)
    return FunctionRefId::invalid();
  for (std::uint32_t index = 0; index < session.sem_ir.functionRefCount();
       ++index) {
    const auto id = FunctionRefId(index);
    if (session.sem_ir.functionRef(id).public_entity == canonical)
      return id;
  }
  return FunctionRefId::invalid();
}

LowBlockId LoweringCleanupService::tail(
    const PlaceCleanupPlan &plan, InstId origin, LowBlockId successor,
    LoweringSessionState &session,
    std::unordered_map<std::string, LowBlockId> &cleanup_tails,
    std::vector<std::vector<LowInstId>> &pending_blocks,
    const std::function<LowBlockId()> &new_block,
    const std::function<LowBlockId(LowBlockId, InstId,
                                   const PlaceCleanupPlan &)> &emit_cleanup) {
  if (plan.actions.empty())
    return successor;
  std::string key = std::to_string(successor.index);
  for (const auto &action : plan.actions) {
    key += ":" + std::to_string(action.place.index) + ":" +
           std::to_string(static_cast<unsigned>(action.kind)) + ":" +
           std::to_string(action.block.index) + ":" +
           std::to_string(action.local.index);
  }
  if (const auto found = cleanup_tails.find(key); found != cleanup_tails.end())
    return found->second;
  const auto tail_block = new_block();
  const auto cleanup_end = emit_cleanup(tail_block, origin, plan);
  const auto branch = session.low_ir.addInst(
      LowBranch{session.sem_ir.voidType(), successor, {}}, origin);
  // The callback-created block is always a pending block, as in the original
  // context. Keep this service independent of the context's emit template.
  pending_blocks[cleanup_end.index - session.low_ir.blockCount()].push_back(
      branch);
  cleanup_tails.emplace(std::move(key), tail_block);
  return tail_block;
}

SlotId LoweringCoroutineService::wakeSlotFor(
    const LoweringSessionState &session, LowInstId value) {
  std::unordered_set<std::uint32_t> seen;
  while (value.hasValue() && value.index < session.low_ir.instCount() &&
         seen.insert(value.index).second) {
    const auto &inst = session.low_ir.inst(value);
    if (inst.kind == LowInstKind::Load || inst.kind == LowInstKind::Borrow)
      return SlotId(inst.arg0);
    if (inst.kind == LowInstKind::MoveOut)
      return session.low_ir.place(LowPlaceId(inst.arg1)).root;
    if (inst.kind == LowInstKind::CopyValue ||
        inst.kind == LowInstKind::ForeignResourceUnwrap ||
        inst.kind == LowInstKind::Dereference)
      value = LowInstId(inst.arg0);
    else
      break;
  }
  return SlotId::invalid();
}

CoroutineTaskCreatePlanId LoweringCoroutineService::buildTaskCreatePlan(
    LoweringSessionState &session, FunctionRefId target,
    CoroutineTaskCreateMode mode, TypeId checked_type) {
  const auto &reference = session.sem_ir.functionRef(target);
  const auto constructor_entity =
      reference.local_function.hasValue()
          ? session.sem_ir.coroutineConstructorEntity(reference.local_function)
          : reference.public_entity;
  const auto *constructor =
      session.sem_ir.importIRs().tryGetEntity(constructor_entity);
  assert(constructor && constructor->coroutine_constructor.epoch == 1);
  std::vector<TypeId> parameter_types;
  for (const auto parameter : session.sem_ir.typeBlock(
           TypeBlockId(session.sem_ir.type(reference.local_type).arg0)))
    parameter_types.push_back(parameter);
  return session.low_ir.addCoroutineTaskCreatePlan(
      {.target = target,
       .scaffold = reference.local_function,
       .constructor_entity = constructor_entity,
       .constructor_abi_epoch = constructor->coroutine_constructor.epoch,
       .task_type = session.sem_ir.coroutineCheckedPayloadType(checked_type),
       .mode = mode,
       .parameter_types = std::move(parameter_types)});
}

CoroutineTaskCompletionSetPlanId LoweringCoroutineService::buildCompletionSetPlan(
    LoweringSessionState &session, FunctionId scaffold,
    std::span<const InstId> operands, TypeId set_type) {
  const auto count = static_cast<std::uint32_t>(operands.size());
  std::vector<InstId> ordered(operands.begin(), operands.end());
  return session.low_ir.addCoroutineTaskCompletionSetPlan(
      {.scaffold = scaffold,
       .set_type = set_type,
       .provider = session.low_ir.completionProviderFor(set_type),
       .operand_count = count,
       .bitmap_word_count = coroutineTaskCompletionBitmapWordCount(count),
       .ordered_operands = std::move(ordered)});
}

CoroutineTaskCompletionCombinePlanId
LoweringCoroutineService::buildCompletionCombinePlan(
    LoweringSessionState &session, FunctionId scaffold, InstId semantic_id,
    InstId set, CoroutineTaskCompletionCombineKind operation,
    TypeId result_type) {
  const auto set_type = TypeId(session.sem_ir.inst(set).type);
  const auto count = session.sem_ir.coroutineTaskCompletionCapacity(set_type);
  std::vector<std::uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0U);
  const auto winner = operation == CoroutineTaskCompletionCombineKind::WaitAll
                          ? CoroutineTaskCompletionWinnerPolicy::None
                          : CoroutineTaskCompletionWinnerPolicy::LowestCanonicalIndex;
  const auto losers = operation == CoroutineTaskCompletionCombineKind::WaitAll
                          ? CoroutineTaskCompletionLoserPolicy::ConsumeAll
                      : operation == CoroutineTaskCompletionCombineKind::Select
                          ? CoroutineTaskCompletionLoserPolicy::TransferRemaining
                          : CoroutineTaskCompletionLoserPolicy::ReleaseRemaining;
  return session.low_ir.addCoroutineTaskCompletionCombinePlan(
      {.scaffold = scaffold,
       .operation = operation,
       .set_type = set_type,
       .result_type = result_type,
       .provider = session.low_ir.completionProviderFor(set_type),
       .operand_count = count,
       .bitmap_word_count = coroutineTaskCompletionBitmapWordCount(count),
       .canonical_operand_order = std::move(order),
       .winner_policy = winner,
       .loser_policy = losers,
       .semantic_suspension = semantic_id});
}

} // namespace chtholly::compiler::internal
