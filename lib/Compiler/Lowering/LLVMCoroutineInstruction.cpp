#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMCoroutineInstructionService::lowerCancellationCheck(
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineInstructionState &state) {
  assert(state.coroutine.task && state.coroutine.cancel);
  auto cancellation = state.module.getOrInsertFunction(
      "chtholly_next_task_v1_task_cancellation_requested",
      llvm::FunctionType::get(builder.getInt8Ty(), {builder.getPtrTy()}, false));
  auto *continuation =
      llvm::BasicBlock::Create(state.context, "task.check.continue", &function);
  auto *requested = builder.CreateCall(cancellation, {state.coroutine.task},
                                       "task.explicit.cancelled");
  builder.CreateCondBr(
      builder.CreateICmpNE(requested, builder.getInt8(0)),
      state.cancellation_target(builder, function), continuation);
  builder.SetInsertPoint(continuation);
  return nullptr;
}

llvm::Value *LLVMCoroutineInstructionService::lowerCancellationRequested(
    llvm::IRBuilder<> &builder, llvm::Function &,
    LLVMCoroutineInstructionState &state) {
  assert(state.coroutine.task);
  auto *requested = builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_cancellation_requested",
          llvm::FunctionType::get(builder.getInt8Ty(), {builder.getPtrTy()},
                                   false)),
      {state.coroutine.task}, "task.cancel.requested");
  return builder.CreateICmpNE(requested, builder.getInt8(0));
}

llvm::Value *LLVMCoroutineInstructionService::lowerRuntimeFault(
    LowCoroutineRuntimeFault inst, llvm::IRBuilder<> &builder, llvm::Function &,
    const SemIR &sem_ir, LLVMCoroutineInstructionState &state) {
  auto *trap_type = llvm::FunctionType::get(
      builder.getVoidTy(), {builder.getInt32Ty()}, false);
  const auto reason =
      static_cast<std::uint32_t>(sem_ir.integer(IntegerId(inst.arg0)));
  builder.CreateCall(
      state.module.getOrInsertFunction("chtholly_next_runtime_v1_trap_coroutine",
                                       trap_type),
      {builder.getInt32(reason)});
  builder.CreateUnreachable();
  return nullptr;
}

llvm::Value *LLVMCoroutineInstructionService::lowerTerminalError(
    std::string_view message, llvm::IRBuilder<> &, llvm::Function &,
    LLVMCoroutineInstructionState &state) {
  state.instruction_error = std::string(message);
  return nullptr;
}

llvm::Value *LLVMCoroutineInstructionService::lowerExecutorSwitch(
    LowCoroutineExecutorSwitch inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineInstructionState &state) {
  assert(state.coroutine.task && state.coroutine.cancel);
  auto rebind = state.module.getOrInsertFunction(
      "chtholly_next_task_v1_task_rebind_executor",
      llvm::FunctionType::get(builder.getInt32Ty(),
                              {builder.getPtrTy(), builder.getPtrTy()}, false));
  auto cancellation = state.module.getOrInsertFunction(
      "chtholly_next_task_v1_task_cancellation_requested",
      llvm::FunctionType::get(builder.getInt8Ty(), {builder.getPtrTy()}, false));
  auto *status = builder.CreateCall(
      rebind, {state.coroutine.task, state.value(inst.arg0)},
      "task.executor.switch.status");
  auto *not_cancelled = llvm::BasicBlock::Create(
      state.context, "task.executor.not_cancelled", &function);
  auto *reschedule = llvm::BasicBlock::Create(
      state.context, "task.executor.reschedule", &function);
  auto *failed = llvm::BasicBlock::Create(
      state.context, "task.executor.failed", &function);
  auto *requested = builder.CreateCall(
      cancellation, {state.coroutine.task}, "task.executor.cancelled");
  builder.CreateCondBr(
      builder.CreateICmpNE(requested, builder.getInt8(0)),
      state.cancellation_target(builder, function), not_cancelled);
  llvm::IRBuilder<> checked(not_cancelled);
  checked.CreateCondBr(checked.CreateICmpEQ(status, checked.getInt32(0)),
                       reschedule, failed);
  llvm::IRBuilder<>(reschedule)
      .CreateRet(llvm::ConstantInt::get(builder.getInt32Ty(), 1));
  builder.SetInsertPoint(failed);
  return status;
}

} // namespace chtholly::compiler
