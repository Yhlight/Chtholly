#pragma once

#include <cstdint>

namespace chtholly::compiler {

enum class ModuleEmissionRole : std::uint8_t {
  Library,
  ExecutableEntry,
  CoroutineExecutionEntry,
  ComponentLibrary,
  Count,
};

enum class DebugInfoMode : std::uint8_t {
  None,
  LineTablesOnly,
  Full,
};

} // namespace chtholly::compiler
