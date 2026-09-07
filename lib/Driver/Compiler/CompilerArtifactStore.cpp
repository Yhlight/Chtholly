#include "chtholly/Driver/CompilerArtifactStore.h"

#include "CompilerArtifactStoreInternal.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

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

ArtifactReadResult readArtifactFile(const std::string &path,
                                    CompilerArtifactLoadMetrics *metrics) {
  return CompilerArtifactReadService::read(path, metrics);
}

class SpecializationComponentCache {
public:
  static constexpr std::size_t MaximumEntries = 128;
  static constexpr std::uint64_t MaximumBytes = 16U * 1024U * 1024U;

  struct Entry {
    std::condition_variable ready;
    ArtifactReadResult read;
    bool finished = false;
    bool admitted = false;
    std::uint64_t last_used = 0;
  };

  struct Lookup {
    std::shared_ptr<Entry> entry;
    bool leader = false;
    bool duplicate_request = false;
  };

  Lookup begin(std::string_view fingerprint,
               CompilerArtifactLoadMetrics *metrics) {
    std::unique_lock lock(mutex_);
    const auto key = std::string(fingerprint);
    const auto duplicate_request =
        metrics && metrics->recordSpecializationComponentRequest(fingerprint);
    if (const auto found = entries_.find(key); found != entries_.end()) {
      auto entry = found->second;
      if (!entry->finished) {
        if (metrics)
          metrics->recordSpecializationComponentCacheLookup(
              CompilerSpecializationComponentCacheLookup::Coalesced);
        entry->ready.wait(lock, [&] { return entry->finished; });
      } else if (metrics) {
        metrics->recordSpecializationComponentCacheLookup(
            CompilerSpecializationComponentCacheLookup::Hit);
      }
      entry->last_used = ++sequence_;
      return {.entry = std::move(entry),
              .duplicate_request = duplicate_request};
    }
    auto entry = std::make_shared<Entry>();
    entry->last_used = ++sequence_;
    entries_.emplace(key, entry);
    if (metrics)
      metrics->recordSpecializationComponentCacheLookup(
          CompilerSpecializationComponentCacheLookup::Miss, duplicate_request);
    return {.entry = std::move(entry),
            .leader = true,
            .duplicate_request = duplicate_request};
  }

  void complete(std::string_view fingerprint,
                const std::shared_ptr<Entry> &entry, ArtifactReadResult read,
                bool verified, CompilerArtifactLoadMetrics *metrics) {
    std::unique_lock lock(mutex_);
    entry->read = std::move(read);
    entry->finished = true;
    const auto bytes = static_cast<std::uint64_t>(entry->read.bytes.size());
    const auto key = std::string(fingerprint);
    const auto found = entries_.find(key);
    const auto indexed = found != entries_.end() && found->second == entry;
    const auto cacheable =
        verified && entry->read.status == CompilerArtifactReadStatus::Found;
    bool bypass = !cacheable || bytes > MaximumBytes;
    std::uint64_t evictions = 0;
    if (!bypass && indexed) {
      while (admittedEntries() >= MaximumEntries ||
             bytes > MaximumBytes - cached_bytes_) {
        auto oldest = entries_.end();
        for (auto candidate = entries_.begin(); candidate != entries_.end();
             ++candidate) {
          if (candidate->second == entry || !candidate->second->finished ||
              !candidate->second->admitted)
            continue;
          if (oldest == entries_.end() ||
              candidate->second->last_used < oldest->second->last_used)
            oldest = candidate;
        }
        if (oldest == entries_.end()) {
          bypass = true;
          break;
        }
        cached_bytes_ -= oldest->second->read.bytes.size();
        entries_.erase(oldest);
        ++evictions;
      }
    }
    if (!bypass && indexed) {
      entry->admitted = true;
      cached_bytes_ += bytes;
    } else if (indexed) {
      entries_.erase(found);
    }
    const auto admitted = entry->admitted;
    const auto entries = admittedEntries();
    const auto cached_bytes = cached_bytes_;
    lock.unlock();
    entry->ready.notify_all();
    if (metrics)
      metrics->recordSpecializationComponentCacheState(
          entries, cached_bytes, evictions, cacheable && !admitted);
  }

private:
  std::size_t admittedEntries() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        entries_, [](const auto &value) { return value.second->admitted; }));
  }

  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
  std::uint64_t cached_bytes_ = 0;
  std::uint64_t sequence_ = 0;
};

bool atomicWrite(const std::string &path, const std::string &bytes,
                 std::string &error) {
  return CompilerArtifactWriteService::atomic(path, bytes, error);
}

std::string manifestPath(const std::filesystem::path &root,
                         const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::manifest(root, fingerprint);
}

std::string artifactObjectPath(const std::filesystem::path &root,
                               const compiler::StableFingerprint &fingerprint,
                               std::string_view extension) {
  return CompilerArtifactPathService::object(root, fingerprint, extension);
}

std::string
specializationComponentPath(const std::filesystem::path &root,
                            const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::specialization(root, fingerprint);
}

std::string specializationIndexPath(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &request_fingerprint) {
  return CompilerArtifactPathService::specializationIndex(root,
                                                          request_fingerprint);
}

std::string
typeSpecificPath(const std::filesystem::path &root,
                 const compiler::StableFingerprint &result_fingerprint) {
  return CompilerArtifactPathService::typeSpecific(root, result_fingerprint);
}

std::string
typeSpecificIndexPath(const std::filesystem::path &root,
                      const compiler::StableFingerprint &request_fingerprint) {
  return CompilerArtifactPathService::typeSpecificIndex(root,
                                                        request_fingerprint);
}

std::string nominalSemanticWitnessPath(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &result_fingerprint) {
  return CompilerArtifactPathService::nominalWitness(root, result_fingerprint);
}

std::string nominalSemanticWitnessIndexPath(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &request_fingerprint) {
  return CompilerArtifactPathService::nominalWitnessIndex(root,
                                                          request_fingerprint);
}

std::string
typeLayoutPath(const std::filesystem::path &root,
               const compiler::StableFingerprint &result_fingerprint) {
  return CompilerArtifactPathService::typeLayout(root, result_fingerprint);
}

std::string
typeLayoutIndexPath(const std::filesystem::path &root,
                    const compiler::StableFingerprint &request_fingerprint) {
  return CompilerArtifactPathService::typeLayoutIndex(root,
                                                      request_fingerprint);
}

std::string referencePath(const std::filesystem::path &root,
                          std::string_view session_key) {
  return CompilerArtifactPathService::reference(root, session_key);
}

std::string leasePath(const std::filesystem::path &root,
                      std::string_view lease_id) {
  return CompilerArtifactPathService::lease(root, lease_id);
}

class StoreLock {
public:
  StoreLock(const std::filesystem::path &root, std::string &error) {
    std::error_code file_error;
    std::filesystem::create_directories(root, file_error);
    if (file_error) {
      error =
          "failed to create compiler artifact store: " + file_error.message();
      return;
    }
    const auto path = root / ".lock";
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      error = "failed to open compiler artifact store lock";
      return;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlapped)) {
      error = "failed to lock compiler artifact store";
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
#else
    descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
      error = "failed to lock compiler artifact store";
      if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
      }
      return;
    }
#endif
    locked_ = true;
  }

  StoreLock(const StoreLock &) = delete;
  StoreLock &operator=(const StoreLock &) = delete;

  ~StoreLock() {
    if (!locked_)
      return;
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle_);
#else
    flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
#endif
  }

  [[nodiscard]] bool locked() const {
    return locked_;
  }

private:
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

class LeaseFileLock {
public:
  LeaseFileLock() = default;
  LeaseFileLock(const LeaseFileLock &) = delete;
  LeaseFileLock &operator=(const LeaseFileLock &) = delete;
  LeaseFileLock(LeaseFileLock &&other) noexcept {
    moveFrom(other);
  }
  LeaseFileLock &operator=(LeaseFileLock &&other) noexcept {
    if (this != &other) {
      close();
      moveFrom(other);
    }
    return *this;
  }
  ~LeaseFileLock() {
    close();
  }

  [[nodiscard]] bool acquireShared(const std::filesystem::path &path,
                                   std::string &error) {
    close();
#if defined(_WIN32)
    handle_ =
        CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      error = "failed to open compiler artifact lease";
      return false;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle_, 0, 0, MAXDWORD, MAXDWORD, &overlapped)) {
      error = "failed to lock compiler artifact lease";
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return false;
    }
#else
    descriptor_ = ::open(path.c_str(), O_RDONLY);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_SH) != 0) {
      error = "failed to lock compiler artifact lease";
      if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
      }
      return false;
    }
#endif
    locked_ = true;
    return true;
  }

  void close() {
    if (!locked_)
      return;
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
#else
    flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
    descriptor_ = -1;
#endif
    locked_ = false;
  }

  [[nodiscard]] bool locked() const {
    return locked_;
  }

private:
  void moveFrom(LeaseFileLock &other) {
    locked_ = std::exchange(other.locked_, false);
#if defined(_WIN32)
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
    descriptor_ = std::exchange(other.descriptor_, -1);
#endif
  }

  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

CompilerArtifactLeaseProbe probeLease(const std::filesystem::path &path,
                                      std::string &error) {
#if defined(_WIN32)
  const auto handle =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "failed to open compiler artifact lease during GC";
    return CompilerArtifactLeaseProbe::Error;
  }
  OVERLAPPED overlapped{};
  if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                  0, MAXDWORD, MAXDWORD, &overlapped)) {
    const auto code = GetLastError();
    CloseHandle(handle);
    if (code == ERROR_LOCK_VIOLATION)
      return CompilerArtifactLeaseProbe::Active;
    error = "failed to inspect compiler artifact lease lock";
    return CompilerArtifactLeaseProbe::Error;
  }
  UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
  CloseHandle(handle);
#else
  const auto descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    error = "failed to open compiler artifact lease during GC";
    return CompilerArtifactLeaseProbe::Error;
  }
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const auto code = errno;
    ::close(descriptor);
    if (code == EWOULDBLOCK || code == EAGAIN)
      return CompilerArtifactLeaseProbe::Active;
    error = "failed to inspect compiler artifact lease lock";
    return CompilerArtifactLeaseProbe::Error;
  }
  flock(descriptor, LOCK_UN);
  ::close(descriptor);
#endif
  return CompilerArtifactLeaseProbe::Stale;
}

bool loadCurrentReference(
    const std::filesystem::path &root, std::string_view session_key,
    std::optional<CompilerSessionArtifactReference> &reference,
    std::string &error) {
  reference.reset();
  const auto path = referencePath(root, session_key);
  std::error_code file_error;
  if (!std::filesystem::exists(pathForFileSystem(path), file_error)) {
    if (file_error) {
      error = "failed to inspect compiler session reference: " +
              file_error.message();
      return false;
    }
    return true;
  }
  const auto bytes = readTextFile(path, error);
  if (!bytes)
    return false;
  reference = CompilerSessionArtifactReference::decode(*bytes, error);
  return reference.has_value();
}

compiler::ObjectArtifactLoadResult
loadObjectFromStore(const std::filesystem::path &root,
                    const compiler::StableFingerprint &fingerprint,
                    const compiler::StableFingerprint &specific_fingerprint,
                    std::string_view target_triple, std::string_view extension,
                    CompilerArtifactLoadMetrics *metrics) {
  if (!fingerprint.hasValue())
    return {.status = compiler::ObjectArtifactLoadStatus::Error,
            .error = "cannot load an invalid compiler object artifact"};
  const auto path = artifactObjectPath(root, fingerprint, extension);
  auto read = readArtifactFile(path, metrics);
  if (read.status == CompilerArtifactReadStatus::Missing)
    return {};
  if (read.status == CompilerArtifactReadStatus::Error)
    return {.status = compiler::ObjectArtifactLoadStatus::Error,
            .error = std::move(read.error)};
  if (read.bytes.empty() ||
      compiler::fingerprintObject(target_triple, read.bytes,
                                  specific_fingerprint) != fingerprint)
    return {.status = compiler::ObjectArtifactLoadStatus::Corrupt};
  return {.status = compiler::ObjectArtifactLoadStatus::Found,
          .bytes = std::move(read.bytes)};
}

compiler::ConcreteSpecializationLoadResult loadSpecializationFromStore(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &request_fingerprint,
    SpecializationComponentCache &component_cache,
    CompilerArtifactLoadMetrics *metrics) {
  CompilerSpecializationClosureObservation observation;
  const auto finish = [&](compiler::ConcreteSpecializationLoadResult result) {
    using LoadStatus = compiler::ConcreteSpecializationLoadStatus;
    switch (result.status) {
    case LoadStatus::Found:
      observation.status =
          CompilerSpecializationClosureObservation::Status::Found;
      break;
    case LoadStatus::Missing:
      observation.status =
          CompilerSpecializationClosureObservation::Status::Missing;
      break;
    case LoadStatus::Corrupt:
      observation.status =
          CompilerSpecializationClosureObservation::Status::Corrupt;
      break;
    case LoadStatus::Error:
      observation.status =
          CompilerSpecializationClosureObservation::Status::Error;
      break;
    }
    if (metrics)
      metrics->recordSpecializationClosure(observation);
    return result;
  };
  const auto elapsed = [](std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  };
  const auto timed = [&](std::uint64_t &total, const auto &operation) {
    if (!metrics)
      return operation();
    const auto start = std::chrono::steady_clock::now();
    auto result = operation();
    total += elapsed(start);
    return result;
  };
  if (!request_fingerprint.hasValue())
    return finish({.status = compiler::ConcreteSpecializationLoadStatus::Error,
                   .error = "cannot load an invalid specialization request"});
  const auto index_path = specializationIndexPath(root, request_fingerprint);
  auto index_read = readArtifactFile(index_path, metrics);
  observation.read_nanoseconds += index_read.elapsed_nanoseconds;
  if (index_read.status == CompilerArtifactReadStatus::Missing)
    return finish({});
  if (index_read.status == CompilerArtifactReadStatus::Error)
    return finish({.status = compiler::ConcreteSpecializationLoadStatus::Error,
                   .error = std::move(index_read.error)});
  observation.index_bytes = index_read.bytes.size();
  const auto root_component = timed(observation.decode_nanoseconds, [&] {
    return CompilerArtifactCodecService::decodeSpecializationReference(
        index_read.bytes);
  });
  if (!root_component)
    return finish(
        {.status = compiler::ConcreteSpecializationLoadStatus::Corrupt});

  std::set<std::string> visiting;
  std::set<std::string> visited;
  std::map<std::string, std::uint64_t> critical_paths;
  std::vector<compiler::ConcreteSpecializationComponentArtifact> components;
  std::string component_io_error;
  const auto dfs_started = metrics ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
  const auto visit = [&](const auto &self,
                         const compiler::StableFingerprint &fingerprint,
                         std::uint64_t depth) -> bool {
    const auto hex = fingerprint.hex();
    observation.maximum_depth = std::max(observation.maximum_depth, depth);
    if (visited.contains(hex))
      return true;
    if (!visiting.insert(hex).second)
      return false;
    ++observation.unique_component_count;
    std::uint64_t local_work = 0;
    const auto path = specializationComponentPath(root, fingerprint);
    auto cache_lookup = component_cache.begin(hex, metrics);
    std::string component_error;
    auto leader_read = cache_lookup.leader ? readArtifactFile(path, metrics)
                                           : ArtifactReadResult{};
    const auto &read =
        cache_lookup.leader ? leader_read : cache_lookup.entry->read;
    const auto read_nanoseconds =
        cache_lookup.leader ? read.elapsed_nanoseconds : 0;
    observation.read_nanoseconds += read_nanoseconds;
    local_work += read_nanoseconds;
    if (read.status == CompilerArtifactReadStatus::Error) {
      component_io_error = read.error;
      if (cache_lookup.leader)
        component_cache.complete(hex, cache_lookup.entry,
                                 std::move(leader_read), false, metrics);
      return false;
    }
    if (read.status == CompilerArtifactReadStatus::Missing) {
      if (cache_lookup.leader)
        component_cache.complete(hex, cache_lookup.entry,
                                 std::move(leader_read), false, metrics);
      return false;
    }
    if (cache_lookup.duplicate_request)
      metrics->recordSpecializationComponentDuplicateBytes(read.bytes.size());
    observation.component_bytes += read.bytes.size();
    const auto decode_started = metrics
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    auto component = compiler::ConcreteSpecializationComponentArtifact::decode(
        read.bytes, component_error);
    if (metrics) {
      const auto duration = elapsed(decode_started);
      observation.decode_nanoseconds += duration;
      local_work += duration;
    }
    if (!component) {
      if (cache_lookup.leader)
        component_cache.complete(hex, cache_lookup.entry,
                                 std::move(leader_read), false, metrics);
      return false;
    }
    const auto verify_started = metrics
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    const auto fingerprint_matches = component->fingerprint() == fingerprint;
    if (metrics) {
      const auto duration = elapsed(verify_started);
      observation.verify_nanoseconds += duration;
      local_work += duration;
    }
    if (cache_lookup.leader)
      component_cache.complete(hex, cache_lookup.entry, std::move(leader_read),
                               fingerprint_matches, metrics);
    if (!fingerprint_matches)
      return false;
    const auto dependencies = component->dependencies();
    observation.edge_count += dependencies.size();
    std::uint64_t dependency_critical_path = 0;
    for (const auto &dependency : dependencies) {
      if (!self(self, dependency, depth + 1))
        return false;
      const auto found = critical_paths.find(dependency.hex());
      if (found != critical_paths.end())
        dependency_critical_path =
            std::max(dependency_critical_path, found->second);
    }
    visiting.erase(hex);
    visited.insert(hex);
    observation.component_work_nanoseconds += local_work;
    critical_paths[hex] = local_work + dependency_critical_path;
    components.push_back(std::move(*component));
    return true;
  };
  const auto visited_closure = visit(visit, *root_component, 1);
  if (metrics)
    observation.dfs_nanoseconds = elapsed(dfs_started);
  observation.component_count = components.size();
  if (const auto root_path = critical_paths.find(root_component->hex());
      root_path != critical_paths.end())
    observation.critical_path_nanoseconds = root_path->second;
  if (!visited_closure)
    return finish(
        component_io_error.empty()
            ? compiler::
                  ConcreteSpecializationLoadResult{.status = compiler::
                                                       ConcreteSpecializationLoadStatus::
                                                           Corrupt}
            : compiler::ConcreteSpecializationLoadResult{
                  .status = compiler::ConcreteSpecializationLoadStatus::Error,
                  .error = std::move(component_io_error)});
  if (components.empty() || !components.back().findNode(request_fingerprint))
    return finish(
        {.status = compiler::ConcreteSpecializationLoadStatus::Corrupt});
  return finish({.status = compiler::ConcreteSpecializationLoadStatus::Found,
                 .components = std::move(components)});
}

} // namespace

struct CompilerArtifactLease::Impl {
  Impl(std::filesystem::path store_root, std::string session,
       std::string target, std::string root_package,
       std::optional<CompilerSessionArtifactReference> reference,
       std::map<std::string, compiler::CompilerPackageArtifactManifest>
           manifests,
       std::string path, LeaseFileLock lock)
      : store_root(std::move(store_root)), session_key(std::move(session)),
        expected_target(std::move(target)),
        expected_root(std::move(root_package)),
        observed_reference(std::move(reference)),
        previous_manifests(std::move(manifests)), lease_path(std::move(path)),
        lease_lock(std::move(lock)) {}

  ~Impl() {
    release();
  }

  void retireUnderStoreLock() {
    if (!active)
      return;
    lease_lock.close();
    if (!lease_path.empty()) {
      std::error_code file_error;
      // An unlocked record is recoverable: GC will classify it as stale.
      removeFile(lease_path, file_error);
      lease_path.clear();
    }
    active = false;
  }

  void release() {
    if (!active)
      return;
    if (lease_path.empty()) {
      active = false;
      return;
    }
    std::string ignored;
    StoreLock store_lock(store_root, ignored);
    if (store_lock.locked())
      retireUnderStoreLock();
  }

  std::filesystem::path store_root;
  std::string session_key;
  std::string expected_target;
  std::string expected_root;
  std::optional<CompilerSessionArtifactReference> observed_reference;
  std::map<std::string, compiler::CompilerPackageArtifactManifest>
      previous_manifests;
  SpecializationComponentCache specialization_component_cache;
  std::string lease_path;
  LeaseFileLock lease_lock;
  bool active = true;
};

CompilerArtifactLease::CompilerArtifactLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CompilerArtifactLease::CompilerArtifactLease(
    CompilerArtifactLease &&) noexcept = default;
CompilerArtifactLease &
CompilerArtifactLease::operator=(CompilerArtifactLease &&) noexcept = default;
CompilerArtifactLease::~CompilerArtifactLease() = default;

bool CompilerArtifactLease::valid() const {
  return impl_ && impl_->active &&
         (!impl_->observed_reference || impl_->lease_lock.locked());
}

const std::map<std::string, compiler::CompilerPackageArtifactManifest> &
CompilerArtifactLease::previousManifests() const {
  if (!valid())
    throw std::logic_error("invalid compiler artifact lease");
  return impl_->previous_manifests;
}

compiler::ObjectArtifactLoadResult CompilerArtifactLease::loadObject(
    const compiler::StableFingerprint &fingerprint,
    const compiler::StableFingerprint &specific_fingerprint,
    std::string_view target_triple, std::string_view extension,
    CompilerArtifactLoadMetrics *metrics) const {
  if (!valid())
    return {.status = compiler::ObjectArtifactLoadStatus::Error,
            .error = "cannot load an object through an invalid compiler lease"};
  return loadObjectFromStore(impl_->store_root, fingerprint,
                             specific_fingerprint, target_triple, extension,
                             metrics);
}

compiler::ConcreteSpecializationLoadResult
CompilerArtifactLease::loadSpecialization(
    const compiler::StableFingerprint &request_fingerprint,
    CompilerArtifactLoadMetrics *metrics) const {
  if (!valid())
    return {
        .status = compiler::ConcreteSpecializationLoadStatus::Error,
        .error =
            "cannot load a specialization through an invalid compiler lease"};
  return loadSpecializationFromStore(impl_->store_root, request_fingerprint,
                                     impl_->specialization_component_cache,
                                     metrics);
}

std::optional<compiler::NominalTypeSpecificArtifact>
CompilerArtifactLease::loadNominalTypeSpecific(
    const compiler::StableFingerprint &request_fingerprint,
    std::string &error) const {
  error.clear();
  if (!valid() || !request_fingerprint.hasValue()) {
    error = "cannot load a nominal type specific through an invalid lease";
    return std::nullopt;
  }
  const auto index = readTextFile(
      typeSpecificIndexPath(impl_->store_root, request_fingerprint), error);
  if (!index)
    return std::nullopt;
  const auto result = CompilerArtifactCodecService::decodeNominalReference(
      *index, CompilerArtifactCodecService::NominalReferenceKind::TypeSpecific);
  if (!result) {
    error = "nominal type specific index is corrupt";
    return std::nullopt;
  }
  const auto bytes =
      readTextFile(typeSpecificPath(impl_->store_root, *result), error);
  auto artifact =
      bytes ? compiler::NominalTypeSpecificArtifact::decode(*bytes, error)
            : std::nullopt;
  if (!artifact || artifact->request_fingerprint != request_fingerprint ||
      artifact->result_fingerprint != *result) {
    if (error.empty())
      error = "nominal type specific CAS entry is corrupt";
    return std::nullopt;
  }
  return artifact;
}

std::optional<compiler::NominalSemanticWitnessArtifact>
CompilerArtifactLease::loadNominalSemanticWitness(
    const compiler::StableFingerprint &request_fingerprint,
    std::string &error) const {
  error.clear();
  if (!valid() || !request_fingerprint.hasValue()) {
    error = "cannot load a nominal semantic witness through an invalid lease";
    return std::nullopt;
  }
  const auto index = readTextFile(
      nominalSemanticWitnessIndexPath(impl_->store_root, request_fingerprint),
      error);
  if (!index)
    return std::nullopt;
  const auto result = CompilerArtifactCodecService::decodeNominalReference(
      *index,
      CompilerArtifactCodecService::NominalReferenceKind::SemanticWitness);
  if (!result) {
    error = "nominal semantic witness index is corrupt";
    return std::nullopt;
  }
  const auto bytes = readTextFile(
      nominalSemanticWitnessPath(impl_->store_root, *result), error);
  auto artifact =
      bytes ? compiler::NominalSemanticWitnessArtifact::decode(*bytes, error)
            : std::nullopt;
  if (!artifact || artifact->request_fingerprint != request_fingerprint ||
      artifact->result_fingerprint != *result) {
    if (error.empty())
      error = "nominal semantic witness CAS entry is corrupt";
    return std::nullopt;
  }
  return artifact;
}

std::optional<compiler::NominalTypeLayoutArtifact>
CompilerArtifactLease::loadNominalTypeLayout(
    const compiler::StableFingerprint &request_fingerprint,
    std::string &error) const {
  error.clear();
  if (!valid() || !request_fingerprint.hasValue()) {
    error = "cannot load a nominal type layout through an invalid lease";
    return std::nullopt;
  }
  const auto index = readTextFile(
      typeLayoutIndexPath(impl_->store_root, request_fingerprint), error);
  if (!index)
    return std::nullopt;
  const auto result = CompilerArtifactCodecService::decodeNominalReference(
      *index, CompilerArtifactCodecService::NominalReferenceKind::TypeLayout);
  if (!result) {
    error = "nominal type layout index is corrupt";
    return std::nullopt;
  }
  const auto bytes =
      readTextFile(typeLayoutPath(impl_->store_root, *result), error);
  auto artifact =
      bytes ? compiler::NominalTypeLayoutArtifact::decode(*bytes, error)
            : std::nullopt;
  if (!artifact || artifact->request_fingerprint != request_fingerprint ||
      artifact->result_fingerprint != *result) {
    if (error.empty())
      error = "nominal type layout CAS entry is corrupt";
    return std::nullopt;
  }
  return artifact;
}

CompilerArtifactStore::CompilerArtifactStore(std::string root)
    : root_(pathForFileSystemTreeRoot(root).string()) {}

std::unique_ptr<CompilerArtifactLease> CompilerArtifactStore::acquireLease(
    std::string_view session_key, std::string_view expected_target,
    std::string_view expected_root, std::string &error) const {
  error.clear();
  if (!CompilerArtifactCodecService::validHexKey(session_key) ||
      expected_target.empty() || expected_root.empty() ||
      CompilerArtifactCodecService::hasInvalidFieldCharacter(expected_target) ||
      CompilerArtifactCodecService::hasInvalidFieldCharacter(expected_root)) {
    error = "cannot acquire an invalid compiler artifact lease";
    return nullptr;
  }

  const auto root = pathForFileSystemTreeRoot(root_);
  StoreLock store_lock(root, error);
  if (!store_lock.locked())
    return nullptr;
  CompilerArtifactAcquireState state{
      root, [&root](std::string_view key,
                    std::optional<CompilerSessionArtifactReference> &reference,
                    std::string &load_error) {
        return loadCurrentReference(root, key, reference, load_error);
      }};
  auto plan = CompilerArtifactAcquireService::prepare(
      session_key, expected_target, expected_root, error, state);
  if (!plan)
    return nullptr;

  LeaseFileLock lease_lock;
  if (!plan->lease_path.empty() &&
      !lease_lock.acquireShared(pathForFileSystem(plan->lease_path), error)) {
    std::error_code file_error;
    removeFile(plan->lease_path, file_error);
    return nullptr;
  }
  auto impl = std::make_unique<CompilerArtifactLease::Impl>(
      root, std::string(session_key), std::string(expected_target),
      std::string(expected_root), std::move(plan->reference),
      std::move(plan->manifests), std::move(plan->lease_path),
      std::move(lease_lock));
  return std::unique_ptr<CompilerArtifactLease>(
      new CompilerArtifactLease(std::move(impl)));
}
bool CompilerArtifactStore::publish(
    CompilerArtifactLease &lease,
    const compiler::CompilerPackageArtifactManifest &root_manifest,
    std::span<const compiler::CompilerPackageArtifactManifest *const> manifests,
    std::span<const CompilerPublishedObject> objects,
    std::span<const CompilerPublishedSpecialization> specializations,
    std::span<const CompilerPublishedNominalTypeSpecific> nominal_specifics,
    std::span<const CompilerPublishedNominalSemanticWitness>
        nominal_semantic_witnesses,
    std::span<const CompilerPublishedNominalTypeLayout> nominal_layouts,
    std::string &error) const {
  error.clear();
  const auto root = pathForFileSystemTreeRoot(root_);
  if (!lease.valid() || lease.impl_->store_root != root ||
      root_manifest.packageName() != lease.impl_->expected_root ||
      root_manifest.targetTriple() != lease.impl_->expected_target ||
      !root_manifest.verify(error)) {
    if (error.empty())
      error = "cannot publish through an invalid compiler artifact lease";
    return false;
  }
  CompilerArtifactPublishState state{
      root,
      lease.impl_->session_key,
      lease.impl_->expected_target,
      lease.impl_->observed_reference,
      [&root](const compiler::StableFingerprint &fingerprint,
              const compiler::StableFingerprint &specific_fingerprint,
              std::string_view target_triple, std::string_view extension) {
        return loadObjectFromStore(root, fingerprint, specific_fingerprint,
                                   target_triple, extension, nullptr);
      },
      [&root](std::string_view session_key,
              std::optional<CompilerSessionArtifactReference> &reference,
              std::string &load_error) {
        return loadCurrentReference(root, session_key, reference, load_error);
      },
      [&root](const std::function<bool()> &operation, std::string &lock_error) {
        StoreLock store_lock(root, lock_error);
        return store_lock.locked() && operation();
      },
      [&lease] { lease.impl_->retireUnderStoreLock(); }};
  return CompilerArtifactPublishService::publish(
      root_manifest, manifests, objects, specializations, nominal_specifics,
      nominal_semantic_witnesses, nominal_layouts, error, state);
}
bool CompilerArtifactStore::collectGarbage(
    bool force, std::chrono::seconds minimum_interval,
    CompilerGarbageCollectionReport &report, std::string &error) const {
  report = {};
  error.clear();
  const auto root = pathForFileSystemTreeRoot(root_);
  StoreLock store_lock(root, error);
  if (!store_lock.locked())
    return false;
  CompilerArtifactGCState state{root, probeLease};
  return CompilerArtifactGCService::collect(force, minimum_interval, report,
                                            error, state);
}
std::string CompilerArtifactStore::objectPath(
    const compiler::StableFingerprint &fingerprint,
    std::string_view extension) const {
  return fingerprint.hasValue()
             ? artifactObjectPath(pathForFileSystemTreeRoot(root_), fingerprint,
                                  extension)
             : std::string{};
}

} // namespace chtholly
