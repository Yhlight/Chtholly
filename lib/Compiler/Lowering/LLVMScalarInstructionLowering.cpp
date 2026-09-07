#include "LLVMInternal.h"

#include "chtholly/Compiler/BuiltinOperator.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace chtholly::compiler {
namespace {

class ScalarEmitter {
public:
  explicit ScalarEmitter(LLVMScalarInstructionState &state) : state_(state) {}
  void emitArithmeticTrap(llvm::Value *condition, std::uint32_t reason,
                          std::string_view name, llvm::IRBuilder<> &builder,
                          llvm::Function &function) {
    auto *trap = llvm::BasicBlock::Create(state_.context, llvm::Twine(name) + ".trap",
                                          &function);
    auto *done = llvm::BasicBlock::Create(state_.context, llvm::Twine(name) + ".done",
                                          &function);
    builder.CreateCondBr(condition, trap, done);
    builder.SetInsertPoint(trap);
    auto *trap_type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(state_.context),
                                {llvm::Type::getInt32Ty(state_.context)}, false);
    builder.CreateCall(
        state_.module.getOrInsertFunction("chtholly_next_runtime_v1_trap_arithmetic",
                                    trap_type),
        {builder.getInt32(reason)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(done);
  }

  llvm::Value *checkedIntegerBinary(llvm::Intrinsic::ID intrinsic,
                                    llvm::Value *left, llvm::Value *right,
                                    TypeId type, std::string_view name,
                                    llvm::IRBuilder<> &builder,
                                    llvm::Function &function) {
    auto *operation = llvm::Intrinsic::getDeclaration(&state_.module, intrinsic,
                                                      {lowerValueType(type)});
    auto *pair = builder.CreateCall(operation, {left, right});
    auto *result =
        builder.CreateExtractValue(pair, 0, llvm::Twine(name) + ".result");
    auto *overflow =
        builder.CreateExtractValue(pair, 1, llvm::Twine(name) + ".overflow");
    emitArithmeticTrap(overflow, 7, name, builder, function);
    return result;
  }

  llvm::Value *lowerInst(LowBuiltinUnary inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto operation =
        static_cast<BuiltinOperatorKind>(state_.sem_ir.integer(inst.arg1));
    auto *operand = value(inst.arg0);
    const auto &type = state_.sem_ir.type(inst.type);
    switch (operation) {
    case BuiltinOperatorKind::Positive:
      return operand;
    case BuiltinOperatorKind::Negate:
      if (type.kind == SemTypeKind::Float)
        return builder.CreateFNeg(operand);
      return checkedIntegerBinary(
          type.arg1 != 0 ? llvm::Intrinsic::ssub_with_overflow
                         : llvm::Intrinsic::usub_with_overflow,
          llvm::ConstantInt::get(operand->getType(), 0), operand, inst.type,
          "negate", builder, function);
    case BuiltinOperatorKind::BitNot:
      return builder.CreateNot(operand);
    default:
      return nullptr;
    }
  }

  llvm::Value *lowerOrdering(TypeId result_type, TypeId operand_type,
                             llvm::Value *left, llvm::Value *right,
                             llvm::IRBuilder<> &builder,
                             llvm::Function &function) {
    const auto &type = state_.sem_ir.type(operand_type);
    llvm::Value *unordered = builder.getFalse();
    llvm::Value *less = nullptr;
    llvm::Value *greater = nullptr;
    if (type.kind == SemTypeKind::Float) {
      unordered = builder.CreateFCmpUNO(left, right, "compare.unordered");
      less = builder.CreateFCmpOLT(left, right, "compare.less");
      greater = builder.CreateFCmpOGT(left, right, "compare.greater");
    } else {
      less = type.arg1 != 0
                 ? builder.CreateICmpSLT(left, right, "compare.less")
                 : builder.CreateICmpULT(left, right, "compare.less");
      greater = type.arg1 != 0
                    ? builder.CreateICmpSGT(left, right, "compare.greater")
                    : builder.CreateICmpUGT(left, right, "compare.greater");
    }
    std::array<std::uint32_t, 4> variants{};
    const auto &nominal =
        state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(result_type).arg0));
    constexpr std::array<std::string_view, 4> names{"Less", "Equal", "Greater",
                                                    "Unordered"};
    for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
      const auto name =
          state_.sem_ir.identifier(state_.sem_ir.name(nominal.variants[index].name).text);
      for (std::uint32_t expected = 0; expected < names.size(); ++expected)
        if (name == names[expected])
          variants[expected] = index;
    }
    auto *tag = builder.CreateSelect(
        unordered, builder.getInt32(variants[3]),
        builder.CreateSelect(
            less, builder.getInt32(variants[0]),
            builder.CreateSelect(greater, builder.getInt32(variants[2]),
                                 builder.getInt32(variants[1]))));
    auto *storage =
        entryAlloca(function, lowerObjectType(result_type), "compare.ordering");
    builder.CreateLifetimeStart(storage);
    auto *record = llvm::cast<llvm::StructType>(lowerObjectType(result_type));
    builder.CreateStore(tag, builder.CreateStructGEP(record, storage, 0));
    return storage;
  }

  llvm::Value *lowerInst(LowBuiltinBinary inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto operation =
        static_cast<BuiltinOperatorKind>(state_.sem_ir.integer(inst.arg1));
    const auto operands = state_.low_ir.valueBlock(inst.arg0);
    assert(operands.size() == 2);
    auto *left = value(operands[0]);
    auto *right = value(operands[1]);
    const auto operand_type = TypeId(state_.low_ir.inst(operands[0]).type);
    const auto &type = state_.sem_ir.type(operand_type);

    if (operation == BuiltinOperatorKind::ThreeWay)
      return lowerOrdering(inst.type, operand_type, left, right, builder,
                           function);

    if (type.kind == SemTypeKind::Float) {
      switch (operation) {
      case BuiltinOperatorKind::Add:
        return builder.CreateFAdd(left, right);
      case BuiltinOperatorKind::Subtract:
        return builder.CreateFSub(left, right);
      case BuiltinOperatorKind::Multiply:
        return builder.CreateFMul(left, right);
      case BuiltinOperatorKind::Divide:
        return builder.CreateFDiv(left, right);
      case BuiltinOperatorKind::Equal:
        return builder.CreateFCmpOEQ(left, right);
      case BuiltinOperatorKind::NotEqual:
        return builder.CreateFCmpUNE(left, right);
      case BuiltinOperatorKind::Less:
        return builder.CreateFCmpOLT(left, right);
      case BuiltinOperatorKind::LessEqual:
        return builder.CreateFCmpOLE(left, right);
      case BuiltinOperatorKind::Greater:
        return builder.CreateFCmpOGT(left, right);
      case BuiltinOperatorKind::GreaterEqual:
        return builder.CreateFCmpOGE(left, right);
      default:
        return nullptr;
      }
    }

    switch (operation) {
    case BuiltinOperatorKind::Add:
      return checkedIntegerBinary(
          type.arg1 != 0 ? llvm::Intrinsic::sadd_with_overflow
                         : llvm::Intrinsic::uadd_with_overflow,
          left, right, operand_type, "add", builder, function);
    case BuiltinOperatorKind::Subtract:
      return checkedIntegerBinary(
          type.arg1 != 0 ? llvm::Intrinsic::ssub_with_overflow
                         : llvm::Intrinsic::usub_with_overflow,
          left, right, operand_type, "subtract", builder, function);
    case BuiltinOperatorKind::Multiply:
      return checkedIntegerBinary(
          type.arg1 != 0 ? llvm::Intrinsic::smul_with_overflow
                         : llvm::Intrinsic::umul_with_overflow,
          left, right, operand_type, "multiply", builder, function);
    case BuiltinOperatorKind::Divide:
    case BuiltinOperatorKind::Remainder: {
      const auto divide = operation == BuiltinOperatorKind::Divide;
      emitArithmeticTrap(
          builder.CreateICmpEQ(right,
                               llvm::ConstantInt::get(right->getType(), 0)),
          divide ? 1 : 2, divide ? "divide.zero" : "remainder.zero", builder,
          function);
      if (type.arg1 != 0) {
        auto *minimum = llvm::ConstantInt::get(
            right->getType(), llvm::APInt::getSignedMinValue(type.arg0));
        auto *minus_one = llvm::ConstantInt::getAllOnesValue(right->getType());
        emitArithmeticTrap(
            builder.CreateAnd(builder.CreateICmpEQ(left, minimum),
                              builder.CreateICmpEQ(right, minus_one)),
            3, divide ? "divide.overflow" : "remainder.overflow", builder,
            function);
      }
      if (divide)
        return type.arg1 != 0 ? builder.CreateSDiv(left, right)
                              : builder.CreateUDiv(left, right);
      return type.arg1 != 0 ? builder.CreateSRem(left, right)
                            : builder.CreateURem(left, right);
    }
    case BuiltinOperatorKind::ShiftLeft:
    case BuiltinOperatorKind::ShiftRight: {
      const auto right_type = TypeId(state_.low_ir.inst(operands[1]).type);
      const auto &count_type = state_.sem_ir.type(right_type);
      auto *invalid = builder.CreateICmpUGE(
          right, llvm::ConstantInt::get(right->getType(), type.arg0));
      if (count_type.arg1 != 0)
        invalid = builder.CreateOr(
            invalid, builder.CreateICmpSLT(
                         right, llvm::ConstantInt::get(right->getType(), 0)));
      emitArithmeticTrap(invalid, 4, "shift.count", builder, function);
      auto *count = builder.CreateIntCast(right, left->getType(), false);
      if (operation == BuiltinOperatorKind::ShiftRight)
        return type.arg1 != 0 ? builder.CreateAShr(left, count)
                              : builder.CreateLShr(left, count);
      auto *result = builder.CreateShl(left, count, "shift.result");
      auto *round_trip = type.arg1 != 0 ? builder.CreateAShr(result, count)
                                        : builder.CreateLShr(result, count);
      emitArithmeticTrap(builder.CreateICmpNE(round_trip, left), 7,
                         "shift.overflow", builder, function);
      return result;
    }
    case BuiltinOperatorKind::BitAnd:
      return builder.CreateAnd(left, right);
    case BuiltinOperatorKind::BitXor:
      return builder.CreateXor(left, right);
    case BuiltinOperatorKind::BitOr:
      return builder.CreateOr(left, right);
    case BuiltinOperatorKind::Equal:
      return builder.CreateICmpEQ(left, right);
    case BuiltinOperatorKind::NotEqual:
      return builder.CreateICmpNE(left, right);
    case BuiltinOperatorKind::Less:
      return type.arg1 != 0 ? builder.CreateICmpSLT(left, right)
                            : builder.CreateICmpULT(left, right);
    case BuiltinOperatorKind::LessEqual:
      return type.arg1 != 0 ? builder.CreateICmpSLE(left, right)
                            : builder.CreateICmpULE(left, right);
    case BuiltinOperatorKind::Greater:
      return type.arg1 != 0 ? builder.CreateICmpSGT(left, right)
                            : builder.CreateICmpUGT(left, right);
    case BuiltinOperatorKind::GreaterEqual:
      return type.arg1 != 0 ? builder.CreateICmpSGE(left, right)
                            : builder.CreateICmpUGE(left, right);
    default:
      return nullptr;
    }
  }

  llvm::Value *lowerInst(LowAdd inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto &type = state_.sem_ir.type(inst.type);
    if (type.kind == SemTypeKind::Float)
      return builder.CreateFAdd(value(inst.arg0), value(inst.arg1));
    const auto intrinsic = type.arg1 != 0 ? llvm::Intrinsic::sadd_with_overflow
                                          : llvm::Intrinsic::uadd_with_overflow;
    auto *operation = llvm::Intrinsic::getDeclaration(
        &state_.module, intrinsic, {lowerValueType(inst.type)});
    auto *pair =
        builder.CreateCall(operation, {value(inst.arg0), value(inst.arg1)});
    auto *result = builder.CreateExtractValue(pair, 0, "add.result");
    auto *overflow = builder.CreateExtractValue(pair, 1, "add.overflow");
    auto *trap = llvm::BasicBlock::Create(state_.context, "add.trap", &function);
    auto *done = llvm::BasicBlock::Create(state_.context, "add.done", &function);
    builder.CreateCondBr(overflow, trap, done);
    builder.SetInsertPoint(trap);
    auto *trap_type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(state_.context),
                                {llvm::Type::getInt32Ty(state_.context)}, false);
    builder.CreateCall(
        state_.module.getOrInsertFunction("chtholly_next_runtime_v1_trap_arithmetic",
                                    trap_type),
        {builder.getInt32(7)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(done);
    return result;
  }

  llvm::Value *lowerInst(LowNumericConvert inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *source_value = value(inst.arg0);
    const auto source_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto &source = state_.sem_ir.type(source_type);
    const auto &target = state_.sem_ir.type(inst.type);
    auto *llvm_target = lowerValueType(inst.type);
    const auto source_integer = source.kind == SemTypeKind::Integer ||
                                source.kind == SemTypeKind::Char;
    const auto target_integer = target.kind == SemTypeKind::Integer ||
                                target.kind == SemTypeKind::Char;
    if (source_integer && target_integer)
      return builder.CreateIntCast(source_value, llvm_target,
                                   source.kind == SemTypeKind::Integer &&
                                       source.arg1 != 0);
    if (source_integer &&
        target.kind == SemTypeKind::Float)
      return source.kind == SemTypeKind::Integer && source.arg1 != 0
                 ? builder.CreateSIToFP(source_value, llvm_target)
                 : builder.CreateUIToFP(source_value, llvm_target);
    if (source.kind == SemTypeKind::Float && target_integer) {
      const auto id = target.kind == SemTypeKind::Integer && target.arg1 != 0
                          ? llvm::Intrinsic::fptosi_sat
                                       : llvm::Intrinsic::fptoui_sat;
      auto *operation = llvm::Intrinsic::getDeclaration(
          &state_.module, id, {llvm_target, source_value->getType()});
      auto *result = builder.CreateCall(operation, {source_value});
      const auto upper =
          std::ldexp(1.0, target.arg1 != 0 ? target.arg0 - 1U : target.arg0);
      const auto lower = target.arg1 != 0 ? -upper : 0.0;
      auto *invalid = builder.CreateOr(
          builder.CreateFCmpUNO(source_value, source_value),
          builder.CreateOr(
              builder.CreateFCmpOLT(
                  source_value,
                  llvm::ConstantFP::get(source_value->getType(), lower)),
              builder.CreateFCmpOGE(
                  source_value,
                  llvm::ConstantFP::get(source_value->getType(), upper))));
      auto *trap = llvm::BasicBlock::Create(state_.context, "cast.trap", &function);
      auto *done = llvm::BasicBlock::Create(state_.context, "cast.done", &function);
      builder.CreateCondBr(invalid, trap, done);
      builder.SetInsertPoint(trap);
      auto *trap_type =
          llvm::FunctionType::get(llvm::Type::getVoidTy(state_.context),
                                  {llvm::Type::getInt32Ty(state_.context)}, false);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_arithmetic", trap_type),
          {builder.getInt32(5)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(done);
      return result;
    }
    return source.arg0 < target.arg0
               ? builder.CreateFPExt(source_value, llvm_target)
               : builder.CreateFPTrunc(source_value, llvm_target);
  }

  llvm::Value *lowerInst(LowCheckedNumericCast inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    auto *source_value = value(inst.arg0);
    const auto source_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto &source = state_.sem_ir.type(source_type);
    const auto result_arguments =
        state_.sem_ir.typeBlock(TypeBlockId(state_.sem_ir.type(inst.type).arg1));
    const auto target_type = result_arguments[0];
    const auto error_type = result_arguments[1];
    const auto &target = state_.sem_ir.type(target_type);
    auto *llvm_target = lowerValueType(target_type);
    auto *false_value = llvm::ConstantInt::getFalse(state_.context);
    llvm::Value *non_finite = false_value;
    llvm::Value *out_of_range = false_value;
    llvm::Value *converted = nullptr;
    llvm::Value *exact = nullptr;

    const auto saturated_float_to_int = [&](llvm::Value *input,
                                            TypeId integer_type) {
      const auto id = state_.sem_ir.type(integer_type).arg1 != 0
                          ? llvm::Intrinsic::fptosi_sat
                          : llvm::Intrinsic::fptoui_sat;
      auto *operation = llvm::Intrinsic::getDeclaration(
          &state_.module, id, {lowerValueType(integer_type), input->getType()});
      return builder.CreateCall(operation, {input}, "cast.saturated");
    };
    const auto is_non_finite = [&](llvm::Value *input) {
      auto *unordered = builder.CreateFCmpUNO(input, input);
      auto *positive_infinity = llvm::ConstantFP::getInfinity(input->getType());
      auto *negative_infinity =
          llvm::ConstantFP::getInfinity(input->getType(), true);
      return builder.CreateOr(
          unordered,
          builder.CreateOr(builder.CreateFCmpOEQ(input, positive_infinity),
                           builder.CreateFCmpOEQ(input, negative_infinity)));
    };

    const auto source_integer = source.kind == SemTypeKind::Integer ||
                                source.kind == SemTypeKind::Char;
    const auto target_integer = target.kind == SemTypeKind::Integer ||
                                target.kind == SemTypeKind::Char;
    const auto target_width = target.kind == SemTypeKind::Char ? 32U : target.arg0;
    const auto source_signed = source.kind == SemTypeKind::Integer && source.arg1 != 0;
    const auto target_signed = target.kind == SemTypeKind::Integer && target.arg1 != 0;
    if (source_integer && target_integer) {
      converted = builder.CreateIntCast(source_value, llvm_target,
                                        source_signed, "cast.value");
      auto *round_trip =
          builder.CreateIntCast(converted, lowerValueType(source_type),
                                target_signed, "cast.roundtrip");
      exact = builder.CreateICmpEQ(source_value, round_trip);
      if (source_signed && !target_signed)
        exact = builder.CreateAnd(
            exact, builder.CreateICmpSGE(
                       source_value,
                       llvm::ConstantInt::get(source_value->getType(), 0)));
      if (!source_signed && target_signed)
        exact = builder.CreateAnd(
            exact,
            builder.CreateICmpSGE(
                converted, llvm::ConstantInt::get(converted->getType(), 0)));
      if (target.kind == SemTypeKind::Char)
        exact = builder.CreateAnd(
            exact, builder.CreateICmpULE(
                       converted,
                       llvm::ConstantInt::get(converted->getType(), 0x10ffffU)));
      out_of_range = builder.CreateNot(exact);
    } else if (source_integer &&
               target.kind == SemTypeKind::Float) {
      converted =
          source_signed
              ? builder.CreateSIToFP(source_value, llvm_target, "cast.value")
              : builder.CreateUIToFP(source_value, llvm_target, "cast.value");
      auto *round_trip = saturated_float_to_int(converted, source_type);
      exact = builder.CreateICmpEQ(source_value, round_trip);
    } else if (source.kind == SemTypeKind::Float && target_integer) {
      non_finite = is_non_finite(source_value);
      const auto upper = target.kind == SemTypeKind::Char
                             ? static_cast<double>(0x110000U)
                             : std::ldexp(1.0, target_signed ? target_width - 1U
                                                              : target_width);
      const auto lower = target_signed ? -upper : 0.0;
      auto *lower_bound = llvm::ConstantFP::get(source_value->getType(), lower);
      auto *upper_bound = llvm::ConstantFP::get(source_value->getType(), upper);
      out_of_range =
          builder.CreateOr(builder.CreateFCmpOLT(source_value, lower_bound),
                           builder.CreateFCmpOGE(source_value, upper_bound));
      converted = target.kind == SemTypeKind::Char
                      ? builder.CreateFPToUI(source_value, llvm_target)
                      : saturated_float_to_int(source_value, target_type);
      auto *round_trip =
          target_signed
              ? builder.CreateSIToFP(converted, source_value->getType())
              : builder.CreateUIToFP(converted, source_value->getType());
      exact = builder.CreateFCmpOEQ(source_value, round_trip);
    } else {
      non_finite = is_non_finite(source_value);
      converted =
          source.arg0 < target.arg0
              ? builder.CreateFPExt(source_value, llvm_target, "cast.value")
              : builder.CreateFPTrunc(source_value, llvm_target, "cast.value");
      auto *converted_finite = builder.CreateFCmpORD(converted, converted);
      out_of_range = builder.CreateAnd(builder.CreateNot(non_finite),
                                       builder.CreateNot(converted_finite));
      auto *round_trip =
          source.arg0 < target.arg0
              ? builder.CreateFPTrunc(converted, source_value->getType())
              : builder.CreateFPExt(converted, source_value->getType());
      exact = builder.CreateFCmpOEQ(source_value, round_trip);
    }
    auto *valid = builder.CreateAnd(
        builder.CreateNot(non_finite),
        builder.CreateAnd(builder.CreateNot(out_of_range), exact));

    const auto &result_nominal =
        state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(inst.type).arg0));
    const auto &error_nominal =
        state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(error_type).arg0));
    const auto variant_named = [&](const SemNominalType &nominal,
                                   std::string_view name) {
      for (std::uint32_t index = 0; index < nominal.variants.size(); ++index)
        if (state_.sem_ir.identifier(
                state_.sem_ir.name(nominal.variants[index].name).text) == name)
          return index;
      return core::AnyId::InvalidIndex;
    };
    const auto ok_variant = variant_named(result_nominal, "Ok");
    const auto err_variant = variant_named(result_nominal, "Err");
    const auto inexact_variant = variant_named(error_nominal, "Inexact");
    const auto range_variant = variant_named(error_nominal, "OutOfRange");
    const auto nonfinite_variant = variant_named(error_nominal, "NonFinite");
    assert(ok_variant != core::AnyId::InvalidIndex &&
           err_variant != core::AnyId::InvalidIndex &&
           inexact_variant != core::AnyId::InvalidIndex &&
           range_variant != core::AnyId::InvalidIndex &&
           nonfinite_variant != core::AnyId::InvalidIndex);

    auto *storage =
        entryAlloca(function, lowerObjectType(inst.type), "checked.cast");
    builder.CreateLifetimeStart(storage);
    auto *success = llvm::BasicBlock::Create(state_.context, "cast.ok", &function);
    auto *failure = llvm::BasicBlock::Create(state_.context, "cast.err", &function);
    auto *done = llvm::BasicBlock::Create(state_.context, "cast.done", &function);
    builder.CreateCondBr(valid, success, failure);

    builder.SetInsertPoint(success);
    auto *result_record =
        llvm::cast<llvm::StructType>(lowerObjectType(inst.type));
    builder.CreateStore(builder.getInt32(ok_variant),
                        builder.CreateStructGEP(result_record, storage, 0));
    storeValueToObject(
        enumPayloadAddress(storage, inst.type, ok_variant, 0, builder),
        converted, target_type, builder);
    builder.CreateBr(done);

    builder.SetInsertPoint(failure);
    builder.CreateStore(builder.getInt32(err_variant),
                        builder.CreateStructGEP(result_record, storage, 0));
    auto *error_tag = builder.CreateSelect(
        non_finite, builder.getInt32(nonfinite_variant),
        builder.CreateSelect(out_of_range, builder.getInt32(range_variant),
                             builder.getInt32(inexact_variant)));
    auto *error_address =
        enumPayloadAddress(storage, inst.type, err_variant, 0, builder);
    auto *error_record =
        llvm::cast<llvm::StructType>(lowerObjectType(error_type));
    builder.CreateStore(
        error_tag, builder.CreateStructGEP(error_record, error_address, 0));
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    return storage;
  }

  llvm::Value *lowerInst(LowEqual inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return state_.sem_ir.type(TypeId(state_.low_ir.inst(inst.arg0).type)).kind ==
                   SemTypeKind::Float
               ? builder.CreateFCmpOEQ(value(inst.arg0), value(inst.arg1))
               : builder.CreateICmpEQ(value(inst.arg0), value(inst.arg1));
  }

  llvm::Value *lowerInst(LowLogicalNot inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return builder.CreateNot(value(inst.arg0));
  }

  llvm::Value *lowerInst(LowLogicalAnd inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return builder.CreateAnd(value(inst.arg0), value(inst.arg1));
  }

  llvm::Value *lowerInst(LowLogicalOr inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return builder.CreateOr(value(inst.arg0), value(inst.arg1));
  }

private:
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return state_.value(id);
  }
  [[nodiscard]] llvm::Type *lowerValueType(TypeId type) const {
    return state_.lower_value_type(type);
  }
  [[nodiscard]] llvm::Type *lowerObjectType(TypeId type) const {
    return state_.lower_object_type(type);
  }
  [[nodiscard]] llvm::AllocaInst *entryAlloca(
      llvm::Function &function, llvm::Type *type, llvm::StringRef name) const {
    return state_.entry_alloca(function, type, name);
  }
  void storeValueToObject(llvm::Value *destination, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) const {
    state_.store_value_to_object(destination, source, type, builder);
  }
  [[nodiscard]] llvm::Value *enumPayloadAddress(
      llvm::Value *owner, TypeId type, std::uint32_t variant,
      std::uint32_t field, llvm::IRBuilder<> &builder) const {
    return state_.enum_payload_address(owner, type, variant, field, builder);
  }

  LLVMScalarInstructionState &state_;
};

} // namespace

void LLVMScalarInstructionService::trap(
    llvm::Value *condition, std::uint32_t reason,
    std::string_view name, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMScalarInstructionState &state) {
  ScalarEmitter(state).emitArithmeticTrap(condition, reason, name, builder,
                                          function);
}

llvm::Value *LLVMScalarInstructionService::unary(
    LowBuiltinUnary inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::binary(
    LowBuiltinBinary inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::add(
    LowAdd inst, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::convert(
    LowNumericConvert inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::checkedCast(
    LowCheckedNumericCast inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::equal(
    LowEqual inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::logicalNot(
    LowLogicalNot inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::logicalAnd(
    LowLogicalAnd inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}
llvm::Value *LLVMScalarInstructionService::logicalOr(
    LowLogicalOr inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMScalarInstructionState &state) {
  return ScalarEmitter(state).lowerInst(inst, builder, function);
}

} // namespace chtholly::compiler
