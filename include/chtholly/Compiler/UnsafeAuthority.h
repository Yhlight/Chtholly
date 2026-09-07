#pragma once

#include "chtholly/Compiler/Diagnostics.h"

#include <cstdint>
#include <string_view>

namespace chtholly::compiler {

enum class UnsafeOperationKind : std::uint8_t {
#define CHTHOLLY_COMPILER_UNSAFE_OPERATION(Name, Diagnostic, Anchor) Name,
#include "chtholly/Compiler/UnsafeAuthority.def"
  Count,
};

[[nodiscard]] constexpr DiagnosticKind
unsafeOperationDiagnostic(UnsafeOperationKind kind) {
  switch (kind) {
#define CHTHOLLY_COMPILER_UNSAFE_OPERATION(Name, Diagnostic, Anchor)               \
  case UnsafeOperationKind::Name:                                              \
    return DiagnosticKind::Diagnostic;
#include "chtholly/Compiler/UnsafeAuthority.def"
  case UnsafeOperationKind::Count:
    return DiagnosticKind::InvalidSemanticShape;
  }
  return DiagnosticKind::InvalidSemanticShape;
}

[[nodiscard]] constexpr std::string_view
unsafeOperationName(UnsafeOperationKind kind) {
  switch (kind) {
#define CHTHOLLY_COMPILER_UNSAFE_OPERATION(Name, Diagnostic, Anchor)               \
  case UnsafeOperationKind::Name:                                              \
    return Anchor;
#include "chtholly/Compiler/UnsafeAuthority.def"
  case UnsafeOperationKind::Count:
    return "invalid";
  }
  return "invalid";
}

} // namespace chtholly::compiler
