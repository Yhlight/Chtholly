#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace chtholly::compiler::semantics_internal {

enum class NumericSuffix : std::uint8_t {
  None,
  I8,
  I16,
  I32,
  I64,
  ISize,
  U8,
  U16,
  U32,
  U64,
  USize,
  F32,
  F64,
};

struct ParsedIntegerLiteral {
  std::uint64_t magnitude = 0;
  NumericSuffix suffix = NumericSuffix::None;
};

[[nodiscard]] NumericSuffix numericSuffix(std::string_view text,
                                          std::size_t &suffix_size);
[[nodiscard]] std::optional<ParsedIntegerLiteral>
parseIntegerLiteral(std::string_view source);
[[nodiscard]] std::optional<double> parseFloatLiteral(std::string_view source,
                                                      NumericSuffix &suffix);

} // namespace chtholly::compiler::semantics_internal
