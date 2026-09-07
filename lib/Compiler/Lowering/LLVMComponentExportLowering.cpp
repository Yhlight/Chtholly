#include "LLVMInternal.h"

#include "chtholly/Compiler/ComponentABI.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"

namespace chtholly::compiler {

bool LLVMComponentExportLoweringService::emit(
    LLVMComponentExportState &state, std::string &error) {
  auto &context = state.context;
  auto &module = state.module;
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *payload = llvm::StructType::get(context, {i64, i64});
  auto *value_type =
      llvm::StructType::get(context, {i32, i32, i32, i32, payload});
  auto *wrapper_type = llvm::FunctionType::get(i32, {pointer, i32, pointer},
                                               false);
  const auto abi_kind = [](ComponentValueKind kind) {
    return static_cast<std::uint32_t>(kind);
  };
  const auto physical_type = [&](ComponentValueKind kind) -> llvm::Type * {
    switch (kind) {
    case ComponentValueKind::Void:
      return llvm::Type::getVoidTy(context);
    case ComponentValueKind::Bool:
      return llvm::Type::getInt1Ty(context);
    case ComponentValueKind::I8:
    case ComponentValueKind::U8:
      return llvm::Type::getInt8Ty(context);
    case ComponentValueKind::I16:
    case ComponentValueKind::U16:
      return llvm::Type::getInt16Ty(context);
    case ComponentValueKind::I32:
    case ComponentValueKind::U32:
      return llvm::Type::getInt32Ty(context);
    case ComponentValueKind::I64:
    case ComponentValueKind::U64:
      return llvm::Type::getInt64Ty(context);
    case ComponentValueKind::F32:
      return llvm::Type::getFloatTy(context);
    case ComponentValueKind::F64:
      return llvm::Type::getDoubleTy(context);
    case ComponentValueKind::Bytes:
      return llvm::StructType::get(context, {pointer, i64});
    case ComponentValueKind::Count:
      return nullptr;
    }
    return nullptr;
  };

  for (const auto &plan : state.exports) {
    if (plan.target_index == core::AnyId::InvalidIndex ||
        !state.functions.contains(plan.target_index)) {
      error = "component export plan has no lowered function target";
      return false;
    }
    auto *target = state.functions.at(plan.target_index);
    auto *target_type = target->getFunctionType();
    if (target_type->getNumParams() != plan.artifact.parameters.size() ||
        target_type->getReturnType() != physical_type(plan.artifact.result)) {
      error = "component export physical result or arity drifted";
      return false;
    }
    for (std::size_t index = 0; index < plan.artifact.parameters.size();
         ++index) {
      const auto *expected = physical_type(plan.artifact.parameters[index]);
      const auto *actual =
          target_type->getParamType(static_cast<unsigned>(index));
      if (actual == expected)
        continue;
      if (plan.artifact.parameters[index] == ComponentValueKind::Bytes &&
          actual->isStructTy() && actual->getStructNumElements() == 2 &&
          actual->getStructElementType(0)->isPointerTy() &&
          actual->getStructElementType(1)->isIntegerTy(64))
        continue;
      error = "component export physical parameter type drifted";
      return false;
    }

    auto *wrapper = llvm::Function::Create(
        wrapper_type, llvm::Function::ExternalLinkage,
        componentWrapperSymbol(plan.artifact.export_id), module);
    wrapper->setVisibility(llvm::GlobalValue::HiddenVisibility);
    auto arguments = wrapper->arg_begin();
    auto *argument_values = &*arguments++;
    auto *argument_count = &*arguments++;
    auto *result = &*arguments;
    auto *entry = llvm::BasicBlock::Create(context, "entry", wrapper);
    auto *invalid_argument =
        llvm::BasicBlock::Create(context, "invalid.argument", wrapper);
    auto *invalid_result =
        llvm::BasicBlock::Create(context, "invalid.result", wrapper);
    auto *invoke = llvm::BasicBlock::Create(context, "invoke", wrapper);
    llvm::IRBuilder<> builder(entry);
    auto *count_ok = builder.CreateICmpEQ(
        argument_count,
        llvm::ConstantInt::get(i32, plan.artifact.parameters.size()));
    auto *arguments_ok = plan.artifact.parameters.empty()
                             ? llvm::ConstantInt::getTrue(context)
                             : builder.CreateIsNotNull(argument_values);
    auto *result_ok = builder.CreateIsNotNull(result);
    builder.CreateCondBr(builder.CreateAnd(count_ok, arguments_ok), invoke,
                         invalid_argument);

    builder.SetInsertPoint(invalid_argument);
    builder.CreateRet(llvm::ConstantInt::get(i32, 1));
    builder.SetInsertPoint(invalid_result);
    builder.CreateRet(llvm::ConstantInt::get(i32, 2));
    builder.SetInsertPoint(invoke);
    builder.CreateCondBr(
        result_ok, llvm::BasicBlock::Create(context, "validate", wrapper),
        invalid_result);
    builder.SetInsertPoint(&wrapper->back());
    auto *result_size = builder.CreateStructGEP(value_type, result, 0);
    auto *result_size_ok =
        builder.CreateICmpEQ(builder.CreateLoad(i32, result_size),
                             llvm::ConstantInt::get(i32, 32));
    auto *validated = llvm::BasicBlock::Create(context, "call", wrapper);
    builder.CreateCondBr(result_size_ok, validated, invalid_result);
    builder.SetInsertPoint(validated);

    llvm::SmallVector<llvm::Value *, 8> call_arguments;
    for (std::size_t index = 0; index < plan.artifact.parameters.size();
         ++index) {
      auto *slot = builder.CreateGEP(value_type, argument_values,
                                     llvm::ConstantInt::get(i64, index));
      auto load_field = [&](unsigned field) {
        auto *address = builder.CreateStructGEP(value_type, slot, field);
        return builder.CreateLoad(i32, address);
      };
      auto *valid = builder.CreateAnd(
          builder.CreateICmpEQ(load_field(0),
                               llvm::ConstantInt::get(i32, 32)),
          builder.CreateAnd(
              builder.CreateICmpEQ(
                  load_field(1),
                  llvm::ConstantInt::get(
                      i32, abi_kind(plan.artifact.parameters[index]))),
              builder.CreateAnd(
                  builder.CreateICmpEQ(load_field(2),
                                       llvm::ConstantInt::get(i32, 0)),
                  builder.CreateICmpEQ(load_field(3),
                                       llvm::ConstantInt::get(i32, 0)))));
      auto *next = llvm::BasicBlock::Create(context, "arg.ok", wrapper);
      builder.CreateCondBr(valid, next, invalid_argument);
      builder.SetInsertPoint(next);
      auto *payload_address = builder.CreateStructGEP(value_type, slot, 4);
      auto *bits_address = builder.CreateStructGEP(payload, payload_address, 0);
      auto *bits = builder.CreateLoad(i64, bits_address);
      const auto kind = plan.artifact.parameters[index];
      if (kind == ComponentValueKind::Bytes) {
        auto *size_address =
            builder.CreateStructGEP(payload, payload_address, 1);
        auto *size = builder.CreateLoad(i64, size_address);
        auto *data = builder.CreateIntToPtr(bits, pointer);
        auto *pointer_ok = builder.CreateOr(
            builder.CreateICmpEQ(size, llvm::ConstantInt::get(i64, 0)),
            builder.CreateIsNotNull(data));
        auto *bytes_ok =
            llvm::BasicBlock::Create(context, "bytes.ok", wrapper);
        builder.CreateCondBr(pointer_ok, bytes_ok, invalid_argument);
        builder.SetInsertPoint(bytes_ok);
        llvm::Value *slice = llvm::PoisonValue::get(
            llvm::StructType::get(context, {pointer, i64}));
        slice = builder.CreateInsertValue(slice, data, 0);
        slice = builder.CreateInsertValue(slice, size, 1);
        call_arguments.push_back(slice);
      } else if (kind == ComponentValueKind::F32) {
        call_arguments.push_back(builder.CreateBitCast(
            builder.CreateTrunc(bits, llvm::Type::getInt32Ty(context)),
            llvm::Type::getFloatTy(context)));
      } else if (kind == ComponentValueKind::F64) {
        call_arguments.push_back(
            builder.CreateBitCast(bits, llvm::Type::getDoubleTy(context)));
      } else {
        const auto width =
            kind == ComponentValueKind::Bool ? 1U
            : kind == ComponentValueKind::I8 || kind == ComponentValueKind::U8
                ? 8U
            : kind == ComponentValueKind::I16 || kind == ComponentValueKind::U16
                ? 16U
            : kind == ComponentValueKind::I32 || kind == ComponentValueKind::U32
                ? 32U
                : 64U;
        if (kind == ComponentValueKind::Bool) {
          auto *bool_ok =
              builder.CreateICmpULE(bits, llvm::ConstantInt::get(i64, 1));
          auto *next_bool =
              llvm::BasicBlock::Create(context, "bool.ok", wrapper);
          builder.CreateCondBr(bool_ok, next_bool, invalid_argument);
          builder.SetInsertPoint(next_bool);
        }
        call_arguments.push_back(
            width == 64
                ? static_cast<llvm::Value *>(bits)
                : builder.CreateTrunc(
                      bits, llvm::IntegerType::get(context, width)));
      }
    }
    auto *call = builder.CreateCall(target, call_arguments);
    builder.CreateStore(llvm::ConstantInt::get(i32, 32), result_size);
    builder.CreateStore(
        llvm::ConstantInt::get(i32, abi_kind(plan.artifact.result)),
        builder.CreateStructGEP(value_type, result, 1));
    builder.CreateStore(llvm::ConstantInt::get(i32, 0),
                        builder.CreateStructGEP(value_type, result, 2));
    builder.CreateStore(llvm::ConstantInt::get(i32, 0),
                        builder.CreateStructGEP(value_type, result, 3));
    auto *result_payload = builder.CreateStructGEP(value_type, result, 4);
    auto *result_bits = builder.CreateStructGEP(payload, result_payload, 0);
    builder.CreateStore(llvm::ConstantInt::get(i64, 0), result_bits);
    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        builder.CreateStructGEP(payload, result_payload, 1));
    if (plan.artifact.result != ComponentValueKind::Void) {
      llvm::Value *encoded = call;
      if (plan.artifact.result == ComponentValueKind::F32)
        encoded = builder.CreateZExt(
            builder.CreateBitCast(call, llvm::Type::getInt32Ty(context)), i64);
      else if (plan.artifact.result == ComponentValueKind::F64)
        encoded = builder.CreateBitCast(call, i64);
      else if (call->getType() != i64)
        encoded = builder.CreateZExt(call, i64);
      builder.CreateStore(encoded, result_bits);
    }
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));
  }
  return true;
}

} // namespace chtholly::compiler
