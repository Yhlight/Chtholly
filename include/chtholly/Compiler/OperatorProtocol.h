#pragma once

#include "chtholly/Compiler/Token.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace chtholly::compiler {

enum class OperatorProtocolResult : std::uint8_t {
  Direct,
  Void,
  LogicalNot,
  OrderingLess,
  OrderingLessEqual,
  OrderingGreater,
  OrderingGreaterEqual,
};

enum class OperatorProtocolReceiver : std::uint8_t {
  ReadOnly,
  Mutable,
};

struct OperatorProtocol {
  std::string_view interface_name;
  std::string_view requirement_name;
  OperatorProtocolResult result = OperatorProtocolResult::Direct;
  bool binary = true;
  OperatorProtocolReceiver receiver = OperatorProtocolReceiver::ReadOnly;
};

[[nodiscard]] constexpr std::optional<OperatorProtocol>
unaryOperatorProtocol(TokenKind token) {
  switch (token) {
  case TokenKind::Minus:
    return OperatorProtocol{"Negation", "negate",
                            OperatorProtocolResult::Direct, false};
  case TokenKind::Tilde:
    return OperatorProtocol{"BitwiseComplement", "complement",
                            OperatorProtocolResult::Direct, false};
  default:
    return std::nullopt;
  }
}

[[nodiscard]] constexpr std::optional<OperatorProtocol>
binaryOperatorProtocol(TokenKind token) {
  switch (token) {
  case TokenKind::Minus:
    return OperatorProtocol{"Subtraction", "subtract"};
  case TokenKind::Plus:
    return OperatorProtocol{"Addition", "add"};
  case TokenKind::Star:
    return OperatorProtocol{"Multiplication", "multiply"};
  case TokenKind::Slash:
    return OperatorProtocol{"Division", "divide"};
  case TokenKind::Percent:
    return OperatorProtocol{"Remainder", "remainder"};
  case TokenKind::Amp:
    return OperatorProtocol{"BitwiseAnd", "bit_and"};
  case TokenKind::Caret:
    return OperatorProtocol{"BitwiseXor", "bit_xor"};
  case TokenKind::Pipe:
    return OperatorProtocol{"BitwiseOr", "bit_or"};
  case TokenKind::LessLess:
    return OperatorProtocol{"LeftShift", "shift_left"};
  case TokenKind::GreaterGreater:
    return OperatorProtocol{"RightShift", "shift_right"};
  case TokenKind::EqualEqual:
    return OperatorProtocol{"Equality", "equal"};
  case TokenKind::BangEqual:
    return OperatorProtocol{"Equality", "equal",
                            OperatorProtocolResult::LogicalNot};
  case TokenKind::Spaceship:
    return OperatorProtocol{"Comparison", "compare"};
  case TokenKind::Less:
    return OperatorProtocol{"Comparison", "compare",
                            OperatorProtocolResult::OrderingLess};
  case TokenKind::LessEqual:
    return OperatorProtocol{"Comparison", "compare",
                            OperatorProtocolResult::OrderingLessEqual};
  case TokenKind::Greater:
    return OperatorProtocol{"Comparison", "compare",
                            OperatorProtocolResult::OrderingGreater};
  case TokenKind::GreaterEqual:
    return OperatorProtocol{"Comparison", "compare",
                            OperatorProtocolResult::OrderingGreaterEqual};
  default:
    return std::nullopt;
  }
}

[[nodiscard]] constexpr std::optional<OperatorProtocol>
compoundAssignmentOperatorProtocol(TokenKind token) {
  switch (token) {
  case TokenKind::PlusEqual:
    return OperatorProtocol{"AdditionAssignment", "add_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::MinusEqual:
    return OperatorProtocol{"SubtractionAssignment", "subtract_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::StarEqual:
    return OperatorProtocol{"MultiplicationAssignment", "multiply_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::SlashEqual:
    return OperatorProtocol{"DivisionAssignment", "divide_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::PercentEqual:
    return OperatorProtocol{"RemainderAssignment", "remainder_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::AmpEqual:
    return OperatorProtocol{"BitwiseAndAssignment", "bit_and_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::CaretEqual:
    return OperatorProtocol{"BitwiseXorAssignment", "bit_xor_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::PipeEqual:
    return OperatorProtocol{"BitwiseOrAssignment", "bit_or_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::LessLessEqual:
    return OperatorProtocol{"LeftShiftAssignment", "shift_left_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  case TokenKind::GreaterGreaterEqual:
    return OperatorProtocol{"RightShiftAssignment", "shift_right_assign",
                            OperatorProtocolResult::Void, true,
                            OperatorProtocolReceiver::Mutable};
  default:
    return std::nullopt;
  }
}

} // namespace chtholly::compiler
