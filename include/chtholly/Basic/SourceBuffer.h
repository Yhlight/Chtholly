#pragma once

#include "chtholly/Basic/SourceLocation.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {
class SourceBuffer {
public:
  SourceBuffer(std::string filename, std::string text,
               SourceFileId source_file_id = 0);

  std::string_view filename() const {
    return filename_;
  }
  std::string_view text() const {
    return text_;
  }
  SourceFileId sourceFileId() const {
    return source_file_id_;
  }
  std::size_t size() const {
    return text_.size();
  }
  bool empty() const {
    return text_.empty();
  }
  SourceLocation location(std::size_t offset) const {
    return SourceLocation(offset, source_file_id_);
  }
  bool owns(SourceLocation location) const {
    return location.isValid() && location.sourceFileId() == source_file_id_;
  }

  char charAt(std::size_t offset) const;
  std::string_view slice(std::size_t offset, std::size_t length) const;
  LineColumn lineColumn(SourceLocation location) const;
  std::string lineText(std::size_t line) const;

private:
  void computeLineStarts();

  std::string filename_;
  std::string text_;
  SourceFileId source_file_id_ = 0;
  std::vector<std::size_t> line_starts_;
};

} // namespace chtholly
