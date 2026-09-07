#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <type_traits>
#include <vector>

namespace chtholly::support {

// Internal open-addressing storage shared by the native container bridge.
// Keys and values are constructed only in occupied buckets; deleted buckets
// retain no payload and are reused by the next insertion.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class OpenAddressingHashTable {
public:
  static_assert(std::is_nothrow_move_constructible_v<Key> &&
                    std::is_nothrow_move_constructible_v<Value>,
                "container rehash requires noexcept-movable entries");
  enum class Status : std::uint8_t { Ok, OutOfMemory, CapacityOverflow };

  explicit OpenAddressingHashTable(std::uint64_t seed = 0x9e3779b97f4a7c15ULL)
      : seed_(seed) {}

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return buckets_.size(); }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

  [[nodiscard]] const Value *find(const Key &key) const noexcept {
    const auto slot = locate(key);
    return slot.has_value() ? &buckets_[*slot].value.value() : nullptr;
  }

  [[nodiscard]] Value *find(const Key &key) noexcept {
    const auto slot = locate(key);
    return slot.has_value() ? &buckets_[*slot].value.value() : nullptr;
  }

  Status reserve(std::size_t requested) {
    if (requested <= buckets_.size())
      return Status::Ok;
    std::size_t capacity = buckets_.empty() ? 8 : buckets_.size();
    while (capacity < requested) {
      if (capacity > std::numeric_limits<std::size_t>::max() / 2)
        return Status::CapacityOverflow;
      capacity *= 2;
    }
    return rehash(capacity);
  }

  Status insert(Key key, Value value, std::optional<Value> &replaced) {
    replaced.reset();
    const auto occupied_or_deleted_overflow =
        deleted_ > std::numeric_limits<std::size_t>::max() - size_;
    const auto saturated =
        occupied_or_deleted_overflow ||
        (size_ + deleted_) >
            (std::numeric_limits<std::size_t>::max() / 4);
    if (buckets_.empty() || saturated ||
        (size_ + deleted_) * 4 >= buckets_.size() * 3) {
      const auto requested = buckets_.empty() ? std::size_t{8}
                                              : buckets_.size() >
                                                        std::numeric_limits<std::size_t>::max() / 2
                                                    ? 0
                                                    : buckets_.size() * 2;
      if (requested == 0)
        return Status::CapacityOverflow;
      const auto status = reserve(requested);
      if (status != Status::Ok)
        return status;
    }
    const auto hash = hashKey(key);
    std::optional<std::size_t> tombstone;
    for (std::size_t step = 0; step < buckets_.size(); ++step) {
      const auto slot = probe(hash, step, buckets_.size());
      auto &bucket = buckets_[slot];
      if (bucket.state == BucketState::Empty) {
        return occupy(tombstone.value_or(slot), hash, std::move(key),
                      std::move(value));
      }
      if (bucket.state == BucketState::Deleted) {
        if (!tombstone)
          tombstone = slot;
        continue;
      }
      if (bucket.hash == hash && equal_(bucket.key.value(), key)) {
        replaced.emplace(std::move(bucket.value.value()));
        bucket.value.emplace(std::move(value));
        return Status::Ok;
      }
    }
    return tombstone ? occupy(*tombstone, hash, std::move(key), std::move(value))
                     : Status::CapacityOverflow;
  }

  bool erase(const Key &key, std::optional<Value> &removed) {
    removed.reset();
    const auto slot = locate(key);
    if (!slot)
      return false;
    auto &bucket = buckets_[*slot];
    removed.emplace(std::move(bucket.value.value()));
    bucket.value.reset();
    bucket.key.reset();
    bucket.state = BucketState::Deleted;
    --size_;
    ++deleted_;
    return true;
  }

  void clear() noexcept {
    for (auto &bucket : buckets_) {
      bucket.key.reset();
      bucket.value.reset();
      bucket.state = BucketState::Empty;
    }
    size_ = 0;
    deleted_ = 0;
  }

private:
  enum class BucketState : std::uint8_t { Empty, Occupied, Deleted };
  struct Bucket {
    BucketState state = BucketState::Empty;
    std::uint64_t hash = 0;
    std::optional<Key> key;
    std::optional<Value> value;
  };

  [[nodiscard]] std::uint64_t hashKey(const Key &key) const noexcept {
    return static_cast<std::uint64_t>(hash_(key)) ^ seed_;
  }

  [[nodiscard]] static std::size_t probe(std::uint64_t hash,
                                          std::size_t step,
                                          std::size_t capacity) noexcept {
    const auto offset = step + step * step;
    return (static_cast<std::size_t>(hash) + offset / 2) & (capacity - 1);
  }

  [[nodiscard]] std::optional<std::size_t> locate(const Key &key) const noexcept {
    if (buckets_.empty())
      return std::nullopt;
    const auto hash = hashKey(key);
    for (std::size_t step = 0; step < buckets_.size(); ++step) {
      const auto slot = probe(hash, step, buckets_.size());
      const auto &bucket = buckets_[slot];
      if (bucket.state == BucketState::Empty)
        return std::nullopt;
      if (bucket.state == BucketState::Occupied && bucket.hash == hash &&
          equal_(bucket.key.value(), key))
        return slot;
    }
    return std::nullopt;
  }

  Status occupy(std::size_t slot, std::uint64_t hash, Key key,
                Value value) {
    auto &bucket = buckets_[slot];
    std::optional<Key> key_holder;
    std::optional<Value> value_holder;
    try {
      key_holder.emplace(std::move(key));
      value_holder.emplace(std::move(value));
    } catch (...) {
      return Status::OutOfMemory;
    }
    if (bucket.state == BucketState::Deleted)
      --deleted_;
    bucket.hash = hash;
    bucket.key = std::move(key_holder);
    bucket.value = std::move(value_holder);
    bucket.state = BucketState::Occupied;
    ++size_;
    return Status::Ok;
  }

  Status rehash(std::size_t capacity) noexcept {
    try {
      std::vector<Bucket> candidate(capacity);
      for (auto &bucket : buckets_) {
        if (bucket.state != BucketState::Occupied)
          continue;
        std::size_t slot = probe(bucket.hash, 0, capacity);
        for (std::size_t step = 1;
             candidate[slot].state == BucketState::Occupied; ++step)
          slot = probe(bucket.hash, step, capacity);
        candidate[slot].state = BucketState::Occupied;
        candidate[slot].hash = bucket.hash;
        candidate[slot].key.emplace(std::move(bucket.key.value()));
        candidate[slot].value.emplace(std::move(bucket.value.value()));
      }
      buckets_.swap(candidate);
      deleted_ = 0;
      return Status::Ok;
    } catch (...) {
      return Status::OutOfMemory;
    }
  }

  std::vector<Bucket> buckets_;
  std::size_t size_ = 0;
  std::size_t deleted_ = 0;
  std::uint64_t seed_;
  Hash hash_;
  Equal equal_;
};

} // namespace chtholly::support
