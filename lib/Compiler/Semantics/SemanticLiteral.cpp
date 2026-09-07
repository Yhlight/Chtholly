#include "SemanticLiteral.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace chtholly::compiler::semantics_internal {

NumericSuffix numericSuffix(std::string_view text, std::size_t &suffix_size) {
  constexpr std::array suffixes{
      std::pair{"isize", NumericSuffix::ISize},
      std::pair{"usize", NumericSuffix::USize},
      std::pair{"i16", NumericSuffix::I16},
      std::pair{"i32", NumericSuffix::I32},
      std::pair{"i64", NumericSuffix::I64},
      std::pair{"u16", NumericSuffix::U16},
      std::pair{"u32", NumericSuffix::U32},
      std::pair{"u64", NumericSuffix::U64},
      std::pair{"f32", NumericSuffix::F32},
      std::pair{"f64", NumericSuffix::F64},
      std::pair{"i8", NumericSuffix::I8},
      std::pair{"u8", NumericSuffix::U8},
  };
  for (const auto &[spelling, suffix] : suffixes)
    if (text.ends_with(spelling)) {
      suffix_size = std::string_view(spelling).size();
      return suffix;
    }
  suffix_size = 0;
  return NumericSuffix::None;
}

std::optional<ParsedIntegerLiteral>
parseIntegerLiteral(std::string_view source) {
  std::size_t suffix_size = 0;
  const auto suffix = numericSuffix(source, suffix_size);
  auto digits = source.substr(0, source.size() - suffix_size);
  unsigned base = 10;
  if (digits.size() >= 2 && digits[0] == '0') {
    switch (digits[1]) {
    case 'b':
    case 'B':
      base = 2;
      break;
    case 'o':
    case 'O':
      base = 8;
      break;
    case 'x':
    case 'X':
      base = 16;
      break;
    default:
      break;
    }
    if (base != 10)
      digits.remove_prefix(2);
  }
  std::uint64_t result = 0;
  bool saw_digit = false;
  for (const auto value : digits) {
    if (value == '_')
      continue;
    unsigned digit = 0;
    if (value >= '0' && value <= '9')
      digit = static_cast<unsigned>(value - '0');
    else if (value >= 'a' && value <= 'f')
      digit = static_cast<unsigned>(value - 'a') + 10U;
    else if (value >= 'A' && value <= 'F')
      digit = static_cast<unsigned>(value - 'A') + 10U;
    else
      return std::nullopt;
    if (digit >= base ||
        result > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
      return std::nullopt;
    result = result * base + digit;
    saw_digit = true;
  }
  if (!saw_digit || suffix == NumericSuffix::F32 ||
      suffix == NumericSuffix::F64)
    return std::nullopt;
  return ParsedIntegerLiteral{result, suffix};
}

std::optional<double> parseFloatLiteral(std::string_view source,
                                        NumericSuffix &suffix) {
  std::size_t suffix_size = 0;
  suffix = numericSuffix(source, suffix_size);
  if (suffix != NumericSuffix::None && suffix != NumericSuffix::F32 &&
      suffix != NumericSuffix::F64)
    return std::nullopt;
  std::string normalized;
  normalized.reserve(source.size());
  for (const auto value : source.substr(0, source.size() - suffix_size))
    if (value != '_')
      normalized.push_back(value);
  double result = 0;
  const auto parsed =
      std::from_chars(normalized.data(), normalized.data() + normalized.size(),
                      result, std::chars_format::general);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != normalized.data() + normalized.size() ||
      !std::isfinite(result))
    return std::nullopt;
  return result;
}

} // namespace chtholly::compiler::semantics_internal
