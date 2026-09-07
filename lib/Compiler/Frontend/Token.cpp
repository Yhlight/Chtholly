#include "chtholly/Compiler/Token.h"

#include <array>

namespace chtholly::compiler {
namespace {

constexpr auto Names = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_TOKEN(Name, Spelling) #Name,
#include "chtholly/Compiler/TokenKind.def"
});

constexpr auto Spellings = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_TOKEN(Name, Spelling) Spelling,
#include "chtholly/Compiler/TokenKind.def"
});

} // namespace

std::string_view tokenKindName(TokenKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < Names.size() ? Names[index] : "InvalidTokenKind";
}

std::string_view tokenSpelling(TokenKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < Spellings.size() ? Spellings[index] : std::string_view{};
}

TokenKind keywordKind(std::string_view text) {
  for (std::size_t index = static_cast<std::size_t>(TokenKind::KwAs);
       index <= static_cast<std::size_t>(TokenKind::KwWhile); ++index) {
    if (Spellings[index] == text)
      return static_cast<TokenKind>(index);
  }
  return TokenKind::Identifier;
}

} // namespace chtholly::compiler
