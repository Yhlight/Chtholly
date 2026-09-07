#pragma once

#include <cstddef>
#include <limits>

namespace chtholly {

using SourceFileId = std::size_t;

inline constexpr SourceFileId InvalidSourceFileId =
    std::numeric_limits<SourceFileId>::max();

class SourceLocation {
public:
  constexpr SourceLocation() = default;

  explicit constexpr SourceLocation(std::size_t offset,
                                    SourceFileId source_file_id = 0)
      : offset_(offset), source_file_id_(source_file_id), valid_(true) {}

  constexpr bool isValid() const {
    return valid_;
  }

  constexpr std::size_t offset() const {
    return offset_;
  }

  constexpr SourceFileId sourceFileId() const {
    return source_file_id_;
  }

  constexpr SourceLocation advanced(std::size_t amount) const {
    return valid_ ? SourceLocation(offset_ + amount, source_file_id_)
                  : SourceLocation();
  }

  friend constexpr bool operator==(const SourceLocation &,
                                   const SourceLocation &) = default;

private:
  std::size_t offset_ = 0;
  SourceFileId source_file_id_ = InvalidSourceFileId;
  bool valid_ = false;
};

struct LineColumn {
  std::size_t line = 1;
  std::size_t column = 1;
};

} // namespace chtholly