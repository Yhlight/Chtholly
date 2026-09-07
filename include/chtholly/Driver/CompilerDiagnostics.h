#pragma once

#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/Source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace chtholly {

struct CompilerSourceDiagnostic {
  struct Related {
    compiler::DiagnosticLevel level = compiler::DiagnosticLevel::Note;
    std::string code;
    std::string message;
    std::string path;
    std::uint32_t offset = 0;
    std::uint32_t length = 1;
    compiler::LineColumn location;
    // Related evidence may point at an imported artifact whose source is not
    // present in the current request. Keep this explicit so clients do not
    // render a fabricated location.
    bool location_available = false;
  };

  std::string path;
  compiler::DiagnosticLevel level = compiler::DiagnosticLevel::Error;
  std::string code;
  std::string message;
  std::uint32_t offset = 0;
  std::uint32_t length = 1;
  compiler::LineColumn location;
  std::vector<Related> related;
};

} // namespace chtholly
