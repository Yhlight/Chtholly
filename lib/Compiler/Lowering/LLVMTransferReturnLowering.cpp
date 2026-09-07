#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"

namespace chtholly::compiler {

llvm::Value *LLVMTransferReturnLoweringService::lower(
    LowTransferReturn inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMTransferReturnState &state) {
  const auto return_type = TypeId(state.low_ir.inst(inst.arg0).type);
  if (state.current_outcome_slot) {
    const auto shape = state.sem_ir.canonicalResultShape(return_type);
    const auto outcome_type = state.sem_ir.canonicalResultOutcomeType(return_type);
    if (!shape || !outcome_type.hasValue()) {
      state.instruction_error =
          "fallible constructor return has no canonical outcome protocol";
      return nullptr;
    }
    auto *source = state.value(inst.arg0);
    auto *source_record =
        llvm::cast<llvm::StructType>(state.lower_object_type(return_type));
    auto *tag = builder.CreateLoad(
        builder.getInt32Ty(), builder.CreateStructGEP(source_record, source, 0),
        "constructor.return.tag");
    auto *ok = llvm::BasicBlock::Create(state.context, "return.ok", &function);
    auto *err = llvm::BasicBlock::Create(state.context, "return.err", &function);
    auto *done = llvm::BasicBlock::Create(state.context, "return.done", &function);
    builder.CreateCondBr(builder.CreateICmpEQ(tag, builder.getInt32(shape->ok_variant)),
                         ok, err);

    auto *outcome_record =
        llvm::cast<llvm::StructType>(state.lower_object_type(outcome_type));
    builder.SetInsertPoint(ok);
    state.copy_object(
        state.current_result_slot,
        state.enum_payload_address(source, return_type, shape->ok_variant, 0,
                                   builder),
        shape->success, builder);
    builder.CreateStore(builder.getInt32(shape->ok_variant),
                        builder.CreateStructGEP(outcome_record,
                                                state.current_outcome_slot, 0));
    builder.CreateBr(done);

    builder.SetInsertPoint(err);
    builder.CreateStore(builder.getInt32(shape->err_variant),
                        builder.CreateStructGEP(outcome_record,
                                                state.current_outcome_slot, 0));
    state.copy_object(
        state.enum_payload_address(state.current_outcome_slot, outcome_type,
                                   shape->err_variant, 0, builder),
        state.enum_payload_address(source, return_type, shape->err_variant, 0,
                                   builder),
        shape->error, builder);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    return nullptr;
  }
  state.copy_object(state.current_result_slot, state.value(inst.arg0),
                    return_type, builder);
  return nullptr;
}

} // namespace chtholly::compiler
