#include "chtholly/Driver/GitRepositoryCache.h"

#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace chtholly {

namespace {

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  value.erase(0, first);
  return value;
}

std::uint64_t fnv1a(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string normalizePath(const std::filesystem::path &path) {
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  return (ec ? path : canonical).lexically_normal().generic_string();
}

std::vector<std::string>
configuredGitArguments(const std::vector<std::string> &arguments) {
  std::vector<std::string> args = {
      "-c", "core.fsync=none", "-c", "gc.auto=0", "-c",
      "credential.helper=", "-c", "credential.interactive=false"};
#ifdef _WIN32
  args.insert(args.end(), {"-c", "core.longpaths=true"});
#endif
  args.insert(args.end(), arguments.begin(), arguments.end());
  return args;
}

bool runGit(const std::vector<std::string> &arguments, std::string &error) {
  auto args = configuredGitArguments(arguments);
  auto result = runProcess("git", args, error);
  if (!result) {
    return false;
  }
  if (result->exit_code != 0) {
    error = summarizeCommandFailure("git", *result);
    return false;
  }
  return true;
}

bool isRetryableGitCloneFailure(std::string_view error) {
#ifdef _WIN32
  std::string normalized(error);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  if (normalized.find("filename too long") != std::string::npos) {
    return false;
  }
  return normalized.find("failed to unlink") != std::string::npos ||
         normalized.find("permission denied") != std::string::npos ||
         normalized.find("access is denied") != std::string::npos;
#else
  (void)error;
  return false;
#endif
}

bool cloneGit(const std::vector<std::string> &arguments,
              const std::filesystem::path &destination, std::string &error) {
#ifdef _WIN32
  constexpr int attempts = 3;
#else
  constexpr int attempts = 1;
#endif
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::error_code remove_error;
    std::filesystem::remove_all(destination, remove_error);
    if (remove_error) {
      error = "failed to clear Git clone destination '" +
              destination.string() + "': " + remove_error.message();
    } else if (runGit(arguments, error)) {
      return true;
    }
    if (attempt + 1 == attempts || !isRetryableGitCloneFailure(error)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
  }
  return false;
}

std::optional<std::string> readGit(const std::vector<std::string> &arguments,
                                   std::string &error) {
  auto args = configuredGitArguments(arguments);
  auto result = runProcess("git", args, error);
  if (!result) {
    return std::nullopt;
  }
  if (result->exit_code != 0) {
    error = summarizeCommandFailure("git", *result);
    return std::nullopt;
  }
  return trim(std::move(result->stdout_text));
}

bool hasGit(std::string &error) {
  auto version = readGit({"--version"}, error);
  if (!version) {
    error = "git executable is required for remote package sources";
    return false;
  }
  return true;
}

bool statusIsCleanExceptMarker(const std::string &status) {
  std::istringstream input(status);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && line != "?? .chtholly-source") {
      return false;
    }
  }
  return true;
}

bool isFullObjectId(std::string_view value) {
  if (value.size() != 40 && value.size() != 64) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char ch) {
    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
  });
}

std::string markerText(const GitRepositoryCacheRequest &request,
                       const std::string &commit) {
  std::ostringstream out;
  out << "chtholly-source-v1\n";
  out << "identity\t" << request.source_identity << "\n";
  out << "url\t" << request.url << "\n";
  if (!request.marker_selector_kind.empty()) {
    out << "selector\t" << request.marker_selector_kind << "\t"
        << request.marker_selector_value << "\n";
  }
  out << "commit\t" << commit << "\n";
  return out.str();
}

std::string modeName(const GitRepositoryCacheRequest &request) {
  return request.locked ? "locked" : (request.offline ? "offline" : "cached");
}

} // namespace

std::optional<GitRepositoryCheckout>
resolveGitRepositoryCheckout(const GitRepositoryCacheRequest &request,
                             std::string &error) {
  if (!hasGit(error)) {
    return std::nullopt;
  }
  const auto mirror = std::filesystem::path(request.cache_root) / "mirrors" /
                      (hex64(fnv1a(request.url)) + ".git");
  const bool mirror_exists = std::filesystem::exists(mirror);
  if (!mirror_exists) {
    if (request.offline || request.locked) {
      error = modeName(request) + " " + request.subject + " missing cached mirror";
      return std::nullopt;
    }
    std::filesystem::create_directories(mirror.parent_path());
    if (!cloneGit({"clone", "--mirror", request.url, mirror.string()}, mirror,
                  error)) {
      return std::nullopt;
    }
  } else if (request.update || request.pinned_commit.empty()) {
    if (!request.offline && !request.locked &&
        !runGit({"-C", mirror.string(), "fetch", "--prune"}, error)) {
      return std::nullopt;
    }
  }

  std::string commit = request.pinned_commit;
  if (commit.empty()) {
    if ((request.offline || request.locked) &&
        !request.allow_unpinned_offline) {
      error = modeName(request) + " " + request.subject + " requires lockfile pin";
      return std::nullopt;
    }
    auto resolved = readGit({"-C", mirror.string(), "rev-list", "-n", "1",
                             request.selector_ref}, error);
    if (!resolved || resolved->empty()) {
      return std::nullopt;
    }
    commit = *resolved;
  }
  if (!isFullObjectId(commit)) {
    error = modeName(request) + " " + request.subject +
            " has invalid pinned commit " + commit;
    return std::nullopt;
  }

  std::string commit_error;
  if (!readGit({"-C", mirror.string(), "cat-file", "-e", commit + "^{commit}"},
               commit_error)) {
    error = modeName(request) + " " + request.subject +
            " missing pinned commit " + commit;
    return std::nullopt;
  }

  const auto checkout = std::filesystem::path(request.cache_root) / "checkouts" /
                        (hex64(fnv1a(request.source_identity)) + "-" + commit);
  const auto marker_path = checkout / ".chtholly-source";
  const auto required_path = checkout / request.required_relative_path;
  const auto expected_marker = markerText(request, commit);
  bool checkout_valid = false;
  if (std::filesystem::exists(required_path)) {
    std::string read_error;
    auto marker = readTextFile(marker_path.string(), read_error);
    auto head = readGit({"-C", checkout.string(), "rev-parse", "HEAD"}, read_error);
    auto status = readGit({"-C", checkout.string(), "status", "--porcelain",
                           "--ignored"}, read_error);
    checkout_valid = marker && *marker == expected_marker && head &&
                     *head == commit && status && statusIsCleanExceptMarker(*status);
    if (!checkout_valid && (request.offline || request.locked)) {
      error = modeName(request) + " " + request.subject +
              " cached checkout does not match lockfile pin";
      return std::nullopt;
    }
  }
  if (!checkout_valid) {
    if (request.offline || request.locked) {
      error = modeName(request) + " " + request.subject +
              " missing cached checkout";
      return std::nullopt;
    }
    std::error_code ec;
    std::filesystem::create_directories(checkout.parent_path(), ec);
    if (!cloneGit(
            {"clone", "--no-checkout", mirror.string(), checkout.string()},
            checkout, error) ||
        !runGit({"-C", checkout.string(), "checkout", "--detach", commit}, error) ||
        !writeTextFile(marker_path.string(), expected_marker, error)) {
      return std::nullopt;
    }
  }

  return GitRepositoryCheckout{commit, normalizePath(checkout)};
}

} // namespace chtholly
