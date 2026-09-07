#include "chtholly/Driver/CliOutput.h"
#include "chtholly/Driver/CompilerDiagnostics.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace chtholly {
namespace {

std::string jsonEscape(std::string_view text) {
  std::ostringstream out;
  for (const unsigned char ch : text) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<unsigned int>(ch) << std::dec;
      } else {
        out << static_cast<char>(ch);
      }
      break;
    }
  }
  return out.str();
}

bool containsInsensitive(std::string_view text, std::string_view needle) {
  return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                     [](char lhs, char rhs) {
                       return std::tolower(static_cast<unsigned char>(lhs)) ==
                              std::tolower(static_cast<unsigned char>(rhs));
                     }) != text.end();
}

std::string positionJson(const DiagnosticPosition &position) {
  return "{\"byte\":" + std::to_string(position.byte) +
         ",\"line\":" + std::to_string(position.line) +
         ",\"column\":" + std::to_string(position.column) + "}";
}

std::string spanJson(const DiagnosticSpan &span) {
  return "{\"path\":\"" + jsonEscape(span.start.path) +
         "\",\"range\":{\"start\":" + positionJson(span.start) +
         ",\"end\":" + positionJson(span.end) + "}}";
}

} // namespace

bool filterCliOutputArguments(int argc, char **argv,
                              std::vector<std::string> &arguments,
                              CliOutputFormat &format, std::string &error) {
  arguments.clear();
  format = CliOutputFormat::Human;
  error.clear();
  arguments.emplace_back(argc > 0 ? argv[0] : "chthollyc");
  bool selected = false;
  bool passthrough = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--") {
      passthrough = true;
      arguments.emplace_back(argument);
      continue;
    }
    if (passthrough) {
      arguments.emplace_back(argument);
      continue;
    }
    std::string_view value;
    if (argument == "--output-format") {
      if (index + 1 >= argc) {
        error = "--output-format requires human, jsonl, or jsonl-v1";
        return false;
      }
      value = argv[++index];
    } else if (argument.starts_with("--output-format=")) {
      value = argument.substr(std::string_view("--output-format=").size());
    } else {
      arguments.emplace_back(argument);
      continue;
    }
    CliOutputFormat parsed;
    if (value == "human") {
      parsed = CliOutputFormat::Human;
    } else if (value == "jsonl") {
      parsed = CliOutputFormat::JsonLines;
    } else if (value == "jsonl-v1") {
      parsed = CliOutputFormat::JsonLinesV1;
    } else {
      error = "--output-format requires human, jsonl, or jsonl-v1";
      return false;
    }
    if (selected && parsed != format) {
      error = "--output-format cannot select multiple formats";
      return false;
    }
    selected = true;
    format = parsed;
  }
  return true;
}

std::string cliFailureCode(const CompilerInvocation *invocation,
                           std::string_view message, bool argument_error) {
  if (argument_error || invocation == nullptr) {
    return "chtholly.cli.invalid-arguments";
  }
  if (invocation->workflow == DriverWorkflow::Doctor) {
    return "chtholly.doctor.failed";
  }
  // Keep compatibility failures actionable at the CLI boundary. The
  // underlying package/artifact loaders still provide their detailed text;
  // this stable family code lets JSONL/LSP consumers route the failure without
  // matching human wording.
  if (containsInsensitive(message, "abi mismatch") ||
      containsInsensitive(message, "component abi") ||
      containsInsensitive(message, "runtime abi")) {
    return "chtholly.abi.mismatch";
  }
  if (containsInsensitive(message, "artifact") &&
      (containsInsensitive(message, "mismatch") ||
       containsInsensitive(message, "epoch") ||
       containsInsensitive(message, "fingerprint") ||
       containsInsensitive(message, "format"))) {
    return "chtholly.artifact.compatibility";
  }
  if (containsInsensitive(message, "package") &&
      (containsInsensitive(message, "mismatch") ||
       containsInsensitive(message, "epoch") ||
       containsInsensitive(message, "format"))) {
    return "chtholly.package.compatibility";
  }
  const auto action = invocation->action;
  if (containsInsensitive(message, "manifest") ||
      containsInsensitive(message, "workspace") ||
      containsInsensitive(message, "package graph")) {
    return "chtholly.manifest.error";
  }
  if (containsInsensitive(message, "linker") ||
      containsInsensitive(message, "link failed") ||
      containsInsensitive(message, "undefined symbol")) {
    return "chtholly.link.error";
  }
  if (containsInsensitive(message, "lexer") ||
      containsInsensitive(message, "unterminated") ||
      containsInsensitive(message, "invalid character")) {
    return "chtholly.lex.error";
  }
  if (containsInsensitive(message, "parse") ||
      containsInsensitive(message, "expected")) {
    return "chtholly.parse.error";
  }
  if (containsInsensitive(message, "codegen") ||
      containsInsensitive(message, "backend")) {
    return "chtholly.codegen.error";
  }
  if (action == DriverAction::EmitLLVM ||
      action == DriverAction::EmitObject ||
      action == DriverAction::EmitExecutable ||
      action == DriverAction::Check) {
    return "chtholly.sema.error";
  }
  return "chtholly.cli.operation-failed";
}

CliOutputSink::CliOutputSink(CliOutputFormat format, std::ostream &out,
                             std::ostream &error)
    : format_(format), out_(out), error_(error) {}

void CliOutputSink::jsonRecord(
    std::string_view kind,
    const std::vector<std::pair<std::string_view, std::string>> &fields) {
  out_ << "{\"schema\":\"chtholly-cli-jsonl-v1\",\"sequence\":" << ++sequence_
       << ",\"kind\":\"" << jsonEscape(kind) << '"';
  for (const auto &[name, value] : fields) {
    out_ << ",\"" << jsonEscape(name) << "\":\"" << jsonEscape(value) << '"';
  }
  out_ << "}\n";
}

void CliOutputSink::jsonRecordV2(
    std::string_view kind,
    const std::vector<std::pair<std::string_view, std::string>> &fields,
    const std::vector<std::pair<std::string_view, std::string>> &raw_fields) {
  out_ << "{\"schema\":\"chtholly-cli-jsonl-v2\",\"sequence\":" << ++sequence_
       << ",\"kind\":\"" << jsonEscape(kind) << '\"';
  for (const auto &[name, value] : fields) {
    out_ << ",\"" << jsonEscape(name) << "\":\"" << jsonEscape(value) << '\"';
  }
  for (const auto &[name, value] : raw_fields) {
    out_ << ",\"" << jsonEscape(name) << "\":" << value;
  }
  out_ << "}\n";
}

void CliOutputSink::output(std::string_view text) {
  if (format_ == CliOutputFormat::Human) {
    out_ << text;
    return;
  }
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    auto line =
        text.substr(begin, end == std::string_view::npos ? text.size() - begin
                                                         : end - begin);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!line.empty()) {
      const auto tab = line.find('\t');
      if (tab != std::string_view::npos &&
          line.find('\t', tab + 1) == std::string_view::npos) {
        if (format_ == CliOutputFormat::JsonLinesV1) {
          jsonRecord("command-output",
                     {{"name", std::string(line.substr(0, tab))},
                      {"value", std::string(line.substr(tab + 1))},
                      {"text", std::string(line)}});
        } else {
          jsonRecordV2("command-output",
                       {{"name", std::string(line.substr(0, tab))},
                        {"value", std::string(line.substr(tab + 1))},
                        {"text", std::string(line)}});
        }
      } else {
        if (format_ == CliOutputFormat::JsonLinesV1) {
          jsonRecord("command-output", {{"text", std::string(line)}});
        } else {
          jsonRecordV2("command-output", {{"text", std::string(line)}});
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
}

void CliOutputSink::diagnostic(std::string_view code, std::string_view message,
                               std::string_view severity) {
  if (format_ == CliOutputFormat::Human) {
    error_ << "chthollyc[" << code << "]: " << message;
    if (message.empty() || message.back() != '\n') {
      error_ << '\n';
    }
    return;
  }
  if (format_ == CliOutputFormat::JsonLinesV1) {
    jsonRecord("diagnostic", {{"severity", std::string(severity)},
                              {"code", std::string(code)},
                              {"message", std::string(message)}});
  } else {
    jsonRecordV2("diagnostic", {{"severity", std::string(severity)},
                                {"code", std::string(code)},
                                {"message", std::string(message)}});
  }
}

void CliOutputSink::diagnostic(const Diagnostic &diagnostic) {
  const auto code = diagnosticStableCode(diagnostic);
  if (format_ == CliOutputFormat::Human) {
    this->diagnostic(code, diagnostic.message,
                     diagnosticSeverityName(diagnostic.severity));
    return;
  }
  std::vector<std::pair<std::string_view, std::string>> fields{
      {"severity", std::string(diagnosticSeverityName(diagnostic.severity))},
      {"code", code},
      {"message", diagnostic.message}};
  fields.reserve(fields.size() + diagnostic.fields.size());
  for (const auto &[name, value] : diagnostic.fields) {
    if (!name.empty() && !value.empty()) {
      fields.emplace_back(name, value);
    }
  }
  if (format_ == CliOutputFormat::JsonLinesV1) {
    jsonRecord("diagnostic", fields);
    return;
  }
  fields.resize(3);
  std::vector<std::pair<std::string_view, std::string>> raw_fields;
  if (diagnostic.primary_span) {
    raw_fields.emplace_back("primary", spanJson(*diagnostic.primary_span));
  }
  std::string related = "[";
  for (std::size_t index = 0; index < diagnostic.related.size(); ++index) {
    const auto &item = diagnostic.related[index];
    if (index != 0)
      related += ',';
    related += "{\"message\":\"" + jsonEscape(item.message) +
               "\",\"span\":" + spanJson(item.span) + "}";
  }
  related += ']';
  std::string fixits = "[";
  for (std::size_t index = 0; index < diagnostic.fixits.size(); ++index) {
    const auto &item = diagnostic.fixits[index];
    if (index != 0)
      fixits += ',';
    fixits += "{\"replacement\":\"" + jsonEscape(item.replacement) +
              "\",\"span\":" + spanJson(item.span) + "}";
  }
  fixits += ']';
  raw_fields.emplace_back("related", std::move(related));
  raw_fields.emplace_back("fixits", std::move(fixits));
  std::string structured_fields = "{";
  for (std::size_t index = 0; index < diagnostic.fields.size(); ++index) {
    if (index != 0)
      structured_fields += ',';
    structured_fields += "\"" + jsonEscape(diagnostic.fields[index].first) +
                         "\":\"" + jsonEscape(diagnostic.fields[index].second) +
                         "\"";
  }
  structured_fields += '}';
  raw_fields.emplace_back("fields", std::move(structured_fields));
  jsonRecordV2("diagnostic", fields, raw_fields);
}

void CliOutputSink::diagnostic(const CompilerSourceDiagnostic &diagnostic) {
  const auto severity = diagnostic.level == compiler::DiagnosticLevel::Error
                            ? "error"
                        : diagnostic.level == compiler::DiagnosticLevel::Warning
                            ? "warning"
                            : "note";
  if (format_ == CliOutputFormat::Human) {
    const auto line = diagnostic.location.line == 0 ? 1 : diagnostic.location.line;
    const auto column = diagnostic.location.column == 0 ? 1 : diagnostic.location.column;
    error_ << diagnostic.path << ':' << line << ':' << column << ": "
           << severity << ": " << diagnostic.message << " ["
           << diagnostic.code << "]\n";
    for (const auto &item : diagnostic.related) {
      error_ << (item.path.empty() ? diagnostic.path : item.path) << ':';
      if (item.location_available)
        error_ << item.location.line << ':' << item.location.column;
      else
        error_ << "?:?";
      error_ << ": note: " << item.message << " [" << item.code << "]";
      if (!item.location_available)
        error_ << " (source unavailable)";
      error_ << '\n';
    }
    return;
  }
  std::vector<std::pair<std::string_view, std::string>> fields{
      {"severity", severity}, {"code", diagnostic.code},
      {"message", diagnostic.message}};
  if (format_ == CliOutputFormat::JsonLinesV1) {
    jsonRecord("diagnostic", fields);
    return;
  }
  std::string related = "[";
  for (std::size_t index = 0; index < diagnostic.related.size(); ++index) {
    if (index != 0)
      related += ',';
    const auto &item = diagnostic.related[index];
    related += "{\"code\":\"" + jsonEscape(item.code) +
               "\",\"message\":\"" + jsonEscape(item.message) +
               "\",\"path\":\"" + jsonEscape(item.path) +
               "\",\"offset\":" + std::to_string(item.offset) +
               ",\"length\":" + std::to_string(item.length) +
               ",\"line\":" + std::to_string(item.location.line) +
               ",\"column\":" + std::to_string(item.location.column) +
               ",\"location-available\":" +
               (item.location_available ? "true" : "false") +
               "}";
  }
  related += ']';
  const std::string primary =
      "{\"path\":\"" + jsonEscape(diagnostic.path) +
      "\",\"offset\":" + std::to_string(diagnostic.offset) +
      ",\"length\":" + std::to_string(diagnostic.length) +
      ",\"line\":" + std::to_string(diagnostic.location.line) +
      ",\"column\":" + std::to_string(diagnostic.location.column) + "}";
  jsonRecordV2("diagnostic", fields,
               {{"primary", primary}, {"related", std::move(related)}});
}

void CliOutputSink::invalidation(
    const WorkspaceArtifactResult::InvalidationExplanation &explanation) {
  std::ostringstream text;
  text << "incremental module=" << explanation.module
       << " query-kind=" << explanation.query_kind
       << " query-key=" << explanation.query_key
       << " result=" << explanation.result << " reason=" << explanation.reason;
  if (!explanation.provider.empty())
    text << " provider=" << explanation.provider;
  if (!explanation.record_id.empty())
    text << " record-id=" << explanation.record_id;
  if (!explanation.old_digest.empty() && !explanation.new_digest.empty() &&
      explanation.old_digest != explanation.new_digest)
    text << " old-digest=" << explanation.old_digest
         << " new-digest=" << explanation.new_digest;
  if (explanation.evaluated_records != 0)
    text << " records=" << explanation.evaluated_records
         << " hits=" << explanation.hit_records
         << " misses=" << explanation.miss_records;
  if (format_ == CliOutputFormat::Human) {
    out_ << text.str() << '\n';
    return;
  }
  if (format_ == CliOutputFormat::JsonLinesV1) {
    jsonRecord("command-output", {{"text", text.str()}});
    return;
  }
  std::string chain = "[";
  for (std::size_t index = 0; index < explanation.chain.size(); ++index) {
    if (index != 0)
      chain += ',';
    chain += '"';
    chain += jsonEscape(explanation.chain[index]);
    chain += '"';
  }
  chain += ']';
  jsonRecordV2(
      "incremental-invalidation",
      {{"module", explanation.module},
       {"query-kind", explanation.query_kind},
       {"query-key", explanation.query_key},
       {"result", explanation.result},
       {"reason", explanation.reason},
       {"provider", explanation.provider},
       {"record-id", explanation.record_id},
       {"old-digest", explanation.old_digest},
       {"new-digest", explanation.new_digest}},
      {{"chain", std::move(chain)},
       {"evaluated-records", std::to_string(explanation.evaluated_records)},
       {"hit-records", std::to_string(explanation.hit_records)},
       {"miss-records", std::to_string(explanation.miss_records)}});
}

void CliOutputSink::result(std::string_view action, int exit_code) {
  if (format_ == CliOutputFormat::JsonLinesV1) {
    jsonRecord("command-result",
               {{"action", std::string(action)},
                {"status", exit_code == 0 ? "success" : "failure"},
                {"exit-code", std::to_string(exit_code)}});
  } else if (format_ == CliOutputFormat::JsonLines) {
    jsonRecordV2("command-result",
                 {{"action", std::string(action)},
                  {"status", exit_code == 0 ? "success" : "failure"}},
                 {{"exit-code", std::to_string(exit_code)}});
  }
}

} // namespace chtholly
