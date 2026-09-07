#pragma once

#include "chtholly/Core/Id.h"

#include <cstdint>
#include <string_view>

namespace chtholly::compiler {

enum class TokenKind : std::uint8_t {
#define CHTHOLLY_COMPILER_TOKEN(Name, Spelling) Name,
#include "chtholly/Compiler/TokenKind.def"
  Count,
};

struct TokenId : core::IndexBase<TokenId> {
  using IndexBase::IndexBase;
};

enum class TokenFlags : std::uint8_t {
  None = 0,
  HasError = 1,
};

struct TokenInfo {
  std::uint32_t offset = 0;
  std::uint16_t length = 0;
  TokenKind kind = TokenKind::Invalid;
  TokenFlags flags = TokenFlags::None;
};

[[nodiscard]] std::string_view tokenKindName(TokenKind kind);
[[nodiscard]] std::string_view tokenSpelling(TokenKind kind);
[[nodiscard]] TokenKind keywordKind(std::string_view text);

static_assert(static_cast<unsigned>(TokenKind::Count) <= 255);
static_assert(sizeof(TokenInfo) == 8);

} // namespace chtholly::compiler
