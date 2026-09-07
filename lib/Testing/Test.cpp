#include "chtholly/Testing/Test.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace chtholly::testing {

Registry &Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(TestCase test) {
  tests_.push_back(std::move(test));
}

Registration::Registration(std::string name, std::string label,
                           std::function<int()> body) {
  Registry::instance().add(
      {std::move(name), std::move(label), std::move(body)});
}

void expect(bool condition, std::string_view expression, std::string_view file,
            int line) {
  if (condition)
    return;
  std::ostringstream message;
  message << file << ':' << line << ": expectation failed: " << expression;
  throw Failure(message.str());
}

namespace {

struct ManifestTest {
  std::string name;
  std::string kind = "process";
  std::vector<std::string> labels;
  std::vector<std::string> capabilities;
  std::vector<std::string> command;
  std::vector<std::string> environment;
  std::string working_directory;
  std::string registry;
  int timeout_seconds = 60;
  int expected_exit_code = 0;
  bool serial = false;
};

struct RunOptions {
  std::string filter;
  std::string label;
  std::string capability;
  std::string format = "text";
  std::string output;
  std::size_t jobs = 1;
  int timeout_override = 0;
  int retries = 0;
  std::string artifact_directory;
  bool progress = false;
};

struct Result {
  std::string name;
  std::string kind;
  std::vector<std::string> labels;
  std::vector<std::string> capabilities;
  std::string failure;
  std::string stdout_text;
  std::string stderr_text;
  int code = 0;
  long long milliseconds = 0;
  bool timed_out = false;
};

std::string trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])))
    ++begin;
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])))
    --end;
  return std::string(value.substr(begin, end - begin));
}

std::string stripComment(std::string_view line) {
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quoted && ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && ch == '#')
      return std::string(line.substr(0, i));
  }
  return std::string(line);
}

bool parseString(std::string_view value, std::string &out) {
  const std::string storage = trim(value);
  value = storage;
  if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    return false;
  out.clear();
  bool escaped = false;
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    const char ch = value[i];
    if (escaped) {
      switch (ch) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      default:
        return false;
      }
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else {
      out.push_back(ch);
    }
  }
  return !escaped;
}

bool parseArray(std::string_view value, std::vector<std::string> &out) {
  const std::string storage = trim(value);
  value = storage;
  if (value.size() < 2 || value.front() != '[' || value.back() != ']')
    return false;
  out.clear();
  std::string item;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    const char ch = value[i];
    if (escaped) {
      item.push_back(ch);
      escaped = false;
      continue;
    }
    if (quoted && ch == '\\') {
      item.push_back(ch);
      escaped = true;
      continue;
    }
    if (ch == '"') {
      quoted = !quoted;
      item.push_back(ch);
      continue;
    }
    if (!quoted && ch == ',') {
      std::string parsed;
      if (!parseString(item, parsed))
        return false;
      out.push_back(std::move(parsed));
      item.clear();
      continue;
    }
    item.push_back(ch);
  }
  item = trim(item);
  if (!item.empty()) {
    std::string parsed;
    if (!parseString(item, parsed))
      return false;
    out.push_back(std::move(parsed));
  }
  return !quoted && !escaped;
}

bool parseBool(std::string_view value, bool &out) {
  const std::string storage = trim(value);
  if (storage == "true") {
    out = true;
    return true;
  }
  if (storage == "false") {
    out = false;
    return true;
  }
  return false;
}

bool parseInt(std::string_view value, int &out) {
  const std::string storage = trim(value);
  if (storage.empty())
    return false;
  char *end = nullptr;
  const long parsed = std::strtol(storage.c_str(), &end, 10);
  if (end == nullptr || *end != '\0')
    return false;
  out = static_cast<int>(parsed);
  return true;
}

bool assignField(ManifestTest &test, std::string_view key,
                 std::string_view value, std::string &error) {
  if (key == "name" || key == "kind" || key == "working_directory" ||
      key == "registry") {
    std::string parsed;
    if (!parseString(value, parsed)) {
      error = "expected string for " + std::string(key);
      return false;
    }
    if (key == "name")
      test.name = std::move(parsed);
    else if (key == "kind")
      test.kind = std::move(parsed);
    else if (key == "working_directory")
      test.working_directory = std::move(parsed);
    else
      test.registry = std::move(parsed);
    return true;
  }
  if (key == "labels" || key == "capabilities" || key == "command" ||
      key == "environment") {
    std::vector<std::string> parsed;
    if (!parseArray(value, parsed)) {
      error = "expected string array for " + std::string(key);
      return false;
    }
    if (key == "labels")
      test.labels = std::move(parsed);
    else if (key == "capabilities")
      test.capabilities = std::move(parsed);
    else if (key == "command")
      test.command = std::move(parsed);
    else
      test.environment = std::move(parsed);
    return true;
  }
  if (key == "timeout_seconds" || key == "expected_exit_code") {
    int parsed = 0;
    if (!parseInt(value, parsed)) {
      error = "expected integer for " + std::string(key);
      return false;
    }
    if (key == "timeout_seconds")
      test.timeout_seconds = parsed;
    else
      test.expected_exit_code = parsed;
    return true;
  }
  if (key == "serial") {
    if (!parseBool(value, test.serial)) {
      error = "expected boolean for serial";
      return false;
    }
    return true;
  }
  error = "unknown test field: " + std::string(key);
  return false;
}

std::optional<std::vector<ManifestTest>> parseManifest(const std::string &path,
                                                       std::string &error) {
  std::ifstream input(path);
  if (!input) {
    error = "cannot open test manifest: " + path;
    return std::nullopt;
  }
  std::vector<ManifestTest> tests;
  std::optional<ManifestTest> current;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::string clean = trim(stripComment(line));
    if (clean.empty() || clean.rfind("format", 0) == 0 ||
        clean.rfind("suite", 0) == 0)
      continue;
    if (clean == "[[test]]") {
      if (current)
        tests.push_back(std::move(*current));
      current.emplace();
      continue;
    }
    if (!current) {
      error = "manifest field outside [[test]] at line " +
              std::to_string(line_number);
      return std::nullopt;
    }
    const std::size_t equals = clean.find('=');
    if (equals == std::string::npos) {
      error = "expected key=value at line " + std::to_string(line_number);
      return std::nullopt;
    }
    const std::string key = trim(clean.substr(0, equals));
    if (!assignField(*current, key, trim(clean.substr(equals + 1)), error)) {
      error += " at line " + std::to_string(line_number);
      return std::nullopt;
    }
  }
  if (current)
    tests.push_back(std::move(*current));
  std::ranges::sort(tests, {}, &ManifestTest::name);
  for (const auto &test : tests) {
    if (test.name.empty()) {
      error = "test has no name";
      return std::nullopt;
    }
    if (test.kind != "process" && test.kind != "inprocess") {
      error = "unknown test kind for " + test.name + ": " + test.kind;
      return std::nullopt;
    }
    if (test.kind == "process" && test.command.empty()) {
      error = "process test has no command: " + test.name;
      return std::nullopt;
    }
    if (test.kind == "inprocess" && test.registry.empty()) {
      error = "inprocess test has no registry name: " + test.name;
      return std::nullopt;
    }
    if (test.timeout_seconds <= 0) {
      error = "test timeout must be positive: " + test.name;
      return std::nullopt;
    }
  }
  for (std::size_t i = 1; i < tests.size(); ++i) {
    if (tests[i - 1].name == tests[i].name) {
      error = "duplicate test name: " + tests[i].name;
      return std::nullopt;
    }
  }
  return tests;
}

std::string jsonEscape(std::string_view value) {
  std::string result;
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result.push_back(ch);
      break;
    }
  }
  return result;
}

std::string xmlEscape(std::string_view value) {
  std::string result;
  for (const char ch : value) {
    if (ch == '&')
      result += "&amp;";
    else if (ch == '<')
      result += "&lt;";
    else if (ch == '>')
      result += "&gt;";
    else if (ch == '"')
      result += "&quot;";
    else
      result.push_back(ch);
  }
  return result;
}

bool hasLabel(const ManifestTest &test, std::string_view label) {
  return std::ranges::find(test.labels, label) != test.labels.end();
}

bool hasCapability(const ManifestTest &test, std::string_view capability) {
  return std::ranges::find(test.capabilities, capability) !=
         test.capabilities.end();
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::filesystem::path temporaryPath(std::string_view stem) {
  static std::atomic<unsigned long long> sequence = 0;
  const auto id = ++sequence;
  const auto process_id =
#if defined(_WIN32)
      static_cast<unsigned long long>(GetCurrentProcessId());
#else
      static_cast<unsigned long long>(getpid());
#endif
  return std::filesystem::temp_directory_path() /
         ("chtholly-test-" + std::string(stem) + "-" +
          std::to_string(process_id) + "-" + std::to_string(id) + ".log");
}

std::string artifactName(std::string_view test_name, std::string_view stream) {
  std::string result;
  result.reserve(test_name.size() + stream.size() + 1);
  for (const char ch : test_name)
    result.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
  result.push_back('.');
  result.append(stream);
  return result;
}

struct ProcessResult {
  int code = -1;
  bool timed_out = false;
  std::string stdout_text;
  std::string stderr_text;
  std::string failure;
};

#if defined(_WIN32)
std::wstring wide(std::string_view value) {
  if (value.empty())
    return {};
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size);
  return result;
}

std::wstring quoteWindows(std::string_view value) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t ch : wide(value)) {
    if (ch == L'\\') {
      ++slashes;
      continue;
    }
    if (ch == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(ch);
    } else {
      result.append(slashes, L'\\');
      result.push_back(ch);
    }
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  result += L"\"";
  return result;
}

std::vector<wchar_t>
windowsEnvironment(const std::vector<std::string> &overrides) {
  std::map<std::wstring, std::wstring> values;
  LPWCH inherited = GetEnvironmentStringsW();
  if (inherited != nullptr) {
    for (const wchar_t *entry = inherited; *entry != L'\0';
         entry += std::wcslen(entry) + 1) {
      const std::wstring item(entry);
      const std::size_t equals = item.find(L'=');
      if (equals != std::wstring::npos && equals != 0)
        values[item.substr(0, equals)] = item.substr(equals + 1);
    }
    FreeEnvironmentStringsW(inherited);
  }
  for (const auto &override : overrides) {
    const std::wstring item = wide(override);
    const std::size_t equals = item.find(L'=');
    if (equals != std::wstring::npos && equals != 0)
      values[item.substr(0, equals)] = item.substr(equals + 1);
  }
  std::vector<wchar_t> block;
  for (const auto &[key, value] : values) {
    block.insert(block.end(), key.begin(), key.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

ProcessResult runProcess(const ManifestTest &test, int timeout_seconds,
                         std::string_view artifact_directory) {
  ProcessResult result;
  const auto stdout_path = temporaryPath("stdout");
  const auto stderr_path = temporaryPath("stderr");
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE stdout_file =
      CreateFileW(stdout_path.wstring().c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  HANDLE stderr_file =
      CreateFileW(stderr_path.wstring().c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (stdout_file == INVALID_HANDLE_VALUE ||
      stderr_file == INVALID_HANDLE_VALUE) {
    result.failure = "cannot create subprocess output files";
    if (stdout_file != INVALID_HANDLE_VALUE)
      CloseHandle(stdout_file);
    if (stderr_file != INVALID_HANDLE_VALUE)
      CloseHandle(stderr_file);
    return result;
  }
  std::wstring command_line;
  for (const auto &argument : test.command) {
    if (!command_line.empty())
      command_line.push_back(L' ');
    command_line += quoteWindows(argument);
  }
  std::vector<wchar_t> mutable_command(command_line.begin(),
                                       command_line.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = stdout_file;
  startup.hStdError = stderr_file;
  PROCESS_INFORMATION process{};
  const std::wstring cwd = wide(test.working_directory);
  const std::vector<wchar_t> environment = windowsEnvironment(test.environment);
  const DWORD creation_flags =
      CREATE_NO_WINDOW |
      (test.environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
  const BOOL created = CreateProcessW(
      nullptr, mutable_command.data(), nullptr, nullptr, TRUE, creation_flags,
      test.environment.empty() ? nullptr
                               : const_cast<wchar_t *>(environment.data()),
      cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
  CloseHandle(stdout_file);
  CloseHandle(stderr_file);
  if (!created) {
    result.failure = "CreateProcess failed: " + std::to_string(GetLastError());
  } else {
    // Keep the complete subprocess tree inside a job. CFFI and install tests
    // legitimately launch compiler/linker helpers; terminating only the
    // direct process on timeout leaves those helpers alive and can stall the
    // next test or hold build artifacts open.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits)) ||
          !AssignProcessToJobObject(job, process.hProcess)) {
        CloseHandle(job);
        job = nullptr;
      }
    }
    const DWORD wait = WaitForSingleObject(
        process.hProcess, static_cast<DWORD>(timeout_seconds * 1000));
    if (wait == WAIT_TIMEOUT) {
      result.timed_out = true;
      if (job != nullptr)
        TerminateJobObject(job, 124);
      else
        TerminateProcess(process.hProcess, 124);
      WaitForSingleObject(process.hProcess, INFINITE);
      result.failure = "process timed out";
    } else if (wait != WAIT_OBJECT_0) {
      result.failure =
          "WaitForSingleObject failed: " + std::to_string(GetLastError());
    }
    if (job != nullptr)
      CloseHandle(job);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.code = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  result.stdout_text = readFile(stdout_path);
  result.stderr_text = readFile(stderr_path);
  if (!artifact_directory.empty()) {
    std::error_code ec;
    const auto directory = std::filesystem::path(artifact_directory);
    std::filesystem::create_directories(directory, ec);
    if (!ec) {
      std::ofstream(directory / artifactName(test.name, "stdout"))
          << result.stdout_text;
      std::ofstream(directory / artifactName(test.name, "stderr"))
          << result.stderr_text;
    }
  }
  std::error_code ignored;
  std::filesystem::remove(stdout_path, ignored);
  std::filesystem::remove(stderr_path, ignored);
  return result;
}
#else
ProcessResult runProcess(const ManifestTest &test, int timeout_seconds,
                         std::string_view artifact_directory) {
  ProcessResult result;
  const auto stdout_path = temporaryPath("stdout");
  const auto stderr_path = temporaryPath("stderr");
  const pid_t child = fork();
  if (child == -1) {
    result.failure = "fork failed";
    return result;
  }
  if (child == 0) {
    const int stdout_fd =
        open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int stderr_fd =
        open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (stdout_fd < 0 || stderr_fd < 0)
      _exit(127);
    dup2(stdout_fd, STDOUT_FILENO);
    dup2(stderr_fd, STDERR_FILENO);
    close(stdout_fd);
    close(stderr_fd);
    if (!test.working_directory.empty())
      chdir(test.working_directory.c_str());
    for (const auto &entry : test.environment) {
      const std::size_t equals = entry.find('=');
      if (equals != std::string::npos && equals != 0)
        setenv(entry.substr(0, equals).c_str(),
               entry.substr(equals + 1).c_str(), 1);
    }
    std::vector<char *> argv;
    argv.reserve(test.command.size() + 1);
    for (const auto &argument : test.command)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  int status = 0;
  while (true) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      break;
    if (waited == -1) {
      result.failure = "waitpid failed";
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      result.failure = "process timed out";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (WIFEXITED(status))
    result.code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    result.code = 128 + WTERMSIG(status);
  result.stdout_text = readFile(stdout_path);
  result.stderr_text = readFile(stderr_path);
  if (!artifact_directory.empty()) {
    std::error_code ec;
    const auto directory = std::filesystem::path(artifact_directory);
    std::filesystem::create_directories(directory, ec);
    if (!ec) {
      std::ofstream(directory / artifactName(test.name, "stdout"))
          << result.stdout_text;
      std::ofstream(directory / artifactName(test.name, "stderr"))
          << result.stderr_text;
    }
  }
  std::error_code ignored;
  std::filesystem::remove(stdout_path, ignored);
  std::filesystem::remove(stderr_path, ignored);
  return result;
}
#endif

Result runOne(const ManifestTest &test, const RunOptions &options) {
  const auto begin = std::chrono::steady_clock::now();
  Result result{.name = test.name,
                .kind = test.kind,
                .labels = test.labels,
                .capabilities = test.capabilities};
  const int timeout = options.timeout_override > 0 ? options.timeout_override
                                                   : test.timeout_seconds;
  if (test.kind == "inprocess") {
    const auto found = std::ranges::find_if(
        Registry::instance().tests(),
        [&](const auto &candidate) { return candidate.name == test.registry; });
    if (found == Registry::instance().tests().end()) {
      result.code = -1;
      result.failure = "registered test not found: " + test.registry;
    } else {
      try {
        result.code = found->body();
      } catch (const Failure &exception) {
        result.code = -1;
        result.failure = exception.what();
      } catch (const std::exception &exception) {
        result.code = -1;
        result.failure = exception.what();
      } catch (...) {
        result.code = -1;
        result.failure = "unknown exception";
      }
      if (result.code != 0 && result.failure.empty())
        result.failure = "test returned non-zero status";
    }
  } else {
    const ProcessResult process =
        runProcess(test, timeout, options.artifact_directory);
    result.code = process.code;
    result.timed_out = process.timed_out;
    result.stdout_text = process.stdout_text;
    result.stderr_text = process.stderr_text;
    if (!process.failure.empty())
      result.failure = process.failure;
    if (result.code != test.expected_exit_code && result.failure.empty()) {
      result.failure = "expected exit code " +
                       std::to_string(test.expected_exit_code) + ", got " +
                       std::to_string(result.code);
    }
  }
  result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
  return result;
}

Result runWithRetries(const ManifestTest &test, const RunOptions &options) {
  Result result;
  for (int attempt = 0; attempt <= options.retries; ++attempt) {
    result = runOne(test, options);
    if (result.code == test.expected_exit_code && !result.timed_out &&
        result.failure.empty())
      break;
  }
  return result;
}

bool selected(const ManifestTest &test, const RunOptions &options) {
  return (options.filter.empty() ||
          test.name.find(options.filter) != std::string::npos) &&
         (options.label.empty() || hasLabel(test, options.label)) &&
         (options.capability.empty() ||
          hasCapability(test, options.capability));
}

void printResult(const Result &result) {
  std::cout << (result.code == 0 && !result.timed_out ? "PASS " : "FAIL ")
            << result.name << " (" << result.milliseconds << " ms)";
  if (!result.failure.empty())
    std::cout << ": " << result.failure;
  std::cout << '\n';
  if (result.code != 0 && !result.stdout_text.empty())
    std::cout << result.stdout_text;
  if (result.code != 0 && !result.stderr_text.empty())
    std::cerr << result.stderr_text;
}

int emitResults(const std::vector<Result> &results, const RunOptions &options) {
  std::ostream *stream = &std::cout;
  std::ofstream file;
  if (!options.output.empty()) {
    const auto parent = std::filesystem::path(options.output).parent_path();
    if (!parent.empty()) {
      std::error_code error;
      std::filesystem::create_directories(parent, error);
      if (error) {
        std::cerr << "cannot create report directory: " << parent.string()
                  << '\n';
        return 2;
      }
    }
    file.open(options.output);
    if (!file) {
      std::cerr << "cannot open report output: " << options.output << '\n';
      return 2;
    }
    stream = &file;
  }
  if (options.format == "text") {
    if (stream != &std::cout) {
      for (const auto &result : results)
        *stream << (result.code == 0 ? "PASS " : "FAIL ") << result.name
                << '\n';
    }
  } else if (options.format == "json") {
    *stream << "{\"tests\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
      if (i)
        *stream << ',';
      const auto &result = results[i];
      *stream << "{\"name\":\"" << jsonEscape(result.name) << "\",\"kind\":\""
              << jsonEscape(result.kind) << "\",\"labels\":[";
      for (std::size_t label_index = 0; label_index < result.labels.size();
           ++label_index) {
        if (label_index)
          *stream << ',';
        *stream << '\"' << jsonEscape(result.labels[label_index]) << '\"';
      }
      *stream << "],\"capabilities\":[";
      for (std::size_t capability_index = 0;
           capability_index < result.capabilities.size(); ++capability_index) {
        if (capability_index)
          *stream << ',';
        *stream << '\"' << jsonEscape(result.capabilities[capability_index])
                << '\"';
      }
      *stream << "],\"failure\":\"" << jsonEscape(result.failure)
              << "\",\"stdout\":\"" << jsonEscape(result.stdout_text)
              << "\",\"stderr\":\"" << jsonEscape(result.stderr_text)
              << "\",\"code\":" << result.code
              << ",\"timed_out\":" << (result.timed_out ? "true" : "false")
              << ",\"milliseconds\":" << result.milliseconds << '}';
    }
    *stream << "]}\n";
  } else if (options.format == "junit") {
    *stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<testsuite tests=\"" << results.size() << "\">\n";
    for (const auto &result : results) {
      *stream << "  <testcase name=\"" << xmlEscape(result.name) << "\" time=\""
              << result.milliseconds / 1000.0 << "\">";
      if (result.code != 0 || result.timed_out) {
        *stream << "<failure message=\"" << xmlEscape(result.failure) << "\">"
                << xmlEscape(result.stdout_text + result.stderr_text)
                << "</failure>";
      }
      *stream << "</testcase>\n";
    }
    *stream << "</testsuite>\n";
  } else {
    std::cerr << "unknown report format: " << options.format << '\n';
    return 2;
  }
  return 0;
}

int runManifest(const std::string &manifest_path, const RunOptions &options) {
  std::string error;
  const auto parsed = parseManifest(manifest_path, error);
  if (!parsed) {
    std::cerr << error << '\n';
    return 2;
  }
  std::vector<ManifestTest> tests;
  for (const auto &test : *parsed)
    if (selected(test, options))
      tests.push_back(test);
  std::vector<Result> results(tests.size());
  std::vector<std::future<Result>> futures;
  std::vector<std::size_t> future_indices;
  auto drain = [&] {
    for (std::size_t i = 0; i < futures.size(); ++i) {
      const std::size_t result_index = future_indices[i];
      results[result_index] = futures[i].get();
      if (options.progress) {
        const auto &result = results[result_index];
        std::cerr << "DONE " << result.name << " code=" << result.code;
        if (result.timed_out)
          std::cerr << " timed_out=true";
        if (!result.failure.empty())
          std::cerr << " failure=" << result.failure;
        std::cerr << '\n';
      }
    }
    futures.clear();
    future_indices.clear();
  };
  for (std::size_t i = 0; i < tests.size(); ++i) {
    if (tests[i].serial || options.jobs <= 1) {
      drain();
      if (options.progress)
        std::cerr << "RUN " << tests[i].name << '\n';
      results[i] = runWithRetries(tests[i], options);
      if (options.progress) {
        const auto &result = results[i];
        std::cerr << "DONE " << result.name << " code=" << result.code;
        if (result.timed_out)
          std::cerr << " timed_out=true";
        if (!result.failure.empty())
          std::cerr << " failure=" << result.failure;
        std::cerr << '\n';
      }
    } else {
      if (options.progress)
        std::cerr << "RUN " << tests[i].name << '\n';
      futures.push_back(
          std::async(std::launch::async, runWithRetries, tests[i], options));
      future_indices.push_back(i);
      if (futures.size() >= options.jobs)
        drain();
    }
  }
  drain();
  if (options.format == "text")
    for (const auto &result : results)
      printResult(result);
  const int report_code = emitResults(results, options);
  if (report_code != 0)
    return report_code;
  return std::ranges::any_of(results,
                             [](const auto &result) {
                               return result.code != 0 || result.timed_out;
                             })
             ? 1
             : 0;
}

int runRegistered(const RunOptions &options) {
  std::vector<Result> results;
  auto tests = Registry::instance().tests();
  std::ranges::sort(tests, {}, &TestCase::name);
  for (const auto &test : tests) {
    if (!options.filter.empty() &&
        test.name.find(options.filter) == std::string::npos)
      continue;
    if (!options.label.empty() && test.label != options.label)
      continue;
    ManifestTest manifest{.name = test.name,
                          .kind = "inprocess",
                          .labels = {test.label},
                          .registry = test.name};
    results.push_back(runWithRetries(manifest, options));
  }
  if (options.format == "text")
    for (const auto &result : results)
      printResult(result);
  const int report_code = emitResults(results, options);
  if (report_code != 0)
    return report_code;
  return std::ranges::any_of(
             results, [](const auto &result) { return result.code != 0; })
             ? 1
             : 0;
}

bool readValueOption(int &index, int argc, char **argv, std::string_view option,
                     std::string &value) {
  if (std::string_view(argv[index]) != option)
    return false;
  if (index + 1 >= argc) {
    std::cerr << "missing value for " << option << '\n';
    return false;
  }
  value = argv[++index];
  return true;
}

int parseAndRun(int argc, char **argv) {
  const std::string_view command = argc > 1 ? argv[1] : "";
  if (command == "validate") {
    std::string manifest;
    for (int i = 2; i < argc; ++i) {
      if (!readValueOption(i, argc, argv, "--manifest", manifest)) {
        std::cerr << "unknown option: " << argv[i] << '\n';
        return 2;
      }
    }
    if (manifest.empty()) {
      std::cerr << "--manifest is required\n";
      return 2;
    }
    std::string error;
    const auto tests = parseManifest(manifest, error);
    if (!tests) {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << "manifest-valid tests=" << tests->size() << '\n';
    return 0;
  }
  if (command == "list" || command == "describe") {
    const int option_start = command == "describe" ? 3 : 2;
    if (command == "describe" && argc < 3) {
      std::cerr << "usage: chtholly-test describe <name> --manifest <file>\n";
      return 2;
    }
    std::string manifest;
    for (int i = option_start; i < argc; ++i) {
      if (!readValueOption(i, argc, argv, "--manifest", manifest)) {
        std::cerr << "unknown option: " << argv[i] << '\n';
        return 2;
      }
    }
    if (manifest.empty()) {
      std::cerr << "--manifest is required\n";
      return 2;
    }
    std::string error;
    const auto tests = parseManifest(manifest, error);
    if (!tests) {
      std::cerr << error << '\n';
      return 2;
    }
    if (command == "list") {
      for (const auto &test : *tests)
        std::cout << test.name << " [" << test.kind << "]\n";
      return 0;
    }
    const std::string requested = argv[2];
    const auto found = std::ranges::find_if(
        *tests, [&](const auto &test) { return test.name == requested; });
    if (found == tests->end()) {
      std::cerr << "test not found: " << requested << '\n';
      return 1;
    }
    std::cout << found->name << "\nkind=" << found->kind
              << "\ntimeout_seconds=" << found->timeout_seconds
              << "\nserial=" << (found->serial ? "true" : "false")
              << "\ncapabilities=";
    for (std::size_t index = 0; index < found->capabilities.size(); ++index) {
      if (index)
        std::cout << ',';
      std::cout << found->capabilities[index];
    }
    std::cout << '\n';
    return 0;
  }

  const bool run_command = command == "run";
  RunOptions options;
  std::string manifest;
  for (int i = run_command ? 2 : 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    std::string value;
    if (readValueOption(i, argc, argv, "--manifest", manifest))
      continue;
    if (readValueOption(i, argc, argv, "--filter", options.filter))
      continue;
    if (readValueOption(i, argc, argv, "--label", options.label))
      continue;
    if (readValueOption(i, argc, argv, "--capability", options.capability))
      continue;
    if (readValueOption(i, argc, argv, "--format", options.format))
      continue;
    if (readValueOption(i, argc, argv, "--output", options.output))
      continue;
    if (readValueOption(i, argc, argv, "--jobs", value)) {
      options.jobs = std::max<std::size_t>(1, std::stoul(value));
      continue;
    }
    if (readValueOption(i, argc, argv, "--timeout", value)) {
      options.timeout_override = std::stoi(value);
      continue;
    }
    if (readValueOption(i, argc, argv, "--retry", value)) {
      options.retries = std::stoi(value);
      if (options.retries < 0)
        options.retries = 0;
      continue;
    }
    if (readValueOption(i, argc, argv, "--artifact-dir",
                        options.artifact_directory))
      continue;
    if (option == "--progress") {
      options.progress = true;
      continue;
    }
    std::cerr << "unknown option: " << option << '\n';
    return 2;
  }
  return manifest.empty() ? runRegistered(options)
                          : runManifest(manifest, options);
}

} // namespace

int run(int argc, char **argv) {
  return parseAndRun(argc, argv);
}

} // namespace chtholly::testing
