#include "LowerToLowIRInternal.h"

#include <array>
#include <cassert>
#include <ranges>

namespace chtholly::compiler::internal {
namespace {

template <typename InstT>
LowInstId emitPlace(LoweringPlaceState &state, LowBlockId block, InstT inst,
                   InstId origin) {
  const auto id = state.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.low_ir.blockCount()].push_back(id);
  return id;
}

} // namespace

void LoweringPlaceService::nameRef(InstId semantic_id, SemNameRef semantic,
                                   LowBlockId current,
                                   LoweringPlaceState &state) {
  if (semantic.type == state.sem_ir.voidType()) {
    state.values[semantic_id.index] = emitPlace(
        state, current, LowVoidValue{semantic.type, {}}, semantic_id);
    return;
  }
  const auto slot = state.slot_for(semantic.arg0);
  if (state.has_conversion(semantic.type)) {
    const auto object = emitPlace(
        state, current, LowObjectAddress{semantic.type, slot, {}}, semantic_id);
    state.values[semantic_id.index] =
        state.pack_value(current, semantic.type, semantic_id, object);
  } else {
    state.values[semantic_id.index] = emitPlace(
        state, current, LowLoad{semantic.type, slot}, semantic_id);
  }
}

void LoweringPlaceService::borrowLocal(InstId semantic_id,
                                       SemBorrowLocal semantic,
                                       LowBlockId current,
                                       LoweringPlaceState &state) {
  state.values[semantic_id.index] = emitPlace(
      state, current, LowBorrow{semantic.type, state.slot_for(semantic.arg0)},
      semantic_id);
}

void LoweringPlaceService::borrowPlace(InstId semantic_id,
                                       SemBorrowPlace semantic,
                                       LowBlockId current,
                                       LoweringPlaceState &state) {
  for (const auto &observation : state.sem_ir.placeStates().observations()) {
    if (observation.instruction != semantic_id ||
        observation.kind != PlaceObservationKind::Borrow)
      continue;
    const auto place = state.place_for(observation.place);
    if ((state.low_ir.place(place).flags & LowPlaceAddressable) != 0) {
      state.values[semantic_id.index] = emitPlace(
          state, current, LowBorrowPlace{semantic.type, place, {}}, semantic_id);
      return;
    }
    break;
  }
  const auto &source = state.sem_ir.inst(semantic.arg0);
  if (source.kind == SemInstKind::MaterializeTemporary) {
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowBorrow{semantic.type, state.slot_for(LocalId(source.arg0))},
        semantic_id);
    return;
  }
  state.values[semantic_id.index] = state.value_for(semantic.arg0);
}

void LoweringPlaceService::carrierView(InstId semantic_id,
                                       SemCarrierView semantic,
                                       LowBlockId current,
                                       LoweringPlaceState &state) {
  assert(state.current_function.hasValue());
  const auto &source = state.sem_ir.inst(semantic.arg0);
  assert(source.kind == SemInstKind::NameRef);
  const auto parameters = state.sem_ir.localBlock(
      state.sem_ir.function(state.current_function).parameters);
  const auto found = std::ranges::find(parameters, LocalId(source.arg0));
  assert(found != parameters.end());
  state.values[semantic_id.index] = emitPlace(
      state, current,
      LowCarrierView{semantic.type, state.value_for(semantic.arg0),
                     FieldIndex(static_cast<std::uint32_t>(
                         state.sem_ir.integer(semantic.arg1)))},
      semantic_id);
}

void LoweringPlaceService::dereference(InstId semantic_id,
                                       SemDereference semantic,
                                       LowBlockId current,
                                       LoweringPlaceState &state) {
  if (state.is_representation_object_type(semantic.type))
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowDereferenceObject{semantic.type, state.value_for(semantic.arg0), {}},
        semantic_id);
  else if (state.has_conversion(semantic.type))
    state.values[semantic_id.index] = state.pack_value(
        current, semantic.type, semantic_id, state.value_for(semantic.arg0));
  else
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowDereference{semantic.type, state.value_for(semantic.arg0), {}},
        semantic_id);
}

void LoweringPlaceService::memberAccess(InstId semantic_id,
                                        SemMemberAccess semantic,
                                        LowBlockId current,
                                        LoweringPlaceState &state) {
  const auto base_type = TypeId(state.sem_ir.inst(semantic.arg0).type);
  state.values[semantic_id.index] =
      state.sem_ir.type(base_type).kind == SemTypeKind::Slice
          ? emitPlace(state, current,
                      LowSliceLength{semantic.type,
                                     state.value_for(semantic.arg0), {}},
                      semantic_id)
          : emitPlace(state, current,
                      LowStringLength{semantic.type,
                                      state.value_for(semantic.arg0), {}},
                      semantic_id);
}

void LoweringPlaceService::structFieldAccess(
    InstId semantic_id, SemStructFieldAccess semantic, LowBlockId current,
    LoweringPlaceState &state) {
  const auto base_type = TypeId(state.sem_ir.inst(semantic.arg0).type);
  const auto semantic_base = state.value_for(semantic.arg0);
  if (state.sem_ir.type(base_type).kind == SemTypeKind::Tuple &&
      !state.borrow_values.contains(semantic_id.index)) {
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowTupleElement{semantic.type, semantic_base, semantic.arg1}, semantic_id);
    return;
  }
  const auto base =
      state.is_representation_object_type(base_type) &&
              state.low_ir.inst(semantic_base).kind ==
                  LowInstKind::DereferenceObject
          ? semantic_base
          : state.unpack_value(current, base_type, semantic_id, semantic_base);
  const auto field = static_cast<std::uint32_t>(
      state.sem_ir.integer(IntegerId(semantic.arg1)));
  const auto &projection =
      state.low_ir.typeRepresentation(base_type).field_projections[field];
  if (const auto borrow = state.borrow_values.find(semantic_id.index);
      borrow != state.borrow_values.end()) {
    state.values[semantic_id.index] =
        state.sem_ir.referenceMutability(borrow->second) ==
                SemReferenceMutability::Mutable
            ? emitPlace(state, current,
                        LowProjectionBorrowMut{borrow->second, base,
                                               FieldIndex(field)},
                        semantic_id)
            : emitPlace(state, current,
                        LowProjectionBorrow{borrow->second, base,
                                             FieldIndex(field)},
                        semantic_id);
  } else if (projection.kind == ObjectFieldProjectionKind::StableAddress) {
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowStructField{semantic.type, base, semantic.arg1}, semantic_id);
  } else if (state.take_values.contains(semantic_id.index)) {
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowProjectionTake{semantic.type, base, FieldIndex(field)}, semantic_id);
  } else {
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowProjectionLoad{semantic.type, base, FieldIndex(field)}, semantic_id);
  }
}

void LoweringPlaceService::unionFieldAccess(InstId semantic_id,
                                            SemUnionFieldAccess semantic,
                                            LowBlockId current,
                                            LoweringPlaceState &state) {
  state.values[semantic_id.index] = emitPlace(
      state, current,
      LowUnionField{semantic.type, state.value_for(semantic.arg0), semantic.arg1},
      semantic_id);
}

void LoweringPlaceService::index(InstId semantic_id, SemIndex semantic,
                                 LowBlockId current,
                                 LoweringPlaceState &state) {
  if (const auto borrow = state.borrow_values.find(semantic_id.index);
      borrow != state.borrow_values.end()) {
    const auto base_type = TypeId(state.sem_ir.inst(semantic.arg0).type);
    if (state.sem_ir.type(base_type).kind == SemTypeKind::Slice) {
      state.values[semantic_id.index] = emitPlace(
          state, current,
          LowArrayIndex{borrow->second, state.value_for(semantic.arg0),
                        state.value_for(semantic.arg1)},
          semantic_id);
      return;
    }
    if (state.sem_ir.type(base_type).kind == SemTypeKind::Array) {
      const auto place = state.borrow_places.find(semantic_id.index);
      if (place != state.borrow_places.end()) {
        auto base_place = state.sem_ir.placeStates().place(place->second);
        if (!base_place.projections.empty() &&
            base_place.projections.back().kind == PlaceProjectionKind::AnyElement) {
          base_place.projections.pop_back();
          base_place.type = base_type;
          state.values[semantic_id.index] = emitPlace(
              state, current,
              LowDynamicIndexBorrow{borrow->second,
                                    state.lower_place(base_place),
                                    state.value_for(semantic.arg1)},
              semantic_id);
          return;
        }
      }
    }
  }
  state.values[semantic_id.index] = emitPlace(
      state, current,
      LowArrayIndex{semantic.type, state.value_for(semantic.arg0),
                    state.value_for(semantic.arg1)},
      semantic_id);
}

void LoweringPlaceService::move(InstId semantic_id, SemMove semantic,
                                LowBlockId current,
                                LoweringPlaceState &state) {
  const auto place = state.sem_ir.placeStates().movePlace(semantic_id);
  assert(place.hasValue());
  const auto &operand = state.sem_ir.inst(semantic.arg0);
  if (operand.kind == SemInstKind::StructFieldAccess) {
    const auto base_type =
        TypeId(state.sem_ir.inst(InstId(operand.arg0)).type);
    const auto field = static_cast<std::uint32_t>(
        state.sem_ir.integer(IntegerId(operand.arg1)));
    const auto &projection =
        state.low_ir.typeRepresentation(base_type).field_projections[field];
    if (projection.kind != ObjectFieldProjectionKind::StableAddress) {
      state.values[semantic_id.index] = state.value_for(semantic.arg0);
      (void)emitPlace(state, current,
                      LowMarkMoved{state.sem_ir.voidType(),
                                   state.place_for(place), {}},
                      semantic_id);
      return;
    }
  }
  const auto &object_representation =
      state.low_ir.typeRepresentation(semantic.type);
  if (object_representation.object_move_init_target.hasValue()) {
    const auto source = state.value_for(semantic.arg0);
    const auto object = emitPlace(
        state, current, LowMakeObjectMove{semantic.type, source, {}}, semantic_id);
    const auto &nominal = state.sem_ir.nominalType(
        NominalTypeId(state.sem_ir.type(semantic.type).arg0));
    for (std::uint32_t field = 0; field < nominal.fields.size(); ++field) {
      const auto field_type = state.sem_ir.nominalFieldType(semantic.type, field);
      const auto taken = emitPlace(
          state, current,
          LowProjectionTake{field_type, source, FieldIndex(field)}, semantic_id);
      const std::array operands{object, taken};
      (void)emitPlace(
          state, current,
          LowProjectionInit{state.sem_ir.voidType(),
                            state.low_ir.addValueBlock(operands),
                            FieldIndex(field)},
          semantic_id);
    }
    state.values[semantic_id.index] = emitPlace(
        state, current,
        LowMoveOut{semantic.type, object, state.place_for(place)}, semantic_id);
    return;
  }
  state.values[semantic_id.index] = emitPlace(
      state, current,
      LowMoveOut{semantic.type, state.value_for(semantic.arg0),
                 state.place_for(place)},
      semantic_id);
}

void LoweringPlaceService::copy(InstId semantic_id, SemCopy semantic,
                                LowBlockId current,
                                LoweringPlaceState &state) {
  const auto copied_place = state.sem_ir.placeStates().copyPlace(semantic_id);
  const auto source =
      copied_place.hasValue()
          ? emitPlace(state, current,
                      LowLoadPlace{semantic.type,
                                   state.place_for(copied_place), {}},
                      semantic_id)
          : state.value_for(semantic.arg0);
  if (state.sem_ir.typeRepresentation(semantic.type).copy ==
      CopyReprKind::Custom) {
    const auto target =
        state.lifecycle_target(semantic.type, SemCanonicalFunctionRole::Copy);
    assert(target.hasValue());
    const auto source_object =
        state.unpack_value(current, semantic.type, semantic_id, source);
    const auto object = emitPlace(
        state, current,
        LowLifecycleCopy{semantic.type, target, source_object}, semantic_id);
    state.values[semantic_id.index] = state.pack_value(
        current, semantic.type, semantic_id, object);
  } else if (state.low_ir.typeRepresentation(semantic.type)
                 .object_copy_init_target.hasValue()) {
    const auto object = emitPlace(
        state, current, LowMakeObjectCopy{semantic.type, source, {}}, semantic_id);
    const auto &nominal = state.sem_ir.nominalType(
        NominalTypeId(state.sem_ir.type(semantic.type).arg0));
    for (std::uint32_t field = 0; field < nominal.fields.size(); ++field) {
      const auto field_type = state.sem_ir.nominalFieldType(semantic.type, field);
      const auto loaded = emitPlace(
          state, current,
          LowProjectionLoad{field_type, source, FieldIndex(field)}, semantic_id);
      const std::array operands{object, loaded};
      (void)emitPlace(
          state, current,
          LowProjectionInit{state.sem_ir.voidType(),
                            state.low_ir.addValueBlock(operands),
                            FieldIndex(field)},
          semantic_id);
    }
    state.values[semantic_id.index] = object;
  } else {
    state.values[semantic_id.index] = emitPlace(
        state, current, LowCopyValue{semantic.type, source, {}}, semantic_id);
  }
}

} // namespace chtholly::compiler::internal
