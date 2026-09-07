#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"

#include <cassert>

namespace chtholly::compiler {

void LLVMCoroutineScaffoldService::detachCompletion(
    const CallbackWakePlan &plan, llvm::Value *completion,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineScaffoldState &state) {
  auto *token = builder.CreateExtractValue(completion, 2);
  auto *callback = builder.CreateExtractValue(completion, 0);
  auto *local =
      llvm::BasicBlock::Create(state.context, "task.detach.local", &function);
  auto *foreign = llvm::BasicBlock::Create(state.context, "task.detach.foreign",
                                           &function);
  auto *done =
      llvm::BasicBlock::Create(state.context, "task.detach.done", &function);
  builder.CreateCondBr(builder.CreateIsNull(token), local, foreign);
  builder.SetInsertPoint(local);
  const auto fields = state.sem_ir.typeBlock(
      TypeBlockId(state.sem_ir.type(plan.completion_type).arg0));
  if (plan.authority == CallbackReleaseAuthority::Retained)
    state.release_callback_adapter(
        callback, fields[0], builder);
  builder.CreateBr(done);
  builder.SetInsertPoint(foreign);
  auto *detach = builder.CreateExtractValue(completion, 6);
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.detach_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  llvm::SmallVector<llvm::Value *, 3> arguments(layout.parameters.size());
  llvm::Value *userdata = nullptr;
  llvm::Value *release = nullptr;
  if (plan.authority == CallbackReleaseAuthority::Retained) {
    userdata = builder.CreateExtractValue(callback, 1);
    release = builder.CreateExtractValue(callback, 2);
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index == plan.detach_parameters[0])
      arguments[index] = token;
    else if (index == plan.detach_parameters[1] && userdata)
      arguments[index] = userdata;
    else if (index == plan.detach_parameters[2] && release)
      arguments[index] = release;
    else
      llvm_unreachable("unassigned coroutine detach parameter");
  }
  (void)state.emit_foreign_call_values(layout, call_layout, detach, arguments,
                                       builder, function);
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
}

} // namespace chtholly::compiler
