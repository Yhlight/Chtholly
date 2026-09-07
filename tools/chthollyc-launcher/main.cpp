#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

std::filesystem::path launcherPath(int argc, char **argv) {
  std::filesystem::path path;
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, buffer.data(),
                                       static_cast<DWORD>(buffer.size()));
  if (size != 0 && size < buffer.size()) {
    buffer.resize(size);
    path = buffer;
  }
#elif defined(__linux__)
  std::error_code link_error;
  path = std::filesystem::read_symlink("/proc/self/exe", link_error);
  if (link_error)
    path.clear();
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0)
    path = buffer.data();
#endif
  if (path.empty() && argc > 0)
    path = argv[0];
  std::error_code ec;
  if (!path.is_absolute())
    path = std::filesystem::absolute(path, ec);
  if (!ec) {
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
      path = canonical;
  }
  return path.lexically_normal();
}

bool validReleaseId(std::string_view value) {
  return !value.empty() && value.size() <= 200 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' ||
                  ch == '+';
         });
}

} // namespace

int main(int argc, char **argv) {
  const auto launcher = launcherPath(argc, argv);
  const auto prefix = launcher.parent_path().parent_path();
  auto generation_root = prefix;
  const auto active_path = prefix / "state" / "active-v1";
  std::string error;
  if (std::filesystem::exists(active_path)) {
    auto active = chtholly::readTextFile(active_path.string(), error);
    if (!active) {
      std::cerr << "chthollyc launcher: " << error << '\n';
      return 1;
    }
    while (!active->empty() &&
           (active->back() == '\r' || active->back() == '\n'))
      active->pop_back();
    if (!validReleaseId(*active)) {
      std::cerr << "chthollyc launcher: invalid active generation state\n";
      return 1;
    }
    generation_root = prefix / "generations" / *active;
  }
#if defined(_WIN32)
  const auto driver =
      generation_root / "libexec" / "chtholly" / "chthollyc-driver.exe";
#else
  const auto driver =
      generation_root / "libexec" / "chtholly" / "chthollyc-driver";
#endif
  if (!std::filesystem::is_regular_file(driver)) {
    std::cerr << "chthollyc launcher: compiler driver is missing from '"
              << driver.string() << "'\n";
    return 1;
  }
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index)
    arguments.emplace_back(argv[index]);
  auto result =
      chtholly::runProcessPassthrough(driver.string(), arguments, error);
  if (!result) {
    std::cerr << "chthollyc launcher: " << error << '\n';
    return 1;
  }
  return *result;
}
