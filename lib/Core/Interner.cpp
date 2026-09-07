#include "chtholly/Core/Interner.h"

#include <functional>

namespace chtholly::core {

StringInterner::StringInterner(Arena &arena) : arena_(&arena) {
  (void)intern({});
}

StringId StringInterner::intern(std::string_view value) {
  if (const auto found = lookup_.find(value); found != lookup_.end())
    return found->second;
  const auto stored = arena_->copyString(value);
  const auto id = values_.add(stored);
  lookup_.emplace(stored, id);
  return id;
}

std::string_view StringInterner::get(StringId id) const {
  return values_.get(id);
}

void StringInterner::collectMetrics(CompilerMetrics &metrics,
                                    std::string_view label) const {
  values_.collectMetrics(metrics, CompilerMetrics::childLabel(label, "values"));
  metrics.addMemory(CompilerMetrics::childLabel(label, "lookup"),
                    lookup_.size() * sizeof(decltype(lookup_)::value_type),
                    lookup_.bucket_count() * sizeof(void *) +
                        lookup_.size() * sizeof(decltype(lookup_)::value_type));
}

std::size_t StringInterner::StringViewHash::operator()(
    std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

} // namespace chtholly::core
