#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMCleanupLoweringService::lowerDestroy(
    LowDestroy inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  const auto type = state.low_ir.place(inst.arg0).type;
  auto *address = state.place_address(inst.arg0, builder);
  if (state.sem_ir.type(type).kind == SemTypeKind::CallbackAdapter)
    state.callback_adapter_release(
        builder.CreateLoad(state.lower_value_type(type), address), type,
        builder);
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerLifecycleDestroy(
    LowLifecycleDestroy inst, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCleanupState &state) {
  const auto role =
      state.sem_ir.functionIntrinsicRole(FunctionRefId(inst.arg0));
  if (role == CompilerIntrinsicRole::VecDrop)
    return state.lower_vec_drop_address(
        TypeId(state.low_ir.place(inst.arg1).type),
        state.place_address(inst.arg1, builder), builder, function);
  if ((isHashMapCompilerIntrinsic(role) || isHashSetCompilerIntrinsic(role)) &&
      (role == CompilerIntrinsicRole::HashMapDrop ||
       role == CompilerIntrinsicRole::HashSetDrop) &&
      state.lower_container_drop_address)
    return state.lower_container_drop_address(
        TypeId(state.low_ir.place(inst.arg1).type),
        state.place_address(inst.arg1, builder), builder, function);
  builder.CreateCall(state.functions.at(inst.arg0.index),
                     {state.place_address(inst.arg1, builder)});
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerIsInitialized(
    LowIsInitialized inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  return builder.CreateLoad(builder.getInt1Ty(),
                            state.place_flags.at(inst.arg0.index));
}

llvm::Value *LLVMCleanupLoweringService::lowerMarkInitialized(
    LowMarkInitialized inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  state.mark_initialized(inst.arg0, builder);
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerMarkMoved(
    LowMarkMoved inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  state.mark_moved(inst.arg0, builder);
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerDestroyValue(
    LowDestroyValue inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  const auto type = TypeId(state.low_ir.inst(inst.arg0).type);
  if (state.sem_ir.type(type).kind == SemTypeKind::CallbackAdapter)
    state.callback_adapter_release(state.value(inst.arg0), type, builder);
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerLifecycleDestroyValue(
    LowLifecycleDestroyValue inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCleanupState &state) {
  const auto value_type = TypeId(state.low_ir.inst(inst.arg1).type);
  auto *object = state.value(inst.arg1);
  const auto indirect = state.uses_pointer_value_representation(value_type);
  if (!indirect) {
    object = state.entry_alloca(function, state.lower_object_type(value_type),
                                "destroy.value.object");
    builder.CreateLifetimeStart(object);
    state.store_value_to_object(object, state.value(inst.arg1), value_type,
                                 builder);
  }
  builder.CreateCall(state.functions.at(inst.arg0.index), {object});
  if (!indirect)
    builder.CreateLifetimeEnd(object);
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerEndLifetime(
    LowEndLifetime inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  const auto type = state.low_ir.slot(inst.arg0).type;
  const auto target = state.low_ir.typeRepresentation(type).object_drop_target;
  if (target.hasValue())
    builder.CreateCall(state.functions.at(target.index),
                       {state.slots.at(inst.arg0.index)});
  builder.CreateLifetimeEnd(state.slots.at(inst.arg0.index));
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerCoroutineCleanupEnd(
    LowCoroutineCleanupEnd, llvm::IRBuilder<> &, llvm::Function &,
    LLVMCleanupState &) {
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerBranch(
    LowBranch inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  builder.CreateBr(state.blocks.at(inst.arg0.index));
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerBranchIf(
    LowBranchIf inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  const auto &targets = state.low_ir.targets(inst.arg1);
  builder.CreateCondBr(state.value(inst.arg0),
                       state.blocks.at(targets.true_block.index),
                       state.blocks.at(targets.false_block.index));
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerReturn(
    LowReturn inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  const auto return_type = TypeId(state.low_ir.inst(inst.arg0).type);
  if (return_type == state.sem_ir.voidType() ||
      (!state.current_representation_pack() &&
       state.low_ir.typeRepresentation(return_type).facts.init_repr ==
           InitReprKind::InPlace))
    builder.CreateRetVoid();
  else
    builder.CreateRet(state.value(inst.arg0));
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerReturnInPlace(
    LowReturnInPlace, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &) {
  builder.CreateRetVoid();
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerUnreachable(
    LowUnreachable, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &) {
  builder.CreateUnreachable();
  return nullptr;
}

llvm::Value *LLVMCleanupLoweringService::lowerFatalFailure(
    LowFatalFailure inst, llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCleanupState &state) {
  auto *trap_type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(state.context),
      {llvm::Type::getInt32Ty(state.context)}, false);
  auto trap = state.module.getOrInsertFunction(
      "chtholly_next_runtime_v1_trap_failure", trap_type);
  auto *function = llvm::dyn_cast<llvm::Function>(trap.getCallee());
  if (function) {
    function->addFnAttr(llvm::Attribute::NoReturn);
    function->addFnAttr(llvm::Attribute::NoUnwind);
    function->addFnAttr(llvm::Attribute::Cold);
  }
  builder.CreateCall(
      trap, llvm::ConstantInt::get(llvm::Type::getInt32Ty(state.context),
                                   state.sem_ir.integer(inst.arg0)));
  builder.CreateUnreachable();
  return nullptr;
}

} // namespace chtholly::compiler
