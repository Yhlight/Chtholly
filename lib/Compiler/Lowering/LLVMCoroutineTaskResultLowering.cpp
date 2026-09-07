#include "LLVMInternal.h"

namespace chtholly::compiler {

llvm::Value *LLVMCoroutineTaskResultService::join(
    LowCoroutineTaskJoin inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineTaskResultState &state) {
  return builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_join",
          llvm::FunctionType::get(
              builder.getInt32Ty(),
              {llvm::PointerType::getUnqual(state.context)}, false)),
      {state.value(inst.arg0)}, "task.join.status");
}

llvm::Value *LLVMCoroutineTaskResultService::query(
    LowCoroutineTaskQuery inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineTaskResultState &state) {
  auto *i8 = builder.getInt8Ty();
  auto *info_type = llvm::StructType::get(
      state.context,
      {builder.getInt64Ty(), builder.getInt32Ty(), i8, i8, i8, i8, i8, i8,
       llvm::ArrayType::get(i8, 6)});
  auto *info = state.entry_alloca(function, info_type, "task.query.info");
  const auto info_size =
      state.module.getDataLayout().getTypeAllocSize(info_type).getFixedValue();
  builder.CreateStore(llvm::Constant::getNullValue(info_type), info);
  builder.CreateStore(builder.getInt64(info_size),
                      builder.CreateStructGEP(info_type, info, 0));
  auto *status = builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_query",
          llvm::FunctionType::get(
              builder.getInt32Ty(),
              {llvm::PointerType::getUnqual(state.context),
               llvm::PointerType::getUnqual(state.context)},
              false)),
      {state.value(inst.arg0), info}, "task.query.status");
  auto *outcome =
      state.entry_alloca(function, builder.getInt32Ty(), "task.query.outcome");
  builder.CreateStore(
      builder.CreateLoad(builder.getInt32Ty(),
                         builder.CreateStructGEP(info_type, info, 1)),
      outcome);
  return state.make_checked(status, outcome, inst.type, builder);
}

llvm::Value *LLVMCoroutineTaskResultService::take(
    LowInstId task, TypeId checked_type, bool error,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineTaskResultState &state) {
  const auto payload = state.sem_ir.coroutineCheckedPayloadType(checked_type);
  auto *storage = state.entry_alloca(
      function, state.lower_object_type(payload),
      error ? "task.error.out" : "task.result.out");
  auto *status = builder.CreateCall(
      state.module.getOrInsertFunction(
          error ? "chtholly_next_task_v1_task_take_error"
                : "chtholly_next_task_v1_task_take_result",
          llvm::FunctionType::get(
              builder.getInt32Ty(),
              {llvm::PointerType::getUnqual(state.context),
               llvm::PointerType::getUnqual(state.context)},
              false)),
      {state.value(task), storage},
      error ? "task.error.status" : "task.result.status");
  return state.make_checked(status, storage, checked_type, builder);
}

llvm::Value *LLVMCoroutineTaskResultService::checkedStatus(
    LowCoroutineCheckedStatus inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineTaskResultState &state) {
  return builder.CreateExtractValue(state.value(inst.arg0), 0,
                                    "task.checked.status");
}

llvm::Value *LLVMCoroutineTaskResultService::checkedTake(
    LowCoroutineCheckedTake inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineTaskResultState &state) {
  auto *storage = builder.CreateExtractValue(state.value(inst.arg0), 1,
                                             "task.checked.storage");
  return state.load_value_from_object(storage, inst.type, builder);
}

llvm::Value *LLVMCoroutineTaskResultService::outcome(
    LowInstId value, std::uint32_t expected, llvm::IRBuilder<> &builder,
    LLVMCoroutineTaskResultState &state) {
  return builder.CreateICmpEQ(state.value(value), builder.getInt32(expected));
}

void LLVMCoroutineTaskResultService::finishTask(
    LowFinishCoroutineTask inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineTaskResultState &state) {
  builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_release",
          llvm::FunctionType::get(
              builder.getVoidTy(),
              {llvm::PointerType::getUnqual(state.context)}, false)),
      {state.value(inst.arg0)});
}

} // namespace chtholly::compiler
