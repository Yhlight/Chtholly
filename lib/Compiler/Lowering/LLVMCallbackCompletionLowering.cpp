#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMCallbackCompletionService::pending(
    LowCallbackCompletionPending inst, llvm::IRBuilder<> &builder,
    LLVMCallbackState &state) {
  auto *token = builder.CreateExtractValue(state.value(inst.arg0), 2,
                                           "completion.token");
  return builder.CreateICmpNE(
      token, llvm::ConstantPointerNull::get(
                 llvm::cast<llvm::PointerType>(token->getType())));
}

llvm::Value *LLVMCallbackCompletionService::poll(
    LowCallbackCompletionPoll inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackState &state) {
  const auto &plan = state.low_ir.callbackReadinessPlan(inst.arg0);
  const auto completion =
      state.value(state.low_ir.valueBlock(inst.arg1).front());
  auto *token =
      builder.CreateExtractValue(completion, 2, "completion.poll.token");
  auto *is_null = builder.CreateICmpEQ(
      token, llvm::ConstantPointerNull::get(
                 llvm::cast<llvm::PointerType>(token->getType())));
  auto *poll_block =
      llvm::BasicBlock::Create(state.context, "completion.poll", &function);
  auto *done_block = llvm::BasicBlock::Create(
      state.context, "completion.poll.done", &function);
  auto *origin_block = builder.GetInsertBlock();
  builder.CreateCondBr(is_null, done_block, poll_block);

  builder.SetInsertPoint(poll_block);
  auto *poll =
      builder.CreateExtractValue(completion, 4, "completion.poll.entry");
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.poll_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  llvm::SmallVector<llvm::Value *, 1> args{token};
  auto *ready = state.emit_foreign_call_values(
      layout, call_layout, poll, args, builder, function);
  assert(ready && ready->getType()->isIntegerTy(1));
  builder.CreateBr(done_block);

  builder.SetInsertPoint(done_block);
  auto *result = builder.CreatePHI(llvm::Type::getInt1Ty(state.context), 2,
                                   "completion.ready");
  result->addIncoming(llvm::ConstantInt::getTrue(state.context), origin_block);
  result->addIncoming(ready, poll_block);
  return result;
}

llvm::Value *LLVMCallbackCompletionService::finishValue(
    CallbackCompletionPlanId plan_id, llvm::Value *completion,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCallbackState &state) {
  const auto &plan = state.low_ir.callbackCompletionPlan(plan_id);
  const auto completion_fields = state.sem_ir.typeBlock(
      TypeBlockId(state.sem_ir.type(plan.completion_type).arg0));
  assert(completion_fields.size() == 4 || completion_fields.size() == 5 ||
         completion_fields.size() == 7);
  auto *token = builder.CreateExtractValue(completion, 2, "completion.token");
  auto *pending = builder.CreateICmpNE(
      token, llvm::ConstantPointerNull::get(
                 llvm::cast<llvm::PointerType>(token->getType())));
  auto *wait_block =
      llvm::BasicBlock::Create(state.context, "completion.wait", &function);
  auto *waited_block =
      llvm::BasicBlock::Create(state.context, "completion.waited", &function);
  builder.CreateCondBr(pending, wait_block, waited_block);

  builder.SetInsertPoint(wait_block);
  auto *wait =
      builder.CreateExtractValue(completion, 3, "completion.wait.entry");
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.wait_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  llvm::SmallVector<llvm::Value *, 1> args{token};
  (void)state.emit_foreign_call_values(layout, call_layout, wait, args, builder,
                                       function);
  builder.CreateBr(waited_block);
  builder.SetInsertPoint(waited_block);

  auto *callback = builder.CreateExtractValue(
      completion, 0, "completion.callback.done");
  if (plan.authority == CallbackReleaseAuthority::Retained) {
    LLVMCallbackRegistrationService::releaseAdapter(
        callback, completion_fields[0], builder, state);
    return nullptr;
  }

  auto *original_handle = builder.CreateExtractValue(
      completion, 1, "completion.original.handle");
  auto *original_null = builder.CreateICmpEQ(
      original_handle,
      llvm::ConstantPointerNull::get(
          llvm::cast<llvm::PointerType>(original_handle->getType())));
  auto *release_block = llvm::BasicBlock::Create(
      state.context, "completion.release.null", &function);
  auto *done_block =
      llvm::BasicBlock::Create(state.context, "completion.done", &function);
  builder.CreateCondBr(original_null, release_block, done_block);
  builder.SetInsertPoint(release_block);
  LLVMCallbackRegistrationService::releaseAdapter(
      callback, completion_fields[0], builder, state);
  builder.CreateBr(done_block);
  builder.SetInsertPoint(done_block);
  return nullptr;
}

llvm::Value *LLVMCallbackCompletionService::finish(
    CallbackCompletionPlanId plan_id, LowValueBlockId operands,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCallbackState &state) {
  return finishValue(
      plan_id, state.value(state.low_ir.valueBlock(operands).front()), builder,
      function, state);
}

} // namespace chtholly::compiler
