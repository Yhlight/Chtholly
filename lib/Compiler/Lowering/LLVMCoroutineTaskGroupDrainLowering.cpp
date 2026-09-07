#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <cassert>

namespace chtholly::compiler {

void LLVMCoroutineScaffoldService::finishTaskGroupDrain(
    llvm::Value *group, const CoroutineResumeState &resume_state,
    llvm::BasicBlock *continuation, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineScaffoldState &state) {
  const auto escalate_child_cancellation =
      resume_state.child_cancellation_policy ==
      CoroutineChildCancellationPolicy::EscalateUnexpected;
  assert(escalate_child_cancellation ==
         (resume_state.cancellation_cause_policy ==
          CoroutineCancellationCausePolicy::OwnerRequestOrUnexpectedChild));
  const auto defer_cancellation =
      resume_state.cancellation_acknowledgement ==
      CoroutineCancellationAcknowledgement::EnclosingTaskScope;
  auto *info_type = llvm::StructType::get(
      state.context,
      {builder.getInt64Ty(), builder.getInt64Ty(), builder.getInt8Ty(),
       builder.getInt8Ty(), builder.getInt8Ty(), builder.getInt8Ty(),
       llvm::ArrayType::get(builder.getInt8Ty(), 4)});
  auto *info = state.entry_alloca(function, info_type, "task.group.query.info");
  const auto info_size = state.module.getDataLayout()
                             .getTypeAllocSize(info_type)
                             .getFixedValue();
  builder.CreateStore(builder.getInt64(info_size),
                      builder.CreateStructGEP(info_type, info, 0));
  auto *status = builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_group_query",
          llvm::FunctionType::get(builder.getInt32Ty(),
                                  {builder.getPtrTy(), builder.getPtrTy()},
                                  false)),
      {group, info}, "task.group.query.status");
  auto *active = builder.CreateLoad(
      builder.getInt64Ty(), builder.CreateStructGEP(info_type, info, 1));
  auto *closed = builder.CreateLoad(
      builder.getInt8Ty(), builder.CreateStructGEP(info_type, info, 2));
  auto *implicit_cancelled = builder.CreateLoad(
      builder.getInt8Ty(), builder.CreateStructGEP(info_type, info, 4));
  auto *implicit_failed = builder.CreateLoad(
      builder.getInt8Ty(), builder.CreateStructGEP(info_type, info, 5));
  builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_group_release",
          llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                  false)),
      {group});
  auto *valid = builder.CreateAnd(
      builder.CreateICmpEQ(status, builder.getInt32(0)),
      builder.CreateAnd(builder.CreateICmpEQ(active, builder.getInt64(0)),
                        builder.CreateICmpNE(closed, builder.getInt8(0))));
  valid = builder.CreateAnd(
      valid, builder.CreateICmpEQ(implicit_failed, builder.getInt8(0)));
  auto *valid_block =
      llvm::BasicBlock::Create(state.context, "task.group.drain.valid", &function);
  auto *failed_block = llvm::BasicBlock::Create(
      state.context, "task.group.drain.invalid", &function);
  builder.CreateCondBr(valid, valid_block, failed_block);
  llvm::IRBuilder<> failed(failed_block);
  state.protocol_trap(
      static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
      failed);
  llvm::IRBuilder<> valid_builder(valid_block);
  auto *owner_cancelled = valid_builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_cancellation_requested",
          llvm::FunctionType::get(valid_builder.getInt8Ty(),
                                  {valid_builder.getPtrTy()}, false)),
      {state.coroutine.task}, "task.group.owner.cancelled");
  auto *cancelled =
      valid_builder.CreateICmpNE(owner_cancelled, valid_builder.getInt8(0));
  if (escalate_child_cancellation)
    cancelled = valid_builder.CreateOr(
        valid_builder.CreateICmpNE(implicit_cancelled,
                                   valid_builder.getInt8(0)),
        cancelled);
  if (defer_cancellation) {
    auto *request = llvm::BasicBlock::Create(
        state.context, "task.group.drain.defer_cancel", &function);
    valid_builder.CreateCondBr(cancelled, request, continuation);
    llvm::IRBuilder<> request_builder(request);
    auto *request_status = request_builder.CreateCall(
        state.module.getOrInsertFunction(
            "chtholly_next_task_v1_task_request_cancel",
            llvm::FunctionType::get(request_builder.getInt32Ty(),
                                    {request_builder.getPtrTy()}, false)),
        {state.coroutine.task}, "task.group.owner.cancel.status");
    auto *deferred_failed = llvm::BasicBlock::Create(
        state.context, "task.group.drain.defer_cancel.failed", &function);
    request_builder.CreateCondBr(
        request_builder.CreateICmpEQ(request_status,
                                     request_builder.getInt32(0)),
        continuation, deferred_failed);
    llvm::IRBuilder<> request_failed(deferred_failed);
    state.protocol_trap(
        static_cast<std::uint32_t>(CoroutineRuntimeFaultReason::TaskGroup),
        request_failed);
    return;
  }
  valid_builder.CreateCondBr(
      cancelled, state.cancellation_target(valid_builder, function),
      continuation);
}

} // namespace chtholly::compiler
