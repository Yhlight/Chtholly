#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMCoroutineTaskCreateService::lower(
    LowCoroutineTaskCreate inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineTaskCreateState &state) {
  const auto &plan = state.low_ir.coroutineTaskCreatePlan(inst.arg0);
  const auto operands = state.low_ir.valueBlock(inst.arg1);
  const auto root = plan.mode == CoroutineTaskCreateMode::Root;
  if (!root)
    assert(state.coroutine.task);
  auto *task_storage = state.entry_alloca(
      function, llvm::PointerType::getUnqual(state.context), "task.create.out");
  llvm::SmallVector<llvm::Value *, 8> arguments;
  arguments.push_back(root ? state.value(operands.front())
                           : state.coroutine.task);
  for (const auto operand : operands.subspan(root ? 1U : 0U))
    arguments.push_back(state.value(operand));
  arguments.push_back(task_storage);
  arguments.push_back(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(state.context), root ? 0 : 1));
  auto constructor = state.constructors.find(plan.constructor_entity.index);
  if (constructor == state.constructors.end()) {
    const auto *entity =
        state.sem_ir.importIRs().tryGetEntity(plan.constructor_entity);
    assert(entity);
    llvm::SmallVector<llvm::Type *, 8> parameters{builder.getPtrTy()};
    for (const auto parameter : plan.parameter_types)
      parameters.push_back(state.lower_value_type(parameter));
    parameters.push_back(builder.getPtrTy());
    parameters.push_back(builder.getInt1Ty());
    auto *declaration = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), parameters, false),
        llvm::Function::ExternalLinkage, state.constructor_symbol(*entity),
        state.module);
    declaration->setVisibility(llvm::GlobalValue::HiddenVisibility);
    constructor = state.constructors
                      .emplace(plan.constructor_entity.index, declaration)
                      .first;
  }
  auto *status = builder.CreateCall(constructor->second, arguments,
                                    "task.create.status");
  return state.make_checked(status, task_storage, inst.type, builder);
}

} // namespace chtholly::compiler
