#pragma once

#include "chtholly/Core/Id.h"
#include "chtholly/Core/Metrics.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chtholly::core {

template <typename IdT, typename ValueT> class ValueStore {
  static_assert(std::is_base_of_v<AnyId, IdT>);

public:
  using IdType = IdT;
  using ValueType = ValueT;

  [[nodiscard]] IdT add(ValueT value) {
    assert(values_.size() < AnyId::InvalidIndex);
    const auto id = IdT(static_cast<std::uint32_t>(values_.size()));
    values_.push_back(std::move(value));
    return id;
  }

  [[nodiscard]] bool contains(IdT id) const {
    return id.hasValue() && id.index < values_.size();
  }

  [[nodiscard]] const ValueT *tryGet(IdT id) const {
    return contains(id) ? &values_[id.index] : nullptr;
  }

  [[nodiscard]] ValueT *tryGet(IdT id) {
    return contains(id) ? &values_[id.index] : nullptr;
  }

  [[nodiscard]] const ValueT &get(IdT id) const {
    assert(contains(id));
    return values_[id.index];
  }

  [[nodiscard]] ValueT &get(IdT id) {
    assert(contains(id));
    return values_[id.index];
  }

  [[nodiscard]] std::size_t size() const {
    return values_.size();
  }
  [[nodiscard]] bool empty() const {
    return values_.empty();
  }

  void reserve(std::size_t count) {
    values_.reserve(count);
  }

  [[nodiscard]] const std::vector<ValueT> &values() const {
    return values_;
  }

  void collectMetrics(CompilerMetrics &metrics, std::string label) const {
    metrics.addMemory(std::move(label), values_.size() * sizeof(ValueT),
                      values_.capacity() * sizeof(ValueT));
  }

private:
  std::vector<ValueT> values_;
};

// Dense-ID storage for graphs that expose references while still appending.
// Deque append preserves references to existing elements.
template <typename IdT, typename ValueT> class StableValueStore {
  static_assert(std::is_base_of_v<AnyId, IdT>);

public:
  using IdType = IdT;
  using ValueType = ValueT;

  [[nodiscard]] IdT add(ValueT value) {
    assert(values_.size() < AnyId::InvalidIndex);
    const auto id = IdT(static_cast<std::uint32_t>(values_.size()));
    values_.push_back(std::move(value));
    return id;
  }
  [[nodiscard]] bool contains(IdT id) const {
    return id.hasValue() && id.index < values_.size();
  }
  [[nodiscard]] const ValueT *tryGet(IdT id) const {
    return contains(id) ? &values_[id.index] : nullptr;
  }
  [[nodiscard]] ValueT *tryGet(IdT id) {
    return contains(id) ? &values_[id.index] : nullptr;
  }
  [[nodiscard]] const ValueT &get(IdT id) const {
    assert(contains(id));
    return values_[id.index];
  }
  [[nodiscard]] ValueT &get(IdT id) {
    assert(contains(id));
    return values_[id.index];
  }
  [[nodiscard]] std::size_t size() const {
    return values_.size();
  }
  [[nodiscard]] bool empty() const {
    return values_.empty();
  }
  void reserve(std::size_t) {}
  [[nodiscard]] const std::deque<ValueT> &values() const {
    return values_;
  }
  void collectMetrics(CompilerMetrics &metrics, std::string label) const {
    metrics.addMemory(std::move(label), values_.size() * sizeof(ValueT),
                      values_.size() * sizeof(ValueT));
  }

private:
  std::deque<ValueT> values_;
};

template <typename IdT, typename ValueT, typename Hash = std::hash<ValueT>,
          typename Equal = std::equal_to<ValueT>>
class CanonicalValueStore {
public:
  [[nodiscard]] IdT add(ValueT value) {
    if (const auto found = lookup_.find(value); found != lookup_.end()) {
      return found->second;
    }
    assert(values_.size() < AnyId::InvalidIndex);
    const auto id = IdT(static_cast<std::uint32_t>(values_.size()));
    values_.push_back(std::move(value));
    lookup_.emplace(values_.back(), id);
    return id;
  }

  [[nodiscard]] const ValueT &get(IdT id) const {
    assert(id.hasValue() && id.index < values_.size());
    return values_[id.index];
  }
  [[nodiscard]] const ValueT *tryGet(IdT id) const {
    return id.hasValue() && id.index < values_.size() ? &values_[id.index]
                                                      : nullptr;
  }
  [[nodiscard]] std::size_t size() const {
    return values_.size();
  }

  void reserve(std::size_t count) {
    lookup_.reserve(count);
  }

  void collectMetrics(CompilerMetrics &metrics, std::string_view label) const {
    metrics.addMemory(CompilerMetrics::childLabel(label, "values"),
                      values_.size() * sizeof(ValueT),
                      values_.size() * sizeof(ValueT));
    const auto buckets = lookup_.bucket_count();
    metrics.addMemory(
        CompilerMetrics::childLabel(label, "lookup"),
        lookup_.size() * sizeof(typename decltype(lookup_)::value_type),
        buckets * sizeof(void *) +
            lookup_.size() * sizeof(typename decltype(lookup_)::value_type));
  }

private:
  // Canonicalization can recursively intern values while callers retain
  // references to values already in the store. A deque keeps those references
  // stable across append operations; IDs remain dense numeric indexes.
  std::deque<ValueT> values_;
  std::unordered_map<ValueT, IdT, Hash, Equal> lookup_;
};

} // namespace chtholly::core
