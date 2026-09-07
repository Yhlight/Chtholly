#include "chtholly/Core/Metrics.h"

#include <algorithm>
#include <sstream>

namespace chtholly::core {
namespace {

void appendJsonString(std::ostringstream &out, std::string_view value) {
  out << '"';
  for (const char ch : value) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << ch;
      break;
    }
  }
  out << '"';
}

} // namespace

void CompilerMetrics::addMemory(std::string label, std::uint64_t used_bytes,
                                std::uint64_t reserved_bytes) {
  memory_.push_back(
      {std::move(label), used_bytes, std::max(used_bytes, reserved_bytes)});
}

std::uint64_t CompilerMetrics::totalUsedBytes() const {
  std::uint64_t total = 0;
  for (const auto &entry : memory_)
    total += entry.used_bytes;
  return total;
}

std::uint64_t CompilerMetrics::totalReservedBytes() const {
  std::uint64_t total = 0;
  for (const auto &entry : memory_)
    total += entry.reserved_bytes;
  return total;
}

std::string CompilerMetrics::toJson(std::string_view unit_name) const {
  std::ostringstream out;
  out << '{';
  if (!unit_name.empty()) {
    out << "\"unit\":";
    appendJsonString(out, unit_name);
    out << ',';
  }
  out << "\"memory\":[";
  for (std::size_t index = 0; index < memory_.size(); ++index) {
    if (index != 0)
      out << ',';
    const auto &entry = memory_[index];
    out << "{\"label\":";
    appendJsonString(out, entry.label);
    out << ",\"used-bytes\":" << entry.used_bytes
        << ",\"reserved-bytes\":" << entry.reserved_bytes << '}';
  }
  out << "],\"total-used-bytes\":" << totalUsedBytes()
      << ",\"total-reserved-bytes\":" << totalReservedBytes() << '}';
  return out.str();
}

std::string CompilerMetrics::childLabel(std::string_view parent,
                                        std::string_view child) {
  if (parent.empty())
    return std::string(child);
  std::string result(parent);
  result += '.';
  result += child;
  return result;
}

} // namespace chtholly::core
