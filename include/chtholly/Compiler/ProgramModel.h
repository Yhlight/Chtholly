#pragma once

#include <string_view>

namespace chtholly::compiler {

enum class ProgramEnvironment {
  Hosted,
};

inline constexpr ProgramEnvironment V1ProgramEnvironment =
    ProgramEnvironment::Hosted;
inline constexpr std::string_view V1SourceEntryName = "main";
inline constexpr std::string_view V1EmbeddedEntrySymbol = "chtholly.entry";

[[nodiscard]] constexpr bool isV1SourceEntryName(std::string_view name) {
  return name == V1SourceEntryName;
}

[[nodiscard]] constexpr std::string_view
v1HostedEntrySymbol(bool windows) {
  return windows ? "wmain" : "main";
}

} // namespace chtholly::compiler
