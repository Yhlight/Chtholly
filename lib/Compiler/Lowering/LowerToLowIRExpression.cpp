#include "LowerToLowIRInternal.h"

#include <array>
#include <cassert>

namespace chtholly::compiler::internal {
namespace {

template <typename InstT>
LowInstId emit(LoweringExpressionState &state, LowBlockId block, InstT inst,
               InstId origin) {
  const auto id = state.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.low_ir.blockCount()].push_back(id);
  return id;
}

} // namespace

bool LoweringExpressionService::isConstructor(const SemIR &sem_ir,
                                              FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue())
    return sem_ir.functionSemanticContract(reference.local_function).role ==
           CallableSemanticRole::Constructor;
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity &&
         entity->semantic_contract.role == CallableSemanticRole::Constructor;
}

bool LoweringExpressionService::isInfallibleConstructorFor(
    const SemIR &sem_ir, FunctionRefId target, TypeId destination) {
  if (!isConstructor(sem_ir, target))
    return false;
  const auto function_type = sem_ir.functionRef(target).local_type;
  if (!function_type.hasValue() ||
      sem_ir.type(function_type).kind != SemTypeKind::Function)
    return false;
  const auto return_type = TypeId(sem_ir.type(function_type).arg1);
  return return_type == destination &&
         !sem_ir.canonicalResultShape(return_type).has_value();
}

bool LoweringExpressionService::hasConversion(const SemIR &sem_ir,
                                              TypeId type) {
  return sem_ir.typeRepresentation(type).init_repr == InitReprKind::ByConversion;
}

bool LoweringExpressionService::isRepresentationObjectType(
    const SemIR &sem_ir, FunctionId current_function, TypeId type) {
  const auto &contract = sem_ir.functionSemanticContract(current_function);
  const auto &semantic_type = sem_ir.type(type);
  return contract.domain == CallableSemanticDomain::ValueRepresentation &&
         contract.owner.hasValue() && semantic_type.kind == SemTypeKind::Nominal &&
         NominalTypeId(semantic_type.arg0) == contract.owner;
}

bool LoweringExpressionService::isRepresentationPack(
    const SemIR &sem_ir, FunctionId current_function) {
  return sem_ir.functionSemanticContract(current_function).role ==
         SemCanonicalFunctionRole::Pack;
}

void LoweringExpressionService::integer(InstId semantic_id,
                                         SemIntegerLiteral semantic,
                                         LowBlockId current,
                                         LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowIntegerConstant{semantic.type, semantic.arg0},
           semantic_id);
}

void LoweringExpressionService::floating(InstId semantic_id,
                                          SemFloatLiteral semantic,
                                          LowBlockId current,
                                          LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowFloatConstant{semantic.type, semantic.arg0},
           semantic_id);
}

void LoweringExpressionService::boolean(InstId semantic_id,
                                         SemBoolLiteral semantic,
                                         LowBlockId current,
                                         LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowBoolConstant{semantic.type, semantic.arg0},
           semantic_id);
}

void LoweringExpressionService::character(
    InstId semantic_id, SemCharLiteral semantic, LowBlockId current,
    LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowIntegerConstant{semantic.type, semantic.arg0},
           semantic_id);
}

void LoweringExpressionService::voidValue(InstId semantic_id,
                                           SemVoidValue semantic,
                                           LowBlockId current,
                                           LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowVoidValue{semantic.type, {}}, semantic_id);
}

void LoweringExpressionService::string(InstId semantic_id,
                                        SemStringLiteral semantic,
                                        LowBlockId current,
                                        LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowStringConstant{semantic.type, semantic.arg0},
           semantic_id);
}

void LoweringExpressionService::nullPointer(InstId semantic_id,
                                             SemNullPointer semantic,
                                             LowBlockId current,
                                             LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current, LowNullPointer{semantic.type, {}}, semantic_id);
}

void LoweringExpressionService::collectWriteOnlyValues(
    InstBlockId block_id, LoweringExpressionState &state) {
  const auto mark_write_only = [&](auto &&self, InstId id) -> void {
    const auto &instruction = state.sem_ir.inst(id);
    if (instruction.kind == SemInstKind::StructFieldAccess ||
        instruction.kind == SemInstKind::EnumPayloadAccess) {
      state.write_only_values.insert(id.index);
      return;
    }
    if (!state.write_only_values.insert(id.index).second)
      return;
    if (instruction.kind == SemInstKind::StructFieldAccess ||
        instruction.kind == SemInstKind::EnumPayloadAccess ||
        instruction.kind == SemInstKind::Dereference ||
        instruction.kind == SemInstKind::MemberAccess ||
        instruction.kind == SemInstKind::Move ||
        instruction.kind == SemInstKind::Copy) {
      self(self, InstId(instruction.arg0));
    } else if (instruction.kind == SemInstKind::Index) {
      const auto &index = state.sem_ir.inst(InstId(instruction.arg1));
      if (index.kind == SemInstKind::IntegerLiteral) {
        self(self, InstId(instruction.arg0));
        self(self, InstId(instruction.arg1));
      }
      // A dynamic store still evaluates its base and index values.
    }
  };
  const auto collect = [&](auto &&self, InstBlockId block_id) -> void {
    for (const auto id : state.sem_ir.instBlock(block_id)) {
      const auto &instruction = state.sem_ir.inst(id);
      if (instruction.kind == SemInstKind::Assign)
        mark_write_only(mark_write_only, InstId(instruction.arg0));
      else if (instruction.kind == SemInstKind::Placement)
        mark_write_only(mark_write_only, InstId(instruction.arg0));
      else if (instruction.kind == SemInstKind::Move)
        state.take_values.insert(instruction.arg0);
      else if (instruction.kind == SemInstKind::BorrowPlace) {
        state.borrow_values.insert_or_assign(instruction.arg0,
                                              TypeId(instruction.type));
        for (const auto &observation :
             state.sem_ir.placeStates().observations())
          if (observation.instruction == id &&
              observation.kind == PlaceObservationKind::Borrow) {
            state.borrow_places.insert_or_assign(instruction.arg0,
                                                 observation.place);
            break;
          }
      } else if (instruction.kind == SemInstKind::If) {
        for (const auto arm_id :
             state.sem_ir.instBlock(InstBlockId(instruction.arg1))) {
          const auto &arm = state.sem_ir.inst(arm_id);
          if (arm.kind == SemInstKind::IfArm)
            self(self, InstBlockId(arm.arg0));
        }
      } else if (instruction.kind == SemInstKind::While) {
        self(self, InstBlockId(instruction.arg0));
        self(self, InstBlockId(instruction.arg1));
      } else if (instruction.kind == SemInstKind::For) {
        for (const auto clause_id :
             state.sem_ir.instBlock(InstBlockId(instruction.arg0))) {
          const auto &clause = state.sem_ir.inst(clause_id);
          if (clause.kind == SemInstKind::ForClause)
            self(self, InstBlockId(clause.arg1));
        }
        self(self, InstBlockId(instruction.arg1));
      } else if (instruction.kind == SemInstKind::DoWhile) {
        self(self, InstBlockId(instruction.arg0));
        self(self, InstBlockId(instruction.arg1));
      } else if (instruction.kind == SemInstKind::Switch) {
        for (const auto arm_id :
             state.sem_ir.instBlock(InstBlockId(instruction.arg1))) {
          const auto &arm = state.sem_ir.inst(arm_id);
          if (arm.kind == SemInstKind::SwitchArm)
            self(self, InstBlockId(arm.arg1));
        }
      } else if (instruction.kind == SemInstKind::ScopedBlock ||
                 instruction.kind == SemInstKind::Defer ||
                 instruction.kind == SemInstKind::CoroutineTaskScope) {
        self(self, InstBlockId(instruction.arg0));
      }
    }
  };
  collect(collect, block_id);
}

LowInstId LoweringExpressionService::lowerConstant(
    ConstantId constant, InstId semantic_id, LowBlockId current,
    LoweringExpressionState &state,
    const std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)> &
        pack_value) {
  const auto emit = [&](auto inst) {
    const auto id = state.low_ir.addInst(inst, semantic_id);
    state.pending_blocks[current.index - state.low_ir.blockCount()].push_back(id);
    return id;
  };
  const auto &value = state.sem_ir.constantValue(constant);
  switch (value.kind) {
  case ConstantValueKind::Integer:
  case ConstantValueKind::ForeignEnum:
    return emit(LowIntegerConstant{
        value.type, IntegerId(static_cast<std::uint32_t>(value.payload))});
  case ConstantValueKind::Float:
    return emit(LowFloatConstant{
        value.type, IntegerId(static_cast<std::uint32_t>(value.payload))});
  case ConstantValueKind::Bool:
    return emit(LowBoolConstant{
        value.type, IntegerId(static_cast<std::uint32_t>(value.payload))});
  case ConstantValueKind::String:
    return emit(LowStringConstant{
        value.type, StringLiteralId(static_cast<std::uint32_t>(value.payload))});
  case ConstantValueKind::Null:
    return emit(LowNullPointer{value.type, {}});
  case ConstantValueKind::Array: {
    std::vector<LowInstId> elements;
    for (const auto element : state.sem_ir.constantBlock(value.elements))
      elements.push_back(lowerConstant(element, semantic_id, current, state,
                                        pack_value));
    return emit(LowMakeArray{value.type, state.low_ir.addValueBlock(elements), {}});
  }
  case ConstantValueKind::Tuple: {
    std::vector<LowInstId> elements;
    for (const auto element : state.sem_ir.constantBlock(value.elements))
      elements.push_back(lowerConstant(element, semantic_id, current, state,
                                        pack_value));
    return emit(LowMakeTuple{value.type, state.low_ir.addValueBlock(elements), {}});
  }
  case ConstantValueKind::Aggregate: {
    const auto object = emit(LowMakeObject{value.type, {}, {}});
    std::uint32_t field = 0;
    for (const auto element : state.sem_ir.constantBlock(value.elements)) {
      const std::array operands{
          object, lowerConstant(element, semantic_id, current, state, pack_value)};
      const auto id = state.low_ir.addInst(
          LowProjectionInit{state.sem_ir.voidType(),
                            state.low_ir.addValueBlock(operands),
                            FieldIndex(field++)},
          semantic_id);
      state.pending_blocks[current.index - state.low_ir.blockCount()].push_back(id);
    }
    return pack_value(current, value.type, semantic_id, object);
  }
  case ConstantValueKind::Union: {
    const auto elements = state.sem_ir.constantBlock(value.elements);
    assert(elements.size() == 1);
    return emit(LowMakeUnion{
        value.type,
        lowerConstant(elements.front(), semantic_id, current, state, pack_value),
        IntegerId(static_cast<std::uint32_t>(value.payload))});
  }
  case ConstantValueKind::Enum: {
    std::vector<LowInstId> elements;
    for (const auto element : state.sem_ir.constantBlock(value.elements))
      elements.push_back(lowerConstant(element, semantic_id, current, state,
                                        pack_value));
    return emit(LowMakeEnum{
        value.type, state.low_ir.addValueBlock(elements),
        IntegerId(static_cast<std::uint32_t>(value.payload))});
  }
  }
  assert(false && "unknown constant value kind");
  return LowInstId::invalid();
}

namespace {

template <typename InstT>
LowInstId emitAggregate(LoweringAggregateState &state, LowBlockId block,
                        InstT inst, InstId origin) {
  const auto id = state.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.low_ir.blockCount()].push_back(id);
  return id;
}

} // namespace

void LoweringAggregateService::arrayLiteral(
    InstId semantic_id, SemArrayLiteral semantic, LowBlockId current,
    LoweringAggregateState &state) {
  std::vector<LowInstId> elements;
  for (const auto element : state.sem_ir.instBlock(semantic.arg0))
    elements.push_back(state.value_for(element));
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowMakeArray{semantic.type, state.low_ir.addValueBlock(elements), {}},
      semantic_id);
}

void LoweringAggregateService::tupleLiteral(
    InstId semantic_id, SemTupleLiteral semantic, LowBlockId current,
    LoweringAggregateState &state) {
  std::vector<LowInstId> elements;
  for (const auto element : state.sem_ir.instBlock(semantic.arg0))
    elements.push_back(state.value_for(element));
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowMakeTuple{semantic.type, state.low_ir.addValueBlock(elements), {}},
      semantic_id);
}

void LoweringAggregateService::slice(InstId semantic_id, SemSlice semantic,
                                     LowBlockId current,
                                     LoweringAggregateState &state) {
  std::vector<LowInstId> operands;
  for (const auto operand : state.sem_ir.instBlock(semantic.arg0))
    operands.push_back(state.value_for(operand));
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowMakeSlice{semantic.type, state.low_ir.addValueBlock(operands), {}},
      semantic_id);
}

void LoweringAggregateService::aggregateInit(
    InstId semantic_id, SemAggregateInit semantic, LowBlockId current,
    LoweringAggregateState &state) {
  const auto object = emitAggregate(
      state, current, LowMakeObject{semantic.type, {}, {}}, semantic_id);
  std::uint32_t field_index = 0;
  for (const auto field : state.sem_ir.instBlock(semantic.arg0)) {
    const std::array operands{object, state.value_for(field)};
    (void)emitAggregate(
        state, current,
        LowProjectionInit{state.sem_ir.voidType(),
                          state.low_ir.addValueBlock(operands),
                          FieldIndex(field_index++)},
        semantic_id);
  }
  state.values[semantic_id.index] = state.pack_value(
      current, semantic.type, semantic_id, object);
}

void LoweringAggregateService::closure(InstId semantic_id, SemClosure semantic,
                                       LowBlockId current,
                                       LoweringAggregateState &state) {
  const auto object = emitAggregate(
      state, current, LowMakeObject{semantic.type, {}, {}}, semantic_id);
  std::uint32_t field_index = 0;
  for (const auto capture : state.sem_ir.instBlock(semantic.arg1)) {
    const std::array operands{object, state.value_for(capture)};
    (void)emitAggregate(
        state, current,
        LowProjectionInit{state.sem_ir.voidType(),
                          state.low_ir.addValueBlock(operands),
                          FieldIndex(field_index++)},
        semantic_id);
  }
  state.values[semantic_id.index] = state.pack_value(
      current, semantic.type, semantic_id, object);
}

void LoweringAggregateService::boundMethod(
    InstId semantic_id, SemBoundMethod semantic, LowBlockId current,
    LoweringAggregateState &state) {
  const auto object = emitAggregate(
      state, current, LowMakeObject{semantic.type, {}, {}}, semantic_id);
  const std::array operands{object, state.value_for(semantic.arg0)};
  (void)emitAggregate(
      state, current,
      LowProjectionInit{state.sem_ir.voidType(),
                        state.low_ir.addValueBlock(operands), FieldIndex(0)},
      semantic_id);
  state.values[semantic_id.index] = state.pack_value(
      current, semantic.type, semantic_id, object);
}

void LoweringAggregateService::unionInit(InstId semantic_id,
                                         SemUnionInit semantic,
                                         LowBlockId current,
                                         LoweringAggregateState &state) {
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowMakeUnion{semantic.type, state.value_for(semantic.arg0), semantic.arg1},
      semantic_id);
}

void LoweringAggregateService::enumInit(InstId semantic_id, SemEnumInit semantic,
                                       LowBlockId current,
                                       LoweringAggregateState &state) {
  std::vector<LowInstId> payload;
  for (const auto field : state.sem_ir.instBlock(semantic.arg0))
    payload.push_back(state.value_for(field));
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowMakeEnum{semantic.type, state.low_ir.addValueBlock(payload),
                  semantic.arg1},
      semantic_id);
}

void LoweringAggregateService::enumTag(InstId semantic_id, SemEnumTag semantic,
                                       LowBlockId current,
                                       LoweringAggregateState &state) {
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowEnumTag{semantic.type, state.value_for(semantic.arg0)}, semantic_id);
}

void LoweringAggregateService::enumPayloadAccess(
    InstId semantic_id, SemEnumPayloadAccess semantic, LowBlockId current,
    LoweringAggregateState &state) {
  state.values[semantic_id.index] = emitAggregate(
      state, current,
      LowEnumPayload{semantic.type, state.value_for(semantic.arg0),
                     semantic.arg1},
      semantic_id);
}

} // namespace chtholly::compiler::internal
