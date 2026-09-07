#include "LLVMInternal.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

namespace chtholly::compiler {

llvm::Value *LLVMValueLoweringService::integer(
    LowIntegerConstant inst, const SemIR &sem_ir,
    const std::function<llvm::Type *(TypeId)> &lower_object_type) {
  return llvm::ConstantInt::get(lower_object_type(inst.type),
                                sem_ir.integer(inst.arg0), true);
}

llvm::Value *LLVMValueLoweringService::floating(LowFloatConstant inst,
                                                 const SemIR &sem_ir,
                                                 llvm::LLVMContext &context) {
  const auto width = sem_ir.type(inst.type).arg0;
  const auto bits = static_cast<std::uint64_t>(sem_ir.integer(inst.arg0));
  const auto value =
      width == 32
          ? llvm::APFloat(llvm::APFloat::IEEEsingle(), llvm::APInt(32, bits))
          : llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, bits));
  return llvm::ConstantFP::get(context, value);
}

llvm::Value *LLVMValueLoweringService::boolean(LowBoolConstant inst,
                                                const SemIR &sem_ir,
                                                llvm::LLVMContext &context) {
  return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context),
                                sem_ir.integer(inst.arg0) != 0);
}

llvm::Value *LLVMValueLoweringService::nullPointer(
    LowNullPointer inst,
    const std::function<llvm::Type *(TypeId)> &lower_value_type) {
  return llvm::ConstantPointerNull::get(
      llvm::cast<llvm::PointerType>(lower_value_type(inst.type)));
}

llvm::Type *LLVMValueLoweringService::valueType(
    const LowIR &low_ir, TypeId id, llvm::LLVMContext &context,
    const std::function<llvm::Type *(TypeId)> &lower_object_type,
    const std::function<llvm::Type *(TypeId)> &lower_value_type) {
  const auto &representation = low_ir.typeRepresentation(id);
  switch (representation.facts.value_repr) {
  case ValueReprKind::None:
    return llvm::Type::getVoidTy(context);
  case ValueReprKind::Copy:
    return lower_object_type(id);
  case ValueReprKind::Pointer:
    return llvm::PointerType::getUnqual(context);
  case ValueReprKind::Custom:
    return lower_value_type(representation.value_type);
  case ValueReprKind::Dependent:
  case ValueReprKind::Count:
    return llvm::Type::getVoidTy(context);
  }
  llvm_unreachable("invalid value representation");
}

llvm::Value *LLVMValueLoweringService::stringConstant(
    StringLiteralId id, const SemIR &sem_ir, llvm::Module &module,
    llvm::LLVMContext &context, llvm::IRBuilder<> &builder,
    const std::function<llvm::Type *(TypeId)> &lower_object_type) {
  const auto text = sem_ir.string(id);
  auto *data = llvm::ConstantDataArray::getString(context, text, false);
  auto *global = new llvm::GlobalVariable(module, data->getType(), true,
                                           llvm::GlobalValue::PrivateLinkage,
                                           data, ".str");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::Value *value =
      llvm::UndefValue::get(lower_object_type(sem_ir.stringType()));
  value = builder.CreateInsertValue(value, global, 0);
  return builder.CreateInsertValue(
      value, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), text.size()),
      1);
}

llvm::Value *LLVMValueLoweringService::cstringConstant(
    StringLiteralId id, const SemIR &sem_ir, llvm::Module &module,
    llvm::LLVMContext &context) {
  const auto text = sem_ir.string(id);
  auto *data = llvm::ConstantDataArray::getString(context, text, true);
  auto *global = new llvm::GlobalVariable(module, data->getType(), true,
                                           llvm::GlobalValue::PrivateLinkage,
                                           data, ".cstr");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return global;
}

} // namespace chtholly::compiler
