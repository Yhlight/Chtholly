#include "chtholly/Compiler/Grammar.h"

namespace chtholly::compiler {

TokenCategory tokenCategory(TokenKind kind) {
  if (kind == TokenKind::EndOfFile || kind == TokenKind::Invalid)
    return TokenCategory::Structural;
  if (kind == TokenKind::Identifier)
    return TokenCategory::Identifier;
  if (kind >= TokenKind::IntegerLiteral && kind <= TokenKind::CharLiteral)
    return TokenCategory::Literal;
  if (kind >= TokenKind::KwAs && kind <= TokenKind::KwWhile)
    return TokenCategory::Keyword;
  return TokenCategory::Punctuation;
}

TokenAdmission tokenAdmission(TokenKind kind) {
  switch (kind) {
#define CHTHOLLY_COMPILER_RESERVED_TOKEN(Name) case TokenKind::Name:
#include "chtholly/Compiler/Grammar.def"
    return TokenAdmission::Reserved;
  default:
    return TokenAdmission::Admitted;
  }
}

std::optional<BinaryOperatorInfo> binaryOperatorInfo(TokenKind kind) {
  switch (kind) {
#define CHTHOLLY_COMPILER_BINARY_OPERATOR(Name, Group, Precedence, Assoc)          \
  case TokenKind::Name:                                                        \
    return BinaryOperatorInfo{PrecedenceGroup::Group, Precedence,              \
                              Associativity::Assoc};
#include "chtholly/Compiler/Grammar.def"
  default:
    return std::nullopt;
  }
}

std::string_view precedenceGroupName(PrecedenceGroup group) {
  switch (group) {
  case PrecedenceGroup::LogicalOr: return "LogicalOr";
  case PrecedenceGroup::LogicalAnd: return "LogicalAnd";
  case PrecedenceGroup::BitwiseOr: return "BitwiseOr";
  case PrecedenceGroup::BitwiseXor: return "BitwiseXor";
  case PrecedenceGroup::BitwiseAnd: return "BitwiseAnd";
  case PrecedenceGroup::Equality: return "Equality";
  case PrecedenceGroup::Relational: return "Relational";
  case PrecedenceGroup::ThreeWayComparison: return "ThreeWayComparison";
  case PrecedenceGroup::Shift: return "Shift";
  case PrecedenceGroup::Additive: return "Additive";
  case PrecedenceGroup::Multiplicative: return "Multiplicative";
  }
  return "invalid";
}

} // namespace chtholly::compiler
