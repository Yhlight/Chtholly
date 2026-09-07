#pragma once

#include "chtholly/Basic/Diagnostic.h"
#include "chtholly/Driver/WorkspaceArtifactTypes.h"
#include "chtholly/Driver/CompilerInvocation.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {

struct CompilerSourceDiagnostic;

enum class CliOutputFormat {
  Human,
  JsonLinesV1,
  JsonLines,
};

bool filterCliOutputArguments(int argc, char **argv,
                              std::vector<std::string> &arguments,
                              CliOutputFormat &format, std::string &error);

std::string cliFailureCode(const CompilerInvocation *invocation,
                           std::string_view message, bool argument_error);

class CliOutputSink {
public:
  CliOutputSink(CliOutputFormat format, std::ostream &out, std::ostream &error);

  CliOutputFormat format() const { return format_; }
  void output(std::string_view text);
  void diagnostic(std::string_view code, std::string_view message,
                  std::string_view severity = "error");
  void diagnostic(const Diagnostic &diagnostic);
  void diagnostic(const CompilerSourceDiagnostic &diagnostic);
  void invalidation(
      const WorkspaceArtifactResult::InvalidationExplanation &explanation);
  void result(std::string_view action, int exit_code);

private:
  void jsonRecord(
      std::string_view kind,
      const std::vector<std::pair<std::string_view, std::string>> &fields);
  void jsonRecordV2(
      std::string_view kind,
      const std::vector<std::pair<std::string_view, std::string>> &fields,
      const std::vector<std::pair<std::string_view, std::string>> &raw_fields = {});

  CliOutputFormat format_;
  std::ostream &out_;
  std::ostream &error_;
  std::size_t sequence_ = 0;
};

} // namespace chtholly
