#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace chtholly {

std::string sha256Hex(std::string_view text);
std::optional<std::string> sha256File(const std::string &path);

} // namespace chtholly
