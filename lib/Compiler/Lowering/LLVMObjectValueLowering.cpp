#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

void LLVMObjectValueLoweringService::copy(
    llvm::Value *destination, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectValueState &state) {
  if (!destination || !destination->getType()->isPointerTy() || !source ||
      !source->getType()->isPointerTy()) {
    state.instruction_error =
        "LLVM object copy requires address-valued source and destination";
    return;
  }
  auto *object_type = state.lower_object_type(type);
  if (!object_type->isSized()) {
    state.instruction_error = "LLVM object copy requires a sized type";
    return;
  }
  const auto layout = state.module.getDataLayout().getTypeAllocSize(object_type);
  const auto alignment =
      state.module.getDataLayout().getABITypeAlign(object_type);
  builder.CreateMemCpy(destination, alignment, source, alignment, layout);
}

llvm::Value *LLVMObjectValueLoweringService::load(
    llvm::Value *address, TypeId type, llvm::IRBuilder<> &builder,
    LLVMObjectValueState &state) {
  if (state.uses_pointer_value_representation(type))
    return address;
  if (state.low_ir.typeRepresentation(type).facts.value_repr ==
      ValueReprKind::Custom) {
    const auto target = state.low_ir.typeRepresentation(type).pack_target;
    assert(target.hasValue());
    return builder.CreateCall(state.functions.at(target.index), {address});
  }
  return builder.CreateLoad(state.lower_value_type(type), address);
}

void LLVMObjectValueLoweringService::store(
    llvm::Value *address, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectValueState &state) {
  if (state.uses_pointer_value_representation(type)) {
    copy(address, source, type, builder, state);
    return;
  }
  if (state.low_ir.typeRepresentation(type).facts.value_repr ==
      ValueReprKind::Custom) {
    const auto target = state.low_ir.typeRepresentation(type).init_target;
    assert(target.hasValue());
    builder.CreateCall(state.functions.at(target.index), {address, source});
    return;
  }
  builder.CreateStore(source, address);
}

void LLVMObjectValueLoweringService::moveObject(
    llvm::Value *destination, llvm::Value *source_address, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectValueState &state) {
  const auto &representation = state.low_ir.typeRepresentation(type);
  if (representation.object_move_init_target.hasValue()) {
    builder.CreateCall(
        state.functions.at(representation.object_move_init_target.index),
        {destination, source_address});
    return;
  }
  store(destination, load(source_address, type, builder, state), type, builder,
        state);
}

void LLVMObjectValueLoweringService::moveValue(
    llvm::Value *destination, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, LLVMObjectValueState &state) {
  const auto &representation = state.low_ir.typeRepresentation(type);
  if (representation.object_move_init_target.hasValue()) {
    builder.CreateCall(
        state.functions.at(representation.object_move_init_target.index),
        {destination, source});
    return;
  }
  store(destination, source, type, builder, state);
}

} // namespace chtholly::compiler
