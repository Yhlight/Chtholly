#include "RegistryServerSupportInternal.h"

#include "chtholly/Support/Digest.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace chtholly::registry_internal {

bool isHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool isSafeRegistryPackageName(std::string_view value) {
  if (value.empty() || value == "." || value == ".." || value.size() > 255)
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
  });
}

bool isValidHttpsBaseUrl(std::string_view value) {
  if (!value.starts_with("https://") ||
      value.find_first_of("\t\r\n ?#") != std::string_view::npos)
    return false;
  const auto authority_end = value.find('/', 8);
  const auto authority = value.substr(
      8, authority_end == std::string_view::npos
             ? std::string_view::npos
             : authority_end - 8);
  return !authority.empty() && authority.find('@') == std::string_view::npos;
}

bool pathIsWithin(const std::filesystem::path &path,
                  const std::filesystem::path &root) {
  std::error_code ec;
  const auto normalized_path = std::filesystem::weakly_canonical(path, ec);
  if (ec)
    return false;
  const auto normalized_root = std::filesystem::weakly_canonical(root, ec);
  if (ec)
    return false;
  auto path_it = normalized_path.begin();
  for (auto root_it = normalized_root.begin(); root_it != normalized_root.end();
       ++root_it, ++path_it) {
    if (path_it == normalized_path.end() || *path_it != *root_it)
      return false;
  }
  return true;
}

void appendMutationField(std::ostringstream &out, std::string_view name,
                         std::string_view value) {
  out << name << '\t' << value.size() << ':' << value << '\n';
}

std::string mutationDigest(std::string_view registry_name,
                           const SignedRegistryEntry &entry,
                           const RegistryArtifactVariant &variant) {
  const auto entry_digest = sha256Hex(renderSignedRegistryEntry(entry));
  std::ostringstream out;
  out << "chtholly-registry-publish-mutation-v1\n";
  appendMutationField(out, "registry", registry_name);
  appendMutationField(out, "package", entry.package_name);
  appendMutationField(out, "version", entry.version);
  appendMutationField(out, "variant", variant.name);
  appendMutationField(out, "archive-sha256", variant.archive_sha256);
  appendMutationField(out, "entry-sha256", entry_digest);
  return "sha256:" + sha256Hex(out.str());
}

std::string rfc3339(std::int64_t seconds) {
  std::time_t converted = static_cast<std::time_t>(seconds);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &converted) != 0)
    return {};
#else
  if (gmtime_r(&converted, &utc) == nullptr)
    return {};
#endif
  char output[21]{};
  if (std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc) !=
      20)
    return {};
  return output;
}

std::int64_t effectiveNow(std::int64_t requested) {
  return requested >= 0
             ? requested
             : static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
                   std::chrono::system_clock::now()));
}

bool runGit(const std::string &worktree,
            const std::vector<std::string> &arguments, std::string &error) {
  std::vector<std::string> args{"-c", "user.name=Chtholly Registry",
                                "-c", "user.email=registry@chtholly.invalid",
                                "-C", worktree};
  args.insert(args.end(), arguments.begin(), arguments.end());
  auto result = runProcess("git", args, error);
  if (!result || result->exit_code != 0) {
    if (result)
      error = summarizeCommandFailure("git", *result);
    return false;
  }
  return true;
}

std::optional<RegistryRootChainVerificationRequest> rootRequest(
    const RegistryServerConfig &config, std::int64_t now,
    std::string &error) {
  RegistryRootChainVerificationRequest request;
  request.registry_name = config.registry_name;
  request.checkout_root = config.index_worktree;
  request.bootstrap_root_keys = config.bootstrap_root_keys;
  request.bootstrap_root_threshold = config.bootstrap_root_threshold;
  request.now_unix_seconds = now;
  if (request.registry_name.empty() || request.checkout_root.empty()) {
    error = "registry server root configuration is incomplete";
    return std::nullopt;
  }
  return request;
}

bool copyIntoCas(const std::string &source, const std::filesystem::path &target,
                 std::uint64_t expected_size, std::string_view expected_digest,
                 std::string &error) {
  std::error_code ec;
  if (std::filesystem::exists(target, ec)) {
    const auto digest = sha256File(target.string());
    const auto size = std::filesystem::file_size(target, ec);
    if (!ec && digest && *digest == expected_digest && size == expected_size)
      return true;
    error = "registry CAS contains conflicting archive bytes";
    return false;
  }
  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) {
    error = "failed to create registry CAS directory: " + ec.message();
    return false;
  }
  const auto temporary = target.string() + ".tmp";
  std::filesystem::copy_file(
      source, temporary, std::filesystem::copy_options::overwrite_existing,
      ec);
  if (ec) {
    error = "failed to stage registry CAS archive: " + ec.message();
    return false;
  }
  const auto digest = sha256File(temporary);
  const auto size = std::filesystem::file_size(temporary, ec);
  if (ec || !digest || *digest != expected_digest || size != expected_size) {
    removeFile(temporary, ec);
    error = "staged registry CAS archive changed during publication";
    return false;
  }
  if (!replaceFile(temporary, target.string(), ec)) {
    removeFile(temporary, ec);
    error = "failed to publish registry CAS archive: " + ec.message();
    return false;
  }
  return true;
}

std::string resolveConfigPath(const std::filesystem::path &base,
                              std::string value) {
  auto path = std::filesystem::path(std::move(value));
  if (path.is_relative())
    path = base / path;
  return path.lexically_normal().string();
}

} // namespace chtholly::registry_internal
