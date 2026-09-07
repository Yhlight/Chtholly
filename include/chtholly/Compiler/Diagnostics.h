#pragma once

#include "chtholly/Compiler/Token.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

enum class DiagnosticLevel : std::uint8_t { Note, Warning, Error };

enum class DiagnosticKind : std::uint16_t {
#define CHTHOLLY_COMPILER_DIAGNOSTIC(Name, Level, Code) Name,
#include "chtholly/Compiler/DiagnosticKind.def"
  Count,
};

struct Diagnostic {
  struct Note {
    DiagnosticLevel level = DiagnosticLevel::Note;
    DiagnosticKind kind = DiagnosticKind::InvalidParseTree;
    std::string code;
    std::string message;
    std::string path;
    std::uint32_t offset = 0;
    std::uint32_t length = 1;
  };

  DiagnosticKind kind = DiagnosticKind::InvalidParseTree;
  std::uint32_t offset = 0;
  std::uint32_t length = 1;
  TokenKind expected = TokenKind::Invalid;
  TokenKind actual = TokenKind::Invalid;
  std::vector<Note> notes;
};

class SourceBuffer;

class DiagnosticEmitter {
public:
  void emit(Diagnostic diagnostic);
  void emit(DiagnosticKind kind, std::uint32_t offset, std::uint32_t length = 1,
            TokenKind expected = TokenKind::Invalid,
            TokenKind actual = TokenKind::Invalid);

  [[nodiscard]] bool hasError() const;
  [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const {
    return diagnostics_;
  }
  [[nodiscard]] std::string format(const SourceBuffer &source) const;

private:
  std::vector<Diagnostic> diagnostics_;
};

[[nodiscard]] DiagnosticLevel diagnosticLevel(DiagnosticKind kind);
[[nodiscard]] std::string_view diagnosticCode(DiagnosticKind kind);
[[nodiscard]] std::string diagnosticMessage(const Diagnostic &diagnostic);

} // namespace chtholly::compiler
