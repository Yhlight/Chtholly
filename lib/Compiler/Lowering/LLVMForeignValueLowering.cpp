#include "LLVMInternal.h"

#include <algorithm>
#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMForeignValueLoweringService::defaultPromote(
    LowForeignDefaultPromote inst, llvm::Value *source, const LowIR &low_ir,
    const SemIR &sem_ir,
    llvm::IRBuilder<> &builder,
    const std::function<llvm::Type *(TypeId)> &lower_value_type) {
  const auto source_type = TypeId(low_ir.inst(inst.arg0).type);
  const auto &semantic = sem_ir.type(source_type);
  if (semantic.kind == SemTypeKind::Bool)
    return builder.CreateZExt(source, lower_value_type(inst.type));
  if (semantic.kind == SemTypeKind::Char)
    return builder.CreateZExt(source, lower_value_type(inst.type));
  if (semantic.kind == SemTypeKind::Integer)
    return semantic.arg1 != 0
               ? builder.CreateSExt(source, lower_value_type(inst.type))
               : builder.CreateZExt(source, lower_value_type(inst.type));
  assert(semantic.kind == SemTypeKind::Float && semantic.arg0 == 32);
  return builder.CreateFPExt(source, lower_value_type(inst.type));
}

llvm::Constant *LLVMForeignValueLoweringService::invalidSentinel(
    const SemNominalType &resource, TypeId value_type,
    LLVMForeignValueState &state) {
  const auto handle_type =
      state.sem_ir.type(value_type).kind == SemTypeKind::ForeignCompletion
          ? resource.foreign_completion_handle_type
          : resource.foreign_handle_type;
  const auto &handle = state.sem_ir.nominalType(
      NominalTypeId(state.sem_ir.type(handle_type).arg0));
  auto *physical = state.lower_value_type(value_type);
  if (handle.foreign_invalid_state == ForeignResourceInvalidState::Null)
    return llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(physical));
  if (handle.foreign_invalid_state ==
      ForeignResourceInvalidState::PointerBitPattern) {
    auto *bits = llvm::ConstantInt::get(
        llvm::IntegerType::get(state.context,
                               state.sem_ir.targetLayout().pointer_width),
        handle.foreign_invalid_integer, true);
    return llvm::ConstantExpr::getIntToPtr(
        bits, llvm::cast<llvm::PointerType>(physical));
  }
  return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(physical),
                                handle.foreign_invalid_integer, true);
}

llvm::Value *LLVMForeignValueLoweringService::valid(
    LowForeignResourceValid inst, llvm::IRBuilder<> &builder,
    LLVMForeignValueState &state) {
  const auto source_type = TypeId(state.low_ir.inst(inst.arg0).type);
  const auto &source = state.sem_ir.type(source_type);
  assert(source.kind == SemTypeKind::Nominal);
  const auto &resource =
      state.sem_ir.nominalType(NominalTypeId(source.arg0));
  if (resource.foreign_registration_storage_type.hasValue()) {
    auto *handle = builder.CreateExtractValue(state.value(inst.arg0), 1,
                                              "foreign.resource.handle");
    return builder.CreateICmpNE(
        handle,
        llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(handle->getType())),
        "foreign.resource.valid");
  }
  return builder.CreateICmpNE(
      state.value(inst.arg0), invalidSentinel(resource, source_type, state),
      "foreign.resource.valid");
}

llvm::Value *LLVMForeignValueLoweringService::wrap(
    LowForeignResourceWrap inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMForeignValueState &state) {
  const auto source_type = TypeId(state.low_ir.inst(inst.arg0).type);
  auto *source = state.value(inst.arg0);
  if (state.uses_pointer_value_representation(inst.type) &&
      !state.uses_pointer_value_representation(source_type)) {
    auto *storage = state.entry_alloca(
        function, state.lower_object_type(inst.type), "foreign.resource");
    builder.CreateLifetimeStart(storage);
    state.store_value_to_object(storage, source, source_type, builder);
    return storage;
  }
  return source;
}

llvm::Value *LLVMForeignValueLoweringService::unwrap(
    LowForeignResourceUnwrap inst, llvm::IRBuilder<> &builder,
    LLVMForeignValueState &state) {
  const auto source_type = TypeId(state.low_ir.inst(inst.arg0).type);
  auto *source = state.value(inst.arg0);
  if (!state.uses_pointer_value_representation(inst.type) &&
      state.uses_pointer_value_representation(source_type))
    return state.load_value_from_object(source, inst.type, builder);
  return source;
}

llvm::Value *LLVMForeignValueLoweringService::finish(
    TypeId source_type, llvm::Value *handle, llvm::IRBuilder<> &builder,
    LLVMForeignValueState &state) {
  const auto owner = NominalTypeId(state.sem_ir.type(source_type).arg0);
  const auto &resource = state.sem_ir.nominalType(owner);
  const auto &protocol = state.sem_ir.foreignResourceProtocol(source_type);
  auto *sentinel = invalidSentinel(resource, source_type, state);
  auto *function = builder.GetInsertBlock()->getParent();
  auto *active = llvm::BasicBlock::Create(state.context,
                                          "foreign.cleanup.active", function);
  auto *done = llvm::BasicBlock::Create(state.context,
                                        "foreign.cleanup.done", function);
  builder.CreateCondBr(builder.CreateICmpNE(handle, sentinel), active, done);
  builder.SetInsertPoint(active);
  llvm::Value *current = handle;
  for (const auto role_kind : protocol.facts.cleanup_path) {
    const auto operation = std::ranges::find(
        resource.foreign_resource_operations, role_kind,
        &SemForeignResourceOperation::role);
    assert(operation != resource.foreign_resource_operations.end());
    current = builder.CreateCall(state.functions.at(operation->target.index),
                                 {current});
  }
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(done);
  builder.SetInsertPoint(done);
  return nullptr;
}

} // namespace chtholly::compiler
