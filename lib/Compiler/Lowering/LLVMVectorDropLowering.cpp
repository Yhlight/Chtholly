#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"

#include <algorithm>

namespace chtholly::compiler {

llvm::Value *LLVMVectorDropLoweringService::lower(
    TypeId vec_type, llvm::Value *vec, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMVectorDropState &state) {
  if (state.sem_ir.type(vec_type).kind != SemTypeKind::Nominal)
    return nullptr;
  const auto &nominal =
      state.sem_ir.nominalType(NominalTypeId(state.sem_ir.type(vec_type).arg0));
  if (nominal.kind != NominalKind::Struct || nominal.fields.size() != 3) {
    state.instruction_error = "Vec drop has an invalid object shape";
    return nullptr;
  }
  const auto pointer_type = state.sem_ir.nominalFieldType(vec_type, 0);
  const auto element_type = state.sem_ir.rawPointerPointee(pointer_type);
  auto *record = llvm::cast<llvm::StructType>(state.lower_object_type(vec_type));
  const auto field_address = [&](unsigned field, std::string_view name) {
    return builder.CreateStructGEP(record, vec, field, llvm::Twine(name));
  };
  auto *data_address = field_address(0, "vec.drop.data");
  auto *length_address = field_address(1, "vec.drop.length");
  auto *capacity_address = field_address(2, "vec.drop.capacity");
  auto *data = builder.CreateLoad(builder.getPtrTy(), data_address);
  auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address);
  auto *preheader = builder.GetInsertBlock();
  auto *test = llvm::BasicBlock::Create(state.context, "vec.drop.test", &function);
  auto *step = llvm::BasicBlock::Create(state.context, "vec.drop.step", &function);
  auto *done = llvm::BasicBlock::Create(state.context, "vec.drop.done", &function);
  builder.CreateBr(test);
  builder.SetInsertPoint(test);
  auto *remaining = builder.CreatePHI(builder.getInt64Ty(), 2,
                                      "vec.drop.remaining");
  remaining->addIncoming(length, preheader);
  builder.CreateCondBr(builder.CreateICmpEQ(remaining, builder.getInt64(0)),
                       done, step);
  builder.SetInsertPoint(step);
  auto *next = builder.CreateSub(remaining, builder.getInt64(1));
  builder.CreateStore(next, length_address);
  auto *element = builder.CreateInBoundsGEP(
      state.lower_object_type(element_type), data, next, "vec.drop.element");
  state.destroy_address(element_type, element, builder, function);
  auto *backedge = builder.GetInsertBlock();
  builder.CreateBr(test);
  remaining->addIncoming(next, backedge);
  builder.SetInsertPoint(done);
  const auto layout = state.module.getDataLayout().getTypeAllocSize(
      state.lower_object_type(element_type));
  const auto stride = std::max<std::uint64_t>(1, layout.getFixedValue());
  const auto alignment = std::max<std::uint64_t>(
      state.module.getDataLayout()
          .getABITypeAlign(state.lower_object_type(element_type))
          .value(),
      state.module.getDataLayout().getPointerSize());
  auto *capacity = builder.CreateLoad(builder.getInt64Ty(), capacity_address);
  auto *bytes = builder.CreateMul(capacity, builder.getInt64(stride));
  builder.CreateCall(
      state.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_deallocate",
          llvm::FunctionType::get(builder.getVoidTy(),
                                  {builder.getPtrTy(), builder.getInt64Ty(),
                                   builder.getInt64Ty()},
                                  false)),
      {data, bytes, builder.getInt64(alignment)});
  builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                      data_address);
  builder.CreateStore(builder.getInt64(0), capacity_address);
  return nullptr;
}

} // namespace chtholly::compiler
