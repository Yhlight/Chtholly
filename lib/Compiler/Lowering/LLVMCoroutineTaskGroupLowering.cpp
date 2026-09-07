#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {
namespace {

class TaskGroupEmitter {
public:
  explicit TaskGroupEmitter(LLVMCoroutineTaskGroupState &state) : state_(state) {}
  llvm::Value *lowerInst(LowCoroutineTaskGroupCreate,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    assert(state_.coroutine.task);
    auto *storage =
        entryAlloca(function, builder.getPtrTy(), "task.group.create.out");
    auto *status = builder.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_group_create",
            llvm::FunctionType::get(builder.getInt32Ty(),
                                    {builder.getPtrTy(), builder.getPtrTy()},
                                    false)),
        {state_.coroutine.task, storage}, "task.group.create.status");
    auto *created = llvm::BasicBlock::Create(
        state_.context, "task.group.create.succeeded", &function);
    auto *failed = llvm::BasicBlock::Create(
        state_.context, "task.group.create.failed", &function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                         created, failed);
    llvm::IRBuilder<> failure(failed);
    emitCoroutineProtocolTrap(
        static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
        failure);
    builder.SetInsertPoint(created);
    return builder.CreateLoad(builder.getPtrTy(), storage, "task.group");
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupAttach inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto operands = state_.low_ir.valueBlock(inst.arg0);
    assert(operands.size() == 2);
    auto *group = value(operands[0]);
    auto *checked = value(operands[1]);
    auto *create_status = builder.CreateExtractValue(checked, 0);
    auto *attach =
        llvm::BasicBlock::Create(state_.context, "task.group.attach", &function);
    auto *done =
        llvm::BasicBlock::Create(state_.context, "task.group.attach.done", &function);
    builder.CreateCondBr(
        builder.CreateICmpEQ(create_status, builder.getInt32(0)), attach, done);
    llvm::IRBuilder<> attaching(attach);
    auto *task_storage = attaching.CreateExtractValue(checked, 1);
    auto *task = attaching.CreateLoad(attaching.getPtrTy(), task_storage,
                                      "task.group.child");
    const auto task_type =
        state_.sem_ir.coroutineCheckedPayloadType(TypeId(inst.type));
    const auto flags = state_.sem_ir.coroutineTaskErrorType(task_type) ? 0U : 1U;
    auto *attach_status = attaching.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_group_attach",
            llvm::FunctionType::get(attaching.getInt32Ty(),
                                    {attaching.getPtrTy(), attaching.getPtrTy(),
                                     attaching.getInt32Ty()},
                                    false)),
        {group, task, attaching.getInt32(flags)}, "task.group.attach.status");
    auto *attached = llvm::BasicBlock::Create(
        state_.context, "task.group.attach.succeeded", &function);
    auto *failed = llvm::BasicBlock::Create(
        state_.context, "task.group.attach.failed", &function);
    attaching.CreateCondBr(
        attaching.CreateICmpEQ(attach_status, attaching.getInt32(0)), attached,
        failed);
    llvm::IRBuilder<>(attached).CreateBr(done);
    llvm::IRBuilder<> failure(failed);
    failure.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_request_cancel",
            llvm::FunctionType::get(failure.getInt32Ty(), {failure.getPtrTy()},
                                    false)),
        {task});
    failure.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_release",
            llvm::FunctionType::get(failure.getVoidTy(), {failure.getPtrTy()},
                                    false)),
        {task});
    emitCoroutineProtocolTrap(
        static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
        failure);
    builder.SetInsertPoint(done);
    return checked;
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupRequestCancel inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    auto *status = builder.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_group_request_cancel",
            llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()},
                                    false)),
        {value(inst.arg0)}, "task.group.cancel.status");
    auto *done =
        llvm::BasicBlock::Create(state_.context, "task.group.cancel.done", &function);
    auto *failed = llvm::BasicBlock::Create(
        state_.context, "task.group.cancel.failed", &function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                         done, failed);
    llvm::IRBuilder<> failure(failed);
    emitCoroutineProtocolTrap(
        static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
        failure);
    builder.SetInsertPoint(done);
    return nullptr;
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupClose inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    auto *status = builder.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_group_close",
            llvm::FunctionType::get(builder.getInt32Ty(),
                                    {builder.getPtrTy(), builder.getInt8Ty()},
                                    false)),
        {value(inst.arg0), builder.getInt8(0)}, "task.group.close.status");
    auto *done =
        llvm::BasicBlock::Create(state_.context, "task.group.close.done", &function);
    auto *failed = llvm::BasicBlock::Create(state_.context, "task.group.close.failed",
                                            &function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                         done, failed);
    llvm::IRBuilder<> failure(failed);
    emitCoroutineProtocolTrap(
        static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
        failure);
    builder.SetInsertPoint(done);
    return nullptr;
  }

  llvm::Value *lowerInst(LowCoroutineTaskGroupCompletionArm inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    assert(state_.coroutine.task && state_.coroutine.wake_entry &&
           state_.coroutine.wake_release);
    auto *completion_storage =
        entryAlloca(function, builder.getPtrTy(), "task.group.completion.out");
    auto *disposition_storage = entryAlloca(
        function, builder.getInt32Ty(), "task.group.completion.disposition");
    auto retain = state_.module.getOrInsertFunction(
        "chtholly_next_task_v1_task_retain",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    auto release = state_.module.getOrInsertFunction(
        "chtholly_next_task_v1_task_release",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    builder.CreateCall(retain, {state_.coroutine.task});
    auto *status = builder.CreateCall(
        state_.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_group_completion_arm",
            llvm::FunctionType::get(builder.getInt32Ty(),
                                    {builder.getPtrTy(), builder.getPtrTy(),
                                     builder.getPtrTy(), builder.getPtrTy(),
                                     builder.getPtrTy(), builder.getPtrTy()},
                                    false)),
        {value(inst.arg0), state_.coroutine.wake_entry,
         state_.coroutine.wake_release, state_.coroutine.task,
         completion_storage, disposition_storage},
        "task.group.completion.arm.status");
    auto *disposition_ok = llvm::BasicBlock::Create(
        state_.context, "task.group.completion.disposition", &function);
    auto *armed = llvm::BasicBlock::Create(
        state_.context, "task.group.completion.armed", &function);
    auto *caller_owned = llvm::BasicBlock::Create(
        state_.context, "task.group.completion.caller_owned", &function);
    auto *failed = llvm::BasicBlock::Create(
        state_.context, "task.group.completion.failed", &function);
    auto *disposition =
        builder.CreateLoad(builder.getInt32Ty(), disposition_storage);
    auto *status_ok = builder.CreateICmpEQ(status, builder.getInt32(0));
    auto *owned = builder.CreateICmpEQ(disposition, builder.getInt32(1));
    auto *result_done = llvm::BasicBlock::Create(
        state_.context, "task.group.completion.arm.done", &function);
    builder.CreateCondBr(status_ok, disposition_ok, failed);
    llvm::IRBuilder<>(disposition_ok).CreateCondBr(owned, armed, caller_owned);
    llvm::IRBuilder<>(armed).CreateBr(result_done);
    llvm::IRBuilder<> caller(caller_owned);
    caller.CreateCall(release, {state_.coroutine.task});
    caller.CreateBr(result_done);
    llvm::IRBuilder<> failure(failed);
    failure.CreateCall(release, {state_.coroutine.task});
    emitCoroutineProtocolTrap(
        static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
        failure);
    builder.SetInsertPoint(result_done);
    return builder.CreateLoad(builder.getPtrTy(), completion_storage,
                              "task.group.completion");
  }

private:
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return state_.value(id);
  }
  [[nodiscard]] llvm::AllocaInst *entryAlloca(
      llvm::Function &function, llvm::Type *type, llvm::StringRef name) const {
    return state_.entry_alloca(function, type, name);
  }
  void emitCoroutineProtocolTrap(std::uint32_t reason,
                                 llvm::IRBuilder<> &builder) const {
    state_.protocol_trap(reason, builder);
  }

  LLVMCoroutineTaskGroupState &state_;
};

} // namespace

llvm::Value *LLVMCoroutineTaskGroupService::create(
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineTaskGroupState &state) {
  return TaskGroupEmitter(state).lowerInst(
      LowCoroutineTaskGroupCreate{}, builder, function);
}

llvm::Value *LLVMCoroutineTaskGroupService::attach(
    LowCoroutineTaskGroupAttach inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineTaskGroupState &state) {
  return TaskGroupEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCoroutineTaskGroupService::requestCancel(
    LowCoroutineTaskGroupRequestCancel inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineTaskGroupState &state) {
  return TaskGroupEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCoroutineTaskGroupService::close(
    LowCoroutineTaskGroupClose inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineTaskGroupState &state) {
  return TaskGroupEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCoroutineTaskGroupService::completionArm(
    LowCoroutineTaskGroupCompletionArm inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineTaskGroupState &state) {
  return TaskGroupEmitter(state).lowerInst(inst, builder, function);
}

} // namespace chtholly::compiler