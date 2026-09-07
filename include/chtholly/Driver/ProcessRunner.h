#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chtholly {

struct CommandResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
};

struct ProcessRunOptions {
  std::optional<std::string> stdin_text;
  std::uint64_t timeout_milliseconds = 0;
  std::size_t max_stdout_bytes = static_cast<std::size_t>(-1);
  std::size_t max_stderr_bytes = static_cast<std::size_t>(-1);
  std::vector<std::pair<std::string, std::string>> environment_overrides;
};

std::optional<CommandResult>
runProcess(const std::string &program,
           const std::vector<std::string> &arguments, std::string &error);

std::optional<CommandResult>
runProcess(const std::string &program,
           const std::vector<std::string> &arguments,
           const ProcessRunOptions &options, std::string &error);

std::optional<int>
runProcessPassthrough(const std::string &program,
                      const std::vector<std::string> &arguments,
                      std::string &error);

std::string summarizeCommandFailure(const std::string &program,
                                    const CommandResult &result);

} // namespace chtholly
