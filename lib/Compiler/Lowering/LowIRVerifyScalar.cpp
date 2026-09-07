#include "LowIRVerificationContext.h"

#include "chtholly/Compiler/BuiltinOperator.h"

#include <ranges>

namespace chtholly::compiler::internal {

bool LowIRVerificationContext::verifyScalarInstruction(
    LowInstId id, std::string &error) const {
  const auto &value = low_ir_.inst(id);
  const auto *sem_ir_ = low_ir_.sem_ir_;
  const auto instruction_type = TypeId(value.type);
  const auto value_type = [&](std::uint32_t raw) {
    return TypeId(low_ir_.inst(LowInstId(raw)).type);
  };
  const auto valueBlock = [&](LowValueBlockId value_id) {
    return low_ir_.valueBlock(value_id);
  };
  const auto inst = [&](LowInstId value_id) -> const LowInst & {
    return low_ir_.inst(value_id);
  };
  switch (value.kind) {
    case LowInstKind::BuiltinUnary: {
      const auto raw = sem_ir_->integer(IntegerId(value.arg1));
      const auto operation = static_cast<BuiltinOperatorKind>(raw);
      const auto kind = sem_ir_->type(instruction_type).kind;
      if (raw < 0 || operation >= BuiltinOperatorKind::Count ||
          !isBuiltinUnaryOperator(operation) ||
          value_type(value.arg0) != instruction_type ||
          (operation == BuiltinOperatorKind::BitNot
               ? kind != SemTypeKind::Integer
               : kind != SemTypeKind::Integer && kind != SemTypeKind::Float)) {
        error = "builtin unary has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::BuiltinBinary: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      const auto raw = sem_ir_->integer(IntegerId(value.arg1));
      const auto operation = static_cast<BuiltinOperatorKind>(raw);
      if (raw < 0 || operation >= BuiltinOperatorKind::Count ||
          isBuiltinUnaryOperator(operation) || operands.size() != 2) {
        error = "builtin binary has invalid operands";
        return false;
      }
      const auto left = value_type(operands[0].index);
      const auto right = value_type(operands[1].index);
      const auto left_kind = sem_ir_->type(left).kind;
      const auto shift = isBuiltinShiftOperator(operation);
      const auto comparison = isBuiltinComparisonOperator(operation);
      const auto equality = isBuiltinEqualityOperator(operation);
      const auto integer_only = isBuiltinIntegerOnlyOperator(operation);
      const auto compatible =
          shift ? left_kind == SemTypeKind::Integer &&
                      sem_ir_->type(right).kind == SemTypeKind::Integer
                : left == right;
      const auto domain =
          integer_only ? left_kind == SemTypeKind::Integer
          : comparison
              ? left_kind == SemTypeKind::Integer ||
                    left_kind == SemTypeKind::Float ||
                    (equality &&
                     (left_kind == SemTypeKind::Bool ||
                      left_kind == SemTypeKind::RawPointer ||
                      left_kind == SemTypeKind::CFunctionPointer ||
                      left_kind == SemTypeKind::CVariadicFunctionPointer ||
                      left_kind == SemTypeKind::Char))
              : left_kind == SemTypeKind::Integer ||
                    left_kind == SemTypeKind::Float;
      const auto result_ok =
          operation == BuiltinOperatorKind::ThreeWay
              ? sem_ir_->type(instruction_type).kind == SemTypeKind::Nominal
          : comparison ? instruction_type == sem_ir_->boolType()
                       : instruction_type == left;
      if (!compatible || !domain || !result_ok) {
        error = "builtin binary has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::Add:
      if (value_type(value.arg0) != instruction_type ||
          value_type(value.arg1) != instruction_type ||
          (sem_ir_->type(instruction_type).kind != SemTypeKind::Integer &&
           sem_ir_->type(instruction_type).kind != SemTypeKind::Float)) {
        error = "add has invalid types";
        return false;
      }
      break;
    case LowInstKind::NumericConvert: {
      const auto source = sem_ir_->type(value_type(value.arg0)).kind;
      const auto target = sem_ir_->type(instruction_type).kind;
      if ((source != SemTypeKind::Integer && source != SemTypeKind::Float &&
           source != SemTypeKind::Char) ||
          (target != SemTypeKind::Integer && target != SemTypeKind::Float &&
           target != SemTypeKind::Char)) {
        error = "numeric conversion has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::CheckedNumericCast: {
      const auto source = sem_ir_->type(value_type(value.arg0)).kind;
      const auto &result = sem_ir_->type(instruction_type);
      if ((source != SemTypeKind::Integer && source != SemTypeKind::Float &&
           source != SemTypeKind::Char) ||
          result.kind != SemTypeKind::Nominal ||
          sem_ir_->typeBlock(TypeBlockId(result.arg1)).size() != 2) {
        error = "checked numeric cast has invalid types";
        return false;
      }
      const auto target = sem_ir_->typeBlock(TypeBlockId(result.arg1)).front();
      if (sem_ir_->type(target).kind != SemTypeKind::Integer &&
          sem_ir_->type(target).kind != SemTypeKind::Float &&
          sem_ir_->type(target).kind != SemTypeKind::Char) {
        error = "checked numeric cast has a non-numeric target";
        return false;
      }
      break;
    }
    case LowInstKind::Equal:
      if (instruction_type != sem_ir_->boolType() ||
          value_type(value.arg0) != value_type(value.arg1)) {
        error = "equality has invalid types";
        return false;
      }
      break;
    case LowInstKind::LogicalNot:
      if (instruction_type != sem_ir_->boolType() ||
          value_type(value.arg0) != sem_ir_->boolType()) {
        error = "logical not has invalid types";
        return false;
      }
      break;
    case LowInstKind::LogicalAnd:
    case LowInstKind::LogicalOr:
      if (instruction_type != sem_ir_->boolType() ||
          value_type(value.arg0) != sem_ir_->boolType() ||
          value_type(value.arg1) != sem_ir_->boolType()) {
        error = "logical binary operation has invalid types";
        return false;
      }
      break;
    case LowInstKind::Parameter:
      if (instruction_type != sem_ir_->voidType()) {
        error = "parameter binding has a non-void result type";
        return false;
      }
      break;
    case LowInstKind::ParameterValue:
      if (instruction_type == sem_ir_->voidType()) {
        error = "parameter value has a void type";
        return false;
      }
      break;
  default:
    return true;
  }
  return true;
}

} // namespace chtholly::compiler::internal
