#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/Source.h"
#include "chtholly/Driver/CliOutput.h"
#include "chtholly/Driver/CompilerDiagnostics.h"

#include "test_check.h"
#include <sstream>
#include <string>
#include <utility>

int main() {
  for (std::uint16_t index = 0;
       index < static_cast<std::uint16_t>(
                   chtholly::compiler::DiagnosticKind::Count);
       ++index) {
    const chtholly::compiler::Diagnostic diagnostic{
        .kind = static_cast<chtholly::compiler::DiagnosticKind>(index),
        .offset = 0,
        .length = 1};
    CHTHOLLY_TEST_CHECK(chtholly::compiler::diagnosticMessage(diagnostic) !=
           "unknown diagnostic");
  }
  const chtholly::compiler::Diagnostic missing_hash{
      .kind = chtholly::compiler::DiagnosticKind::MissingHashWitness};
  CHTHOLLY_TEST_CHECK(chtholly::compiler::diagnosticCode(missing_hash.kind) ==
         "chtholly.next.sem.witness.missing-hash");
  CHTHOLLY_TEST_CHECK(chtholly::compiler::diagnosticMessage(missing_hash).find("Hash") !=
         std::string::npos);
  const chtholly::compiler::Diagnostic missing_equal{
      .kind = chtholly::compiler::DiagnosticKind::MissingEqualWitness};
  CHTHOLLY_TEST_CHECK(chtholly::compiler::diagnosticCode(missing_equal.kind) ==
         "chtholly.next.sem.witness.missing-equal");

  const std::string text =
      "module main;\n"
      "fn main(): i32 { return missing_value; }\n";
  chtholly::compiler::SourceBuffer source(
      chtholly::compiler::SourceInput("diagnostic.cns", text));
  chtholly::compiler::DiagnosticEmitter diagnostics;
  const auto missing = static_cast<std::uint32_t>(text.find("missing_value"));
  diagnostics.emit(chtholly::compiler::DiagnosticKind::UnknownName, missing, 13);
  diagnostics.emit(chtholly::compiler::DiagnosticKind::UnreachableCode, 0, 6);
  chtholly::compiler::Diagnostic explained{
      .kind = chtholly::compiler::DiagnosticKind::BorrowConflict,
      .offset = missing,
      .length = 1};
  explained.notes.push_back({
      .level = chtholly::compiler::DiagnosticLevel::Note,
      .kind = chtholly::compiler::DiagnosticKind::OwnershipBorrowOrigin,
      .code = "chtholly.next.note.ownership.borrow-origin",
      .message = "borrow established here",
      .offset = 0,
      .length = 1});
  diagnostics.emit(std::move(explained));

  const auto formatted = diagnostics.format(source);
  CHTHOLLY_TEST_CHECK(diagnostics.hasError());
  CHTHOLLY_TEST_CHECK(formatted.find("diagnostic.cns:2:25: error: unknown name") !=
         std::string::npos);
  CHTHOLLY_TEST_CHECK(formatted.find("^~~~~~~~~~~~~") != std::string::npos);
  CHTHOLLY_TEST_CHECK(formatted.find("diagnostic.cns:1:1: warning: statement is unreachable") !=
         std::string::npos);
  CHTHOLLY_TEST_CHECK(formatted.find("note: borrow established here") != std::string::npos);
  CHTHOLLY_TEST_CHECK(formatted.find("chtholly.next.note.ownership.borrow-origin") !=
         std::string::npos);

  chtholly::CompilerSourceDiagnostic unavailable{
      .path = "consumer.cns",
      .level = chtholly::compiler::DiagnosticLevel::Error,
      .code = "chtholly.next.sem.borrow.region-conflict",
      .message = "borrow conflicts with a later write",
      .offset = 4,
      .length = 1,
      .location = {2, 3},
      .related = {{
          .level = chtholly::compiler::DiagnosticLevel::Note,
          .code = "chtholly.next.note.ownership.borrow-origin",
          .message = "borrow established in imported provider",
          .path = "provider.cns",
          .offset = 0,
          .length = 1,
          .location = {0, 0},
          .location_available = false,
      }}};
  std::ostringstream human_out;
  std::ostringstream human_error;
  chtholly::CliOutputSink human_sink(chtholly::CliOutputFormat::Human,
                                     human_out, human_error);
  human_sink.diagnostic(unavailable);
  CHTHOLLY_TEST_CHECK(human_error.str().find("provider.cns:?:?: note:") !=
         std::string::npos);
  CHTHOLLY_TEST_CHECK(human_error.str().find("(source unavailable)") !=
         std::string::npos);

  std::ostringstream json_out;
  std::ostringstream json_error;
  chtholly::CliOutputSink json_sink(chtholly::CliOutputFormat::JsonLines,
                                    json_out, json_error);
  json_sink.diagnostic(unavailable);
  CHTHOLLY_TEST_CHECK(json_out.str().find("\"location-available\":false") !=
         std::string::npos);
  CHTHOLLY_TEST_CHECK(json_out.str().find("\"path\":\"provider.cns\"") !=
         std::string::npos);
  return 0;
}
