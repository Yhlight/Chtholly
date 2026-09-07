#pragma once

#include "chtholly/Basic/SourceLocation.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {

class SourceBuffer;

struct DiagnosticPosition {
  std::string path;
  std::size_t byte = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

struct DiagnosticSpan {
  DiagnosticPosition start;
  DiagnosticPosition end;
};

struct DiagnosticRelated {
  DiagnosticSpan span;
  std::string message;
};

struct DiagnosticFixIt {
  DiagnosticSpan span;
  std::string replacement;
};

enum class DiagnosticSeverity {
  Note,
  Warning,
  Error,
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceLocation location;
  std::optional<DiagnosticSpan> primary_span;
  std::string message;
  std::string stable_code;
  std::vector<std::pair<std::string, std::string>> fields;
  std::vector<DiagnosticRelated> related;
  std::vector<DiagnosticFixIt> fixits;
};

class DiagnosticEngine {
public:
  using Snapshot = std::size_t;

  void report(DiagnosticSeverity severity, SourceLocation location, std::string message);
  void report(
      DiagnosticSeverity severity, SourceLocation location,
      std::string message, std::string stable_code,
      std::vector<std::pair<std::string, std::string>> fields = {});
  void error(SourceLocation location, std::string message);
  void error(
      SourceLocation location, std::string message, std::string stable_code,
      std::vector<std::pair<std::string, std::string>> fields = {});

  bool hasError() const;
  const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }
  std::string format(const SourceBuffer &source) const;
  std::string format(const std::vector<const SourceBuffer *> &sources) const;
  void materializeSpans(const std::vector<const SourceBuffer *> &sources);
  Snapshot snapshot() const { return diagnostics_.size(); }
  void restore(Snapshot snapshot);
  void deduplicateSince(Snapshot snapshot);
  void clear() { diagnostics_.clear(); }

private:
  std::vector<Diagnostic> diagnostics_;
};

std::string_view diagnosticSeverityName(DiagnosticSeverity severity);
std::string diagnosticStableCode(const Diagnostic &diagnostic);

} // namespace chtholly
