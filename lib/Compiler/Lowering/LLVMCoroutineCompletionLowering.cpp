#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMCoroutineCompletionService::arm(
    LowCoroutineTaskCompletionArm inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineCompletionState &state) {
  auto &group = state.task_group;
  assert(group.coroutine.task && group.coroutine.wake_entry &&
         group.coroutine.wake_release);
  const auto &plan = group.low_ir.coroutineTaskCompletionArmPlan(inst.arg0);
  (void)plan;
  auto *pointer = builder.getPtrTy();
  auto *completion_storage = group.entry_alloca(
      function, pointer, "task.completion.out");
  auto *disposition_storage = group.entry_alloca(
      function, builder.getInt32Ty(), "task.completion.disposition");
  auto retain = group.module.getOrInsertFunction(
      "chtholly_next_task_v1_task_retain",
      llvm::FunctionType::get(builder.getVoidTy(), {pointer}, false));
  auto release = group.module.getOrInsertFunction(
      "chtholly_next_task_v1_task_release",
      llvm::FunctionType::get(builder.getVoidTy(), {pointer}, false));
  builder.CreateCall(retain, {group.coroutine.task});
  auto arm = group.module.getOrInsertFunction(
      "chtholly_next_task_v1_completion_arm",
      llvm::FunctionType::get(
          builder.getInt32Ty(),
          {pointer, pointer, pointer, pointer, pointer, pointer}, false));
  auto *status = builder.CreateCall(
      arm,
      {group.value(inst.arg1), group.coroutine.wake_entry,
       group.coroutine.wake_release, group.coroutine.task, completion_storage,
       disposition_storage},
      "task.completion.arm.status");
  auto *armed = llvm::BasicBlock::Create(
      group.context, "task.completion.arm.owned", &function);
  auto *not_armed = llvm::BasicBlock::Create(
      group.context, "task.completion.arm.caller_owned", &function);
  auto *done = llvm::BasicBlock::Create(
      group.context, "task.completion.arm.done", &function);
  auto *disposition =
      builder.CreateLoad(builder.getInt32Ty(), disposition_storage);
  builder.CreateCondBr(
      builder.CreateAnd(
          builder.CreateICmpEQ(status, builder.getInt32(0)),
          builder.CreateICmpEQ(disposition, builder.getInt32(1))),
      armed, not_armed);
  llvm::IRBuilder<>(armed).CreateBr(done);
  llvm::IRBuilder<> caller_owned(not_armed);
  caller_owned.CreateCall(release, {group.coroutine.task});
  caller_owned.CreateBr(done);
  builder.SetInsertPoint(done);
  return state.make_checked(status, completion_storage, inst.type, builder);
}

llvm::Value *LLVMCoroutineCompletionService::ready(
    LowCoroutineTaskCompletionReady inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineCompletionState &state) {
  auto &group = state.task_group;
  auto *ready = builder.CreateCall(
      group.module.getOrInsertFunction(
          "chtholly_next_task_v1_completion_ready",
          llvm::FunctionType::get(builder.getInt8Ty(), {builder.getPtrTy()},
                                  false)),
      {group.value(inst.arg0)}, "task.completion.ready");
  return builder.CreateICmpNE(ready, builder.getInt8(0));
}

void LLVMCoroutineCompletionService::finish(
    LowFinishCoroutineTaskCompletion inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineCompletionState &state) {
  auto &group = state.task_group;
  builder.CreateCall(
      group.module.getOrInsertFunction(
          "chtholly_next_task_v1_completion_release",
          llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                  false)),
      {group.value(inst.arg0)});
}

} // namespace chtholly::compiler
