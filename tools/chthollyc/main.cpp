#include "chtholly/Driver/CliOutput.h"
#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/Doctor.h"
#include "chtholly/Driver/CompilerPipeline.h"
#include "chtholly/Driver/ProjectScaffold.h"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cwchar>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int runCompilerMain(int argc, char **argv) {
  chtholly::CliOutputFormat output_format;
  std::vector<std::string> filtered_arguments;
  std::string message;
  if (!chtholly::filterCliOutputArguments(argc, argv, filtered_arguments,
                                          output_format, message)) {
    chtholly::CliOutputSink output(output_format, std::cout, std::cerr);
    output.diagnostic("chtholly.cli.invalid-arguments", message);
    output.result("parse", 2);
    return 2;
  }

  std::vector<char *> filtered_argv;
  filtered_argv.reserve(filtered_arguments.size());
  for (auto &argument : filtered_arguments)
    filtered_argv.push_back(argument.data());

  chtholly::CliOutputSink output(output_format, std::cout, std::cerr);
  chtholly::CompilerInvocation invocation;
  if (!chtholly::parseCompilerInvocation(static_cast<int>(filtered_argv.size()),
                                         filtered_argv.data(), invocation,
                                         message)) {
    output.diagnostic(chtholly::cliFailureCode(nullptr, message, true),
                      message);
    output.result("parse", 2);
    return 2;
  }

  const auto action_name =
      invocation.workflow == chtholly::DriverWorkflow::New
          ? std::string_view("new")
      : invocation.workflow == chtholly::DriverWorkflow::Init
          ? std::string_view("init")
          : chtholly::driverActionName(invocation.action);
  if (output_format != chtholly::CliOutputFormat::Human &&
      invocation.workflow == chtholly::DriverWorkflow::Run) {
    message = "run is unavailable with --output-format jsonl because program "
              "stdio is passed through";
    output.diagnostic("chtholly.cli.incompatible-output-format", message);
    output.result(action_name, 2);
    return 2;
  }

  std::ostringstream captured_output;
  auto *original_output = std::cout.rdbuf(captured_output.rdbuf());
  int exit_code = 0;
  std::vector<chtholly::WorkspaceArtifactResult::InvalidationExplanation>
      invalidation_explanations;
  std::vector<chtholly::CompilerSourceDiagnostic> source_diagnostics;
  if (invocation.action == chtholly::DriverAction::Help) {
    const std::string program =
        filtered_arguments.empty() ? "chthollyc" : filtered_arguments.front();
    std::cout << chtholly::compilerUsage(program);
  } else if (invocation.action == chtholly::DriverAction::Version) {
    std::cout << chtholly::compilerVersion();
  } else if (invocation.action == chtholly::DriverAction::Scaffold) {
    std::string created_root;
    if (!chtholly::createProjectScaffold(
            {.root_path = invocation.scaffold_path,
             .package_name = invocation.scaffold_name,
             .library = invocation.scaffold_library},
            created_root, message)) {
      exit_code = 1;
    } else {
      std::cout << "created\t" << invocation.scaffold_name << "\tproject\t"
                << created_root << '\n';
    }
  } else if (invocation.action == chtholly::DriverAction::Doctor) {
    exit_code = chtholly::runCompilerDoctor(invocation, std::cout, message);
  } else {
    exit_code = chtholly::runCompilerPipeline(invocation, message,
                                                  &invalidation_explanations,
                                                  &source_diagnostics);
  }
  std::cout.rdbuf(original_output);

  output.output(captured_output.str());
  for (const auto &diagnostic : source_diagnostics)
    output.diagnostic(diagnostic);
  if (invocation.explain_invalidation)
    for (const auto &explanation : invalidation_explanations)
      output.invalidation(explanation);
  if (exit_code != 0 && !message.empty() && source_diagnostics.empty())
    output.diagnostic(chtholly::cliFailureCode(&invocation, message, false),
                      message);
  output.result(action_name, exit_code);
  return exit_code;
}

#ifdef _WIN32
std::string utf8Argument(const wchar_t *argument) {
  if (argument == nullptr)
    return {};
  const int length = static_cast<int>(wcslen(argument));
  if (length == 0)
    return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument,
                                       length, nullptr, 0, nullptr, nullptr);
  if (size <= 0)
    return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, length,
                          result.data(), size, nullptr, nullptr) != size)
    return {};
  return result;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> utf8_arguments;
  utf8_arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    auto argument = utf8Argument(argv[index]);
    if (argv[index] != nullptr && !argument.empty())
      utf8_arguments.push_back(std::move(argument));
    else if (argv[index] != nullptr && argv[index][0] == L'\0')
      utf8_arguments.emplace_back();
    else
      return 2;
  }
  std::vector<char *> narrow_argv;
  narrow_argv.reserve(utf8_arguments.size());
  for (auto &argument : utf8_arguments)
    narrow_argv.push_back(argument.data());
  return runCompilerMain(static_cast<int>(narrow_argv.size()),
                         narrow_argv.data());
}
#else
int main(int argc, char **argv) { return runCompilerMain(argc, argv); }
#endif
