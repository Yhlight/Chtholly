#include "ManifestToml.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <functional>
#include <string>
#include <utility>

namespace chtholly::manifest_toml {

std::string Assignment::fullKey() const {
  return table.empty() ? key : table + "." + key;
}

bool parseUnsigned(std::string_view value, std::uint64_t &out) {
  const auto trimmed = trim(value);
  const auto result = std::from_chars(trimmed.data(),
                                      trimmed.data() + trimmed.size(), out);
  return !trimmed.empty() && result.ec == std::errc{} &&
         result.ptr == trimmed.data() + trimmed.size();
}

std::string trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::string stripComment(std::string_view line) {
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (in_string && ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      in_string = !in_string;
      continue;
    }
    if (!in_string && ch == '#') {
      return std::string(line.substr(0, i));
    }
  }
  return std::string(line);
}

bool parseString(std::string_view value, std::string &out) {
  const auto trimmed = trim(value);
  value = trimmed;
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return false;
  }
  out.clear();
  bool escaped = false;
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    const char ch = value[i];
    if (escaped) {
      switch (ch) {
      case '"':
      case '\\':
        out.push_back(ch);
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        return false;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      return false;
    }
    out.push_back(ch);
  }
  return !escaped;
}

bool parseBool(std::string_view value, bool &out) {
  const auto trimmed = trim(value);
  if (trimmed == "true") {
    out = true;
    return true;
  }
  if (trimmed == "false") {
    out = false;
    return true;
  }
  return false;
}

bool parseStringArray(std::string_view value, std::vector<std::string> &out) {
  const auto trimmed = trim(value);
  value = trimmed;
  if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
    return false;
  }
  out.clear();
  std::size_t cursor = 1;
  while (cursor + 1 < value.size()) {
    while (cursor + 1 < value.size() &&
           std::isspace(static_cast<unsigned char>(value[cursor]))) {
      ++cursor;
    }
    if (cursor + 1 >= value.size() || value[cursor] == ']') {
      break;
    }
    if (value[cursor] != '"') {
      return false;
    }
    std::size_t end = cursor + 1;
    bool escaped = false;
    for (; end + 1 < value.size(); ++end) {
      const char ch = value[end];
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        break;
      }
    }
    if (end + 1 >= value.size() || value[end] != '"') {
      return false;
    }
    std::string element;
    if (!parseString(value.substr(cursor, end - cursor + 1), element)) {
      return false;
    }
    out.push_back(std::move(element));
    cursor = end + 1;
    while (cursor + 1 < value.size() &&
           std::isspace(static_cast<unsigned char>(value[cursor]))) {
      ++cursor;
    }
    if (cursor + 1 < value.size() && value[cursor] == ',') {
      ++cursor;
      continue;
    }
    while (cursor + 1 < value.size() &&
           std::isspace(static_cast<unsigned char>(value[cursor]))) {
      ++cursor;
    }
    if (cursor + 1 < value.size() && value[cursor] != ']') {
      return false;
    }
  }
  return true;
}

bool parseInlineTable(std::string_view value,
                      std::vector<std::pair<std::string, std::string>> &out) {
  const auto trimmed = trim(value);
  value = trimmed;
  if (value.size() < 2 || value.front() != '{' || value.back() != '}') {
    return false;
  }
  out.clear();
  std::size_t cursor = 1;
  while (cursor + 1 < value.size()) {
    while (cursor + 1 < value.size() &&
           std::isspace(static_cast<unsigned char>(value[cursor]))) {
      ++cursor;
    }
    if (cursor + 1 >= value.size() || value[cursor] == '}') {
      break;
    }
    const auto equal = value.find('=', cursor);
    if (equal == std::string_view::npos) {
      return false;
    }
    auto key = trim(value.substr(cursor, equal - cursor));
    cursor = equal + 1;
    std::size_t end = cursor;
    bool in_string = false;
    bool escaped = false;
    int array_depth = 0;
    int table_depth = 0;
    for (; end + 1 < value.size(); ++end) {
      const char ch = value[end];
      if (escaped) {
        escaped = false;
        continue;
      }
      if (in_string && ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        in_string = !in_string;
        continue;
      }
      if (!in_string) {
        if (ch == '[') {
          ++array_depth;
          continue;
        }
        if (ch == ']') {
          if (array_depth == 0) {
            return false;
          }
          --array_depth;
          continue;
        }
        if (ch == '{') {
          ++table_depth;
          continue;
        }
        if (ch == '}') {
          if (table_depth == 0) {
            break;
          }
          --table_depth;
          continue;
        }
        if (array_depth == 0 && table_depth == 0 && ch == ',') {
          break;
        }
      }
    }
    if (in_string || escaped || array_depth != 0 || table_depth != 0) {
      return false;
    }
    auto raw_value = trim(value.substr(cursor, end - cursor));
    if (key.empty() || raw_value.empty()) {
      return false;
    }
    out.emplace_back(std::move(key), std::move(raw_value));
    cursor = end;
    if (cursor + 1 < value.size() && value[cursor] == ',') {
      ++cursor;
    }
  }
  return true;
}

std::optional<std::vector<Assignment>> parseAssignments(
    std::string_view text, const std::vector<std::string> &allowed_tables,
    std::string_view manifest_kind, std::string &error,
    const std::function<std::optional<std::string>(std::string_view,
                                                   std::size_t)> &
        unsupported_table_reason) {
  std::vector<Assignment> assignments;
  std::string table;
  std::size_t line_number = 0;
  std::size_t line_start = 0;
  while (line_start <= text.size()) {
    ++line_number;
    const auto line_end = text.find('\n', line_start);
    const auto raw_line = text.substr(line_start, line_end == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : line_end - line_start);
    auto line = trim(stripComment(raw_line));
    if (line.empty()) {
      if (line_end == std::string_view::npos) {
        break;
      }
      line_start = line_end + 1;
      continue;
    }
    if (line.front() == '[') {
      if (line.size() < 3 || line.back() != ']') {
        error = "invalid " + std::string(manifest_kind) + " table at line " +
                std::to_string(line_number);
        return std::nullopt;
      }
      table = trim(std::string_view(line).substr(1, line.size() - 2));
      if (std::find(allowed_tables.begin(), allowed_tables.end(), table) ==
          allowed_tables.end()) {
        if (unsupported_table_reason) {
          if (auto reason = unsupported_table_reason(table, line_number)) {
            error = *reason;
            return std::nullopt;
          }
        }
        error = "unknown " + std::string(manifest_kind) + " table '" + table +
                "' at line " + std::to_string(line_number);
        return std::nullopt;
      }
    } else {
      const auto equal = line.find('=');
      if (equal == std::string::npos) {
        error = "expected key/value assignment at line " + std::to_string(line_number);
        return std::nullopt;
      }
      assignments.push_back(Assignment{table,
                                       trim(std::string_view(line).substr(0, equal)),
                                       trim(std::string_view(line).substr(equal + 1)),
                                       line_number});
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }
  return assignments;
}

std::optional<std::vector<Assignment>> parseAssignments(
    std::string_view text, const std::vector<std::string> &allowed_tables,
    std::string_view manifest_kind, std::string &error) {
  return parseAssignments(text, allowed_tables, manifest_kind, error, {});
}

} // namespace chtholly::manifest_toml
