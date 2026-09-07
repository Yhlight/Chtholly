#pragma once

#include "chtholly/Core/Id.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <llvm/Support/Allocator.h>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace chtholly::core {

class Arena {
public:
  Arena() = default;
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;

  [[nodiscard]] void *allocate(std::size_t bytes, std::size_t alignment) {
    return allocator_.Allocate(bytes, alignment);
  }

  [[nodiscard]] std::string_view copyString(std::string_view value) {
    auto *storage =
        static_cast<char *>(allocate(value.size() + 1, alignof(char)));
    if (!value.empty())
      std::memcpy(storage, value.data(), value.size());
    storage[value.size()] = '\0';
    return {storage, value.size()};
  }

  [[nodiscard]] std::uint64_t reservedBytes() const {
    return allocator_.getTotalMemory();
  }

  void collectMetrics(CompilerMetrics &metrics, std::string label) const {
    metrics.addMemory(std::move(label), reservedBytes(), reservedBytes());
  }

private:
  llvm::BumpPtrAllocator allocator_;
};

template <typename Tag> struct BlockId : IdBase<BlockId<Tag>> {
  using IdBase<BlockId<Tag>>::IdBase;
};

template <typename IdT, typename ElementT> class BlockStore {
  static_assert(std::is_trivially_copyable_v<ElementT>);
  static_assert(std::is_trivially_destructible_v<ElementT>);

  struct Block {
    const ElementT *data = nullptr;
    std::uint32_t size = 0;
  };

public:
  explicit BlockStore(Arena &arena) : arena_(&arena) {}

  [[nodiscard]] IdT add(std::span<const ElementT> values) {
    if (values.empty()) {
      return blocks_.add({});
    }
    assert(values.size() <= std::numeric_limits<std::uint32_t>::max());
    auto *storage = static_cast<ElementT *>(
        arena_->allocate(values.size_bytes(), alignof(ElementT)));
    std::memcpy(storage, values.data(), values.size_bytes());
    return blocks_.add({storage, static_cast<std::uint32_t>(values.size())});
  }

  [[nodiscard]] IdT addCanonical(std::span<const ElementT> values) {
    const auto hash = hashBlock(values);
    const auto [begin, end] = canonical_.equal_range(hash);
    for (auto current = begin; current != end; ++current) {
      const auto existing = get(current->second);
      if (existing.size() == values.size() &&
          (values.empty() || std::memcmp(existing.data(), values.data(),
                                         values.size_bytes()) == 0)) {
        return current->second;
      }
    }
    const auto id = add(values);
    canonical_.emplace(hash, id);
    return id;
  }

  [[nodiscard]] std::span<const ElementT> get(IdT id) const {
    const auto &block = blocks_.get(id);
    return {block.data, block.size};
  }

  [[nodiscard]] std::size_t size() const {
    return blocks_.size();
  }

  void collectMetrics(CompilerMetrics &metrics, std::string_view label) const {
    blocks_.collectMetrics(metrics,
                           CompilerMetrics::childLabel(label, "blocks"));
    metrics.addMemory(
        CompilerMetrics::childLabel(label, "canonical"),
        canonical_.size() * sizeof(typename decltype(canonical_)::value_type),
        canonical_.bucket_count() * sizeof(void *) +
            canonical_.size() *
                sizeof(typename decltype(canonical_)::value_type));
  }

private:
  static std::size_t hashBlock(std::span<const ElementT> values) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(values.data());
    std::size_t hash = sizeof(std::size_t) == 8
                           ? static_cast<std::size_t>(1469598103934665603ULL)
                           : static_cast<std::size_t>(2166136261U);
    const auto prime = sizeof(std::size_t) == 8
                           ? static_cast<std::size_t>(1099511628211ULL)
                           : static_cast<std::size_t>(16777619U);
    for (std::size_t index = 0; index < values.size_bytes(); ++index) {
      hash ^= bytes[index];
      hash *= prime;
    }
    return hash;
  }

  Arena *arena_;
  ValueStore<IdT, Block> blocks_;
  std::unordered_multimap<std::size_t, IdT> canonical_;
};

} // namespace chtholly::core
