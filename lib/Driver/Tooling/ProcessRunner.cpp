#include "chtholly/Driver/ProcessRunner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace chtholly {

namespace {

std::string trimForSummary(std::string text) {
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                           text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  constexpr std::size_t summary_limit = 2048;
  if (text.size() > summary_limit) {
    constexpr std::string_view separator = "\n... output truncated ...\n";
    const auto prefix_size = (summary_limit - separator.size()) / 2;
    const auto suffix_size = summary_limit - separator.size() - prefix_size;
    text = text.substr(0, prefix_size) + std::string(separator) +
           text.substr(text.size() - suffix_size);
  }
  return text;
}

#ifdef _WIN32

std::wstring widen(const std::string &text) {
  if (text.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) {
    return std::wstring(text.begin(), text.end());
  }
  std::wstring out(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), count);
  return out;
}

std::wstring quoteWindowsArg(const std::string &arg) {
  const auto wide = widen(arg);
  if (wide.empty()) {
    return L"\"\"";
  }
  const bool needs_quotes =
      wide.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) {
    return wide;
  }
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t ch : wide) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(ch);
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(ch);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'\"');
  return out;
}

std::wstring
buildWindowsCommandLine(const std::string &program,
                        const std::vector<std::string> &arguments) {
  std::wstring command = quoteWindowsArg(program);
  for (const auto &argument : arguments) {
    command.push_back(L' ');
    command += quoteWindowsArg(argument);
  }
  return command;
}

struct WideCaseInsensitiveLess {
  bool operator()(std::wstring_view lhs, std::wstring_view rhs) const {
    return _wcsicmp(std::wstring(lhs).c_str(), std::wstring(rhs).c_str()) < 0;
  }
};

std::vector<wchar_t> buildWindowsEnvironment(const ProcessRunOptions &options,
                                             std::string &error) {
  if (options.environment_overrides.empty())
    return {};
  std::map<std::wstring, std::wstring, WideCaseInsensitiveLess> values;
  auto *environment = GetEnvironmentStringsW();
  if (!environment) {
    error = "failed to read the process environment";
    return {};
  }
  for (auto *cursor = environment; *cursor != L'\0';) {
    std::wstring entry(cursor);
    cursor += entry.size() + 1;
    const auto split = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
    if (split != std::wstring::npos)
      values[entry.substr(0, split)] = entry.substr(split + 1);
  }
  FreeEnvironmentStringsW(environment);
  for (const auto &[name, value] : options.environment_overrides) {
    if (name.empty() || name.find('=') != std::string::npos ||
        name.find('\0') != std::string::npos ||
        value.find('\0') != std::string::npos) {
      error = "process environment override is invalid";
      return {};
    }
    values[widen(name)] = widen(value);
  }
  std::vector<wchar_t> block;
  for (const auto &[name, value] : values) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

bool readPipe(HANDLE pipe, std::string &out, std::size_t limit,
              std::atomic<bool> &cancel, std::string &error) {
  std::array<char, 4096> buffer{};
  DWORD read = 0;
  while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                  nullptr) != 0) {
    if (read > 0) {
      if (read > limit - (std::min)(limit, out.size())) {
        error = "process output exceeded its configured limit";
        cancel.store(true);
        return false;
      }
      out.append(buffer.data(), read);
    }
  }
  const auto code = GetLastError();
  if (code != ERROR_BROKEN_PIPE) {
    error = "failed to read process pipe: " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message();
    return false;
  }
  return true;
}

bool writePipe(HANDLE pipe, std::string_view input, std::string &error) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    const auto remaining = input.size() - offset;
    const auto amount = static_cast<DWORD>((std::min)(
        remaining,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written = 0;
    if (WriteFile(pipe, input.data() + offset, amount, &written, nullptr) ==
        0) {
      const auto code = GetLastError();
      if (code == ERROR_BROKEN_PIPE)
        return true;
      error = "failed to write process standard input: " +
              std::error_code(static_cast<int>(code), std::system_category())
                  .message();
      return false;
    }
    offset += written;
  }
  return true;
}

struct HandleGuard {
  HANDLE handle = nullptr;
  ~HandleGuard() {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

struct AttributeListGuard {
  LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
  bool initialized = false;
  ~AttributeListGuard() {
    if (list != nullptr) {
      if (initialized) {
        DeleteProcThreadAttributeList(list);
      }
      HeapFree(GetProcessHeap(), 0, list);
    }
  }
};

bool duplicateStandardInput(HandleGuard &input, std::string &error) {
  const HANDLE source = GetStdHandle(STD_INPUT_HANDLE);
  if (source != nullptr && source != INVALID_HANDLE_VALUE) {
    if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                        &input.handle, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0) {
      return true;
    }
    if (GetLastError() != ERROR_INVALID_HANDLE) {
      const auto code = GetLastError();
      error = "failed to prepare process standard input: " +
              std::error_code(static_cast<int>(code), std::system_category())
                  .message();
      return false;
    }
  }
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  input.handle =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input.handle != INVALID_HANDLE_VALUE) {
    return true;
  }
  const auto code = GetLastError();
  error =
      "failed to prepare process standard input: " +
      std::error_code(static_cast<int>(code), std::system_category()).message();
  return false;
}

#else

void closeFd(int &fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

bool relocateFd(int &fd, std::string &error) {
  if (fd > STDERR_FILENO) {
    return true;
  }
  int relocated;
  do {
    relocated = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
  } while (relocated == -1 && errno == EINTR);
  if (relocated == -1) {
    error =
        std::string("failed to relocate process pipe: ") + std::strerror(errno);
    return false;
  }
  close(fd);
  fd = relocated;
  return true;
}

bool waitForChild(pid_t pid, int &status, std::string *error = nullptr) {
  pid_t result;
  do {
    result = waitpid(pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    if (error != nullptr) {
      *error =
          std::string("failed to wait for process: ") + std::strerror(errno);
    }
    return false;
  }
  return true;
}

bool createPipe(int (&pipe_fds)[2], std::string &error) {
  if (pipe(pipe_fds) != 0) {
    error =
        std::string("failed to create process pipe: ") + std::strerror(errno);
    return false;
  }
  if (!relocateFd(pipe_fds[0], error) || !relocateFd(pipe_fds[1], error)) {
    closeFd(pipe_fds[0]);
    closeFd(pipe_fds[1]);
    return false;
  }
  for (const int descriptor : pipe_fds) {
    const int flags = fcntl(descriptor, F_GETFD, 0);
    if (flags == -1 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == -1) {
      error = std::string("failed to isolate process pipe: ") +
              std::strerror(errno);
      closeFd(pipe_fds[0]);
      closeFd(pipe_fds[1]);
      return false;
    }
  }
  return true;
}

ssize_t writePipeWithoutSignal(int descriptor, const char *data,
                               std::size_t size) {
  sigset_t blocked{};
  sigset_t pending{};
  sigset_t previous{};
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  const auto mask_status = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  if (mask_status != 0) {
    errno = mask_status;
    return -1;
  }
  if (sigpending(&pending) != 0) {
    const auto saved_errno = errno;
    (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = saved_errno;
    return -1;
  }
  const bool had_pending_sigpipe = sigismember(&pending, SIGPIPE) == 1;
  const auto count = write(descriptor, data, size);
  const auto saved_errno = errno;
  if (count < 0 && saved_errno == EPIPE && !had_pending_sigpipe) {
#if defined(__APPLE__)
    int received_signal = 0;
    (void)sigwait(&blocked, &received_signal);
#else
    timespec timeout{};
    while (sigtimedwait(&blocked, nullptr, &timeout) == -1 && errno == EINTR) {}
#endif
  }
  (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  errno = saved_errno;
  return count;
}

void reportExecError(int fd, int code) {
  const auto *data = reinterpret_cast<const char *>(&code);
  std::size_t offset = 0;
  while (offset < sizeof(code)) {
    const auto written = write(fd, data + offset, sizeof(code) - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written == -1 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

bool readExecError(int fd, int &code, std::string &error) {
  auto *data = reinterpret_cast<char *>(&code);
  std::size_t size = 0;
  code = 0;
  while (size < sizeof(code)) {
    const auto count = read(fd, data + size, sizeof(code) - size);
    if (count > 0) {
      size += static_cast<std::size_t>(count);
    } else if (count == 0) {
      break;
    } else if (errno == EINTR) {
      continue;
    } else {
      error = std::string("failed to read exec error pipe: ") +
              std::strerror(errno);
      return false;
    }
  }
  if (size != 0 && size != sizeof(code)) {
    error = "failed to read complete exec error";
    return false;
  }
  return true;
}

#endif

} // namespace

std::string summarizeCommandFailure(const std::string &program,
                                    const CommandResult &result) {
  std::ostringstream out;
  out << program << " failed with exit code " << result.exit_code;
  const auto stderr_summary = trimForSummary(result.stderr_text);
  if (!stderr_summary.empty()) {
    out << ": " << stderr_summary;
  } else {
    const auto stdout_summary = trimForSummary(result.stdout_text);
    if (!stdout_summary.empty()) {
      out << ": " << stdout_summary;
    }
  }
  return out.str();
}

std::optional<CommandResult>
runProcess(const std::string &program,
           const std::vector<std::string> &arguments, std::string &error) {
  return runProcess(program, arguments, ProcessRunOptions{}, error);
}

std::optional<CommandResult>
runProcess(const std::string &program,
           const std::vector<std::string> &arguments,
           const ProcessRunOptions &options, std::string &error) {
  error.clear();
#ifdef _WIN32
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(SECURITY_ATTRIBUTES);
  security.bInheritHandle = TRUE;

  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  HANDLE stderr_read = nullptr;
  HANDLE stderr_write = nullptr;
  HANDLE stdin_read = nullptr;
  HANDLE stdin_write = nullptr;
  HandleGuard stdout_read_guard;
  HandleGuard stdout_write_guard;
  HandleGuard stderr_read_guard;
  HandleGuard stderr_write_guard;
  HandleGuard stdin_guard;
  HandleGuard stdin_write_guard;
  if (CreatePipe(&stdout_read, &stdout_write, &security, 0) == 0) {
    error = "failed to create process pipes";
    return std::nullopt;
  }
  stdout_read_guard.handle = stdout_read;
  stdout_write_guard.handle = stdout_write;
  if (CreatePipe(&stderr_read, &stderr_write, &security, 0) == 0) {
    error = "failed to create process pipes";
    return std::nullopt;
  }
  stderr_read_guard.handle = stderr_read;
  stderr_write_guard.handle = stderr_write;
  if (options.stdin_text) {
    if (CreatePipe(&stdin_read, &stdin_write, &security, 0) == 0) {
      error = "failed to create process input pipe";
      return std::nullopt;
    }
    stdin_guard.handle = stdin_read;
    stdin_write_guard.handle = stdin_write;
  } else if (!duplicateStandardInput(stdin_guard, error)) {
    return std::nullopt;
  }
  if (SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) == 0 ||
      SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0) == 0 ||
      (stdin_write != nullptr &&
       SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) == 0)) {
    if (error.empty()) {
      error = "failed to isolate process pipe handles";
    }
    return std::nullopt;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = stdin_guard.handle;
  startup.StartupInfo.hStdOutput = stdout_write;
  startup.StartupInfo.hStdError = stderr_write;
  SIZE_T attribute_size = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  AttributeListGuard attributes;
  attributes.list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      HeapAlloc(GetProcessHeap(), 0, attribute_size));
  std::array<HANDLE, 3> inherited_handles = {stdin_guard.handle, stdout_write,
                                             stderr_write};
  if (attribute_size == 0 || attributes.list == nullptr ||
      InitializeProcThreadAttributeList(attributes.list, 1, 0,
                                        &attribute_size) == 0) {
    error = "failed to configure process handle inheritance";
    return std::nullopt;
  }
  attributes.initialized = true;
  if (UpdateProcThreadAttribute(
          attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          inherited_handles.data(), inherited_handles.size() * sizeof(HANDLE),
          nullptr, nullptr) == 0) {
    error = "failed to configure process handle inheritance";
    return std::nullopt;
  }
  startup.lpAttributeList = attributes.list;
  PROCESS_INFORMATION process{};
  auto command = buildWindowsCommandLine(program, arguments);
  auto environment = buildWindowsEnvironment(options, error);
  if (!options.environment_overrides.empty() && environment.empty())
    return std::nullopt;
  const DWORD creation_flags =
      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
      (environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                     creation_flags,
                     environment.empty() ? nullptr : environment.data(),
                     nullptr, &startup.StartupInfo, &process) == 0) {
    const auto code = GetLastError();
    error = "failed to start " + program + ": " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message();
    return std::nullopt;
  }
  HandleGuard process_guard{process.hProcess};
  HandleGuard thread_guard{process.hThread};
  stdout_write_guard.handle = nullptr;
  stderr_write_guard.handle = nullptr;
  CloseHandle(stdout_write);
  CloseHandle(stderr_write);
  if (stdin_read != nullptr) {
    stdin_guard.handle = nullptr;
    CloseHandle(stdin_read);
  }

  CommandResult result;
  std::string stdout_error;
  std::string stderr_error;
  std::string stdin_error;
  std::atomic<bool> cancel{false};
  std::thread stdout_thread([&]() {
    (void)readPipe(stdout_read, result.stdout_text, options.max_stdout_bytes,
                   cancel, stdout_error);
  });
  std::thread stderr_thread([&]() {
    (void)readPipe(stderr_read, result.stderr_text, options.max_stderr_bytes,
                   cancel, stderr_error);
  });
  std::optional<std::thread> stdin_thread;
  if (options.stdin_text) {
    stdin_thread.emplace([&]() {
      (void)writePipe(stdin_write, *options.stdin_text, stdin_error);
      stdin_write_guard.handle = nullptr;
      CloseHandle(stdin_write);
    });
  }

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  for (;;) {
    const auto status = WaitForSingleObject(process.hProcess, 10);
    if (status == WAIT_OBJECT_0)
      break;
    if (status == WAIT_FAILED) {
      error = "failed to wait for process";
      cancel.store(true);
      break;
    }
    if (cancel.load())
      break;
    if (options.timeout_milliseconds != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
                .count() >=
            static_cast<std::int64_t>(options.timeout_milliseconds)) {
      timed_out = true;
      break;
    }
  }
  if (timed_out || cancel.load() || !error.empty())
    TerminateProcess(process.hProcess, 1);
  WaitForSingleObject(process.hProcess, INFINITE);
  if (stdin_thread)
    stdin_thread->join();
  stdout_thread.join();
  stderr_thread.join();
  if (timed_out) {
    error = "process timed out";
    return std::nullopt;
  }
  if (!stdout_error.empty() || !stderr_error.empty() || !stdin_error.empty()) {
    error = !stdout_error.empty()
                ? stdout_error
                : (!stderr_error.empty() ? stderr_error : stdin_error);
    return std::nullopt;
  }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(process.hProcess, &exit_code) == 0) {
    error = "failed to read process exit code";
    return std::nullopt;
  }
  result.exit_code = static_cast<int>(exit_code);
  return result;
#else
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int exec_pipe[2] = {-1, -1};
  int stdin_pipe[2] = {-1, -1};
  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1);
  storage.push_back(program);
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (auto &value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);
  if (!createPipe(stdout_pipe, error) || !createPipe(stderr_pipe, error) ||
      !createPipe(exec_pipe, error) ||
      (options.stdin_text && !createPipe(stdin_pipe, error))) {
    closeFd(stdout_pipe[0]);
    closeFd(stdout_pipe[1]);
    closeFd(stderr_pipe[0]);
    closeFd(stderr_pipe[1]);
    closeFd(exec_pipe[0]);
    closeFd(exec_pipe[1]);
    closeFd(stdin_pipe[0]);
    closeFd(stdin_pipe[1]);
    if (!error.empty()) {
      return std::nullopt;
    }
    error =
        std::string("failed to create process pipes: ") + std::strerror(errno);
    return std::nullopt;
  }
  const pid_t pid = fork();
  if (pid == -1) {
    error = std::string("failed to fork process: ") + std::strerror(errno);
    closeFd(stdout_pipe[0]);
    closeFd(stdout_pipe[1]);
    closeFd(stderr_pipe[0]);
    closeFd(stderr_pipe[1]);
    closeFd(exec_pipe[0]);
    closeFd(exec_pipe[1]);
    closeFd(stdin_pipe[0]);
    closeFd(stdin_pipe[1]);
    return std::nullopt;
  }
  if (pid == 0) {
    close(exec_pipe[0]);
    if ((options.stdin_text && dup2(stdin_pipe[0], STDIN_FILENO) == -1) ||
        dup2(stdout_pipe[1], STDOUT_FILENO) == -1 ||
        dup2(stderr_pipe[1], STDERR_FILENO) == -1) {
      const int setupError = errno;
      reportExecError(exec_pipe[1], setupError);
      _exit(127);
    }
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    closeFd(stdin_pipe[0]);
    closeFd(stdin_pipe[1]);
    for (const auto &[name, value] : options.environment_overrides) {
      if (name.empty() || name.find('=') != std::string::npos ||
          name.find('\0') != std::string::npos ||
          value.find('\0') != std::string::npos ||
          setenv(name.c_str(), value.c_str(), 1) != 0) {
        reportExecError(exec_pipe[1], errno == 0 ? EINVAL : errno);
        _exit(127);
      }
    }
    execvp(program.c_str(), argv.data());
    reportExecError(exec_pipe[1], errno);
    _exit(127);
  }
  closeFd(exec_pipe[1]);
  closeFd(stdout_pipe[1]);
  closeFd(stderr_pipe[1]);
  closeFd(stdin_pipe[0]);
  int exec_error = 0;
  if (!readExecError(exec_pipe[0], exec_error, error)) {
    closeFd(exec_pipe[0]);
    closeFd(stdout_pipe[0]);
    closeFd(stderr_pipe[0]);
    closeFd(stdin_pipe[1]);
    kill(pid, SIGKILL);
    int status = 0;
    (void)waitForChild(pid, status);
    return std::nullopt;
  }
  closeFd(exec_pipe[0]);
  if (exec_error != 0) {
    closeFd(stdout_pipe[0]);
    closeFd(stderr_pipe[0]);
    closeFd(stdin_pipe[1]);
    int status = 0;
    (void)waitForChild(pid, status);
    error = "failed to start " + program + ": " + std::strerror(exec_error);
    return std::nullopt;
  }
  CommandResult result;
  bool stdout_open = true;
  bool stderr_open = true;
  bool stdin_open = options.stdin_text.has_value();
  std::size_t stdin_offset = 0;
  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  std::array<char, 4096> buffer{};
  while (stdout_open || stderr_open || stdin_open) {
    if (stdin_open && stdin_offset == options.stdin_text->size()) {
      closeFd(stdin_pipe[1]);
      stdin_open = false;
    }
    std::array<pollfd, 3> streams{{
        {stdout_open ? stdout_pipe[0] : -1, POLLIN, 0},
        {stderr_open ? stderr_pipe[0] : -1, POLLIN, 0},
        {stdin_open ? stdin_pipe[1] : -1, POLLOUT, 0},
    }};
    int wait = -1;
    if (options.timeout_milliseconds != 0) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed >= static_cast<std::int64_t>(options.timeout_milliseconds)) {
        timed_out = true;
        break;
      }
      wait = static_cast<int>(
          (std::min<std::uint64_t>)(options.timeout_milliseconds -
                                        static_cast<std::uint64_t>(elapsed),
                                    static_cast<std::uint64_t>(
                                        (std::numeric_limits<int>::max)())));
    }
    const auto poll_result = poll(streams.data(), streams.size(), wait);
    if (poll_result == 0) {
      timed_out = true;
      break;
    }
    if (poll_result == -1) {
      if (errno == EINTR) {
        continue;
      }
      error =
          std::string("failed to read process pipes: ") + std::strerror(errno);
      break;
    }
    if (stdout_open && (streams[0].revents & (POLLIN | POLLHUP)) != 0) {
      const ssize_t count = read(stdout_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        if (static_cast<std::size_t>(count) >
            options.max_stdout_bytes - (std::min)(options.max_stdout_bytes,
                                                  result.stdout_text.size())) {
          error = "process stdout exceeded its configured limit";
          break;
        }
        result.stdout_text.append(buffer.data(),
                                  static_cast<std::size_t>(count));
      } else if (count == 0) {
        stdout_open = false;
        closeFd(stdout_pipe[0]);
      } else if (errno != EINTR) {
        error =
            std::string("failed to read stdout pipe: ") + std::strerror(errno);
        break;
      }
    }
    if (stderr_open && (streams[1].revents & (POLLIN | POLLHUP)) != 0) {
      const ssize_t count = read(stderr_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        if (static_cast<std::size_t>(count) >
            options.max_stderr_bytes - (std::min)(options.max_stderr_bytes,
                                                  result.stderr_text.size())) {
          error = "process stderr exceeded its configured limit";
          break;
        }
        result.stderr_text.append(buffer.data(),
                                  static_cast<std::size_t>(count));
      } else if (count == 0) {
        stderr_open = false;
        closeFd(stderr_pipe[0]);
      } else if (errno != EINTR) {
        error =
            std::string("failed to read stderr pipe: ") + std::strerror(errno);
        break;
      }
    }
    if (stdin_open && (streams[2].revents & POLLOUT) != 0) {
      const auto remaining = options.stdin_text->size() - stdin_offset;
      const auto count = writePipeWithoutSignal(
          stdin_pipe[1], options.stdin_text->data() + stdin_offset, remaining);
      if (count > 0) {
        stdin_offset += static_cast<std::size_t>(count);
      } else if (count < 0 && errno != EINTR) {
        error = std::string("failed to write process stdin: ") +
                std::strerror(errno);
        break;
      }
    }
    if ((stdout_open && (streams[0].revents & (POLLERR | POLLNVAL)) != 0) ||
        (stderr_open && (streams[1].revents & (POLLERR | POLLNVAL)) != 0) ||
        (stdin_open && (streams[2].revents & (POLLERR | POLLNVAL)) != 0)) {
      error = "failed to poll process pipes";
      break;
    }
    if (stdin_open && (streams[2].revents & POLLHUP) != 0) {
      closeFd(stdin_pipe[1]);
      stdin_open = false;
    }
  }
  int status = 0;
  if (!error.empty() || timed_out) {
    closeFd(stdout_pipe[0]);
    closeFd(stderr_pipe[0]);
    closeFd(stdin_pipe[1]);
    kill(pid, SIGKILL);
  }
  if (!waitForChild(pid, status, &error)) {
    return std::nullopt;
  }
  if (timed_out) {
    error = "process timed out";
    return std::nullopt;
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  if (!error.empty()) {
    return std::nullopt;
  }
  return result;
#endif
}

std::optional<int>
runProcessPassthrough(const std::string &program,
                      const std::vector<std::string> &arguments,
                      std::string &error) {
  error.clear();
#ifdef _WIN32
  auto command = buildWindowsCommandLine(program, arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                     nullptr, nullptr, &startup, &process) == 0) {
    const auto code = GetLastError();
    error = "failed to start process '" + program + "': " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message();
    return std::nullopt;
  }
  HandleGuard process_handle{process.hProcess};
  HandleGuard thread_handle{process.hThread};
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
    const auto code = GetLastError();
    error = "failed to wait for process '" + program + "': " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message();
    return std::nullopt;
  }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(process.hProcess, &exit_code) == 0) {
    const auto code = GetLastError();
    error = "failed to read process exit code: " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message();
    return std::nullopt;
  }
  return static_cast<int>(exit_code);
#else
  const auto pid = fork();
  if (pid == -1) {
    error = std::string("failed to fork process '") + program +
            "': " + std::strerror(errno);
    return std::nullopt;
  }
  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char *>(program.c_str()));
    for (const auto &argument : arguments) {
      argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execvp(program.c_str(), argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }
  int status = 0;
  if (!waitForChild(pid, status, &error)) {
    return std::nullopt;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  error = "process ended without an exit status";
  return std::nullopt;
#endif
}

} // namespace chtholly
