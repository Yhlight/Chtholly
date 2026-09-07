#include "chtholly/Compiler/ConstantEvaluation.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/TypeLayout.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/APSInt.h>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace chtholly::compiler {
namespace {

struct Flow {
  enum class Kind : std::uint8_t {
    Normal,
    Return,
    Yield,
    Break,
    Continue,
    Failure,
  } kind = Kind::Normal;
  ConstantId value;
  std::uint32_t loop_distance = 0;
};

class EvaluationContext {
  using Environment = std::unordered_map<std::uint32_t, ConstantId>;

public:
  EvaluationContext(SemIR &sem_ir, ConstantEvaluationLimits limits)
      : sem_ir_(sem_ir), limits_(limits), steps_left_(limits.steps) {}

  ConstantEvaluationOutcome evaluateEntity(ConstantEntityId entity,
                                           Environment environment = {}) {
    failure_ = ConstantEvaluationFailure::None;
    failure_location_ = NodeId::invalid();
    steps_left_ = limits_.steps;
    call_depth_ = 0;
    auto value = evaluateConstantEntity(entity, environment);
    return outcome(value);
  }

  ConstantEvaluationOutcome evaluateExpression(InstId value) {
    failure_ = ConstantEvaluationFailure::None;
    failure_location_ = NodeId::invalid();
    steps_left_ = limits_.steps;
    call_depth_ = 0;
    Environment environment;
    return outcome(evaluate(value, environment));
  }

private:
  ConstantEvaluationOutcome outcome(ConstantId value) const {
    if (value.hasValue())
      return {{ConstantEvalState::Concrete, value},
              ConstantEvaluationFailure::None,
              NodeId::invalid()};
    return {{failure_ == ConstantEvaluationFailure::NotConstant
                 ? ConstantEvalState::NotConstant
                 : ConstantEvalState::Error,
             ConstantId::invalid()},
            failure_,
            failure_location_};
  }

  void fail(ConstantEvaluationFailure failure, InstId location) {
    if (failure_ != ConstantEvaluationFailure::None)
      return;
    failure_ = failure;
    failure_location_ =
        location.hasValue() ? sem_ir_.location(location) : NodeId::invalid();
  }

  bool step(InstId location) {
    if (steps_left_ != 0) {
      --steps_left_;
      return true;
    }
    fail(ConstantEvaluationFailure::StepLimit, location);
    return false;
  }

  ConstantId add(ConstantValueKind kind, TypeId type, std::uint64_t payload,
                 std::span<const ConstantId> elements = {},
                 bool target_dependent = false) {
    if (kind == ConstantValueKind::Integer ||
        kind == ConstantValueKind::Float || kind == ConstantValueKind::Bool ||
        kind == ConstantValueKind::Union || kind == ConstantValueKind::Enum ||
        kind == ConstantValueKind::ForeignEnum)
      payload = sem_ir_.addInteger(std::bit_cast<std::int64_t>(payload)).index;
    return sem_ir_.addConstantValue({kind, type, payload,
                                     sem_ir_.addConstantBlock(elements),
                                     target_dependent});
  }

  std::optional<llvm::APInt> integer(ConstantId id) const {
    if (!id.hasValue())
      return std::nullopt;
    const auto &value = sem_ir_.constantValue(id);
    const auto &type = sem_ir_.type(value.type);
    if (value.kind != ConstantValueKind::Integer ||
        (type.kind != SemTypeKind::Integer && type.kind != SemTypeKind::Char))
      return std::nullopt;
    const auto width = type.kind == SemTypeKind::Char ? 32U : type.arg0;
    return llvm::APInt(
        width, static_cast<std::uint64_t>(sem_ir_.integer(
                       IntegerId(static_cast<std::uint32_t>(value.payload)))));
  }

  std::optional<bool> boolean(ConstantId id) const {
    if (!id.hasValue())
      return std::nullopt;
    const auto &value = sem_ir_.constantValue(id);
    if (value.kind != ConstantValueKind::Bool)
      return std::nullopt;
    return sem_ir_.integer(
               IntegerId(static_cast<std::uint32_t>(value.payload))) != 0;
  }

  ConstantId evaluateConstantEntity(ConstantEntityId id,
                                    Environment &environment) {
    if (!id.hasValue() || id.index >= sem_ir_.constantEntityCount()) {
      fail(ConstantEvaluationFailure::NotConstant, InstId::invalid());
      return ConstantId::invalid();
    }
    const auto cached = sem_ir_.constantEntity(id).result;
    if (environment.empty() && cached.isConcrete())
      return cached.value;
    if (!evaluating_entities_.insert(id.index).second) {
      fail(ConstantEvaluationFailure::Cycle, sem_ir_.constantEntity(id).value);
      return ConstantId::invalid();
    }
    const auto entity = sem_ir_.constantEntity(id);
    auto value = evaluate(entity.value, environment);
    evaluating_entities_.erase(id.index);
    if (environment.empty()) {
      auto updated = entity;
      updated.result =
          value.hasValue()
              ? ConstantEvalResult{ConstantEvalState::Concrete, value}
              : ConstantEvalResult{
                    failure_ == ConstantEvaluationFailure::NotConstant
                        ? ConstantEvalState::NotConstant
                        : ConstantEvalState::Error,
                    ConstantId::invalid()};
      sem_ir_.setConstantEntity(id, std::move(updated));
    }
    return value;
  }

  ConstantId evaluate(InstId id, Environment &environment) {
    if (!id.hasValue() || failure_ != ConstantEvaluationFailure::None ||
        !step(id))
      return ConstantId::invalid();
    const auto inst = sem_ir_.inst(id);
    const auto type = TypeId(inst.type);
    switch (inst.kind) {
    case SemInstKind::IntegerLiteral:
      return add(
          ConstantValueKind::Integer, type,
          static_cast<std::uint64_t>(sem_ir_.integer(IntegerId(inst.arg0))));
    case SemInstKind::CharLiteral:
      return add(
          ConstantValueKind::Integer, type,
          static_cast<std::uint64_t>(sem_ir_.integer(IntegerId(inst.arg0))));
    case SemInstKind::FloatLiteral:
      return add(
          ConstantValueKind::Float, type,
          static_cast<std::uint64_t>(sem_ir_.integer(IntegerId(inst.arg0))));
    case SemInstKind::BoolLiteral:
      return add(ConstantValueKind::Bool, type,
                 sem_ir_.integer(IntegerId(inst.arg0)) != 0 ? 1U : 0U);
    case SemInstKind::StringLiteral:
      return add(ConstantValueKind::String, type, inst.arg0);
    case SemInstKind::NullPointer:
      return add(ConstantValueKind::Null, type, 0);
    case SemInstKind::VoidValue:
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    case SemInstKind::ConstantRef:
      return evaluateConstantEntity(ConstantEntityId(inst.arg0), environment);
    case SemInstKind::StaticRef:
      fail(ConstantEvaluationFailure::NotConstant, id);
      return ConstantId::invalid();
    case SemInstKind::NameRef: {
      const auto found = environment.find(inst.arg0);
      if (found != environment.end())
        return found->second;
      fail(ConstantEvaluationFailure::NotConstant, id);
      return ConstantId::invalid();
    }
    case SemInstKind::BorrowLocal: {
      if (sem_ir_.type(type).kind != SemTypeKind::Reference ||
          sem_ir_.referenceMutability(type) !=
              SemReferenceMutability::ReadOnly) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      const auto found = environment.find(inst.arg0);
      if (found != environment.end())
        return found->second;
      fail(ConstantEvaluationFailure::NotConstant, id);
      return ConstantId::invalid();
    }
    case SemInstKind::BorrowPlace:
      if (sem_ir_.type(type).kind != SemTypeKind::Reference ||
          sem_ir_.referenceMutability(type) !=
              SemReferenceMutability::ReadOnly) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      // Immutable constant evaluation observes references as value snapshots.
      // Assignment remains unsupported, so this does not model mutable aliasing.
      return evaluate(InstId(inst.arg0), environment);
    case SemInstKind::Dereference: {
      const auto operand = InstId(inst.arg0);
      const auto operand_type = TypeId(sem_ir_.inst(operand).type);
      if (sem_ir_.type(operand_type).kind != SemTypeKind::Reference ||
          sem_ir_.referenceMutability(operand_type) !=
              SemReferenceMutability::ReadOnly) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      return evaluate(operand, environment);
    }
    case SemInstKind::ArrayLiteral:
    case SemInstKind::TupleLiteral:
    case SemInstKind::AggregateInit:
    case SemInstKind::EnumInit: {
      std::vector<ConstantId> elements;
      bool target_dependent = false;
      for (const auto element : sem_ir_.instBlock(InstBlockId(inst.arg0))) {
        const auto value = evaluate(element, environment);
        if (!value.hasValue())
          return ConstantId::invalid();
        target_dependent |= sem_ir_.constantValue(value).target_dependent;
        elements.push_back(value);
      }
      const auto kind = inst.kind == SemInstKind::ArrayLiteral
                            ? ConstantValueKind::Array
                        : inst.kind == SemInstKind::TupleLiteral
                            ? ConstantValueKind::Tuple
                        : inst.kind == SemInstKind::AggregateInit
                            ? ConstantValueKind::Aggregate
                            : ConstantValueKind::Enum;
      const auto payload = inst.kind == SemInstKind::EnumInit
                               ? static_cast<std::uint64_t>(
                                     sem_ir_.integer(IntegerId(inst.arg1)))
                               : 0U;
      return add(kind, type, payload, elements, target_dependent);
    }
    case SemInstKind::UnionInit: {
      const auto value = evaluate(InstId(inst.arg0), environment);
      if (!value.hasValue())
        return ConstantId::invalid();
      const std::array elements{value};
      return add(
          ConstantValueKind::Union, type,
          static_cast<std::uint64_t>(sem_ir_.integer(IntegerId(inst.arg1))),
          elements, sem_ir_.constantValue(value).target_dependent);
    }
    case SemInstKind::BuiltinUnary:
      return evaluateUnary(id, inst, environment);
    case SemInstKind::BuiltinBinary:
      return evaluateBinary(id, inst, environment);
    case SemInstKind::NumericConvert:
      return evaluateConversion(id, inst, environment);
    case SemInstKind::LogicalNot: {
      const auto value = boolean(evaluate(InstId(inst.arg0), environment));
      if (!value) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      return add(ConstantValueKind::Bool, type, *value ? 0U : 1U);
    }
    case SemInstKind::LogicalAnd:
    case SemInstKind::LogicalOr: {
      const auto left = boolean(evaluate(InstId(inst.arg0), environment));
      if (!left) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      if ((inst.kind == SemInstKind::LogicalAnd && !*left) ||
          (inst.kind == SemInstKind::LogicalOr && *left))
        return add(ConstantValueKind::Bool, type, *left ? 1U : 0U);
      const auto right = boolean(evaluate(InstId(inst.arg1), environment));
      if (!right) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      return add(ConstantValueKind::Bool, type, *right ? 1U : 0U);
    }
    case SemInstKind::If:
      return evaluateIf(id, inst, environment);
    case SemInstKind::Call:
      return evaluateCall(id, inst, environment);
    case SemInstKind::CompilerIntrinsicCall: {
      if (sem_ir_.functionIntrinsicRole(FunctionRefId(inst.arg0)) !=
          CompilerIntrinsicRole::WrappingMul) {
        fail(ConstantEvaluationFailure::NotConstant, id);
        return ConstantId::invalid();
      }
      const auto operands = sem_ir_.instBlock(InstBlockId(inst.arg1));
      if (operands.size() != 2)
        return ConstantId::invalid();
      const auto left = integer(evaluate(operands[0], environment));
      const auto right = integer(evaluate(operands[1], environment));
      const auto &result_type = sem_ir_.type(TypeId(inst.type));
      if (!left || !right || result_type.kind != SemTypeKind::Integer ||
          left->getBitWidth() != result_type.arg0 ||
          right->getBitWidth() != result_type.arg0) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      const auto result = *left * *right;
      return add(ConstantValueKind::Integer, TypeId(inst.type),
                 result.getZExtValue());
    }
    case SemInstKind::StructFieldAccess:
    case SemInstKind::UnionFieldAccess:
    case SemInstKind::EnumPayloadAccess: {
      const auto base = evaluate(InstId(inst.arg0), environment);
      if (!base.hasValue())
        return ConstantId::invalid();
      const auto index = inst.kind == SemInstKind::EnumPayloadAccess
                             ? static_cast<std::uint32_t>(inst.arg1)
                             : static_cast<std::uint32_t>(
                                   sem_ir_.integer(IntegerId(inst.arg1)));
      const auto elements =
          sem_ir_.constantBlock(sem_ir_.constantValue(base).elements);
      if (index >= elements.size()) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      return elements[index];
    }
    case SemInstKind::Index: {
      const auto base = evaluate(InstId(inst.arg0), environment);
      const auto index_value = evaluate(InstId(inst.arg1), environment);
      const auto index = integer(index_value);
      if (!base.hasValue() || !index || index->getActiveBits() > 64) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      const auto elements =
          sem_ir_.constantBlock(sem_ir_.constantValue(base).elements);
      const auto raw = index->getZExtValue();
      if (raw >= elements.size()) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      return elements[static_cast<std::size_t>(raw)];
    }
    case SemInstKind::Copy:
    case SemInstKind::Move:
      return evaluate(InstId(inst.arg0), environment);
    case SemInstKind::SizeOf:
    case SemInstKind::AlignOf: {
      std::string error;
      const auto layout = querySemanticTypeLayout(
          sem_ir_, TypeId(inst.arg0), sem_ir_.targetLayout(), error);
      if (!layout) {
        fail(ConstantEvaluationFailure::InvalidOperation, id);
        return ConstantId::invalid();
      }
      return add(ConstantValueKind::Integer, type,
                 inst.kind == SemInstKind::SizeOf ? layout->size
                                                  : layout->alignment,
                 {}, true);
    }
    default:
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
  }

  ConstantId evaluateUnary(InstId id, const SemInst &inst,
                           Environment &environment) {
    const auto operand = evaluate(InstId(inst.arg0), environment);
    const auto bits = integer(operand);
    if (!bits) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    const auto operation =
        static_cast<BuiltinOperatorKind>(sem_ir_.integer(IntegerId(inst.arg1)));
    llvm::APInt result = *bits;
    bool overflow = false;
    if (operation == BuiltinOperatorKind::Negate) {
      const llvm::APInt zero(bits->getBitWidth(), 0);
      const auto &type = sem_ir_.type(TypeId(inst.type));
      result = type.arg1 != 0 ? zero.ssub_ov(*bits, overflow)
                              : zero.usub_ov(*bits, overflow);
    } else if (operation == BuiltinOperatorKind::BitNot) {
      result.flipAllBits();
    } else if (operation != BuiltinOperatorKind::Positive) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    if (overflow) {
      fail(ConstantEvaluationFailure::Overflow, id);
      return ConstantId::invalid();
    }
    return add(ConstantValueKind::Integer, TypeId(inst.type),
               result.getZExtValue());
  }

  ConstantId evaluateBinary(InstId id, const SemInst &inst,
                            Environment &environment) {
    const auto operands = sem_ir_.instBlock(InstBlockId(inst.arg0));
    if (operands.size() != 2) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    const auto left_id = evaluate(operands[0], environment);
    const auto right_id = evaluate(operands[1], environment);
    if (!left_id.hasValue() || !right_id.hasValue())
      return ConstantId::invalid();
    const auto operation =
        static_cast<BuiltinOperatorKind>(sem_ir_.integer(IntegerId(inst.arg1)));
    const auto &left_value = sem_ir_.constantValue(left_id);
    const auto &right_value = sem_ir_.constantValue(right_id);
    const auto left = integer(left_id);
    const auto right = integer(right_id);
    if (left && right)
      return evaluateIntegerBinary(id, operation, *left, *right,
                                   left_value.type, TypeId(inst.type));
    if (left_value.kind == ConstantValueKind::Float &&
        right_value.kind == ConstantValueKind::Float)
      return evaluateFloatBinary(id, operation, left_value, right_value,
                                 TypeId(inst.type));
    if ((operation == BuiltinOperatorKind::Equal ||
         operation == BuiltinOperatorKind::NotEqual) &&
        left_value.kind == right_value.kind) {
      const auto equal = left_value == right_value;
      return add(
          ConstantValueKind::Bool, TypeId(inst.type),
          (operation == BuiltinOperatorKind::Equal ? equal : !equal) ? 1U : 0U);
    }
    fail(ConstantEvaluationFailure::InvalidOperation, id);
    return ConstantId::invalid();
  }

  ConstantId evaluateIntegerBinary(InstId id, BuiltinOperatorKind operation,
                                   const llvm::APInt &left,
                                   const llvm::APInt &right,
                                   TypeId operand_type_id, TypeId result_type) {
    const auto &operand_type = sem_ir_.type(operand_type_id);
    const auto is_signed =
        operand_type.kind == SemTypeKind::Integer && operand_type.arg1 != 0;
    bool overflow = false;
    llvm::APInt result = left;
    bool comparison = false;
    bool comparison_value = false;
    switch (operation) {
    case BuiltinOperatorKind::Add:
      result = is_signed ? left.sadd_ov(right, overflow)
                         : left.uadd_ov(right, overflow);
      break;
    case BuiltinOperatorKind::Subtract:
      result = is_signed ? left.ssub_ov(right, overflow)
                         : left.usub_ov(right, overflow);
      break;
    case BuiltinOperatorKind::Multiply:
      result = is_signed ? left.smul_ov(right, overflow)
                         : left.umul_ov(right, overflow);
      break;
    case BuiltinOperatorKind::Divide:
    case BuiltinOperatorKind::Remainder:
      if (right.isZero()) {
        fail(operation == BuiltinOperatorKind::Divide
                 ? ConstantEvaluationFailure::DivisionByZero
                 : ConstantEvaluationFailure::RemainderByZero,
             id);
        return ConstantId::invalid();
      }
      if (is_signed && left.isMinSignedValue() && right.isAllOnes()) {
        fail(ConstantEvaluationFailure::Overflow, id);
        return ConstantId::invalid();
      }
      result = operation == BuiltinOperatorKind::Divide
                   ? (is_signed ? left.sdiv(right) : left.udiv(right))
                   : (is_signed ? left.srem(right) : left.urem(right));
      break;
    case BuiltinOperatorKind::ShiftLeft:
    case BuiltinOperatorKind::ShiftRight:
      if ((is_signed && right.isNegative()) || right.getActiveBits() > 64 ||
          right.getZExtValue() >= left.getBitWidth()) {
        fail(ConstantEvaluationFailure::ShiftOutOfRange, id);
        return ConstantId::invalid();
      }
      if (operation == BuiltinOperatorKind::ShiftLeft) {
        const auto amount = static_cast<unsigned>(right.getZExtValue());
        result = is_signed ? left.sshl_ov(amount, overflow)
                           : left.ushl_ov(amount, overflow);
      } else {
        result = is_signed
                     ? left.ashr(static_cast<unsigned>(right.getZExtValue()))
                     : left.lshr(static_cast<unsigned>(right.getZExtValue()));
      }
      break;
    case BuiltinOperatorKind::BitAnd:
      result = left & right;
      break;
    case BuiltinOperatorKind::BitXor:
      result = left ^ right;
      break;
    case BuiltinOperatorKind::BitOr:
      result = left | right;
      break;
    case BuiltinOperatorKind::Equal:
      comparison = true;
      comparison_value = left == right;
      break;
    case BuiltinOperatorKind::NotEqual:
      comparison = true;
      comparison_value = left != right;
      break;
    case BuiltinOperatorKind::Less:
      comparison = true;
      comparison_value = is_signed ? left.slt(right) : left.ult(right);
      break;
    case BuiltinOperatorKind::LessEqual:
      comparison = true;
      comparison_value = is_signed ? left.sle(right) : left.ule(right);
      break;
    case BuiltinOperatorKind::Greater:
      comparison = true;
      comparison_value = is_signed ? left.sgt(right) : left.ugt(right);
      break;
    case BuiltinOperatorKind::GreaterEqual:
      comparison = true;
      comparison_value = is_signed ? left.sge(right) : left.uge(right);
      break;
    case BuiltinOperatorKind::ThreeWay: {
      const auto ordering = left == right ? 1U
                            : (is_signed ? left.slt(right) : left.ult(right))
                                ? 0U
                                : 2U;
      return addOrdering(id, result_type, ordering);
    }
    default:
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    if (overflow) {
      fail(ConstantEvaluationFailure::Overflow, id);
      return ConstantId::invalid();
    }
    return comparison ? add(ConstantValueKind::Bool, result_type,
                            comparison_value ? 1U : 0U)
                      : add(ConstantValueKind::Integer, result_type,
                            result.getZExtValue());
  }

  ConstantId evaluateFloatBinary(InstId id, BuiltinOperatorKind operation,
                                 const ConstantValue &left,
                                 const ConstantValue &right,
                                 TypeId result_type) {
    const auto width = sem_ir_.type(left.type).arg0;
    const auto left_bits = static_cast<std::uint64_t>(
        sem_ir_.integer(IntegerId(static_cast<std::uint32_t>(left.payload))));
    const auto right_bits = static_cast<std::uint64_t>(
        sem_ir_.integer(IntegerId(static_cast<std::uint32_t>(right.payload))));
    const auto lhs = width == 32 ? static_cast<double>(std::bit_cast<float>(
                                       static_cast<std::uint32_t>(left_bits)))
                                 : std::bit_cast<double>(left_bits);
    const auto rhs = width == 32 ? static_cast<double>(std::bit_cast<float>(
                                       static_cast<std::uint32_t>(right_bits)))
                                 : std::bit_cast<double>(right_bits);
    bool comparison = false;
    bool comparison_value = false;
    double result = lhs;
    switch (operation) {
    case BuiltinOperatorKind::Add:
      result = lhs + rhs;
      break;
    case BuiltinOperatorKind::Subtract:
      result = lhs - rhs;
      break;
    case BuiltinOperatorKind::Multiply:
      result = lhs * rhs;
      break;
    case BuiltinOperatorKind::Divide:
      result = lhs / rhs;
      break;
    case BuiltinOperatorKind::Equal:
      comparison = true;
      comparison_value = lhs == rhs;
      break;
    case BuiltinOperatorKind::NotEqual:
      comparison = true;
      comparison_value = lhs != rhs;
      break;
    case BuiltinOperatorKind::Less:
      comparison = true;
      comparison_value = lhs < rhs;
      break;
    case BuiltinOperatorKind::LessEqual:
      comparison = true;
      comparison_value = lhs <= rhs;
      break;
    case BuiltinOperatorKind::Greater:
      comparison = true;
      comparison_value = lhs > rhs;
      break;
    case BuiltinOperatorKind::GreaterEqual:
      comparison = true;
      comparison_value = lhs >= rhs;
      break;
    case BuiltinOperatorKind::ThreeWay:
      return addOrdering(id, result_type,
                         std::isunordered(lhs, rhs) ? 3U
                         : lhs < rhs                ? 0U
                         : lhs == rhs               ? 1U
                                                    : 2U);
    default:
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    if (comparison)
      return add(ConstantValueKind::Bool, result_type,
                 comparison_value ? 1U : 0U);
    const auto payload =
        width == 32 ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(
                          static_cast<float>(result)))
                    : std::bit_cast<std::uint64_t>(result);
    return add(ConstantValueKind::Float, result_type, payload);
  }

  ConstantId addOrdering(InstId id, TypeId result_type,
                         std::uint32_t ordering) {
    const auto &type = sem_ir_.type(result_type);
    if (type.kind != SemTypeKind::Nominal || ordering >= 4) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(type.arg0));
    constexpr std::array<std::string_view, 4> names{"Less", "Equal", "Greater",
                                                    "Unordered"};
    for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
      const auto &variant = nominal.variants[index];
      if (variant.shape == SemEnumPayloadShape::Unit &&
          sem_ir_.identifier(sem_ir_.name(variant.name).text) ==
              names[ordering])
        return add(ConstantValueKind::Enum, result_type, index);
    }
    fail(ConstantEvaluationFailure::InvalidOperation, id);
    return ConstantId::invalid();
  }

  ConstantId evaluateConversion(InstId id, const SemInst &inst,
                                Environment &environment) {
    const auto source_id = evaluate(InstId(inst.arg0), environment);
    if (!source_id.hasValue())
      return ConstantId::invalid();
    const auto &source = sem_ir_.constantValue(source_id);
    const auto &source_type = sem_ir_.type(source.type);
    const auto &target_type = sem_ir_.type(TypeId(inst.type));
    const auto source_integer = source_type.kind == SemTypeKind::Integer ||
                                source_type.kind == SemTypeKind::Char;
    const auto target_integer = target_type.kind == SemTypeKind::Integer ||
                                target_type.kind == SemTypeKind::Char;
    if (source_integer && target_integer) {
      auto bits = *integer(source_id);
      const auto target_width = target_type.kind == SemTypeKind::Char
                                    ? 32U
                                    : target_type.arg0;
      bits = source_type.kind == SemTypeKind::Integer && source_type.arg1 != 0
                 ? bits.sextOrTrunc(target_width)
                 : bits.zextOrTrunc(target_width);
      return add(ConstantValueKind::Integer, TypeId(inst.type),
                 bits.getZExtValue(), {}, source.target_dependent);
    }
    if (source_integer &&
        target_type.kind == SemTypeKind::Float) {
      llvm::APFloat result(target_type.arg0 == 32
                               ? llvm::APFloat::IEEEsingle()
                               : llvm::APFloat::IEEEdouble());
      (void)result.convertFromAPInt(*integer(source_id),
                                    source_type.kind == SemTypeKind::Integer &&
                                        source_type.arg1 != 0,
                                    llvm::APFloat::rmNearestTiesToEven);
      return add(ConstantValueKind::Float, TypeId(inst.type),
                 result.bitcastToAPInt().getZExtValue(), {},
                 source.target_dependent);
    }
    if (source_type.kind == SemTypeKind::Float) {
      const auto bits = static_cast<std::uint64_t>(sem_ir_.integer(
          IntegerId(static_cast<std::uint32_t>(source.payload))));
      llvm::APFloat result(source_type.arg0 == 32 ? llvm::APFloat::IEEEsingle()
                                                  : llvm::APFloat::IEEEdouble(),
                           llvm::APInt(source_type.arg0, bits));
      if (target_type.kind == SemTypeKind::Float) {
        bool loses_info = false;
        (void)result.convert(target_type.arg0 == 32
                                 ? llvm::APFloat::IEEEsingle()
                                 : llvm::APFloat::IEEEdouble(),
                             llvm::APFloat::rmNearestTiesToEven, &loses_info);
        return add(ConstantValueKind::Float, TypeId(inst.type),
                   result.bitcastToAPInt().getZExtValue(), {},
                   source.target_dependent);
      }
      if (target_integer) {
        const auto target_width = target_type.kind == SemTypeKind::Char
                                      ? 32U
                                      : target_type.arg0;
        llvm::APSInt converted(target_width,
                               target_type.kind == SemTypeKind::Integer
                                   ? target_type.arg1 == 0
                                   : true);
        bool exact = false;
        const auto status = result.convertToInteger(
            converted, llvm::APFloat::rmTowardZero, &exact);
        if ((status & llvm::APFloat::opInvalidOp) != 0 ||
            (status & llvm::APFloat::opOverflow) != 0) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return ConstantId::invalid();
        }
        if (target_type.kind == SemTypeKind::Char &&
            converted.getZExtValue() > 0x10ffffU) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return ConstantId::invalid();
        }
        return add(ConstantValueKind::Integer, TypeId(inst.type),
                   converted.getZExtValue(), {}, source.target_dependent);
      }
    }
    if (source.type == TypeId(inst.type))
      return source_id;
    fail(ConstantEvaluationFailure::InvalidOperation, id);
    return ConstantId::invalid();
  }

  ConstantId evaluateIf(InstId id, const SemInst &inst,
                        Environment &environment) {
    const auto condition = boolean(evaluate(InstId(inst.arg0), environment));
    if (!condition) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    const auto arms = sem_ir_.instBlock(InstBlockId(inst.arg1));
    const auto index = *condition ? 0U : 1U;
    if (index >= arms.size())
      return ConstantId::invalid();
    const auto arm = sem_ir_.inst(arms[index]);
    const auto flow = executeBlock(InstBlockId(arm.arg0), environment);
    if (flow.kind == Flow::Kind::Yield || flow.kind == Flow::Kind::Return)
      return flow.value;
    fail(ConstantEvaluationFailure::InvalidOperation, id);
    return ConstantId::invalid();
  }

  ConstantId evaluateCall(InstId id, const SemInst &inst,
                          Environment &environment) {
    const auto &reference = sem_ir_.functionRef(FunctionRefId(inst.arg0));
    auto local_function = reference.local_function;
    if (!local_function.hasValue() && reference.public_entity.hasValue()) {
      for (std::uint32_t index = 0; index < sem_ir_.functionRefCount();
           ++index) {
        const auto &candidate = sem_ir_.functionRef(FunctionRefId(index));
        if (candidate.public_entity != reference.public_entity ||
            !candidate.local_function.hasValue())
          continue;
        const auto &function = sem_ir_.function(candidate.local_function);
        if ((function.flags & SemFunctionEvaluatorArtifact) != 0) {
          local_function = candidate.local_function;
          break;
        }
      }
    }
    if (!local_function.hasValue()) {
      fail(ConstantEvaluationFailure::NotConstant, id);
      return ConstantId::invalid();
    }
    const auto function = sem_ir_.function(local_function);
    if ((function.flags & SemFunctionConst) == 0 ||
        sem_ir_.functionDeclaration(local_function).kind !=
            SemCallableDeclarationKind::Definition) {
      fail(ConstantEvaluationFailure::NotConstant, id);
      return ConstantId::invalid();
    }
    if (call_depth_ >= limits_.call_depth) {
      fail(ConstantEvaluationFailure::CallDepthLimit, id);
      return ConstantId::invalid();
    }
    const auto argument_insts = sem_ir_.instBlock(InstBlockId(inst.arg1));
    const auto parameters = sem_ir_.localBlock(function.parameters);
    if (argument_insts.size() != parameters.size()) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    Environment callee;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const auto argument = evaluate(argument_insts[index], environment);
      if (!argument.hasValue())
        return ConstantId::invalid();
      callee.emplace(parameters[index].index, argument);
    }
    ++call_depth_;
    const auto flow = executeBlock(function.body, callee);
    --call_depth_;
    if (flow.kind != Flow::Kind::Return) {
      fail(ConstantEvaluationFailure::InvalidOperation, id);
      return ConstantId::invalid();
    }
    return flow.value;
  }

  Flow executeBlock(InstBlockId block, Environment &environment) {
    std::vector<InstBlockId> deferred;
    const auto leave = [&](Flow flow) {
      if (flow.kind == Flow::Kind::Failure)
        return flow;
      for (auto cleanup = deferred.rbegin(); cleanup != deferred.rend();
           ++cleanup) {
        const auto cleanup_flow = executeBlock(*cleanup, environment);
        if (cleanup_flow.kind != Flow::Kind::Normal)
          return cleanup_flow;
      }
      return flow;
    };
    for (const auto id : sem_ir_.instBlock(block)) {
      if (failure_ != ConstantEvaluationFailure::None || !step(id))
        return leave({Flow::Kind::Failure, {}});
      const auto inst = sem_ir_.inst(id);
      switch (inst.kind) {
      case SemInstKind::Parameter:
      case SemInstKind::ConstantDecl:
      case SemInstKind::DiscardValue:
      case SemInstKind::EndFullExpression:
      case SemInstKind::ExtendTemporary:
        break;
      case SemInstKind::Defer:
        deferred.push_back(InstBlockId(inst.arg0));
        break;
      case SemInstKind::Assert: {
        const auto condition =
            boolean(evaluate(InstId(inst.arg0), environment));
        if (!condition) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return leave({Flow::Kind::Failure, {}});
        }
        if (!*condition) {
          fail(ConstantEvaluationFailure::FatalFailure, id);
          return {Flow::Kind::Failure, {}};
        }
        break;
      }
      case SemInstKind::UnrecoverableFailure:
        fail(ConstantEvaluationFailure::FatalFailure, id);
        return {Flow::Kind::Failure, {}};
      case SemInstKind::BindName: {
        const auto value = evaluate(InstId(inst.arg1), environment);
        if (!value.hasValue())
          return leave({Flow::Kind::Failure, {}});
        environment[inst.arg0] = value;
        break;
      }
      case SemInstKind::MaterializeTemporary: {
        const auto value = evaluate(InstId(inst.arg1), environment);
        if (!value.hasValue())
          return leave({Flow::Kind::Failure, {}});
        environment[inst.arg0] = value;
        break;
      }
      case SemInstKind::Assign: {
        const auto target = sem_ir_.inst(InstId(inst.arg0));
        if (target.kind != SemInstKind::NameRef) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return leave({Flow::Kind::Failure, {}});
        }
        const auto value = evaluate(InstId(inst.arg1), environment);
        if (!value.hasValue())
          return leave({Flow::Kind::Failure, {}});
        environment[target.arg0] = value;
        break;
      }
      case SemInstKind::Return: {
        const auto value = evaluate(InstId(inst.arg0), environment);
        return leave(value.hasValue() ? Flow{Flow::Kind::Return, value}
                                      : Flow{Flow::Kind::Failure, {}});
      }
      case SemInstKind::Yield: {
        const auto value = evaluate(InstId(inst.arg0), environment);
        return leave(value.hasValue() ? Flow{Flow::Kind::Yield, value}
                                      : Flow{Flow::Kind::Failure, {}});
      }
      case SemInstKind::Break:
        return leave({Flow::Kind::Break, {}, static_cast<std::uint32_t>(
                                                sem_ir_.integer(IntegerId(inst.arg0)))});
      case SemInstKind::Continue:
        return leave({Flow::Kind::Continue, {}, static_cast<std::uint32_t>(
                                                   sem_ir_.integer(IntegerId(inst.arg0)))});
      case SemInstKind::If: {
        if (TypeId(inst.type) != sem_ir_.voidType())
          break;
        const auto condition =
            boolean(evaluate(InstId(inst.arg0), environment));
        if (!condition)
          return leave({Flow::Kind::Failure, {}});
        const auto arms = sem_ir_.instBlock(InstBlockId(inst.arg1));
        const auto arm_index = *condition ? 0U : 1U;
        if (arm_index < arms.size()) {
          const auto arm = sem_ir_.inst(arms[arm_index]);
          const auto flow = executeBlock(InstBlockId(arm.arg0), environment);
          if (flow.kind != Flow::Kind::Normal)
            return leave(flow);
        }
        break;
      }
      case SemInstKind::While: {
        while (true) {
          const auto condition_flow =
              executeBlock(InstBlockId(inst.arg0), environment);
          if (condition_flow.kind != Flow::Kind::Normal)
            return leave(condition_flow);
          const auto condition_block =
              sem_ir_.instBlock(InstBlockId(inst.arg0));
          if (condition_block.empty())
            break;
          const auto condition_value =
              sem_ir_.inst(condition_block.back()).kind ==
                      SemInstKind::EndFullExpression
                  ? condition_block.size() >= 2
                        ? condition_block[condition_block.size() - 2]
                        : InstId::invalid()
                  : condition_block.back();
          const auto condition =
              boolean(evaluate(condition_value, environment));
          if (!condition || !*condition)
            break;
          auto body = executeBlock(InstBlockId(inst.arg1), environment);
          if (body.kind == Flow::Kind::Break) {
            if (body.loop_distance == 0)
              break;
            --body.loop_distance;
            return leave(body);
          }
          if (body.kind == Flow::Kind::Continue && body.loop_distance != 0) {
            --body.loop_distance;
            return leave(body);
          }
          if (body.kind != Flow::Kind::Normal &&
              body.kind != Flow::Kind::Continue)
            return leave(body);
        }
        break;
      }
      case SemInstKind::For: {
        const auto clauses = sem_ir_.instBlock(InstBlockId(inst.arg0));
        if (clauses.size() != 3) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return leave({Flow::Kind::Failure, {}});
        }
        std::array<InstBlockId, 3> blocks;
        for (std::size_t index = 0; index < clauses.size(); ++index) {
          const auto clause = sem_ir_.inst(clauses[index]);
          if (clause.kind != SemInstKind::ForClause ||
              sem_ir_.integer(IntegerId(clause.arg0)) !=
                  static_cast<std::int64_t>(index)) {
            fail(ConstantEvaluationFailure::InvalidOperation, clauses[index]);
            return leave({Flow::Kind::Failure, {}});
          }
          blocks[index] = InstBlockId(clause.arg1);
        }
        const auto init = executeBlock(blocks[0], environment);
        if (init.kind != Flow::Kind::Normal)
          return leave(init);
        while (true) {
          const auto condition_flow = executeBlock(blocks[1], environment);
          if (condition_flow.kind != Flow::Kind::Normal)
            return leave(condition_flow);
          const auto condition_insts = sem_ir_.instBlock(blocks[1]);
          const auto condition_value =
              !condition_insts.empty() &&
                      sem_ir_.inst(condition_insts.back()).kind ==
                          SemInstKind::EndFullExpression
                  ? condition_insts.size() >= 2
                        ? condition_insts[condition_insts.size() - 2]
                        : InstId::invalid()
              : condition_insts.empty() ? InstId::invalid()
                                        : condition_insts.back();
          const auto condition =
              condition_insts.empty()
                  ? std::optional<bool>(true)
                  : boolean(evaluate(condition_value, environment));
          if (!condition) {
            fail(ConstantEvaluationFailure::InvalidOperation, id);
            return leave({Flow::Kind::Failure, {}});
          }
          if (!*condition)
            break;
          auto body = executeBlock(InstBlockId(inst.arg1), environment);
          if (body.kind == Flow::Kind::Break) {
            if (body.loop_distance == 0)
              break;
            --body.loop_distance;
            return leave(body);
          }
          if (body.kind == Flow::Kind::Continue && body.loop_distance != 0) {
            --body.loop_distance;
            return leave(body);
          }
          if (body.kind != Flow::Kind::Normal &&
              body.kind != Flow::Kind::Continue)
            return leave(body);
          const auto step_flow = executeBlock(blocks[2], environment);
          if (step_flow.kind != Flow::Kind::Normal)
            return leave(step_flow);
        }
        break;
      }
      case SemInstKind::DoWhile: {
        while (true) {
          auto body = executeBlock(InstBlockId(inst.arg1), environment);
          if (body.kind == Flow::Kind::Break) {
            if (body.loop_distance == 0)
              break;
            --body.loop_distance;
            return leave(body);
          }
          if (body.kind == Flow::Kind::Continue && body.loop_distance != 0) {
            --body.loop_distance;
            return leave(body);
          }
          if (body.kind != Flow::Kind::Normal &&
              body.kind != Flow::Kind::Continue)
            return leave(body);
          const auto condition_flow =
              executeBlock(InstBlockId(inst.arg0), environment);
          if (condition_flow.kind != Flow::Kind::Normal)
            return leave(condition_flow);
          const auto condition_block =
              sem_ir_.instBlock(InstBlockId(inst.arg0));
          const auto condition_value =
              !condition_block.empty() &&
                      sem_ir_.inst(condition_block.back()).kind ==
                          SemInstKind::EndFullExpression
                  ? condition_block.size() >= 2
                        ? condition_block[condition_block.size() - 2]
                        : InstId::invalid()
              : condition_block.empty() ? InstId::invalid()
                                        : condition_block.back();
          const auto condition =
              condition_block.empty()
                  ? std::optional<bool>(false)
                  : boolean(evaluate(condition_value, environment));
          if (!condition) {
            fail(ConstantEvaluationFailure::InvalidOperation, id);
            return leave({Flow::Kind::Failure, {}});
          }
          if (!*condition)
            break;
        }
        break;
      }
      case SemInstKind::Switch: {
        if (TypeId(inst.type) != sem_ir_.voidType())
          break;
        const auto scrutinee = evaluate(InstId(inst.arg0), environment);
        if (!scrutinee.hasValue())
          return leave({Flow::Kind::Failure, {}});
        const auto &value = sem_ir_.constantValue(scrutinee);
        if (value.kind != ConstantValueKind::Enum) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return leave({Flow::Kind::Failure, {}});
        }
        const auto discriminator = sem_ir_.integer(
            IntegerId(static_cast<std::uint32_t>(value.payload)));
        InstId fallback;
        InstId selected;
        for (const auto arm_id : sem_ir_.instBlock(InstBlockId(inst.arg1))) {
          const auto arm = sem_ir_.inst(arm_id);
          if (arm.kind != SemInstKind::SwitchArm) {
            fail(ConstantEvaluationFailure::InvalidOperation, arm_id);
            return leave({Flow::Kind::Failure, {}});
          }
          const auto pattern = sem_ir_.integer(IntegerId(arm.arg0));
          if (pattern == discriminator) {
            selected = arm_id;
            break;
          }
          if (pattern == -1)
            fallback = arm_id;
        }
        if (!selected.hasValue())
          selected = fallback;
        if (!selected.hasValue()) {
          fail(ConstantEvaluationFailure::InvalidOperation, id);
          return leave({Flow::Kind::Failure, {}});
        }
        const auto flow =
            executeBlock(InstBlockId(sem_ir_.inst(selected).arg1), environment);
        if (flow.kind != Flow::Kind::Normal)
          return leave(flow);
        break;
      }
      default:
        // Pure expression instructions are evaluated lazily by their users.
        break;
      }
    }
    return leave({});
  }

  SemIR &sem_ir_;
  ConstantEvaluationLimits limits_;
  std::uint64_t steps_left_ = 0;
  std::uint32_t call_depth_ = 0;
  ConstantEvaluationFailure failure_ = ConstantEvaluationFailure::None;
  NodeId failure_location_;
  std::unordered_set<std::uint32_t> evaluating_entities_;
};

} // namespace

ConstantEvaluator::ConstantEvaluator(SemIR &sem_ir,
                                     ConstantEvaluationLimits limits)
    : sem_ir_(&sem_ir), limits_(limits) {}

ConstantEvaluationOutcome
ConstantEvaluator::evaluateEntity(ConstantEntityId entity) {
  EvaluationContext context(*sem_ir_, limits_);
  return context.evaluateEntity(entity);
}

ConstantEvaluationOutcome ConstantEvaluator::evaluateExpression(InstId value) {
  EvaluationContext context(*sem_ir_, limits_);
  return context.evaluateExpression(value);
}

} // namespace chtholly::compiler
