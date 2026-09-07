#include "chtholly/Compiler/Source.h"

#include <algorithm>
#include <limits>

namespace chtholly::compiler {

SourceInput::SourceInput(std::string filename, std::string text)
    : filename_(std::move(filename)),
      text_(std::make_shared<const std::string>(std::move(text))) {}

SourceBuffer::SourceBuffer(SourceInput input)
    : input_(std::move(input)), line_starts_{0} {
  const auto source_text = input_.text();
  const auto compact_size = std::min(
      source_text.size(),
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  for (std::size_t offset = 0; offset < compact_size; ++offset) {
    if (source_text[offset] == '\n')
      line_starts_.push_back(static_cast<std::uint32_t>(offset + 1));
  }
}

char SourceBuffer::at(std::size_t offset) const {
  const auto source_text = input_.text();
  return offset < source_text.size() ? source_text[offset] : '\0';
}

std::string_view SourceBuffer::slice(std::size_t offset,
                                     std::size_t length) const {
  const auto source_text = input_.text();
  if (offset >= source_text.size())
    return {};
  return source_text.substr(offset, length);
}

LineColumn SourceBuffer::lineColumn(std::uint32_t offset) const {
  const auto source_text = input_.text();
  const auto compact_size = static_cast<std::uint32_t>(std::min(
      source_text.size(),
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
  offset = std::min(offset, compact_size);
  const auto found =
      std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
  const auto line_index =
      static_cast<std::size_t>(found - line_starts_.begin() - 1);
  return {static_cast<std::uint32_t>(line_index + 1),
          offset - line_starts_[line_index] + 1};
}

std::string_view SourceBuffer::lineText(std::uint32_t line) const {
  if (line == 0 || line > line_starts_.size())
    return {};
  const auto source_text = input_.text();
  const auto start = line_starts_[line - 1];
  auto end = source_text.find('\n', start);
  if (end == std::string::npos)
    end = source_text.size();
  if (end > start && source_text[end - 1] == '\r')
    --end;
  return source_text.substr(start, end - start);
}

} // namespace chtholly::compiler
