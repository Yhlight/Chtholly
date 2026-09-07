#pragma once

#include <charconv>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace chtholly {

struct LanguageVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;

  [[nodiscard]] std::string str() const {
    return std::to_string(major) + "." + std::to_string(minor);
  }

  [[nodiscard]] static std::optional<LanguageVersion>
  parse(std::string_view text) {
    const auto separator = text.find('.');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == text.size() ||
        text.find('.', separator + 1) != std::string_view::npos)
      return std::nullopt;
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    const auto parse_component = [](std::string_view component,
                                    std::uint32_t &value) {
      if (component.size() > 1 && component.front() == '0')
        return false;
      const auto parsed = std::from_chars(
          component.data(), component.data() + component.size(), value);
      return parsed.ec == std::errc{} &&
             parsed.ptr == component.data() + component.size();
    };
    if (!parse_component(text.substr(0, separator), major) ||
        !parse_component(text.substr(separator + 1), minor))
      return std::nullopt;
    return LanguageVersion{major, minor};
  }

  friend bool operator==(const LanguageVersion &,
                         const LanguageVersion &) = default;
  friend auto operator<=>(const LanguageVersion &,
                          const LanguageVersion &) = default;
};

inline constexpr LanguageVersion FrozenV1LanguageVersion{1, 0};
inline constexpr LanguageVersion FrozenV11LanguageVersion{1, 1};
inline constexpr LanguageVersion FrozenV12LanguageVersion{1, 2};
inline constexpr LanguageVersion FrozenV13LanguageVersion{1, 3};
inline constexpr LanguageVersion FrozenV14LanguageVersion{1, 4};
inline constexpr LanguageVersion FrozenV15LanguageVersion{1, 5};
inline constexpr LanguageVersion FrozenV16LanguageVersion{1, 6};
inline constexpr LanguageVersion FrozenV17LanguageVersion{1, 7};
inline constexpr LanguageVersion FrozenV18LanguageVersion{1, 8};
inline constexpr LanguageVersion FrozenV19LanguageVersion{1, 9};
inline constexpr LanguageVersion FrozenV110LanguageVersion{1, 10};
inline constexpr LanguageVersion DefaultLanguageVersion =
    FrozenV1LanguageVersion;
inline constexpr LanguageVersion LatestLanguageVersion =
    FrozenV110LanguageVersion;
// Retained for code that means the stable default rather than the newest
// opt-in candidate.
inline constexpr LanguageVersion CurrentLanguageVersion =
    DefaultLanguageVersion;
// Conditional callable postconditions change the meaning of persisted
// ownership summaries; older semantic artifacts must not be replayed.
inline constexpr std::uint32_t CurrentSemanticArtifactEpoch = 27;
inline constexpr std::uint32_t CurrentStandardLibraryEpoch = 13;

[[nodiscard]] constexpr bool isSupportedLanguageVersion(LanguageVersion value) {
  return value == FrozenV1LanguageVersion ||
         value == FrozenV11LanguageVersion ||
         value == FrozenV12LanguageVersion ||
         value == FrozenV13LanguageVersion ||
         value == FrozenV14LanguageVersion ||
         value == FrozenV15LanguageVersion ||
         value == FrozenV16LanguageVersion ||
         value == FrozenV17LanguageVersion ||
         value == FrozenV18LanguageVersion ||
         value == FrozenV19LanguageVersion ||
         value == FrozenV110LanguageVersion;
}

[[nodiscard]] constexpr bool usesWideSliceIndices(LanguageVersion value) {
  return value >= FrozenV110LanguageVersion;
}

struct LanguageContract {
  LanguageVersion source = DefaultLanguageVersion;
  std::uint32_t semantic_artifact_epoch = CurrentSemanticArtifactEpoch;
  std::uint32_t standard_library_epoch = CurrentStandardLibraryEpoch;

  [[nodiscard]] bool isSupported() const {
    return isSupportedLanguageVersion(source) &&
           semantic_artifact_epoch == CurrentSemanticArtifactEpoch &&
           standard_library_epoch == CurrentStandardLibraryEpoch;
  }

  [[nodiscard]] bool
  isDependencyCompatibleWith(const LanguageContract &dependency) const {
    return semantic_artifact_epoch == dependency.semantic_artifact_epoch &&
           standard_library_epoch == dependency.standard_library_epoch;
  }

  friend bool operator==(const LanguageContract &,
                         const LanguageContract &) = default;
};

inline constexpr LanguageContract CurrentLanguageContract{};

} // namespace chtholly
