#include "chtholly/Basic/SourceBuffer.h"

#include <algorithm>

namespace chtholly {

SourceBuffer::SourceBuffer(std::string filename, std::string text,
                           SourceFileId source_file_id)
    : filename_(std::move(filename)), text_(std::move(text)),
      source_file_id_(source_file_id) {
  computeLineStarts();
}

char SourceBuffer::charAt(std::size_t offset) const {
  if (offset >= text_.size()) {
    return '\0';
  }
  return text_[offset];
}

std::string_view SourceBuffer::slice(std::size_t offset,
                                     std::size_t length) const {
  if (offset >= text_.size()) {
    return {};
  }
  return std::string_view(text_).substr(offset, length);
}

LineColumn SourceBuffer::lineColumn(SourceLocation location) const {
  if (!owns(location)) {
    return {};
  }

  const auto offset = std::min(location.offset(), text_.size());
  auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
  const std::size_t line_index =
      static_cast<std::size_t>(std::distance(line_starts_.begin(), it)) - 1;
  return LineColumn{line_index + 1, offset - line_starts_[line_index] + 1};
}

std::string SourceBuffer::lineText(std::size_t line) const {
  if (line == 0 || line > line_starts_.size()) {
    return {};
  }

  const std::size_t start = line_starts_[line - 1];
  std::size_t end = text_.size();
  if (line < line_starts_.size()) {
    end = line_starts_[line] - 1;
  }
  if (end > start && text_[end - 1] == '\r') {
    --end;
  }
  return text_.substr(start, end - start);
}

void SourceBuffer::computeLineStarts() {
  line_starts_.clear();
  line_starts_.push_back(0);
  for (std::size_t i = 0; i < text_.size(); ++i) {
    if (text_[i] == '\n') {
      line_starts_.push_back(i + 1);
    }
  }
}

} // namespace chtholly
