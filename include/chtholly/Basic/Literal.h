#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace chtholly {

std::optional<std::uint64_t>
parseIntegerLiteralMagnitude(std::string_view text);

} // namespace chtholly
