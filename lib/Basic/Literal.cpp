#include "chtholly/Basic/Literal.h"

#include <string>

namespace chtholly {

std::optional<std::uint64_t>
parseIntegerLiteralMagnitude(std::string_view source) {
  std::string text(source);
  while (!text.empty()) {
    const char suffix = text.back();
    if (suffix == 'u' || suffix == 'U' || suffix == 'l' || suffix == 'L')
      text.pop_back();
    else
      break;
  }
  try {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 10);
    if (consumed != text.size())
      return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace chtholly
