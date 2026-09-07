#include "chtholly/Driver/CompilerArtifactLoadMetrics.h"
#include "chtholly/Driver/CompilerArtifactStore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace chtholly {
namespace {

constexpr std::size_t familyIndex(CompilerArtifactLoadFamily family) {
  return static_cast<std::size_t>(family);
}

struct FamilyCounters {
  std::uint64_t submitted = 0;
  std::uint64_t started = 0;
  std::uint64_t completed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t queue_wait_nanoseconds = 0;
  std::uint64_t execution_nanoseconds = 0;
  std::uint64_t consumer_wait_nanoseconds = 0;
};

void writeFamily(std::ostringstream &out, std::string_view name,
                 const FamilyCounters &family) {
  out << "    \"" << name << "\": {\n"
      << "      \"submitted\": " << family.submitted << ",\n"
      << "      \"started\": " << family.started << ",\n"
      << "      \"completed\": " << family.completed << ",\n"
      << "      \"cancelled\": " << family.cancelled << ",\n"
      << "      \"queue-wait-nanoseconds\": "
      << family.queue_wait_nanoseconds << ",\n"
      << "      \"execution-nanoseconds\": "
      << family.execution_nanoseconds << ",\n"
      << "      \"consumer-wait-nanoseconds\": "
      << family.consumer_wait_nanoseconds << "\n"
      << "    }";
}

struct ArtifactStoreFamilyCounters {
  std::uintmax_t total_bytes = 0;
  std::uintmax_t reachable_bytes = 0;
  std::uintmax_t unreachable_bytes = 0;
};

void writeArtifactStoreFamily(std::ostringstream &out, std::string_view name,
                              const ArtifactStoreFamilyCounters &family) {
  out << "      \"" << name << "\": {\n"
      << "        \"total-bytes\": " << family.total_bytes << ",\n"
      << "        \"reachable-bytes\": " << family.reachable_bytes << ",\n"
      << "        \"unreachable-bytes\": " << family.unreachable_bytes
      << "\n"
      << "      }";
}

void writeJsonString(std::ostringstream &out, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  out << '"';
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (character < 0x20)
        out << "\\u00" << hex[(character >> 4) & 0xf] << hex[character & 0xf];
      else
        out << static_cast<char>(character);
      break;
    }
  }
  out << '"';
}

} // namespace

struct CompilerArtifactLoadMetrics::Impl {
  Impl(std::size_t requested_jobs, std::size_t workers,
       std::size_t queue_capacity)
      : jobs(requested_jobs), worker_count(workers),
        queue_capacity(queue_capacity), created_at(std::chrono::steady_clock::now()) {}

  const std::size_t jobs;
  const std::size_t worker_count;
  const std::size_t queue_capacity;
  const std::chrono::steady_clock::time_point created_at;
  mutable std::mutex mutex;
  std::array<FamilyCounters, 3> families;
  std::uint64_t submitted = 0;
  std::uint64_t started = 0;
  std::uint64_t completed = 0;
  std::uint64_t cancelled = 0;
  std::size_t queue_high_water = 0;
  std::size_t active_high_water = 0;
  std::uint64_t backpressure_wait_count = 0;
  std::uint64_t backpressure_wait_nanoseconds = 0;
  std::uint64_t specialization_inflight_hits = 0;
  std::uint64_t specialization_completed_hits = 0;
  std::uint64_t artifact_read_attempts = 0;
  std::uint64_t artifact_read_found = 0;
  std::uint64_t artifact_read_missing = 0;
  std::uint64_t artifact_read_errors = 0;
  std::uint64_t artifact_read_bytes = 0;
  std::uint64_t artifact_read_nanoseconds = 0;
  std::uint64_t archive_install_attempts = 0;
  std::uint64_t archive_install_closure_hits = 0;
  std::uint64_t archive_install_fresh_installs = 0;
  std::uint64_t archive_install_bytes = 0;
  std::uint64_t specialization_component_requests = 0;
  std::uint64_t specialization_component_duplicate_requests = 0;
  std::uint64_t specialization_component_duplicate_bytes = 0;
  std::unordered_set<std::string> specialization_component_fingerprints;
  std::uint64_t specialization_component_cache_hits = 0;
  std::uint64_t specialization_component_cache_misses = 0;
  std::uint64_t specialization_component_cache_coalesced = 0;
  std::uint64_t specialization_component_cache_evictions = 0;
  std::uint64_t specialization_component_cache_bypasses = 0;
  std::uint64_t specialization_component_duplicate_disk_reads = 0;
  std::size_t specialization_component_cache_entry_high_water = 0;
  std::uint64_t specialization_component_cache_byte_high_water = 0;
  std::optional<std::chrono::steady_clock::time_point> first_artifact_submit;
  std::optional<std::chrono::steady_clock::time_point> last_artifact_terminal;
  std::uint64_t closure_requests = 0;
  std::uint64_t closure_found = 0;
  std::uint64_t closure_missing = 0;
  std::uint64_t closure_corrupt = 0;
  std::uint64_t closure_error = 0;
  CompilerSpecializationClosureObservation closure_totals;
  std::size_t package_count = 0;
  std::size_t package_worker_count = 0;
  std::uint64_t package_started = 0;
  std::uint64_t package_completed = 0;
  std::uint64_t package_failed = 0;
  std::size_t package_active_high_water = 0;
  std::uint64_t package_worker_wait_nanoseconds = 0;
  std::uint64_t package_execution_nanoseconds = 0;
  std::uint64_t package_wall_nanoseconds = 0;
  std::vector<std::uint64_t> package_critical_paths;
  bool artifact_store_recorded = false;
  bool artifact_store_valid = true;
  std::string artifact_store_recovery_instruction;
  std::array<ArtifactStoreFamilyCounters, 10> artifact_store_families;
  std::uint64_t artifact_store_active_lease_count = 0;
  std::uint64_t artifact_store_stale_lease_count = 0;
  std::uintmax_t artifact_store_quarantine_bytes = 0;
  std::uintmax_t artifact_store_reclaimed_bytes = 0;
};

CompilerArtifactLoadMetrics::CompilerArtifactLoadMetrics(std::size_t jobs,
                                                 std::size_t worker_count,
                                                 std::size_t queue_capacity)
    : impl_(std::make_unique<Impl>(jobs, worker_count, queue_capacity)) {}

CompilerArtifactLoadMetrics::~CompilerArtifactLoadMetrics() = default;

void CompilerArtifactLoadMetrics::recordArchiveInstallSummary(
    std::uint64_t attempts, std::uint64_t closure_hits,
    std::uint64_t fresh_installs, std::uint64_t archive_bytes) {
  std::lock_guard lock(impl_->mutex);
  impl_->archive_install_attempts += attempts;
  impl_->archive_install_closure_hits += closure_hits;
  impl_->archive_install_fresh_installs += fresh_installs;
  impl_->archive_install_bytes += archive_bytes;
}

void CompilerArtifactLoadMetrics::recordArtifactStoreReport(
    const CompilerGarbageCollectionReport &report) {
  std::lock_guard lock(impl_->mutex);
  impl_->artifact_store_recorded = true;
  impl_->artifact_store_valid = report.valid;
  impl_->artifact_store_recovery_instruction = report.recovery_instruction;
  impl_->artifact_store_active_lease_count = report.active_lease_count;
  impl_->artifact_store_stale_lease_count = report.stale_lease_count;
  impl_->artifact_store_quarantine_bytes = report.quarantine_bytes;
  impl_->artifact_store_reclaimed_bytes = report.reclaimed_bytes;
  auto &families = impl_->artifact_store_families;
  families[0] = {report.manifest_total_bytes,
                 report.manifest_reachable_bytes,
                 report.manifest_unreachable_bytes};
  families[1] = {report.object_total_bytes, report.object_reachable_bytes,
                 report.object_unreachable_bytes};
  families[2] = {report.specialization_component_total_bytes,
                 report.specialization_component_reachable_bytes,
                 report.specialization_component_unreachable_bytes};
  families[3] = {report.specialization_index_total_bytes,
                 report.specialization_index_reachable_bytes,
                 report.specialization_index_unreachable_bytes};
  families[4] = {report.nominal_type_specific_total_bytes,
                 report.nominal_type_specific_reachable_bytes,
                 report.nominal_type_specific_unreachable_bytes};
  families[5] = {report.nominal_type_specific_index_total_bytes,
                 report.nominal_type_specific_index_reachable_bytes,
                 report.nominal_type_specific_index_unreachable_bytes};
  families[6] = {report.nominal_semantic_witness_total_bytes,
                 report.nominal_semantic_witness_reachable_bytes,
                 report.nominal_semantic_witness_unreachable_bytes};
  families[7] = {report.nominal_semantic_witness_index_total_bytes,
                 report.nominal_semantic_witness_index_reachable_bytes,
                 report.nominal_semantic_witness_index_unreachable_bytes};
  families[8] = {report.nominal_type_layout_total_bytes,
                 report.nominal_type_layout_reachable_bytes,
                 report.nominal_type_layout_unreachable_bytes};
  families[9] = {report.nominal_type_layout_index_total_bytes,
                 report.nominal_type_layout_index_reachable_bytes,
                 report.nominal_type_layout_index_unreachable_bytes};
}

void CompilerArtifactLoadMetrics::recordSubmitted(CompilerArtifactLoadFamily family) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->first_artifact_submit)
    impl_->first_artifact_submit = std::chrono::steady_clock::now();
  ++impl_->submitted;
  ++impl_->families[familyIndex(family)].submitted;
}

void CompilerArtifactLoadMetrics::recordStarted(CompilerArtifactLoadFamily family,
                                            std::uint64_t queue_wait,
                                            std::size_t active_workers) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->started;
  impl_->active_high_water =
      std::max(impl_->active_high_water, active_workers);
  auto &counters = impl_->families[familyIndex(family)];
  ++counters.started;
  counters.queue_wait_nanoseconds += queue_wait;
}

void CompilerArtifactLoadMetrics::recordCompleted(CompilerArtifactLoadFamily family,
                                              std::uint64_t execution) {
  std::lock_guard lock(impl_->mutex);
  impl_->last_artifact_terminal = std::chrono::steady_clock::now();
  ++impl_->completed;
  auto &counters = impl_->families[familyIndex(family)];
  ++counters.completed;
  counters.execution_nanoseconds += execution;
}

void CompilerArtifactLoadMetrics::recordCancelled(CompilerArtifactLoadFamily family,
                                              std::uint64_t execution) {
  std::lock_guard lock(impl_->mutex);
  impl_->last_artifact_terminal = std::chrono::steady_clock::now();
  ++impl_->cancelled;
  auto &counters = impl_->families[familyIndex(family)];
  ++counters.cancelled;
  counters.execution_nanoseconds += execution;
}

void CompilerArtifactLoadMetrics::recordQueueDepth(std::size_t depth) {
  std::lock_guard lock(impl_->mutex);
  impl_->queue_high_water = std::max(impl_->queue_high_water, depth);
}

void CompilerArtifactLoadMetrics::recordBackpressure(std::uint64_t wait) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->backpressure_wait_count;
  impl_->backpressure_wait_nanoseconds += wait;
}

void CompilerArtifactLoadMetrics::recordConsumerWait(
    CompilerArtifactLoadFamily family, std::uint64_t wait) {
  std::lock_guard lock(impl_->mutex);
  impl_->families[familyIndex(family)].consumer_wait_nanoseconds += wait;
}

void CompilerArtifactLoadMetrics::recordSpecializationDedup(bool completed) {
  std::lock_guard lock(impl_->mutex);
  if (completed)
    ++impl_->specialization_completed_hits;
  else
    ++impl_->specialization_inflight_hits;
}

void CompilerArtifactLoadMetrics::recordSpecializationClosure(
    const CompilerSpecializationClosureObservation &value) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->closure_requests;
  switch (value.status) {
  case CompilerSpecializationClosureObservation::Status::Found:
    ++impl_->closure_found;
    break;
  case CompilerSpecializationClosureObservation::Status::Missing:
    ++impl_->closure_missing;
    break;
  case CompilerSpecializationClosureObservation::Status::Corrupt:
    ++impl_->closure_corrupt;
    break;
  case CompilerSpecializationClosureObservation::Status::Error:
    ++impl_->closure_error;
    break;
  }
  auto &total = impl_->closure_totals;
  total.component_count += value.component_count;
  total.unique_component_count += value.unique_component_count;
  total.edge_count += value.edge_count;
  total.maximum_depth = std::max(total.maximum_depth, value.maximum_depth);
  total.index_bytes += value.index_bytes;
  total.component_bytes += value.component_bytes;
  total.exists_nanoseconds += value.exists_nanoseconds;
  total.read_nanoseconds += value.read_nanoseconds;
  total.decode_nanoseconds += value.decode_nanoseconds;
  total.verify_nanoseconds += value.verify_nanoseconds;
  total.dfs_nanoseconds += value.dfs_nanoseconds;
  total.component_work_nanoseconds += value.component_work_nanoseconds;
  total.critical_path_nanoseconds += value.critical_path_nanoseconds;
}

void CompilerArtifactLoadMetrics::recordArtifactRead(CompilerArtifactReadStatus status,
                                                 std::uint64_t bytes,
                                                 std::uint64_t read_nanoseconds) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->artifact_read_attempts;
  switch (status) {
  case CompilerArtifactReadStatus::Found:
    ++impl_->artifact_read_found;
    break;
  case CompilerArtifactReadStatus::Missing:
    ++impl_->artifact_read_missing;
    break;
  case CompilerArtifactReadStatus::Error:
    ++impl_->artifact_read_errors;
    break;
  }
  impl_->artifact_read_bytes += bytes;
  impl_->artifact_read_nanoseconds += read_nanoseconds;
}

bool CompilerArtifactLoadMetrics::recordSpecializationComponentRequest(
    std::string_view fingerprint) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->specialization_component_requests;
  const auto inserted =
      impl_->specialization_component_fingerprints.emplace(fingerprint).second;
  if (!inserted)
    ++impl_->specialization_component_duplicate_requests;
  return !inserted;
}

void CompilerArtifactLoadMetrics::recordSpecializationComponentDuplicateBytes(
    std::uint64_t bytes) {
  std::lock_guard lock(impl_->mutex);
  impl_->specialization_component_duplicate_bytes += bytes;
}

void CompilerArtifactLoadMetrics::recordSpecializationComponentCacheLookup(
    CompilerSpecializationComponentCacheLookup lookup, bool duplicate_disk_read) {
  std::lock_guard lock(impl_->mutex);
  switch (lookup) {
  case CompilerSpecializationComponentCacheLookup::Hit:
    ++impl_->specialization_component_cache_hits;
    break;
  case CompilerSpecializationComponentCacheLookup::Miss:
    ++impl_->specialization_component_cache_misses;
    break;
  case CompilerSpecializationComponentCacheLookup::Coalesced:
    ++impl_->specialization_component_cache_coalesced;
    break;
  }
  if (duplicate_disk_read)
    ++impl_->specialization_component_duplicate_disk_reads;
}

void CompilerArtifactLoadMetrics::recordSpecializationComponentCacheState(
    std::size_t entries, std::uint64_t bytes, std::uint64_t evictions,
    bool bypass) {
  std::lock_guard lock(impl_->mutex);
  impl_->specialization_component_cache_evictions += evictions;
  if (bypass)
    ++impl_->specialization_component_cache_bypasses;
  impl_->specialization_component_cache_entry_high_water =
      std::max(impl_->specialization_component_cache_entry_high_water, entries);
  impl_->specialization_component_cache_byte_high_water =
      std::max(impl_->specialization_component_cache_byte_high_water, bytes);
}

void CompilerArtifactLoadMetrics::configurePackageScheduling(
    std::size_t package_count, std::size_t worker_count) {
  std::lock_guard lock(impl_->mutex);
  impl_->package_count = package_count;
  impl_->package_worker_count = worker_count;
  impl_->package_critical_paths.assign(package_count, 0);
}

void CompilerArtifactLoadMetrics::recordPackageQueryStarted(
    std::uint64_t worker_wait_nanoseconds, std::size_t active_workers) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->package_started;
  impl_->package_worker_wait_nanoseconds += worker_wait_nanoseconds;
  impl_->package_active_high_water =
      std::max(impl_->package_active_high_water, active_workers);
}

void CompilerArtifactLoadMetrics::recordPackageQueryCompleted(
    std::size_t package_index, std::span<const std::size_t> dependencies,
    std::uint64_t execution_nanoseconds, bool failed) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->package_completed;
  if (failed)
    ++impl_->package_failed;
  impl_->package_execution_nanoseconds += execution_nanoseconds;
  std::uint64_t dependency_path = 0;
  for (const auto dependency : dependencies)
    if (dependency < impl_->package_critical_paths.size())
      dependency_path =
          std::max(dependency_path, impl_->package_critical_paths[dependency]);
  if (package_index < impl_->package_critical_paths.size())
    impl_->package_critical_paths[package_index] =
        dependency_path + execution_nanoseconds;
}

void CompilerArtifactLoadMetrics::recordPackageSchedulingCompleted(
    std::uint64_t wall_nanoseconds) {
  std::lock_guard lock(impl_->mutex);
  impl_->package_wall_nanoseconds = wall_nanoseconds;
}

std::string CompilerArtifactLoadMetrics::json() const {
  std::lock_guard lock(impl_->mutex);
  const auto observation_nanoseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - impl_->created_at)
          .count());
  const auto worker_busy_nanoseconds =
      impl_->families[0].execution_nanoseconds +
      impl_->families[1].execution_nanoseconds +
      impl_->families[2].execution_nanoseconds;
  const auto artifact_load_span_nanoseconds =
      !impl_->first_artifact_submit || !impl_->last_artifact_terminal
          ? 0
          : static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    *impl_->last_artifact_terminal -
                    *impl_->first_artifact_submit)
                    .count());
  const auto package_critical_path =
      impl_->package_critical_paths.empty()
          ? 0
          : *std::ranges::max_element(impl_->package_critical_paths);
  const auto &c = impl_->closure_totals;
  const auto parallelism_milli =
      c.critical_path_nanoseconds == 0
          ? 0
          : (c.component_work_nanoseconds / c.critical_path_nanoseconds) *
                    1000 +
                ((c.component_work_nanoseconds %
                  c.critical_path_nanoseconds) *
                 1000) /
                    c.critical_path_nanoseconds;
  std::ostringstream out;
  out << "{\n"
      << "  \"schema\": \"chtholly-compiler-artifact-load-metrics-v1\",\n"
      << "  \"configuration\": {\n"
      << "    \"jobs\": " << impl_->jobs << ",\n"
      << "    \"worker-count\": " << impl_->worker_count << ",\n"
      << "    \"queue-capacity\": " << impl_->queue_capacity << "\n"
      << "  },\n"
      << "  \"executor\": {\n"
      << "    \"submitted\": " << impl_->submitted << ",\n"
      << "    \"started\": " << impl_->started << ",\n"
      << "    \"completed\": " << impl_->completed << ",\n"
      << "    \"cancelled\": " << impl_->cancelled << ",\n"
      << "    \"queue-high-water\": " << impl_->queue_high_water << ",\n"
      << "    \"active-high-water\": " << impl_->active_high_water << ",\n"
      << "    \"observation-nanoseconds\": " << observation_nanoseconds
      << ",\n"
      << "    \"worker-busy-nanoseconds\": " << worker_busy_nanoseconds
      << ",\n"
      << "    \"artifact-load-span-nanoseconds\": "
      << artifact_load_span_nanoseconds << ",\n"
      << "    \"backpressure-wait-count\": "
      << impl_->backpressure_wait_count << ",\n"
      << "    \"backpressure-wait-nanoseconds\": "
      << impl_->backpressure_wait_nanoseconds << ",\n"
      << "    \"specialization-inflight-hits\": "
      << impl_->specialization_inflight_hits << ",\n"
      << "    \"specialization-completed-result-hits\": "
      << impl_->specialization_completed_hits << "\n"
      << "  },\n"
      << "  \"artifact-io\": {\n"
      << "    \"read-attempts\": " << impl_->artifact_read_attempts << ",\n"
      << "    \"found\": " << impl_->artifact_read_found << ",\n"
      << "    \"missing\": " << impl_->artifact_read_missing << ",\n"
      << "    \"error\": " << impl_->artifact_read_errors << ",\n"
      << "    \"bytes\": " << impl_->artifact_read_bytes << ",\n"
      << "    \"read-nanoseconds\": " << impl_->artifact_read_nanoseconds
      << ",\n"
      << "    \"metadata-probes\": 0\n"
      << "  },\n"
      << "  \"archive-install\": {\n"
      << "    \"attempts\": " << impl_->archive_install_attempts
      << ",\n"
      << "    \"closure-hits\": "
      << impl_->archive_install_closure_hits << ",\n"
      << "    \"fresh-installs\": "
      << impl_->archive_install_fresh_installs << ",\n"
      << "    \"archive-bytes\": " << impl_->archive_install_bytes
      << "\n"
      << "  },\n"
      << "  \"specialization-component-reuse\": {\n"
      << "    \"requests\": " << impl_->specialization_component_requests
      << ",\n"
      << "    \"unique-components\": "
      << impl_->specialization_component_fingerprints.size() << ",\n"
      << "    \"duplicate-requests\": "
      << impl_->specialization_component_duplicate_requests << ",\n"
      << "    \"duplicate-bytes\": "
      << impl_->specialization_component_duplicate_bytes << ",\n"
      << "    \"cache-hits\": "
      << impl_->specialization_component_cache_hits << ",\n"
      << "    \"cache-misses\": "
      << impl_->specialization_component_cache_misses << ",\n"
      << "    \"coalesced-waits\": "
      << impl_->specialization_component_cache_coalesced << ",\n"
      << "    \"duplicate-disk-reads\": "
      << impl_->specialization_component_duplicate_disk_reads << ",\n"
      << "    \"evictions\": "
      << impl_->specialization_component_cache_evictions << ",\n"
      << "    \"bypasses\": "
      << impl_->specialization_component_cache_bypasses << ",\n"
      << "    \"entry-high-water\": "
      << impl_->specialization_component_cache_entry_high_water << ",\n"
      << "    \"byte-high-water\": "
      << impl_->specialization_component_cache_byte_high_water << "\n"
      << "  },\n"
      << "  \"families\": {\n";
  writeFamily(out, "object", impl_->families[0]);
  out << ",\n";
  writeFamily(out, "nominal-witness", impl_->families[1]);
  out << ",\n";
  writeFamily(out, "specialization", impl_->families[2]);
  out << "\n  },\n"
      << "  \"specialization-closure\": {\n"
      << "    \"requests\": " << impl_->closure_requests << ",\n"
      << "    \"found\": " << impl_->closure_found << ",\n"
      << "    \"missing\": " << impl_->closure_missing << ",\n"
      << "    \"corrupt\": " << impl_->closure_corrupt << ",\n"
      << "    \"error\": " << impl_->closure_error << ",\n"
      << "    \"component-count\": " << c.component_count << ",\n"
      << "    \"unique-component-count\": " << c.unique_component_count
      << ",\n"
      << "    \"edge-count\": " << c.edge_count << ",\n"
      << "    \"maximum-depth\": " << c.maximum_depth << ",\n"
      << "    \"index-bytes\": " << c.index_bytes << ",\n"
      << "    \"component-bytes\": " << c.component_bytes << ",\n"
      << "    \"exists-nanoseconds\": " << c.exists_nanoseconds << ",\n"
      << "    \"read-nanoseconds\": " << c.read_nanoseconds << ",\n"
      << "    \"decode-nanoseconds\": " << c.decode_nanoseconds << ",\n"
      << "    \"verify-nanoseconds\": " << c.verify_nanoseconds << ",\n"
      << "    \"dfs-nanoseconds\": " << c.dfs_nanoseconds << ",\n"
      << "    \"component-work-nanoseconds\": "
      << c.component_work_nanoseconds << ",\n"
      << "    \"critical-path-nanoseconds\": "
      << c.critical_path_nanoseconds << ",\n"
      << "    \"available-parallelism-milli\": " << parallelism_milli
      << "\n"
      << "  },\n"
      << "  \"package-scheduling\": {\n"
      << "    \"package-count\": " << impl_->package_count << ",\n"
      << "    \"worker-count\": " << impl_->package_worker_count << ",\n"
      << "    \"started\": " << impl_->package_started << ",\n"
      << "    \"completed\": " << impl_->package_completed << ",\n"
      << "    \"failed\": " << impl_->package_failed << ",\n"
      << "    \"active-high-water\": " << impl_->package_active_high_water
      << ",\n"
      << "    \"worker-wait-nanoseconds\": "
      << impl_->package_worker_wait_nanoseconds << ",\n"
      << "    \"execution-nanoseconds\": "
      << impl_->package_execution_nanoseconds << ",\n"
      << "    \"wall-nanoseconds\": " << impl_->package_wall_nanoseconds
      << ",\n"
      << "    \"critical-path-nanoseconds\": " << package_critical_path << "\n"
      << "  }";
  if (impl_->artifact_store_recorded) {
    constexpr std::array<std::string_view, 10> names = {
        "manifests", "objects", "specializations", "specialization-index",
        "type-specifics", "type-specific-index",
        "nominal-semantic-witnesses", "nominal-semantic-witness-index",
        "type-layouts", "type-layout-index"};
    out << ",\n"
        << "  \"artifact-store\": {\n"
        << "    \"valid\": "
        << (impl_->artifact_store_valid ? "true" : "false") << ",\n"
        << "    \"active-lease-count\": "
        << impl_->artifact_store_active_lease_count << ",\n"
        << "    \"stale-lease-count\": "
        << impl_->artifact_store_stale_lease_count << ",\n"
        << "    \"quarantine-bytes\": "
        << impl_->artifact_store_quarantine_bytes << ",\n"
        << "    \"reclaimed-bytes\": "
        << impl_->artifact_store_reclaimed_bytes;
    if (!impl_->artifact_store_recovery_instruction.empty()) {
      out << ",\n    \"recovery-instruction\": ";
      writeJsonString(out, impl_->artifact_store_recovery_instruction);
    }
    out << ",\n    \"families\": {\n";
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (index)
        out << ",\n";
      writeArtifactStoreFamily(out, names[index],
                               impl_->artifact_store_families[index]);
    }
    out << "\n    }\n  }";
  }
  out << "\n" << '}';
  return out.str();
}

} // namespace chtholly
