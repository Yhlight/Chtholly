#include "chtholly/Basic/Diagnostic.h"

#include "chtholly/Basic/SourceBuffer.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace chtholly {

void DiagnosticEngine::report(DiagnosticSeverity severity, SourceLocation location, std::string message) {
  diagnostics_.push_back(Diagnostic{severity, location, std::nullopt,
                                    std::move(message)});
}

void DiagnosticEngine::report(
    DiagnosticSeverity severity, SourceLocation location, std::string message,
    std::string stable_code,
    std::vector<std::pair<std::string, std::string>> fields) {
  diagnostics_.push_back(Diagnostic{severity, location, std::nullopt,
                                    std::move(message),
                                    std::move(stable_code),
                                    std::move(fields)});
}

void DiagnosticEngine::error(SourceLocation location, std::string message) {
  report(DiagnosticSeverity::Error, location, std::move(message));
}

void DiagnosticEngine::error(
    SourceLocation location, std::string message, std::string stable_code,
    std::vector<std::pair<std::string, std::string>> fields) {
  report(DiagnosticSeverity::Error, location, std::move(message),
         std::move(stable_code), std::move(fields));
}

bool DiagnosticEngine::hasError() const {
  for (const auto &diagnostic : diagnostics_) {
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      return true;
    }
  }
  return false;
}

std::string DiagnosticEngine::format(const SourceBuffer &source) const {
  return format(std::vector<const SourceBuffer *>{&source});
}

std::string DiagnosticEngine::format(
    const std::vector<const SourceBuffer *> &sources) const {
  std::ostringstream out;
  for (const auto &diagnostic : diagnostics_) {
    const SourceBuffer *source = nullptr;
    if (diagnostic.location.isValid()) {
      for (const auto *candidate : sources) {
        if (candidate != nullptr && candidate->owns(diagnostic.location)) {
          source = candidate;
          break;
        }
      }
    }
    if (source != nullptr) {
      const auto lc = source->lineColumn(diagnostic.location);
      out << source->filename() << ':' << lc.line << ':' << lc.column << ": "
          << diagnosticSeverityName(diagnostic.severity) << ": "
          << diagnostic.message << " [" << diagnosticStableCode(diagnostic)
          << "]\n";
      out << source->lineText(lc.line) << '\n';
      for (std::size_t i = 1; i < lc.column; ++i) {
        out << ' ';
      }
      out << "^\n";
    } else {
      out << diagnosticSeverityName(diagnostic.severity) << ": "
          << diagnostic.message << " [" << diagnosticStableCode(diagnostic)
          << "]\n";
    }
  }
  return out.str();
}

void DiagnosticEngine::materializeSpans(
    const std::vector<const SourceBuffer *> &sources) {
  for (auto &diagnostic : diagnostics_) {
    if (diagnostic.primary_span || !diagnostic.location.isValid()) {
      continue;
    }
    const auto found = std::find_if(
        sources.begin(), sources.end(), [&](const SourceBuffer *source) {
          return source != nullptr && source->owns(diagnostic.location);
        });
    if (found == sources.end()) {
      continue;
    }
    const auto &source = **found;
    const auto start = source.lineColumn(diagnostic.location);
    const auto end_offset =
        std::min(diagnostic.location.offset() + 1, source.size());
    const auto end_location = source.location(end_offset);
    const auto end = source.lineColumn(end_location);
    std::error_code path_error;
    auto path = std::filesystem::absolute(
        std::filesystem::path(source.filename()), path_error);
    const auto normalized_path =
        path_error ? std::string(source.filename()) : path.lexically_normal().string();
    DiagnosticSpan span;
    span.start = {normalized_path, diagnostic.location.offset(),
                  start.line, start.column};
    span.end = {normalized_path, end_offset, end.line,
                end.column};
    diagnostic.primary_span = std::move(span);
  }
}

void DiagnosticEngine::restore(Snapshot snapshot) {
  if (snapshot < diagnostics_.size()) {
    diagnostics_.resize(snapshot);
  }
}

void DiagnosticEngine::deduplicateSince(Snapshot snapshot) {
  if (snapshot >= diagnostics_.size()) {
    return;
  }
  auto same = [](const Diagnostic &lhs, const Diagnostic &rhs) {
    return lhs.severity == rhs.severity &&
           lhs.location.isValid() == rhs.location.isValid() &&
           (!lhs.location.isValid() ||
            (lhs.location.sourceFileId() == rhs.location.sourceFileId() &&
             lhs.location.offset() == rhs.location.offset())) &&
           lhs.message == rhs.message &&
           lhs.stable_code == rhs.stable_code && lhs.fields == rhs.fields;
  };
  auto write = diagnostics_.begin() + static_cast<std::ptrdiff_t>(snapshot);
  for (auto read = write; read != diagnostics_.end(); ++read) {
    if (std::find_if(diagnostics_.begin(), write, [&](const Diagnostic &prior) {
          return same(prior, *read);
        }) != write) {
      continue;
    }
    if (write != read) {
      *write = std::move(*read);
    }
    ++write;
  }
  diagnostics_.erase(write, diagnostics_.end());
}

std::string_view diagnosticSeverityName(DiagnosticSeverity severity) {
  switch (severity) {
  case DiagnosticSeverity::Note:
    return "note";
  case DiagnosticSeverity::Warning:
    return "warning";
  case DiagnosticSeverity::Error:
    return "error";
  }
  return "error";
}

std::string diagnosticStableCode(const Diagnostic &diagnostic) {
  if (!diagnostic.stable_code.empty()) {
    return diagnostic.stable_code;
  }
  switch (diagnostic.severity) {
  case DiagnosticSeverity::Note:
    return "chtholly.source.note";
  case DiagnosticSeverity::Warning:
    return "chtholly.source.warning";
  case DiagnosticSeverity::Error:
    return "chtholly.source.error";
  }
  return "chtholly.source.error";
}

} // namespace chtholly
