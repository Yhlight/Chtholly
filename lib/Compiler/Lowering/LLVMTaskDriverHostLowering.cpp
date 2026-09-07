#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace chtholly::compiler {

llvm::Function *LLVMCoroutineScaffoldService::taskDriverHost(
    llvm::Function &driver, LLVMCoroutineScaffoldState &state) {
  auto *pointer = llvm::PointerType::getUnqual(state.context);
  auto *i32 = llvm::Type::getInt32Ty(state.context);
  auto *i64 = llvm::Type::getInt64Ty(state.context);
  auto *host = llvm::Function::Create(
      llvm::FunctionType::get(i32, false), llvm::Function::InternalLinkage,
      driver.getName().str() + "$host", state.module);
  auto *entry = llvm::BasicBlock::Create(state.context, "task.host.entry", host);
  auto *scope_block =
      llvm::BasicBlock::Create(state.context, "task.host.scope", host);
  auto *call_block =
      llvm::BasicBlock::Create(state.context, "task.host.call", host);
  auto *scope_failed =
      llvm::BasicBlock::Create(state.context, "task.host.scope.failed", host);
  auto *executor_failed = llvm::BasicBlock::Create(
      state.context, "task.host.executor.failed", host);
  llvm::IRBuilder<> builder(entry);
  auto *executor_out = builder.CreateAlloca(pointer);
  auto *scope_out = builder.CreateAlloca(pointer);
  auto *config_type = llvm::StructType::get(state.context, {i64, i32, i32});
  auto *config = builder.CreateAlloca(config_type);
  const auto size = state.module.getDataLayout()
                        .getTypeAllocSize(config_type)
                        .getFixedValue();
  builder.CreateStore(
      llvm::ConstantStruct::get(
          config_type,
          {builder.getInt64(size), builder.getInt32(1), builder.getInt32(0)}),
      config);
  auto *executor_status = builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_executor_create",
          llvm::FunctionType::get(i32, {pointer, pointer}, false)),
      {config, executor_out});
  builder.CreateCondBr(
      builder.CreateICmpEQ(executor_status, builder.getInt32(0)), scope_block,
      executor_failed);
  llvm::IRBuilder<> scoped(scope_block);
  auto *executor = scoped.CreateLoad(pointer, executor_out);
  auto *scope_status = scoped.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_scope_create",
          llvm::FunctionType::get(i32, {pointer, pointer}, false)),
      {executor, scope_out});
  scoped.CreateCondBr(scoped.CreateICmpEQ(scope_status, scoped.getInt32(0)),
                      call_block, scope_failed);
  llvm::IRBuilder<> call(call_block);
  auto *scope = call.CreateLoad(pointer, scope_out);
  auto *result = call.CreateCall(&driver, {scope}, "task.host.result");
  call.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_scope_release",
          llvm::FunctionType::get(call.getVoidTy(), {pointer}, false)),
      {scope});
  call.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_executor_release",
          llvm::FunctionType::get(call.getVoidTy(), {pointer}, false)),
      {executor});
  call.CreateRet(result);
  llvm::IRBuilder<> failed_scope(scope_failed);
  failed_scope.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_executor_release",
          llvm::FunctionType::get(failed_scope.getVoidTy(), {pointer}, false)),
      {executor});
  failed_scope.CreateRet(failed_scope.getInt32(1));
  llvm::IRBuilder<>(executor_failed)
      .CreateRet(llvm::ConstantInt::get(i32, 1));
  return host;
}

} // namespace chtholly::compiler
