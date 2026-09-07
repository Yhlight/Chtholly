#include "chtholly/Driver/ArtifactStore.h"

#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace chtholly {
namespace {

bool isHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::string environmentValue(const char *name) {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const auto *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

std::filesystem::path absoluteNormalized(const std::filesystem::path &path) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    absolute = path;
  }
  return absolute.lexically_normal();
}

std::filesystem::path utf8Path(std::string_view value) {
  std::u8string converted;
  converted.reserve(value.size());
  for (const unsigned char ch : value) {
    converted.push_back(static_cast<char8_t>(ch));
  }
  auto path = std::filesystem::path(converted);
  path.make_preferred();
  return path;
}

bool pathWithin(const std::filesystem::path &path,
                const std::filesystem::path &root) {
  const auto candidate = absoluteNormalized(path);
  const auto boundary = absoluteNormalized(root);
  auto candidate_it = candidate.begin();
  for (auto root_it = boundary.begin(); root_it != boundary.end();
       ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::filesystem::path closurePath(const std::filesystem::path &root,
                                  std::string_view digest) {
  return root / "closures" / "sha256" /
         utf8Path(digest);
}

std::filesystem::path referencePath(const std::filesystem::path &root,
                                    const ArtifactStoreLocator &locator) {
  return root / "identities" / "sha256" /
         utf8Path(locator.artifact_identity) /
         utf8Path(locator.closure_digest + ".ref");
}

std::filesystem::path blobPath(const std::filesystem::path &root,
                               std::string_view digest) {
  return root / "blobs" / "sha256" /
         utf8Path(digest.substr(0, 2)) / utf8Path(digest);
}

std::string nextStagingNonce() {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(clock) + "-" + std::to_string(serial);
}

class StoreLock {
public:
  StoreLock(const std::filesystem::path &root, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
      error = "failed to create artifact store: " + ec.message();
      return;
    }
    const auto path = root / ".lock";
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      error = "failed to open artifact store lock";
      return;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlapped)) {
      error = "failed to lock artifact store";
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
#else
    descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
      error = "failed to lock artifact store";
      if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
      }
      return;
    }
#endif
    locked_ = true;
  }

  ~StoreLock() {
    if (!locked_) {
      return;
    }
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle_);
#else
    flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
#endif
  }

  bool locked() const { return locked_; }

private:
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

class DirectoryCleanup {
public:
  explicit DirectoryCleanup(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~DirectoryCleanup() {
    if (active_) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }
  void release() { active_ = false; }

private:
  std::filesystem::path path_;
  bool active_ = true;
};

bool copyFileExclusive(const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       std::string_view expected_digest,
                       std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    error = "failed to create artifact store directory: " + ec.message();
    return false;
  }
  if (std::filesystem::exists(destination, ec)) {
    const auto digest = sha256File(destination.string());
    if (!digest || *digest != expected_digest) {
      error = "artifact store blob digest collision or corruption: '" +
              destination.string() + "'";
      return false;
    }
    return true;
  }
  auto temporary = destination;
  temporary += ".tmp";
  std::filesystem::copy_file(source, temporary,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    error = "failed to stage artifact store blob: " + ec.message();
    return false;
  }
  const auto staged_digest = sha256File(temporary.string());
  if (!staged_digest || *staged_digest != expected_digest) {
    error = "staged artifact store blob SHA-256 mismatch: '" +
            temporary.string() + "'";
    removeFile(temporary.string(), ec);
    return false;
  }
  if (!replaceFile(temporary.string(), destination.string(), ec)) {
    error = "failed to publish artifact store blob: " + ec.message();
    removeFile(temporary.string(), ec);
    return false;
  }
  return true;
}

bool validateClosureTree(
    const std::filesystem::path &tree,
    const std::vector<PackageArtifactArchiveFile> &files,
    std::string_view description, std::string &error) {
  std::set<std::string> expected;
  std::error_code ec;
  for (const auto &file : files) {
    expected.insert(file.relative_path);
    const auto path = tree / utf8Path(file.relative_path);
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status) ||
        std::filesystem::file_size(path, ec) != file.size || ec) {
      error = std::string(description) +
              " file is missing or unsafe: '" + path.string() + "'";
      return false;
    }
    const auto digest = sha256File(path.string());
    if (!digest || *digest != file.sha256) {
      error = std::string(description) + " file SHA-256 mismatch: '" +
              path.string() + "'";
      return false;
    }
  }
  for (std::filesystem::recursive_directory_iterator it(tree, ec), end;
       !ec && it != end; it.increment(ec)) {
    const auto status = it->symlink_status(ec);
    if (ec || std::filesystem::is_symlink(status)) {
      error = std::string(description) + " contains a symlink";
      return false;
    }
    if (!std::filesystem::is_regular_file(status)) {
      continue;
    }
    const auto relative = std::filesystem::relative(it->path(), tree, ec)
                              .generic_string();
    if (ec || !expected.erase(relative)) {
      error = std::string(description) + " contains an unindexed file";
      return false;
    }
  }
  if (ec || !expected.empty()) {
    error = "failed to enumerate " + std::string(description);
    return false;
  }
  return true;
}

bool linkOrCopy(const std::filesystem::path &source,
                const std::filesystem::path &destination,
                std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    error = "failed to create artifact closure directory: " + ec.message();
    return false;
  }
  std::filesystem::create_hard_link(source, destination, ec);
  if (!ec) {
    return true;
  }
  ec.clear();
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::none, ec);
  if (ec) {
    error = "failed to materialize artifact closure file: " + ec.message();
    return false;
  }
  return true;
}

bool writeAtomicText(const std::filesystem::path &path, std::string_view text,
                     std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "failed to create artifact reference directory: " + ec.message();
    return false;
  }
  const auto temporary = path.string() + ".tmp";
  if (!writeTextFile(temporary, std::string(text), error)) {
    return false;
  }
  if (!replaceFile(temporary, path.string(), ec)) {
    error = "failed to publish artifact store reference: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}

std::string renderReference(const PackageArtifactArchiveInfo &info) {
  std::ostringstream out;
  out << "chtholly-artifact-ref-v1\n"
      << "artifact-identity\t" << info.artifact_identity << '\n'
      << "closure-digest\t" << info.closure_digest << '\n'
      << "archive-sha256\t" << info.archive_sha256 << '\n'
      << "end\n";
  return out.str();
}

std::optional<PackageArtifactArchiveInfo>
inspectUnlocked(const std::filesystem::path &root,
                const ArtifactStoreLocator &locator, std::string &error) {
  const auto reference = referencePath(root, locator);
  std::error_code ec;
  const auto reference_status = std::filesystem::symlink_status(reference, ec);
  if (ec || !std::filesystem::is_regular_file(reference_status) ||
      std::filesystem::is_symlink(reference_status)) {
    error = "artifact store locator is not installed: " +
            renderArtifactStoreLocator(locator);
    return std::nullopt;
  }
  const auto reference_text = readTextFile(reference.string(), error);
  if (!reference_text) {
    return std::nullopt;
  }
  std::istringstream reference_input(*reference_text);
  std::string header;
  std::string identity_line;
  std::string closure_line;
  std::string archive_line;
  std::string end_line;
  std::string trailing;
  if (!std::getline(reference_input, header) ||
      !std::getline(reference_input, identity_line) ||
      !std::getline(reference_input, closure_line) ||
      !std::getline(reference_input, archive_line) ||
      !std::getline(reference_input, end_line) ||
      static_cast<bool>(std::getline(reference_input, trailing)) ||
      header != "chtholly-artifact-ref-v1" ||
      identity_line != "artifact-identity\t" + locator.artifact_identity ||
      closure_line != "closure-digest\t" + locator.closure_digest ||
      !archive_line.starts_with("archive-sha256\t") ||
      !isHexDigest(archive_line.substr(std::string("archive-sha256\t").size())) ||
      end_line != "end") {
    error = "artifact store reference is invalid or does not match its locator";
    return std::nullopt;
  }
  const auto closure = closurePath(root, locator.closure_digest);
  const auto closure_status = std::filesystem::symlink_status(closure, ec);
  if (ec || !std::filesystem::is_directory(closure_status) ||
      std::filesystem::is_symlink(closure_status)) {
    error = "installed artifact closure directory is missing or unsafe";
    return std::nullopt;
  }
  const auto index_path = closure / "closure.index";
  auto index = readTextFile(index_path.string(), error);
  if (!index) {
    error = "installed artifact closure index is missing: " + error;
    return std::nullopt;
  }
  auto info = parsePackageArtifactArchiveIndex(*index, error);
  if (!info || info->artifact_identity != locator.artifact_identity ||
      info->closure_digest != locator.closure_digest) {
    if (error.empty()) {
      error = "installed artifact closure does not match its locator";
    }
    return std::nullopt;
  }
  const auto tree = closure / "tree";
  const auto tree_status = std::filesystem::symlink_status(tree, ec);
  if (ec || !std::filesystem::is_directory(tree_status) ||
      std::filesystem::is_symlink(tree_status)) {
    error = "installed artifact closure tree is missing or unsafe";
    return std::nullopt;
  }
  if (!validateClosureTree(tree, info->files,
                           "installed artifact closure tree", error)) {
    return std::nullopt;
  }
  info->archive_path.clear();
  return info;
}

bool closureHasReference(const std::filesystem::path &root,
                         std::string_view closure_digest) {
  const auto identities = root / "identities" / "sha256";
  std::error_code ec;
  if (!std::filesystem::exists(identities, ec)) {
    return false;
  }
  const auto filename = std::string(closure_digest) + ".ref";
  for (std::filesystem::recursive_directory_iterator it(identities, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().filename() == filename) {
      return true;
    }
  }
  return false;
}

} // namespace

std::string defaultArtifactStorePath() {
#if defined(_WIN32)
  auto base = environmentValue("LOCALAPPDATA");
  if (base.empty()) {
    base = environmentValue("USERPROFILE");
  }
  return (std::filesystem::path(base) / "Chtholly" / "artifact-store" / "v1")
      .string();
#else
  auto base = environmentValue("XDG_DATA_HOME");
  if (base.empty()) {
    base = (std::filesystem::path(environmentValue("HOME")) / ".local" /
            "share")
               .string();
  }
  return (std::filesystem::path(base) / "chtholly" / "artifact-store" / "v1")
      .string();
#endif
}

std::string resolveArtifactStorePath(std::string_view cli_path) {
  if (!cli_path.empty()) {
    return absoluteNormalized(utf8Path(cli_path))
        .string();
  }
  const auto environment = environmentValue("CHTHOLLY_ARTIFACT_STORE");
  return absoluteNormalized(environment.empty() ? defaultArtifactStorePath()
                                                : environment)
      .string();
}

std::optional<ArtifactStoreLocator>
parseArtifactStoreLocator(std::string_view locator, std::string &error) {
  constexpr std::string_view prefix = "store:sha256:";
  constexpr std::string_view separator = "@sha256:";
  if (!locator.starts_with(prefix)) {
    error = "invalid artifact store locator";
    return std::nullopt;
  }
  locator.remove_prefix(prefix.size());
  const auto split = locator.find(separator);
  if (split == std::string_view::npos) {
    error = "artifact store locator requires identity and closure digests";
    return std::nullopt;
  }
  ArtifactStoreLocator result{std::string(locator.substr(0, split)),
                              std::string(locator.substr(split + separator.size()))};
  if (!isHexDigest(result.artifact_identity) ||
      !isHexDigest(result.closure_digest)) {
    error = "artifact store locator contains an invalid SHA-256 digest";
    return std::nullopt;
  }
  return result;
}

std::string renderArtifactStoreLocator(const ArtifactStoreLocator &locator) {
  return "store:sha256:" + locator.artifact_identity + "@sha256:" +
         locator.closure_digest;
}

ArtifactStore::ArtifactStore(std::string root)
    : root_(resolveArtifactStorePath(root)) {}

std::optional<PackageArtifactArchiveInfo>
ArtifactStore::install(const std::string &archive_path, std::string &error,
                       ArtifactStoreInstallObservation *observation) const {
  auto info = inspectPackageArtifactArchive(archive_path, error);
  if (!info) {
    return std::nullopt;
  }
  if (observation) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(archive_path, size_error);
    observation->archive_bytes = size_error ? 0 : size;
  }
  const auto root = pathForFileSystemTreeRoot(root_);
  StoreLock lock(root, error);
  if (!lock.locked()) {
    return std::nullopt;
  }
  const ArtifactStoreLocator locator{info->artifact_identity,
                                     info->closure_digest};
  if (observation)
    observation->closure_hit =
        std::filesystem::exists(closurePath(root, info->closure_digest));
  if (std::filesystem::exists(referencePath(root, locator))) {
    auto installed = inspectUnlocked(root, locator, error);
    if (installed) {
      installed->archive_path = archive_path;
      installed->archive_sha256 = info->archive_sha256;
    }
    return installed;
  }

  const auto staging = root / (".staging-" + nextStagingNonce());
  DirectoryCleanup staging_cleanup(staging);
  PackageArtifactArchiveInfo extracted;
  if (!extractPackageArtifactArchive(archive_path, staging.string(), extracted,
                                     error)) {
    return std::nullopt;
  }
  if (extracted.archive_sha256 != info->archive_sha256 ||
      extracted.artifact_identity != info->artifact_identity ||
      extracted.closure_digest != info->closure_digest) {
    error = "artifact archive changed while acquiring the store lock";
    return std::nullopt;
  }
  for (const auto &file : info->files) {
    const auto source = staging / "tree" /
                        utf8Path(file.relative_path);
    if (!copyFileExclusive(source, blobPath(root, file.sha256), file.sha256,
                           error)) {
      return std::nullopt;
    }
  }

  const auto closure = closurePath(root, info->closure_digest);
  if (!std::filesystem::exists(closure)) {
    const auto closure_staging =
        closure.parent_path() / (info->closure_digest + ".tmp");
    DirectoryCleanup closure_cleanup(closure_staging);
    std::error_code ec;
    std::filesystem::create_directories(closure_staging / "tree", ec);
    if (ec || !writeTextFile((closure_staging / "closure.index").string(),
                             info->canonical_index, error)) {
      if (error.empty()) {
        error = "failed to stage artifact closure: " + ec.message();
      }
      return std::nullopt;
    }
    for (const auto &file : info->files) {
      if (!linkOrCopy(blobPath(root, file.sha256),
                      closure_staging / "tree" /
                          utf8Path(file.relative_path),
                      error)) {
        return std::nullopt;
      }
    }
    std::filesystem::create_directories(closure.parent_path(), ec);
    std::filesystem::rename(closure_staging, closure, ec);
    if (ec) {
      error = "failed to publish artifact closure: " + ec.message();
      return std::nullopt;
    }
    closure_cleanup.release();
  }
  if (!writeAtomicText(referencePath(root, locator), renderReference(*info),
                       error)) {
    return std::nullopt;
  }
  auto installed = inspectUnlocked(root, locator, error);
  if (!installed) {
    std::error_code ec;
    std::filesystem::remove(referencePath(root, locator), ec);
    return std::nullopt;
  }
  installed->archive_path = archive_path;
  installed->archive_sha256 = info->archive_sha256;
  return installed;
}

bool ArtifactStore::uninstall(std::string_view locator_text,
                              std::string &error) const {
  auto locator = parseArtifactStoreLocator(locator_text, error);
  if (!locator) {
    return false;
  }
  const auto root = pathForFileSystemTreeRoot(root_);
  StoreLock lock(root, error);
  if (!lock.locked()) {
    return false;
  }
  const auto reference = referencePath(root, *locator);
  if (!pathWithin(reference, root / "identities" / "sha256")) {
    error = "artifact uninstall target escapes the identity store";
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::remove(reference, ec)) {
    error = ec ? "failed to remove artifact reference: " + ec.message()
               : "artifact store locator is not installed: " +
                     std::string(locator_text);
    return false;
  }
  std::filesystem::remove(reference.parent_path(), ec);
  if (!closureHasReference(root, locator->closure_digest)) {
    const auto closure = closurePath(root, locator->closure_digest);
    if (!pathWithin(closure, root / "closures" / "sha256")) {
      error = "artifact uninstall target escapes the closure store";
      return false;
    }
    std::filesystem::remove_all(closure, ec);
    if (ec) {
      error = "failed to remove unreferenced artifact closure: " + ec.message();
      return false;
    }
  }
  return true;
}

std::optional<PackageArtifactArchiveInfo>
ArtifactStore::inspect(std::string_view locator_text, std::string &error) const {
  auto locator = parseArtifactStoreLocator(locator_text, error);
  if (!locator) {
    return std::nullopt;
  }
  const auto root = pathForFileSystemTreeRoot(root_);
  StoreLock lock(root, error);
  if (!lock.locked()) {
    return std::nullopt;
  }
  return inspectUnlocked(root, *locator, error);
}

std::optional<std::vector<std::string>>
ArtifactStore::list(std::string &error) const {
  const auto root = pathForFileSystemTreeRoot(root_);
  StoreLock lock(root, error);
  if (!lock.locked()) {
    return std::nullopt;
  }
  std::vector<std::string> locators;
  const auto identities = root / "identities" / "sha256";
  std::error_code ec;
  if (!std::filesystem::exists(identities, ec)) {
    return locators;
  }
  for (std::filesystem::directory_iterator identity_it(identities, ec), end;
       !ec && identity_it != end; identity_it.increment(ec)) {
    if (!identity_it->is_directory(ec) ||
        !isHexDigest(identity_it->path().filename().string())) {
      continue;
    }
    for (std::filesystem::directory_iterator ref_it(identity_it->path(), ec),
         ref_end;
         !ec && ref_it != ref_end; ref_it.increment(ec)) {
      auto closure = ref_it->path().stem().string();
      if (ref_it->is_regular_file(ec) && ref_it->path().extension() == ".ref" &&
          isHexDigest(closure)) {
        locators.push_back(renderArtifactStoreLocator(
            {identity_it->path().filename().string(), std::move(closure)}));
      }
    }
  }
  if (ec) {
    error = "failed to enumerate artifact store: " + ec.message();
    return std::nullopt;
  }
  std::sort(locators.begin(), locators.end());
  return locators;
}

std::optional<std::size_t>
ArtifactStore::garbageCollect(std::string &error) const {
  const auto store_root = pathForFileSystemTreeRoot(root_);
  StoreLock lock(store_root, error);
  if (!lock.locked()) {
    return std::nullopt;
  }
  const auto root = store_root;
  std::set<std::string> referenced_closures;
  std::set<std::string> retained;
  std::error_code ec;
  const auto identities = root / "identities" / "sha256";
  if (std::filesystem::exists(identities, ec)) {
    for (std::filesystem::directory_iterator identity_it(identities, ec), end;
         !ec && identity_it != end; identity_it.increment(ec)) {
      const auto identity = identity_it->path().filename().string();
      if (!identity_it->is_directory(ec) || !isHexDigest(identity)) {
        continue;
      }
      for (std::filesystem::directory_iterator ref_it(identity_it->path(), ec),
           ref_end;
           !ec && ref_it != ref_end; ref_it.increment(ec)) {
        const auto closure_digest = ref_it->path().stem().string();
        if (!ref_it->is_regular_file(ec) ||
            ref_it->path().extension() != ".ref" ||
            !isHexDigest(closure_digest)) {
          continue;
        }
        const ArtifactStoreLocator locator{identity, closure_digest};
        if (!inspectUnlocked(root, locator, error)) {
          error = "cannot collect a store with an invalid reference: " +
                  error;
          return std::nullopt;
        }
        referenced_closures.insert(closure_digest);
      }
    }
  }
  if (ec) {
    error = "failed to enumerate artifact references during GC: " +
            ec.message();
    return std::nullopt;
  }

  const auto closures = root / "closures" / "sha256";
  std::vector<std::filesystem::path> orphaned_closures;
  if (std::filesystem::exists(closures, ec)) {
    for (std::filesystem::directory_iterator it(closures, ec), end;
         !ec && it != end; it.increment(ec)) {
      const auto closure_digest = it->path().filename().string();
      if (!it->is_directory(ec) || !isHexDigest(closure_digest)) {
        continue;
      }
      if (!referenced_closures.contains(closure_digest)) {
        orphaned_closures.push_back(it->path());
        continue;
      }
      auto index = readTextFile((it->path() / "closure.index").string(), error);
      auto info = index ? parsePackageArtifactArchiveIndex(*index, error)
                        : std::nullopt;
      if (!info || info->closure_digest != closure_digest) {
        error = "cannot collect a store with an invalid closure index: " + error;
        return std::nullopt;
      }
      for (const auto &file : info->files) {
        retained.insert(file.sha256);
      }
    }
  }
  if (ec) {
    error = "failed to enumerate artifact closures during GC: " + ec.message();
    return std::nullopt;
  }
  for (const auto &orphan : orphaned_closures) {
    if (!pathWithin(orphan, closures)) {
      error = "artifact GC target escapes the closure store";
      return std::nullopt;
    }
    std::filesystem::remove_all(orphan, ec);
    if (ec) {
      error = "failed to remove unreferenced artifact closure: " +
              ec.message();
      return std::nullopt;
    }
  }
  std::size_t removed = 0;
  const auto blobs = root / "blobs" / "sha256";
  if (!std::filesystem::exists(blobs, ec)) {
    return removed;
  }
  for (std::filesystem::recursive_directory_iterator it(blobs, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const auto digest = it->path().filename().string();
    if (!isHexDigest(digest) || retained.contains(digest)) {
      continue;
    }
    if (!pathWithin(it->path(), blobs)) {
      error = "artifact GC target escapes the blob store";
      return std::nullopt;
    }
    std::filesystem::remove(it->path(), ec);
    if (ec) {
      error = "failed to remove unreferenced artifact blob: " + ec.message();
      return std::nullopt;
    }
    ++removed;
  }
  if (ec) {
    error = "failed to enumerate artifact blobs during GC: " + ec.message();
    return std::nullopt;
  }
  return removed;
}

std::optional<std::string>
ArtifactStore::materialize(std::string_view locator_text,
                           const std::string &destination_root,
                           std::string &error) const {
  auto locator = parseArtifactStoreLocator(locator_text, error);
  if (!locator) {
    return std::nullopt;
  }
  const auto store_root = pathForFileSystemTreeRoot(root_);
  StoreLock lock(store_root, error);
  if (!lock.locked()) {
    return std::nullopt;
  }
  auto info = inspectUnlocked(store_root, *locator, error);
  if (!info) {
    return std::nullopt;
  }
  const auto destination_base =
      absoluteNormalized(pathForFileSystemTreeRoot(destination_root));
  const auto destination = destination_base / locator->closure_digest;
  const auto manifest = destination /
                        utf8Path(info->root_manifest_relative_path);
  std::error_code ec;
  if (std::filesystem::is_regular_file(manifest, ec)) {
    if (validateClosureTree(destination, info->files,
                            "existing artifact materialization", error)) {
      return manifest.string();
    }
    return std::nullopt;
  }
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto staging = destination_base /
                       (locator->closure_digest + ".tmp-" +
                        std::to_string(nonce));
  DirectoryCleanup cleanup(staging);
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    error = "failed to create artifact materialization directory: " +
            ec.message();
    return std::nullopt;
  }
  const auto source_tree =
      closurePath(store_root, locator->closure_digest) / "tree";
  for (const auto &file : info->files) {
    if (!linkOrCopy(source_tree / utf8Path(file.relative_path),
                    staging / utf8Path(file.relative_path),
                    error)) {
      return std::nullopt;
    }
  }
  std::filesystem::create_directories(destination_base, ec);
  std::filesystem::rename(staging, destination, ec);
  if (ec) {
    error = "failed to publish artifact materialization: " + ec.message();
    return std::nullopt;
  }
  cleanup.release();
  return manifest.string();
}

std::optional<std::string>
ArtifactStore::filePath(std::string_view locator_text,
                        std::string_view relative_path,
                        std::string &error) const {
  auto locator = parseArtifactStoreLocator(locator_text, error);
  if (!locator)
    return std::nullopt;
  const auto root = pathForFileSystemTreeRoot(root_);
  const auto closure = closurePath(root, locator->closure_digest);
  const auto candidate = closure / "tree" /
                        utf8Path(relative_path);
  if (!pathWithin(candidate, closure / "tree")) {
    error = "artifact store file path escapes closure";
    return std::nullopt;
  }
  std::error_code ec;
  const auto filesystem_candidate = pathForFileSystem(candidate.string());
  if (!std::filesystem::is_regular_file(filesystem_candidate, ec) || ec) {
    error = "artifact store closure file is missing: '" + candidate.string() +
            "'";
    return std::nullopt;
  }
  return candidate.string();
}

} // namespace chtholly
