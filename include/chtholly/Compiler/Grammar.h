#pragma once

#include "chtholly/Compiler/Token.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace chtholly::compiler {

enum class TokenCategory : std::uint8_t {
  Structural,
  Identifier,
  Literal,
  Keyword,
  Punctuation,
};

enum class TokenAdmission : std::uint8_t {
  Admitted,
  Reserved,
};

enum class Associativity : std::uint8_t {
  Left,
  Right,
  None,
};

enum class PrecedenceGroup : std::uint8_t {
  LogicalOr,
  LogicalAnd,
  BitwiseOr,
  BitwiseXor,
  BitwiseAnd,
  Equality,
  Relational,
  ThreeWayComparison,
  Shift,
  Additive,
  Multiplicative,
};

struct BinaryOperatorInfo {
  PrecedenceGroup group;
  std::uint8_t precedence;
  Associativity associativity;
};

[[nodiscard]] TokenCategory tokenCategory(TokenKind kind);
[[nodiscard]] TokenAdmission tokenAdmission(TokenKind kind);
[[nodiscard]] std::optional<BinaryOperatorInfo>
binaryOperatorInfo(TokenKind kind);
[[nodiscard]] std::string_view precedenceGroupName(PrecedenceGroup group);

} // namespace chtholly::compiler
