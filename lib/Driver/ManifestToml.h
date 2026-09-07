#pragma once

#include <optional>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::manifest_toml {

struct Assignment {
  std::string table;
  std::string key;
  std::string value;
  std::size_t line = 0;

  std::string fullKey() const;
};

std::string trim(std::string_view value);
bool parseString(std::string_view value, std::string &out);
bool parseBool(std::string_view value, bool &out);
bool parseUnsigned(std::string_view value, std::uint64_t &out);
bool parseStringArray(std::string_view value, std::vector<std::string> &out);
bool parseInlineTable(std::string_view value,
                      std::vector<std::pair<std::string, std::string>> &out);
std::optional<std::vector<Assignment>> parseAssignments(
    std::string_view text, const std::vector<std::string> &allowed_tables,
    std::string_view manifest_kind, std::string &error);
std::optional<std::vector<Assignment>> parseAssignments(
    std::string_view text, const std::vector<std::string> &allowed_tables,
    std::string_view manifest_kind, std::string &error,
    const std::function<std::optional<std::string>(std::string_view,
                                                   std::size_t)> &
        unsupported_table_reason);

} // namespace chtholly::manifest_toml
