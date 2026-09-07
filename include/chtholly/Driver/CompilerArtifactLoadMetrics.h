#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace chtholly {

struct CompilerGarbageCollectionReport;

enum class CompilerArtifactLoadFamily {
  Object,
  NominalWitness,
  Specialization,
};

struct CompilerSpecializationClosureObservation {
  enum class Status { Found, Missing, Corrupt, Error };

  Status status = Status::Missing;
  std::uint64_t component_count = 0;
  std::uint64_t unique_component_count = 0;
  std::uint64_t edge_count = 0;
  std::uint64_t maximum_depth = 0;
  std::uint64_t index_bytes = 0;
  std::uint64_t component_bytes = 0;
  std::uint64_t exists_nanoseconds = 0;
  std::uint64_t read_nanoseconds = 0;
  std::uint64_t decode_nanoseconds = 0;
  std::uint64_t verify_nanoseconds = 0;
  std::uint64_t dfs_nanoseconds = 0;
  std::uint64_t component_work_nanoseconds = 0;
  std::uint64_t critical_path_nanoseconds = 0;
};

enum class CompilerArtifactReadStatus { Found, Missing, Error };
enum class CompilerSpecializationComponentCacheLookup { Hit, Miss, Coalesced };

class CompilerArtifactLoadMetrics {
public:
  CompilerArtifactLoadMetrics(std::size_t jobs, std::size_t worker_count,
                          std::size_t queue_capacity);
  CompilerArtifactLoadMetrics(const CompilerArtifactLoadMetrics &) = delete;
  CompilerArtifactLoadMetrics &operator=(const CompilerArtifactLoadMetrics &) = delete;
  ~CompilerArtifactLoadMetrics();

  void recordSubmitted(CompilerArtifactLoadFamily family);
  void recordStarted(CompilerArtifactLoadFamily family,
                     std::uint64_t queue_wait_nanoseconds,
                     std::size_t active_workers);
  void recordCompleted(CompilerArtifactLoadFamily family,
                       std::uint64_t execution_nanoseconds);
  void recordCancelled(CompilerArtifactLoadFamily family,
                       std::uint64_t execution_nanoseconds = 0);
  void recordQueueDepth(std::size_t depth);
  void recordBackpressure(std::uint64_t wait_nanoseconds);
  void recordConsumerWait(CompilerArtifactLoadFamily family,
                          std::uint64_t wait_nanoseconds);
  void recordSpecializationDedup(bool completed_result);
  void recordSpecializationClosure(
      const CompilerSpecializationClosureObservation &observation);
  void recordArtifactRead(CompilerArtifactReadStatus status, std::uint64_t bytes,
                          std::uint64_t read_nanoseconds);
  [[nodiscard]] bool
  recordSpecializationComponentRequest(std::string_view fingerprint);
  void recordSpecializationComponentDuplicateBytes(std::uint64_t bytes);
  void recordSpecializationComponentCacheLookup(
      CompilerSpecializationComponentCacheLookup lookup,
      bool duplicate_disk_read = false);
  void recordSpecializationComponentCacheState(std::size_t entries,
                                               std::uint64_t bytes,
                                               std::uint64_t evictions,
                                               bool bypass);
  void recordArchiveInstallSummary(std::uint64_t attempts,
                                   std::uint64_t closure_hits,
                                   std::uint64_t fresh_installs,
                                   std::uint64_t archive_bytes);
  // Attach observational artifact-store/GC counters.  This appends an
  // ``artifact-store`` object to the existing metrics JSON without changing
  // any of the historical fields.
  void recordArtifactStoreReport(const CompilerGarbageCollectionReport &report);
  void configurePackageScheduling(std::size_t package_count,
                                  std::size_t worker_count);
  void recordPackageQueryStarted(std::uint64_t worker_wait_nanoseconds,
                                 std::size_t active_workers);
  void recordPackageQueryCompleted(std::size_t package_index,
                                   std::span<const std::size_t> dependencies,
                                   std::uint64_t execution_nanoseconds,
                                   bool failed);
  void recordPackageSchedulingCompleted(std::uint64_t wall_nanoseconds);

  [[nodiscard]] std::string json() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly
