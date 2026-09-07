#include "LLVMInternal.h"

#include <algorithm>
#include <array>
#include <cassert>

namespace chtholly::compiler {
namespace {

class ObjectInstructionEmitter {
public:
  explicit ObjectInstructionEmitter(LLVMObjectInstructionState &state)
      : state_(state) {}
  llvm::Value *lowerInst(LowStaticLoad inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadValueFromObject(state_.static_globals.at(inst.arg0.index), inst.type,
                               builder);
  }

  llvm::Value *lowerInst(LowMakeArray inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *array = entryAlloca(function, lowerObjectType(inst.type), "array");
    builder.CreateLifetimeStart(array);
    auto *zero = builder.getInt32(0);
    const auto &semantic_type = state_.sem_ir.type(inst.type);
    const auto element_type = TypeId(semantic_type.arg0);
    std::uint32_t index = 0;
    for (const auto element : state_.low_ir.valueBlock(inst.arg0)) {
      auto *address = builder.CreateInBoundsGEP(
          lowerObjectType(inst.type), array, {zero, builder.getInt32(index++)});
      storeValueToObject(address, value(element), element_type, builder);
    }
    if (usesPointerValueRepresentation(inst.type))
      return array;
    auto *result = builder.CreateLoad(lowerValueType(inst.type), array);
    builder.CreateLifetimeEnd(array);
    return result;
  }

  llvm::Value *lowerInst(LowMakeTuple inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *tuple = entryAlloca(function, lowerObjectType(inst.type), "tuple");
    builder.CreateLifetimeStart(tuple);
    const auto element_types =
        state_.sem_ir.typeBlock(TypeBlockId(state_.sem_ir.type(inst.type).arg0));
    std::uint32_t index = 0;
    for (const auto element : state_.low_ir.valueBlock(inst.arg0)) {
      auto *address =
          builder.CreateStructGEP(lowerObjectType(inst.type), tuple, index);
      storeValueToObject(address, value(element), element_types[index],
                         builder);
      ++index;
    }
    if (usesPointerValueRepresentation(inst.type))
      return tuple;
    auto *result = builder.CreateLoad(lowerValueType(inst.type), tuple);
    builder.CreateLifetimeEnd(tuple);
    return result;
  }

  void copyObject(llvm::Value *destination, llvm::Value *source, TypeId type,
                  llvm::IRBuilder<> &builder) {
    LLVMObjectValueLoweringService::copy(destination, source, type, builder,
                                         state_.object_value);
  }

  void moveSemanticObject(llvm::Value *destination, llvm::Value *source_address,
                          TypeId type, llvm::IRBuilder<> &builder) {
    LLVMObjectValueLoweringService::moveObject(
        destination, source_address, type, builder, state_.object_value);
  }

  void moveSemanticValueToObject(llvm::Value *destination, llvm::Value *source,
                                 TypeId type, llvm::IRBuilder<> &builder) {
    LLVMObjectValueLoweringService::moveValue(
        destination, source, type, builder, state_.object_value);
  }

  llvm::Value *loadValueFromObject(llvm::Value *address, TypeId type,
                                   llvm::IRBuilder<> &builder) {
    return LLVMObjectValueLoweringService::load(address, type, builder,
                                                state_.object_value);
  }

  void storeValueToObject(llvm::Value *address, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) {
    LLVMObjectValueLoweringService::store(address, source, type, builder,
                                          state_.object_value);
  }

  llvm::Value *lowerInst(LowMakeAggregate inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *record =
        entryAlloca(function, lowerObjectType(inst.type), "aggregate");
    builder.CreateLifetimeStart(record);
    std::uint32_t index = 0;
    for (const auto value_id : state_.low_ir.valueBlock(inst.arg0)) {
      llvm::Value *field = record;
      const auto &projections =
          state_.low_ir.typeRepresentation(inst.type).field_projections;
      assert(index < projections.size());
      for (const auto projection : projections[index].physical_steps)
        field =
            builder.CreateStructGEP(lowerObjectType(projection.aggregate_type),
                                    field, projection.field_index);
      const auto field_type = TypeId(state_.low_ir.inst(value_id).type);
      storeValueToObject(field, value(value_id), field_type, builder);
      ++index;
    }
    return record;
  }

  llvm::Value *lowerInst(LowMakeUnion inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *storage = entryAlloca(function, lowerObjectType(inst.type), "union");
    builder.CreateLifetimeStart(storage);
    const auto encoded = state_.sem_ir.integer(inst.arg1);
    assert(encoded >= 0);
    const auto member = static_cast<std::uint32_t>(encoded);
    const auto member_type = state_.sem_ir.nominalFieldType(inst.type, member);
    storeValueToObject(storage, value(inst.arg0), member_type, builder);
    return storage;
  }

  llvm::Value *lowerInst(LowMakeEnum inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *storage =
        entryAlloca(function, lowerObjectType(inst.type), "enum.value");
    builder.CreateLifetimeStart(storage);
    auto *record = llvm::cast<llvm::StructType>(lowerObjectType(inst.type));
    const auto variant_value = state_.sem_ir.integer(inst.arg1);
    assert(variant_value >= 0);
    const auto variant = static_cast<std::uint32_t>(variant_value);
    const auto &nominal =
        state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(inst.type).arg0));
    builder.CreateStore(builder.getInt32(static_cast<std::int32_t>(
                            nominal.variants[variant].discriminant)),
                        builder.CreateStructGEP(record, storage, 0));
    const auto payload = state_.low_ir.valueBlock(inst.arg0);
    for (std::uint32_t field = 0; field < payload.size(); ++field) {
      const auto field_type =
          state_.sem_ir.enumPayloadFieldType(inst.type, variant, field);
      if (field_type == state_.sem_ir.voidType())
        continue;
      storeValueToObject(
          enumPayloadAddress(storage, inst.type, variant, field, builder),
          value(payload[field]), field_type, builder);
    }
    return storage;
  }

  llvm::Value *lowerInst(LowMakeObject inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *record =
        entryAlloca(function, lowerObjectType(inst.type), "object.shell");
    builder.CreateLifetimeStart(record);
    builder.CreateStore(
        llvm::Constant::getNullValue(lowerObjectType(inst.type)), record);
    const auto target =
        state_.low_ir.typeRepresentation(inst.type).object_init_target;
    if (target.hasValue())
      builder.CreateCall(state_.functions.at(target.index), {record});
    return record;
  }

  llvm::Value *makeObjectFromShell(LowInstId source, TypeId type,
                                   FunctionRefId target,
                                   llvm::IRBuilder<> &builder,
                                   llvm::Function &function, const char *name) {
    auto *record = entryAlloca(function, lowerObjectType(type), name);
    builder.CreateLifetimeStart(record);
    builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(type)),
                        record);
    assert(target.hasValue());
    builder.CreateCall(state_.functions.at(target.index), {record, value(source)});
    return record;
  }

  llvm::Value *lowerInst(LowMakeObjectCopy inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return makeObjectFromShell(
        inst.arg0, inst.type,
        state_.low_ir.typeRepresentation(inst.type).object_copy_init_target, builder,
        function, "object.copy.shell");
  }

  llvm::Value *lowerInst(LowMakeObjectMove inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return makeObjectFromShell(
        inst.arg0, inst.type,
        state_.low_ir.typeRepresentation(inst.type).object_move_init_target, builder,
        function, "object.move.shell");
  }

  llvm::Value *projectionAddress(llvm::Value *base,
                                 const LowObjectFieldProjection &projection,
                                 llvm::IRBuilder<> &builder) {
    auto *address = base;
    for (const auto step : projection.physical_steps)
      address = builder.CreateStructGEP(lowerObjectType(step.aggregate_type),
                                        address, step.field_index);
    return address;
  }

  llvm::Value *loadProjectionValue(llvm::Value *base, TypeId owner_type,
                                   TypeId field_type, std::uint32_t field_index,
                                   bool take, llvm::IRBuilder<> &builder) {
    const auto &projection =
        state_.low_ir.typeRepresentation(owner_type).field_projections[field_index];
    if (projection.kind == ObjectFieldProjectionKind::Computed) {
      const auto target =
          take ? projection.take_target : projection.load_target;
      assert(target.hasValue());
      if (state_.low_ir.typeRepresentation(field_type).facts.init_repr ==
          InitReprKind::InPlace) {
        auto &function = *builder.GetInsertBlock()->getParent();
        auto *result = entryAlloca(function, lowerObjectType(field_type),
                                   take ? "projection.take.result"
                                        : "projection.load.result");
        builder.CreateLifetimeStart(result);
        builder.CreateCall(state_.functions.at(target.index), {result, base});
        return result;
      }
      return builder.CreateCall(state_.functions.at(target.index), {base});
    }
    auto *address = projectionAddress(base, projection, builder);
    if (projection.kind == ObjectFieldProjectionKind::StableAddress)
      return loadValueFromObject(address, field_type, builder);
    const auto width = projection.bit_end - projection.bit_begin;
    auto *storage = builder.CreateLoad(builder.getInt32Ty(), address,
                                       take ? "projection.take.bits"
                                            : "projection.load.bits");
    auto *shifted = projection.bit_begin == 0
                        ? storage
                        : builder.CreateLShr(
                              storage, builder.getInt32(projection.bit_begin));
    auto *mask = builder.getInt32(
        width == 32 ? 0xffffffffU : ((std::uint32_t{1} << width) - 1U));
    auto *result = builder.CreateAnd(shifted, mask);
    if (width < 32) {
      const auto extend = 32 - width;
      result = builder.CreateAShr(
          builder.CreateShl(result, builder.getInt32(extend)),
          builder.getInt32(extend), "projection.sign.extend");
    }
    if (take) {
      auto *positioned =
          projection.bit_begin == 0
              ? mask
              : builder.CreateShl(mask, builder.getInt32(projection.bit_begin));
      builder.CreateStore(
          builder.CreateAnd(storage, builder.CreateNot(positioned)), address);
    }
    return result;
  }

  llvm::Value *writeProjectionValue(LowValueBlockId operands_id,
                                    TypeId field_type,
                                    std::uint32_t field_index, bool initialize,
                                    llvm::IRBuilder<> &builder) {
    const auto operands = state_.low_ir.valueBlock(operands_id);
    assert(operands.size() == 2);
    auto *base = value(operands[0]);
    auto *source = value(operands[1]);
    const auto owner_type = TypeId(state_.low_ir.inst(operands[0]).type);
    const auto &projection =
        state_.low_ir.typeRepresentation(owner_type).field_projections[field_index];
    if (projection.kind == ObjectFieldProjectionKind::Computed) {
      const auto target =
          initialize ? projection.init_target : projection.store_target;
      assert(target.hasValue());
      builder.CreateCall(state_.functions.at(target.index), {base, source});
      return nullptr;
    }
    auto *address = projectionAddress(base, projection, builder);
    if (projection.kind == ObjectFieldProjectionKind::StableAddress) {
      storeValueToObject(address, source, field_type, builder);
      return nullptr;
    }
    const auto width = projection.bit_end - projection.bit_begin;
    auto *old_value = builder.CreateLoad(builder.getInt32Ty(), address,
                                         "projection.old.bits");
    auto *low_mask = builder.getInt32(
        width == 32 ? 0xffffffffU : ((std::uint32_t{1} << width) - 1U));
    auto *positioned =
        projection.bit_begin == 0
            ? low_mask
            : builder.CreateShl(low_mask,
                                builder.getInt32(projection.bit_begin));
    auto *narrow = builder.CreateAnd(source, low_mask);
    if (projection.bit_begin != 0)
      narrow =
          builder.CreateShl(narrow, builder.getInt32(projection.bit_begin));
    builder.CreateStore(
        builder.CreateOr(
            builder.CreateAnd(old_value, builder.CreateNot(positioned)),
            narrow),
        address);
    return nullptr;
  }

  llvm::Value *lowerInst(LowLoad inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadValueFromObject(state_.slots.at(inst.arg0.index), inst.type, builder);
  }

  llvm::Value *lowerInst(LowLoadPlace inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadValueFromObject(placeAddress(inst.arg0, builder), inst.type,
                               builder);
  }

  llvm::Value *lowerInst(LowBorrow inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return state_.slots.at(inst.arg0.index);
  }

  llvm::Value *lowerInst(LowBorrowPlace inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return placeAddress(inst.arg0, builder);
  }

  llvm::Value *lowerInst(LowCarrierView inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return value(inst.arg0);
  }

  llvm::Value *lowerInst(LowObjectAddress inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return state_.slots.at(inst.arg0.index);
  }

  llvm::Value *lowerInst(LowPlaceAddress inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return placeAddress(inst.arg0, builder);
  }

  llvm::Value *lowerInst(LowDereference inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadValueFromObject(value(inst.arg0), inst.type, builder);
  }

  llvm::Value *lowerInst(LowDereferenceObject inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return value(inst.arg0);
  }

  llvm::Value *lowerInst(LowPackValue inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    const auto target = state_.low_ir.typeRepresentation(inst.type).pack_target;
    assert(target.hasValue());
    return builder.CreateCall(state_.functions.at(target.index), {value(inst.arg0)});
  }

  llvm::Value *lowerInst(LowUnpackValue inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto target = state_.low_ir.typeRepresentation(inst.type).init_target;
    assert(target.hasValue());
    auto *object =
        entryAlloca(function, lowerObjectType(inst.type), "unpack.object");
    builder.CreateLifetimeStart(object);
    builder.CreateCall(state_.functions.at(target.index), {object, value(inst.arg0)});
    return object;
  }

  llvm::Value *initializeSlot(SlotId slot, LowInstId source,
                              llvm::IRBuilder<> &builder) {
    if (state_.initialized_slots.insert(slot.index).second)
      builder.CreateLifetimeStart(state_.slots.at(slot.index));
    const auto slot_type = state_.low_ir.slot(slot).type;
    storeValueToObject(state_.slots.at(slot.index), value(source), slot_type,
                       builder);
    setSlotPlacesInitialized(slot, true, builder);
    return nullptr;
  }

  void setSlotPlacesInitialized(SlotId slot, bool initialized,
                                llvm::IRBuilder<> &builder) {
    for (const auto &[place_index, flag] : state_.place_flags)
      if (state_.low_ir.place(LowPlaceId(place_index)).root == slot)
        builder.CreateStore(builder.getInt1(initialized), flag);
    if (state_.coroutine.bitmap)
      for (const auto &[place_index, bit] : state_.coroutine.place_bits)
        if (state_.low_ir.place(LowPlaceId(place_index)).root == slot)
          setCoroutineInitializationBit(bit, initialized, builder);
  }

  [[nodiscard]] llvm::Value *
  coroutineInitializationWord(std::uint32_t bit, llvm::IRBuilder<> &builder) {
    return builder.CreateInBoundsGEP(builder.getInt64Ty(),
                                     state_.coroutine.bitmap,
                                     builder.getInt32(bit / 64));
  }

  [[nodiscard]] llvm::Value *
  testCoroutineInitializationBit(std::uint32_t bit,
                                 llvm::IRBuilder<> &builder) {
    auto *address = coroutineInitializationWord(bit, builder);
    llvm::Value *word = builder.CreateLoad(builder.getInt64Ty(), address);
    return builder.CreateICmpNE(
        builder.CreateAnd(word, builder.getInt64(UINT64_C(1) << (bit % 64))),
        builder.getInt64(0));
  }

  void setCoroutineInitializationBit(std::uint32_t bit, bool initialized,
                                     llvm::IRBuilder<> &builder) {
    auto *address = coroutineInitializationWord(bit, builder);
    llvm::Value *word = builder.CreateLoad(builder.getInt64Ty(), address);
    const auto mask = UINT64_C(1) << (bit % 64);
    word = initialized ? builder.CreateOr(word, builder.getInt64(mask))
                       : builder.CreateAnd(word, builder.getInt64(~mask));
    builder.CreateStore(word, address);
  }

  static bool pathPrefix(std::span<const LowPlaceProjection> prefix,
                         std::span<const LowPlaceProjection> path) {
    return prefix.size() <= path.size() &&
           std::equal(prefix.begin(), prefix.end(), path.begin());
  }

  void markMoved(LowPlaceId moved, llvm::IRBuilder<> &builder) {
    const auto &moved_place = state_.low_ir.place(moved);
    const auto moved_path = state_.low_ir.logicalPlaceProjections(moved);
    for (const auto &[place_index, flag] : state_.place_flags) {
      const auto &candidate = state_.low_ir.place(LowPlaceId(place_index));
      const auto candidate_path =
          state_.low_ir.logicalPlaceProjections(LowPlaceId(place_index));
      if (candidate.root == moved_place.root &&
          (pathPrefix(moved_path, candidate_path) ||
           pathPrefix(candidate_path, moved_path)))
        builder.CreateStore(builder.getFalse(), flag);
    }
    if (state_.coroutine.bitmap)
      for (const auto &[place_index, bit] : state_.coroutine.place_bits) {
        const auto &candidate = state_.low_ir.place(LowPlaceId(place_index));
        const auto candidate_path =
            state_.low_ir.logicalPlaceProjections(LowPlaceId(place_index));
        if (candidate.root == moved_place.root &&
            (pathPrefix(moved_path, candidate_path) ||
             pathPrefix(candidate_path, moved_path)))
          setCoroutineInitializationBit(bit, false, builder);
      }
  }

  void markInitialized(LowPlaceId initialized, llvm::IRBuilder<> &builder) {
    const auto &initialized_place = state_.low_ir.place(initialized);
    const auto initialized_path = state_.low_ir.logicalPlaceProjections(initialized);
    for (const auto &[place_index, flag] : state_.place_flags) {
      const auto &candidate = state_.low_ir.place(LowPlaceId(place_index));
      const auto candidate_path =
          state_.low_ir.logicalPlaceProjections(LowPlaceId(place_index));
      if (candidate.root == initialized_place.root &&
          pathPrefix(initialized_path, candidate_path))
        builder.CreateStore(builder.getTrue(), flag);
    }
    if (state_.coroutine.bitmap)
      for (const auto &[place_index, bit] : state_.coroutine.place_bits) {
        const auto &candidate = state_.low_ir.place(LowPlaceId(place_index));
        const auto candidate_path =
            state_.low_ir.logicalPlaceProjections(LowPlaceId(place_index));
        if (candidate.root == initialized_place.root &&
            pathPrefix(initialized_path, candidate_path))
          setCoroutineInitializationBit(bit, true, builder);
      }
  }

  llvm::Value *placeAddress(LowPlaceId id, llvm::IRBuilder<> &builder) {
    const auto &place = state_.low_ir.place(id);
    assert((place.flags & LowPlaceAddressable) != 0);
    llvm::Value *address = state_.slots.at(place.root.index);
    for (const auto projection : state_.low_ir.placeProjections(place.projections)) {
      auto *aggregate = lowerObjectType(projection.aggregate_type);
      if (projection.kind == LowPlaceProjectionKind::Dereference) {
        address = builder.CreateLoad(aggregate, address);
      } else if (projection.kind == LowPlaceProjectionKind::StructField) {
        address = builder.CreateStructGEP(aggregate, address, projection.index);
      } else if (projection.kind == LowPlaceProjectionKind::EnumPayload) {
        address =
            enumPayloadAddress(address, projection.aggregate_type,
                               projection.variant, projection.index, builder);
      } else {
        auto *zero = builder.getInt32(0);
        auto *index = builder.getInt32(projection.index);
        address = builder.CreateInBoundsGEP(aggregate, address, {zero, index});
      }
    }
    return address;
  }

  llvm::Value *lowerInst(LowInitialize inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return initializeSlot(inst.arg0, inst.arg1, builder);
  }

  llvm::Value *lowerInst(LowTransfer inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return initializeSlot(inst.arg0, inst.arg1, builder);
  }

  llvm::Value *lowerInst(LowInitializeFromValue inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    if (state_.initialized_slots.insert(inst.arg0.index).second)
      builder.CreateLifetimeStart(state_.slots.at(inst.arg0.index));
    const auto type = state_.low_ir.slot(inst.arg0).type;
    const auto target = state_.low_ir.typeRepresentation(type).init_target;
    assert(target.hasValue());
    builder.CreateCall(state_.functions.at(target.index),
                       {state_.slots.at(inst.arg0.index), value(inst.arg1)});
    setSlotPlacesInitialized(inst.arg0, true, builder);
    return nullptr;
  }

  llvm::Value *lowerInst(LowMoveOut inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    markMoved(inst.arg1, builder);
    if (state_.sem_ir.type(inst.type).kind == SemTypeKind::Void)
      return nullptr;
    return value(inst.arg0);
  }

  llvm::Value *lowerInst(LowCopyValue inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    if (!usesPointerValueRepresentation(inst.type))
      return value(inst.arg0);
    auto *copy =
        entryAlloca(function, lowerObjectType(inst.type), "copy.value");
    builder.CreateLifetimeStart(copy);
    copySemanticObject(copy, value(inst.arg0), inst.type, builder, function);
    return copy;
  }

  llvm::Value *lowerInst(LowLifecycleCopy inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *copy =
        entryAlloca(function, lowerObjectType(inst.type), "copy.custom");
    builder.CreateLifetimeStart(copy);
    builder.CreateCall(state_.functions.at(inst.arg0.index),
                       {copy, value(inst.arg1)});
    return copy;
  }

  llvm::Value *lowerInst(LowInitializePlace inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    auto *address = placeAddress(inst.arg0, builder);
    const auto target_type = state_.low_ir.place(inst.arg0).type;
    storeValueToObject(address, value(inst.arg1), target_type, builder);
    markInitialized(inst.arg0, builder);
    return nullptr;
  }

  llvm::Value *lowerInst(LowInitializePlaceFromValue inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    auto *address = placeAddress(inst.arg0, builder);
    const auto target_type = state_.low_ir.place(inst.arg0).type;
    const auto target = state_.low_ir.typeRepresentation(target_type).init_target;
    assert(target.hasValue());
    builder.CreateCall(state_.functions.at(target.index),
                       {address, value(inst.arg1)});
    markInitialized(inst.arg0, builder);
    return nullptr;
  }

  llvm::Value *lowerInst(LowStringLength inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return builder.CreateTrunc(builder.CreateExtractValue(value(inst.arg0), 1),
                               llvm::Type::getInt32Ty(state_.context));
  }

  llvm::Value *lowerInst(LowMakeSlice inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto operands = state_.low_ir.valueBlock(inst.arg0);
    auto *source_address = value(operands[0]);
    auto *start = value(operands[1]);
    auto *end = value(operands[2]);
    const auto source_reference = TypeId(state_.low_ir.inst(operands[0]).type);
    const auto source_type = state_.sem_ir.referencePointee(source_reference);
    const auto &source = state_.sem_ir.type(source_type);
    const auto wide = start->getType()->isIntegerTy(64);
    assert(end->getType() == start->getType());
    llvm::Value *base_data = nullptr;
    llvm::Value *source_length = nullptr;
    if (source.kind == SemTypeKind::Array) {
      base_data = source_address;
      source_length =
          wide ? static_cast<llvm::Value *>(builder.getInt64(source.arg1))
               : static_cast<llvm::Value *>(builder.getInt32(source.arg1));
    } else {
      auto *loaded = loadValueFromObject(source_address, source_type, builder);
      base_data = builder.CreateExtractValue(loaded, 0);
      source_length =
          wide ? builder.CreateExtractValue(loaded, 1)
               : builder.CreateTrunc(builder.CreateExtractValue(loaded, 1),
                                     builder.getInt32Ty());
    }
    auto *invalid =
        wide ? builder.CreateOr(builder.CreateICmpUGT(start, end),
                                builder.CreateICmpUGT(end, source_length))
             : builder.CreateOr(
                   builder.CreateOr(
                       builder.CreateICmpSLT(start, builder.getInt32(0)),
                       builder.CreateICmpSLT(end, builder.getInt32(0))),
                   builder.CreateOr(builder.CreateICmpSGT(start, end),
                                    builder.CreateICmpSGT(end, source_length)));
    emitArithmeticTrap(invalid, 8, "slice.bounds", builder, function);
    auto *data =
        source.kind == SemTypeKind::Array
            ? builder.CreateInBoundsGEP(lowerObjectType(source_type), base_data,
                                        {builder.getInt32(0), start})
            : builder.CreateInBoundsGEP(lowerObjectType(TypeId(source.arg0)),
                                        base_data, start);
    llvm::Value *result = llvm::UndefValue::get(lowerValueType(inst.type));
    result = builder.CreateInsertValue(result, data, 0);
    auto *length = builder.CreateSub(end, start);
    return builder.CreateInsertValue(
        result,
        wide ? length : builder.CreateZExt(length, builder.getInt64Ty()), 1);
  }

  llvm::Value *lowerInst(LowTupleElement inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    const auto index = static_cast<std::uint32_t>(state_.sem_ir.integer(inst.arg1));
    const auto tuple_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    if (usesPointerValueRepresentation(tuple_type)) {
      auto *address = builder.CreateStructGEP(lowerObjectType(tuple_type),
                                              value(inst.arg0), index);
      return loadValueFromObject(address, inst.type, builder);
    }
    return builder.CreateExtractValue(value(inst.arg0), index);
  }

  llvm::Value *lowerInst(LowSliceLength inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    auto *length = builder.CreateExtractValue(value(inst.arg0), 1);
    return state_.sem_ir.type(inst.type).kind == SemTypeKind::Integer &&
                   state_.sem_ir.type(inst.type).arg0 == 64
               ? length
               : builder.CreateTrunc(length, builder.getInt32Ty());
  }

  llvm::Value *lowerInst(LowStructField inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    const auto index = static_cast<std::uint32_t>(state_.sem_ir.integer(inst.arg1));
    const auto base_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    auto *address = value(inst.arg0);
    const auto &projections =
        state_.low_ir.typeRepresentation(base_type).field_projections;
    assert(index < projections.size());
    for (const auto projection : projections[index].physical_steps)
      address =
          builder.CreateStructGEP(lowerObjectType(projection.aggregate_type),
                                  address, projection.field_index);
    return loadValueFromObject(address, inst.type, builder);
  }

  llvm::Value *lowerInst(LowUnionField inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadValueFromObject(value(inst.arg0), inst.type, builder);
  }

  llvm::Value *lowerInst(LowEnumTag inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    const auto owner = TypeId(state_.low_ir.inst(inst.arg0).type);
    auto *record = llvm::cast<llvm::StructType>(lowerObjectType(owner));
    return builder.CreateLoad(
        builder.getInt32Ty(),
        builder.CreateStructGEP(record, value(inst.arg0), 0), "enum.tag");
  }

  llvm::Value *lowerInst(LowEnumPayload inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    if (state_.sem_ir.type(inst.type).kind == SemTypeKind::Void)
      return nullptr;
    const auto owner = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto encoded = static_cast<std::uint64_t>(state_.sem_ir.integer(inst.arg1));
    const auto variant = static_cast<std::uint32_t>(encoded >> 32U);
    const auto field = static_cast<std::uint32_t>(encoded);
    return loadValueFromObject(
        enumPayloadAddress(value(inst.arg0), owner, variant, field, builder),
        inst.type, builder);
  }

  llvm::Value *lowerInst(LowProjectionLoad inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadProjectionValue(value(inst.arg0),
                               TypeId(state_.low_ir.inst(inst.arg0).type), inst.type,
                               inst.arg1.index, false, builder);
  }

  llvm::Value *lowerInst(LowProjectionTake inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return loadProjectionValue(value(inst.arg0),
                               TypeId(state_.low_ir.inst(inst.arg0).type), inst.type,
                               inst.arg1.index, true, builder);
  }

  llvm::Value *lowerInst(LowProjectionStore inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return writeProjectionValue(inst.arg0, inst.type, inst.arg1.index, false,
                                builder);
  }

  llvm::Value *lowerInst(LowProjectionInit inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    const auto operands = state_.low_ir.valueBlock(inst.arg0);
    assert(operands.size() == 2);
    return writeProjectionValue(inst.arg0,
                                TypeId(state_.low_ir.inst(operands[1]).type),
                                inst.arg1.index, true, builder);
  }

  llvm::Value *lowerInst(LowProjectionBorrow inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    const auto owner = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto &projection =
        state_.low_ir.typeRepresentation(owner).field_projections[inst.arg1.index];
    if (projection.borrow_target.hasValue())
      return builder.CreateCall(state_.functions.at(projection.borrow_target.index),
                                {value(inst.arg0)});
    assert(projection.kind == ObjectFieldProjectionKind::StableAddress);
    return projectionAddress(value(inst.arg0), projection, builder);
  }

  llvm::Value *lowerInst(LowProjectionBorrowMut inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    const auto owner = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto &projection =
        state_.low_ir.typeRepresentation(owner).field_projections[inst.arg1.index];
    if (projection.borrow_mut_target.hasValue())
      return builder.CreateCall(
          state_.functions.at(projection.borrow_mut_target.index),
          {value(inst.arg0)});
    assert(projection.kind == ObjectFieldProjectionKind::StableAddress);
    return projectionAddress(value(inst.arg0), projection, builder);
  }

  llvm::Value *lowerInst(LowIndexStore inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto operands = state_.low_ir.valueBlock(inst.arg0);
    const auto array = state_.low_ir.place(inst.arg1).type;
    const auto element = TypeId(state_.sem_ir.type(array).arg0);
    auto *index = value(operands[0]);
    auto *wide = builder.CreateSExtOrTrunc(index, builder.getInt64Ty());
    auto *invalid = builder.CreateICmpUGE(wide, builder.getInt64(state_.sem_ir.type(array).arg1));
    emitArithmeticTrap(invalid, 8, "array.store", builder, function);
    auto *address = builder.CreateInBoundsGEP(lowerObjectType(array), placeAddress(inst.arg1, builder),
                                             {builder.getInt64(0), wide});
    state_.destroy_address(element, address, builder, function);
    moveSemanticValueToObject(address, value(operands[1]), element, builder);
    return nullptr;
  }

  llvm::Value *lowerInst(LowArrayIndex inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto base_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto borrowed =
        state_.sem_ir.type(inst.type).kind == SemTypeKind::Reference;
    const auto element_type =
        borrowed ? state_.sem_ir.referencePointee(inst.type) : inst.type;
    auto *index = value(inst.arg1);
    const auto wide = index->getType()->isIntegerTy(64);
    if (state_.sem_ir.type(base_type).kind == SemTypeKind::Slice) {
      auto *data = builder.CreateExtractValue(value(inst.arg0), {0});
      auto *length = builder.CreateExtractValue(value(inst.arg0), {1});
      auto *normalized =
          wide ? index : builder.CreateSExt(index, builder.getInt64Ty());
      auto *invalid =
          wide ? builder.CreateICmpUGE(normalized, length)
               : builder.CreateOr(
                     builder.CreateICmpSLT(index, builder.getInt32(0)),
                     builder.CreateICmpUGE(normalized, length));
      emitArithmeticTrap(invalid, 8, "slice.index", builder, function);
      auto *address = builder.CreateInBoundsGEP(lowerObjectType(element_type),
                                                data, normalized);
      return borrowed ? address
                      : loadValueFromObject(address, element_type, builder);
    }
    const auto aggregate_type =
        state_.sem_ir.type(base_type).kind == SemTypeKind::Reference
            ? state_.sem_ir.referencePointee(base_type)
            : base_type;
    const auto bound = state_.sem_ir.type(aggregate_type).arg1;
    auto *invalid =
        wide ? builder.CreateICmpUGE(index, builder.getInt64(bound))
             : builder.CreateOr(
                   builder.CreateICmpSLT(index, builder.getInt32(0)),
                   builder.CreateICmpUGE(index, builder.getInt32(bound)));
    emitArithmeticTrap(invalid, 8, "array.index", builder, function);
    if (state_.sem_ir.type(base_type).kind == SemTypeKind::Reference) {
      auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context), 0);
      auto *address = builder.CreateInBoundsGEP(
          lowerObjectType(aggregate_type), value(inst.arg0), {zero, index});
      return borrowed ? address
                      : loadValueFromObject(address, element_type, builder);
    }
    if (usesPointerValueRepresentation(base_type)) {
      auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context), 0);
      auto *address = builder.CreateInBoundsGEP(
          lowerObjectType(base_type), value(inst.arg0), {zero, index});
      return borrowed ? address
                      : loadValueFromObject(address, element_type, builder);
    }
    auto *temporary =
        entryAlloca(function, value(inst.arg0)->getType(), "index.base");
    builder.CreateLifetimeStart(temporary);
    builder.CreateStore(value(inst.arg0), temporary);
    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context), 0);
    auto *address = builder.CreateInBoundsGEP(value(inst.arg0)->getType(),
                                              temporary, {zero, index});
    auto *result = loadValueFromObject(address, inst.type, builder);
    builder.CreateLifetimeEnd(temporary);
    return result;
  }

  llvm::Value *lowerInst(LowDynamicIndexBorrow inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto aggregate_type = state_.low_ir.place(inst.arg0).type;
    auto *index = value(inst.arg1);
    const auto bound = state_.sem_ir.type(aggregate_type).arg1;
    const auto wide = index->getType()->isIntegerTy(64);
    auto *invalid =
        wide ? builder.CreateICmpUGE(index, builder.getInt64(bound))
             : builder.CreateOr(
                   builder.CreateICmpSLT(index, builder.getInt32(0)),
                   builder.CreateICmpUGE(index, builder.getInt32(bound)));
    emitArithmeticTrap(invalid, 8, "array.index", builder, function);
    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context), 0);
    return builder.CreateInBoundsGEP(lowerObjectType(aggregate_type),
                                     placeAddress(inst.arg0, builder),
                                     {zero, index});
  }

private:
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return state_.value(id);
  }
  [[nodiscard]] llvm::Type *lowerValueType(TypeId type) const {
    return state_.lower_value_type(type);
  }
  [[nodiscard]] llvm::Type *lowerObjectType(TypeId type) const {
    return state_.lower_object_type(type);
  }
  [[nodiscard]] llvm::AllocaInst *entryAlloca(
      llvm::Function &function, llvm::Type *type, llvm::StringRef name) const {
    return state_.entry_alloca(function, type, name);
  }
  [[nodiscard]] bool usesPointerValueRepresentation(TypeId type) const {
    return state_.uses_pointer_value_representation(type);
  }
  void copySemanticObject(llvm::Value *destination, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder,
                          llvm::Function &function) const {
    state_.copy_semantic_object(destination, source, type, builder, function);
  }
  [[nodiscard]] llvm::Value *enumPayloadAddress(
      llvm::Value *owner, TypeId type, std::uint32_t variant,
      std::uint32_t field, llvm::IRBuilder<> &builder) const {
    return state_.enum_payload_address(owner, type, variant, field, builder);
  }
  void emitArithmeticTrap(llvm::Value *condition, std::uint32_t reason,
                          std::string_view name, llvm::IRBuilder<> &builder,
                          llvm::Function &function) const {
    state_.arithmetic_trap(condition, reason, name, builder, function);
  }

  LLVMObjectInstructionState &state_;
};

} // namespace

llvm::Value *LLVMObjectInstructionService::lowerRaw(
    LowInst inst, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMObjectInstructionState &state) {
  auto emitter = ObjectInstructionEmitter(state);
#define CHTHOLLY_LLVM_OBJECT_CASE(Name)                                       \
  case LowInstKind::Name:                                                     \
    return emitter.lowerInst(                                                 \
        Low##Name{TypeId(inst.type), Low##Name::Arg0Type(inst.arg0),          \
                  Low##Name::Arg1Type(inst.arg1)},                            \
        builder, function)
  switch (inst.kind) {
    CHTHOLLY_LLVM_OBJECT_CASE(StaticLoad);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeArray);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeTuple);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeAggregate);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeUnion);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeEnum);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeObject);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeObjectCopy);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeObjectMove);
    CHTHOLLY_LLVM_OBJECT_CASE(Load);
    CHTHOLLY_LLVM_OBJECT_CASE(LoadPlace);
    CHTHOLLY_LLVM_OBJECT_CASE(Borrow);
    CHTHOLLY_LLVM_OBJECT_CASE(BorrowPlace);
    CHTHOLLY_LLVM_OBJECT_CASE(CarrierView);
    CHTHOLLY_LLVM_OBJECT_CASE(ObjectAddress);
    CHTHOLLY_LLVM_OBJECT_CASE(PlaceAddress);
    CHTHOLLY_LLVM_OBJECT_CASE(Dereference);
    CHTHOLLY_LLVM_OBJECT_CASE(DereferenceObject);
    CHTHOLLY_LLVM_OBJECT_CASE(PackValue);
    CHTHOLLY_LLVM_OBJECT_CASE(UnpackValue);
    CHTHOLLY_LLVM_OBJECT_CASE(Initialize);
    CHTHOLLY_LLVM_OBJECT_CASE(Transfer);
    CHTHOLLY_LLVM_OBJECT_CASE(InitializeFromValue);
    CHTHOLLY_LLVM_OBJECT_CASE(MoveOut);
    CHTHOLLY_LLVM_OBJECT_CASE(CopyValue);
    CHTHOLLY_LLVM_OBJECT_CASE(LifecycleCopy);
    CHTHOLLY_LLVM_OBJECT_CASE(InitializePlace);
    CHTHOLLY_LLVM_OBJECT_CASE(InitializePlaceFromValue);
    CHTHOLLY_LLVM_OBJECT_CASE(StringLength);
    CHTHOLLY_LLVM_OBJECT_CASE(MakeSlice);
    CHTHOLLY_LLVM_OBJECT_CASE(TupleElement);
    CHTHOLLY_LLVM_OBJECT_CASE(SliceLength);
    CHTHOLLY_LLVM_OBJECT_CASE(StructField);
    CHTHOLLY_LLVM_OBJECT_CASE(UnionField);
    CHTHOLLY_LLVM_OBJECT_CASE(EnumTag);
    CHTHOLLY_LLVM_OBJECT_CASE(EnumPayload);
    CHTHOLLY_LLVM_OBJECT_CASE(ProjectionLoad);
    CHTHOLLY_LLVM_OBJECT_CASE(ProjectionTake);
    CHTHOLLY_LLVM_OBJECT_CASE(ProjectionStore);
    CHTHOLLY_LLVM_OBJECT_CASE(ProjectionInit);
    CHTHOLLY_LLVM_OBJECT_CASE(ProjectionBorrow);
    CHTHOLLY_LLVM_OBJECT_CASE(ProjectionBorrowMut);
    CHTHOLLY_LLVM_OBJECT_CASE(ArrayIndex);
    CHTHOLLY_LLVM_OBJECT_CASE(IndexStore);
    CHTHOLLY_LLVM_OBJECT_CASE(DynamicIndexBorrow);
  default:
    assert(false && "non-object instruction reached object lowering service");
    return nullptr;
  }
#undef CHTHOLLY_LLVM_OBJECT_CASE
}

void LLVMObjectInstructionService::copy(
    llvm::Value *destination, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).copyObject(
      destination, source, type, builder);
}
void LLVMObjectInstructionService::moveObject(
    llvm::Value *destination, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).moveSemanticObject(
      destination, source, type, builder);
}
void LLVMObjectInstructionService::moveValue(
    llvm::Value *destination, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).moveSemanticValueToObject(
      destination, source, type, builder);
}
llvm::Value *LLVMObjectInstructionService::load(
    llvm::Value *address, TypeId type, llvm::IRBuilder<> &builder,
    LLVMObjectInstructionState &state) {
  return ObjectInstructionEmitter(state).loadValueFromObject(
      address, type, builder);
}
void LLVMObjectInstructionService::store(
    llvm::Value *address, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).storeValueToObject(
      address, source, type, builder);
}
void LLVMObjectInstructionService::markMoved(
    LowPlaceId place, llvm::IRBuilder<> &builder,
    LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).markMoved(place, builder);
}
void LLVMObjectInstructionService::markInitialized(
    LowPlaceId place, llvm::IRBuilder<> &builder,
    LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).markInitialized(place, builder);
}
llvm::Value *LLVMObjectInstructionService::placeAddress(
    LowPlaceId place, llvm::IRBuilder<> &builder,
    LLVMObjectInstructionState &state) {
  return ObjectInstructionEmitter(state).placeAddress(place, builder);
}
void LLVMObjectInstructionService::setSlotPlacesInitialized(
    SlotId slot, bool initialized, llvm::IRBuilder<> &builder,
    LLVMObjectInstructionState &state) {
  ObjectInstructionEmitter(state).setSlotPlacesInitialized(
      slot, initialized, builder);
}
llvm::Value *LLVMObjectInstructionService::testCoroutineInitializationBit(
    std::uint32_t bit, llvm::IRBuilder<> &builder,
    LLVMObjectInstructionState &state) {
  return ObjectInstructionEmitter(state).testCoroutineInitializationBit(
      bit, builder);
}
bool LLVMObjectInstructionService::pathPrefix(
    std::span<const LowPlaceProjection> prefix,
    std::span<const LowPlaceProjection> path) {
  return prefix.size() <= path.size() &&
         std::equal(prefix.begin(), prefix.end(), path.begin());
}

} // namespace chtholly::compiler
