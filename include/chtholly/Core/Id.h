#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <string_view>

namespace chtholly::core {

struct AnyId {
  static constexpr std::uint32_t InvalidIndex =
      std::numeric_limits<std::uint32_t>::max();

  constexpr AnyId() = default;
  explicit constexpr AnyId(std::uint32_t index) : index(index) {}

  [[nodiscard]] constexpr bool hasValue() const {
    return index != InvalidIndex;
  }

  std::uint32_t index = InvalidIndex;
};

template <typename Derived> struct IdBase : AnyId {
  using AnyId::AnyId;

  static constexpr Derived invalid() {
    return Derived();
  }

  friend constexpr bool operator==(IdBase lhs, IdBase rhs) {
    return lhs.index == rhs.index;
  }
};

template <typename Derived> struct IndexBase : IdBase<Derived> {
  using IdBase<Derived>::IdBase;

  friend constexpr auto operator<=>(IndexBase lhs, IndexBase rhs) {
    return lhs.index <=> rhs.index;
  }
};

struct IdHash {
  template <typename IdT> std::size_t operator()(IdT id) const noexcept {
    return static_cast<std::size_t>(id.index);
  }
};

static_assert(sizeof(AnyId) == 4);

} // namespace chtholly::core
