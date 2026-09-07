#include "LLVMInternal.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace chtholly::compiler {

namespace {

llvm::CallingConv::ID callingConvention(ForeignCallingConvention value) {
  switch (value) {
  case ForeignCallingConvention::C:
    return llvm::CallingConv::C;
  case ForeignCallingConvention::Win64:
    return llvm::CallingConv::Win64;
  case ForeignCallingConvention::SysV64:
    return llvm::CallingConv::X86_64_SysV;
  case ForeignCallingConvention::Count:
    return llvm::CallingConv::C;
  }
  return llvm::CallingConv::C;
}

std::optional<llvm::Attribute::AttrKind> extension(ForeignExtensionKind value) {
  if (value == ForeignExtensionKind::Sign)
    return llvm::Attribute::SExt;
  if (value == ForeignExtensionKind::Zero)
    return llvm::Attribute::ZExt;
  return std::nullopt;
}

template <typename Callable>
void applyAttributes(
    Callable &callable, const ForeignAbiFunctionLayout &layout,
    llvm::LLVMContext &context,
    const std::function<llvm::Type *(TypeId)> &lower_object_type,
    const std::function<std::size_t(const ForeignAbiValueLayout &)>
        &physical_count) {
  callable.setCallingConv(callingConvention(layout.calling_convention));
  callable.addFnAttr(llvm::Attribute::NoUnwind);
  if (layout.result.kind == ForeignPassKind::Scalar)
    if (const auto attribute = extension(layout.result.extension))
      callable.addRetAttr(*attribute);
  std::uint32_t physical_index = 0;
  if (layout.result.kind == ForeignPassKind::Indirect) {
    callable.addParamAttr(
        physical_index,
        llvm::Attribute::getWithStructRetType(
            context, lower_object_type(layout.result.semantic_type)));
    callable.addParamAttr(
        physical_index++,
        llvm::Attribute::getWithAlignment(
            context, llvm::Align(layout.result.alignment)));
  }
  for (const auto &parameter : layout.parameters) {
    if (parameter.kind == ForeignPassKind::Scalar)
      if (const auto attribute = extension(parameter.extension))
        callable.addParamAttr(physical_index, *attribute);
    if (parameter.kind == ForeignPassKind::Indirect) {
      if (parameter.by_value)
        callable.addParamAttr(
            physical_index,
            llvm::Attribute::getWithByValType(
                context, lower_object_type(parameter.semantic_type)));
      callable.addParamAttr(
          physical_index,
          llvm::Attribute::getWithAlignment(
              context, llvm::Align(parameter.alignment)));
    }
    physical_index += static_cast<std::uint32_t>(physical_count(parameter));
  }
}

} // namespace

llvm::Type *LLVMInteropLoweringService::lowerForeignLane(
    const ForeignAbiLane &lane, llvm::LLVMContext &context) {
  switch (lane.kind) {
  case ForeignPhysicalKind::Pointer:
    return llvm::PointerType::getUnqual(context);
  case ForeignPhysicalKind::Integer:
    if (lane.elements > 1)
      return llvm::ArrayType::get(llvm::IntegerType::get(context, lane.width),
                                  lane.elements);
    return llvm::IntegerType::get(context, lane.width);
  case ForeignPhysicalKind::Float32:
    return llvm::Type::getFloatTy(context);
  case ForeignPhysicalKind::Float64:
    return llvm::Type::getDoubleTy(context);
  case ForeignPhysicalKind::Float32Vector2:
    return llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 2);
  case ForeignPhysicalKind::HomogeneousFloat:
    return llvm::ArrayType::get(
        lane.width == 32 ? llvm::Type::getFloatTy(context)
                         : llvm::Type::getDoubleTy(context),
        lane.elements);
  case ForeignPhysicalKind::Count:
    return nullptr;
  }
  return nullptr;
}

void LLVMInteropLoweringService::applyForeignAttributes(
    llvm::Function &function, const ForeignAbiFunctionLayout &layout,
    llvm::LLVMContext &context,
    const std::function<llvm::Type *(TypeId)> &lower_object_type,
    const std::function<std::size_t(const ForeignAbiValueLayout &)>
        &physical_count) {
  applyAttributes(function, layout, context, lower_object_type, physical_count);
}

void LLVMInteropLoweringService::applyForeignAttributes(
    llvm::CallBase &call, const ForeignAbiFunctionLayout &layout,
    llvm::LLVMContext &context,
    const std::function<llvm::Type *(TypeId)> &lower_object_type,
    const std::function<std::size_t(const ForeignAbiValueLayout &)>
        &physical_count) {
  applyAttributes(call, layout, context, lower_object_type, physical_count);
}

void LLVMCoroutineLoweringService::emitProtocolTrap(
    std::uint32_t reason, llvm::Module &module, llvm::IRBuilder<> &builder) {
  builder.CreateCall(
      module.getOrInsertFunction(
          "chtholly_next_runtime_v1_trap_coroutine",
          llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()},
                                   false)),
      {builder.getInt32(reason)});
  builder.CreateUnreachable();
}

llvm::Value *LLVMCoroutineLoweringService::makeChecked(
    llvm::Value *status, llvm::Value *storage, TypeId checked_type,
    llvm::IRBuilder<> &builder,
    const std::function<llvm::Type *(TypeId)> &lower_value_type) {
  llvm::Value *result = llvm::UndefValue::get(lower_value_type(checked_type));
  result = builder.CreateInsertValue(result, status, 0);
  return builder.CreateInsertValue(result, storage, 1);
}

std::vector<llvm::Type *> LLVMInteropLoweringService::foreignPhysicalTypes(
    const ForeignAbiValueLayout &layout, llvm::LLVMContext &context,
    const std::function<llvm::Type *(TypeId)> &lower_value_type,
    const std::function<llvm::Type *(const ForeignAbiLane &)> &lower_lane) {
  std::vector<llvm::Type *> result;
  if (layout.kind == ForeignPassKind::Scalar) {
    result.push_back(lower_value_type(layout.semantic_type));
    return result;
  }
  for (const auto &lane : layout.lanes)
    result.push_back(lower_lane(lane));
  (void)context;
  return result;
}

llvm::FunctionType *LLVMInteropLoweringService::foreignFunctionType(
    const ForeignAbiFunctionLayout &layout, llvm::LLVMContext &context,
    const std::function<std::vector<llvm::Type *>(
        const ForeignAbiValueLayout &)> &physical_types,
    const std::function<llvm::Type *(TypeId)> &lower_object_type) {
  std::vector<llvm::Type *> parameters;
  if (layout.result.kind == ForeignPassKind::Indirect)
    parameters.push_back(llvm::PointerType::getUnqual(context));
  for (const auto &parameter : layout.parameters) {
    const auto physical = physical_types(parameter);
    parameters.insert(parameters.end(), physical.begin(), physical.end());
  }
  llvm::Type *result = llvm::Type::getVoidTy(context);
  if (layout.result.kind != ForeignPassKind::Ignore &&
      layout.result.kind != ForeignPassKind::Indirect) {
    const auto physical = physical_types(layout.result);
    result = physical.size() == 1
                 ? physical.front()
                 : static_cast<llvm::Type *>(llvm::StructType::get(context, physical));
  }
  (void)lower_object_type;
  return llvm::FunctionType::get(result, parameters, layout.is_variadic);
}

} // namespace chtholly::compiler
