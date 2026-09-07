#pragma once

#include "chtholly/Core/Arena.h"
#include "chtholly/Core/Id.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"

#include <string_view>
#include <unordered_map>

namespace chtholly::core {

struct StringId : IdBase<StringId> {
  using IdBase<StringId>::IdBase;
};

class StringInterner {
public:
  explicit StringInterner(Arena &arena);

  [[nodiscard]] StringId intern(std::string_view value);
  [[nodiscard]] std::string_view get(StringId id) const;
  [[nodiscard]] std::size_t size() const {
    return values_.size();
  }

  void collectMetrics(CompilerMetrics &metrics, std::string_view label) const;

private:
  struct StringViewHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept;
  };

  Arena *arena_;
  ValueStore<StringId, std::string_view> values_;
  std::unordered_map<std::string_view, StringId, StringViewHash,
                     std::equal_to<>>
      lookup_;
};

} // namespace chtholly::core
