#include "LLVMInternal.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

namespace chtholly::compiler {

bool LLVMCoroutineScaffoldService::registerConstructors(
    std::string &error, LLVMCoroutineScaffoldState &state) {
  auto *pointer_type = llvm::PointerType::getUnqual(state.context);
  auto *i32 = llvm::Type::getInt32Ty(state.context);
  for (std::uint32_t index = 0;
       index < state.low_ir.coroutineFramePlanCount(); ++index) {
    const auto &plan =
        state.low_ir.coroutineFramePlan(CoroutineFramePlanId(index));
    llvm::SmallVector<llvm::Type *, 8> parameters{pointer_type};
    for (const auto parameter : state.sem_ir.typeBlock(TypeBlockId(
             state.sem_ir.type(state.sem_ir.function(plan.function).type).arg0)))
      parameters.push_back(state.lower_value_type(parameter));
    parameters.push_back(pointer_type);
    parameters.push_back(llvm::Type::getInt1Ty(state.context));
    const auto *entity =
        state.sem_ir.importIRs().tryGetEntity(plan.constructor_entity);
    if (!entity) {
      error = "coroutine frame has no canonical constructor entity";
      return false;
    }
    auto *constructor = llvm::Function::Create(
        llvm::FunctionType::get(i32, parameters, false),
        llvm::Function::ExternalLinkage, state.constructor_symbol(*entity),
        state.module);
    constructor->setVisibility(llvm::GlobalValue::HiddenVisibility);
    if (!state.constructors
             .emplace(plan.constructor_entity.index, constructor)
             .second) {
      error = "duplicate coroutine constructor entity definition";
      return false;
    }
  }
  return true;
}

} // namespace chtholly::compiler
