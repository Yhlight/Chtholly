#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"

namespace chtholly::compiler {

llvm::Value *LLVMCallLoweringService::lower(LowCall inst,
                                            llvm::IRBuilder<> &builder,
                                            llvm::Function &function,
                                            LLVMCallLoweringState &state) {
  llvm::SmallVector<llvm::Value *, 4> arguments;
  if (const auto shape = state.fallible_constructor_shape(inst.arg0)) {
    const auto outcome_type = state.sem_ir.canonicalResultOutcomeType(inst.type);
    if (!outcome_type.hasValue()) {
      state.instruction_error =
          "fallible constructor call has no materialized outcome type";
      return nullptr;
    }
    auto *success = state.entry_alloca(
        function, state.lower_object_type(shape->success), "constructor.success");
    auto *outcome = state.entry_alloca(
        function, state.lower_object_type(outcome_type), "constructor.outcome");
    auto *result = state.entry_alloca(
        function, state.lower_object_type(inst.type), "constructor.result");
    builder.CreateLifetimeStart(success);
    builder.CreateLifetimeStart(outcome);
    builder.CreateLifetimeStart(result);
    arguments.push_back(success);
    arguments.push_back(outcome);
    for (const auto argument : state.low_ir.valueBlock(inst.arg1))
      arguments.push_back(state.value(argument));
    builder.CreateCall(state.functions.at(inst.arg0.index), arguments);

    auto *outcome_record =
        llvm::cast<llvm::StructType>(state.lower_object_type(outcome_type));
    auto *tag = builder.CreateLoad(
        builder.getInt32Ty(), builder.CreateStructGEP(outcome_record, outcome, 0),
        "constructor.outcome.tag");
    auto *ok = llvm::BasicBlock::Create(state.context, "constructor.ok", &function);
    auto *err = llvm::BasicBlock::Create(state.context, "constructor.err", &function);
    auto *done = llvm::BasicBlock::Create(state.context, "constructor.done", &function);
    builder.CreateCondBr(builder.CreateICmpEQ(tag, builder.getInt32(shape->ok_variant)),
                         ok, err);

    auto *result_record =
        llvm::cast<llvm::StructType>(state.lower_object_type(inst.type));
    builder.SetInsertPoint(ok);
    builder.CreateStore(builder.getInt32(shape->ok_variant),
                        builder.CreateStructGEP(result_record, result, 0));
    state.copy_object(
        state.enum_payload_address(result, inst.type, shape->ok_variant, 0, builder),
        success, shape->success, builder);
    builder.CreateBr(done);

    builder.SetInsertPoint(err);
    builder.CreateStore(builder.getInt32(shape->err_variant),
                        builder.CreateStructGEP(result_record, result, 0));
    state.copy_object(
        state.enum_payload_address(result, inst.type, shape->err_variant, 0, builder),
        state.enum_payload_address(outcome, outcome_type, shape->err_variant, 0,
                                   builder),
        shape->error, builder);
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    builder.CreateLifetimeEnd(outcome);
    builder.CreateLifetimeEnd(success);
    return result;
  }
  llvm::AllocaInst *result = nullptr;
  if (state.low_ir.typeRepresentation(inst.type).facts.init_repr ==
      InitReprKind::InPlace) {
    result = state.entry_alloca(function, state.lower_object_type(inst.type),
                                 "call.result");
    builder.CreateLifetimeStart(result);
    arguments.push_back(result);
  }
  for (const auto argument : state.low_ir.valueBlock(inst.arg1))
    arguments.push_back(state.value(argument));
  auto *call = builder.CreateCall(state.functions.at(inst.arg0.index), arguments);
  return result ? static_cast<llvm::Value *>(result)
                : static_cast<llvm::Value *>(call);
}

llvm::Value *LLVMCallLoweringService::indirect(
    LowIndirectCall inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallLoweringState &state) {
  llvm::SmallVector<llvm::Value *, 4> arguments;
  llvm::AllocaInst *result = nullptr;
  if (state.low_ir.typeRepresentation(inst.type).facts.init_repr ==
      InitReprKind::InPlace) {
    result = state.entry_alloca(function, state.lower_object_type(inst.type),
                                 "call.result");
    builder.CreateLifetimeStart(result);
    arguments.push_back(result);
  }
  for (const auto argument : state.low_ir.valueBlock(inst.arg1)) {
    if (!state.has_value(argument)) {
      state.instruction_error =
          "indirect call argument has no lowered LLVM value";
      return nullptr;
    }
    arguments.push_back(state.value(argument));
  }
  const auto &callee_type =
      state.sem_ir.type(TypeId(state.low_ir.inst(inst.arg0).type));
  llvm::SmallVector<llvm::Type *, 4> parameter_types;
  if (state.low_ir.typeRepresentation(TypeId(callee_type.arg1))
          .facts.init_repr == InitReprKind::InPlace)
    parameter_types.push_back(llvm::PointerType::getUnqual(state.context));
  for (const auto parameter :
       state.sem_ir.typeBlock(TypeBlockId(callee_type.arg0)))
    parameter_types.push_back(state.lower_value_type(parameter));
  auto *function_type = llvm::FunctionType::get(
      result ? llvm::Type::getVoidTy(state.context)
             : state.lower_value_type(TypeId(callee_type.arg1)),
      parameter_types, false);
  if (!state.has_value(inst.arg0)) {
    state.instruction_error = "indirect call target has no lowered LLVM value";
    return nullptr;
  }
  const auto callee = state.value(inst.arg0);
  if (arguments.size() != function_type->getNumParams()) {
    state.instruction_error = "indirect call LLVM arguments do not match its ABI";
    return nullptr;
  }
  auto *call = builder.CreateCall(function_type, callee, arguments);
  return result ? static_cast<llvm::Value *>(result)
                : static_cast<llvm::Value *>(call);
}

llvm::Value *LLVMCallLoweringService::construct(
    LowConstruct inst, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCallLoweringState &state) {
  const auto &plan = state.low_ir.constructPlan(inst.arg0);
  const auto root = state.low_ir.place(plan.destination).root;
  if (state.initialized_slots.insert(root.index).second)
    builder.CreateLifetimeStart(state.slots.at(root.index));
  llvm::SmallVector<llvm::Value *, 4> arguments;
  arguments.push_back(state.place_address(plan.destination, builder));
  const auto shape = state.fallible_constructor_shape(plan.target);
  llvm::AllocaInst *outcome = nullptr;
  if (shape) {
    outcome = state.entry_alloca(function, state.lower_object_type(inst.type),
                                 "placement.outcome");
    builder.CreateLifetimeStart(outcome);
    arguments.push_back(outcome);
  }
  for (const auto argument : state.low_ir.valueBlock(plan.arguments))
    arguments.push_back(state.value(argument));
  builder.CreateCall(state.functions.at(plan.target.index), arguments);
  if (!shape) {
    state.mark_initialized(plan.destination, builder);
    return nullptr;
  }

  auto *record = llvm::cast<llvm::StructType>(state.lower_object_type(inst.type));
  auto *tag = builder.CreateLoad(
      builder.getInt32Ty(), builder.CreateStructGEP(record, outcome, 0),
      "placement.outcome.tag");
  auto *ok = llvm::BasicBlock::Create(state.context, "placement.ok", &function);
  auto *failed =
      llvm::BasicBlock::Create(state.context, "placement.err", &function);
  auto *done =
      llvm::BasicBlock::Create(state.context, "placement.done", &function);
  builder.CreateCondBr(builder.CreateICmpEQ(tag, builder.getInt32(shape->ok_variant)),
                       ok, failed);
  builder.SetInsertPoint(ok);
  state.mark_initialized(plan.destination, builder);
  builder.CreateBr(done);
  builder.SetInsertPoint(failed);
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  return outcome;
}

} // namespace chtholly::compiler
