#pragma once

#include <cstdint>
#include <string_view>

namespace chtholly::compiler {

enum class BuiltinOperatorKind : std::uint32_t {
  Positive,
  Negate,
  BitNot,
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  ShiftLeft,
  ShiftRight,
  BitAnd,
  BitXor,
  BitOr,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  ThreeWay,
  Count,
};

[[nodiscard]] constexpr bool isBuiltinUnaryOperator(BuiltinOperatorKind kind) {
  return kind == BuiltinOperatorKind::Positive ||
         kind == BuiltinOperatorKind::Negate ||
         kind == BuiltinOperatorKind::BitNot;
}

[[nodiscard]] constexpr bool
isBuiltinComparisonOperator(BuiltinOperatorKind kind) {
  return kind >= BuiltinOperatorKind::Equal &&
         kind <= BuiltinOperatorKind::ThreeWay;
}

[[nodiscard]] constexpr bool
isBuiltinEqualityOperator(BuiltinOperatorKind kind) {
  return kind == BuiltinOperatorKind::Equal ||
         kind == BuiltinOperatorKind::NotEqual;
}

[[nodiscard]] constexpr bool isBuiltinShiftOperator(BuiltinOperatorKind kind) {
  return kind == BuiltinOperatorKind::ShiftLeft ||
         kind == BuiltinOperatorKind::ShiftRight;
}

[[nodiscard]] constexpr bool
isBuiltinBitwiseOperator(BuiltinOperatorKind kind) {
  return kind == BuiltinOperatorKind::BitAnd ||
         kind == BuiltinOperatorKind::BitXor ||
         kind == BuiltinOperatorKind::BitOr;
}

[[nodiscard]] constexpr bool
isBuiltinIntegerOnlyOperator(BuiltinOperatorKind kind) {
  return isBuiltinShiftOperator(kind) ||
         kind == BuiltinOperatorKind::Remainder ||
         isBuiltinBitwiseOperator(kind);
}

[[nodiscard]] constexpr std::string_view
builtinOperatorName(BuiltinOperatorKind kind) {
  switch (kind) {
  case BuiltinOperatorKind::Positive: return "positive";
  case BuiltinOperatorKind::Negate: return "negate";
  case BuiltinOperatorKind::BitNot: return "bit-not";
  case BuiltinOperatorKind::Add: return "add";
  case BuiltinOperatorKind::Subtract: return "subtract";
  case BuiltinOperatorKind::Multiply: return "multiply";
  case BuiltinOperatorKind::Divide: return "divide";
  case BuiltinOperatorKind::Remainder: return "remainder";
  case BuiltinOperatorKind::ShiftLeft: return "shift-left";
  case BuiltinOperatorKind::ShiftRight: return "shift-right";
  case BuiltinOperatorKind::BitAnd: return "bit-and";
  case BuiltinOperatorKind::BitXor: return "bit-xor";
  case BuiltinOperatorKind::BitOr: return "bit-or";
  case BuiltinOperatorKind::Equal: return "equal";
  case BuiltinOperatorKind::NotEqual: return "not-equal";
  case BuiltinOperatorKind::Less: return "less";
  case BuiltinOperatorKind::LessEqual: return "less-equal";
  case BuiltinOperatorKind::Greater: return "greater";
  case BuiltinOperatorKind::GreaterEqual: return "greater-equal";
  case BuiltinOperatorKind::ThreeWay: return "three-way";
  case BuiltinOperatorKind::Count: return "invalid";
  }
  return "invalid";
}

} // namespace chtholly::compiler
