#include "chtholly/Driver/CompilerArtifactLoadExecutor.h"
#include "chtholly/Driver/CompilerArtifactLoadMetrics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Workload {
  std::string_view name;
  std::size_t requests;
  std::vector<std::vector<std::size_t>> edges;
};

std::uint64_t elapsedNanoseconds(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
}

Workload makeIndependent() {
  return {.name = "independent",
          .requests = 64,
          .edges = std::vector<std::vector<std::size_t>>(64)};
}

Workload makeWide() {
  Workload result{.name = "wide", .requests = 32, .edges = {{}}};
  for (std::size_t index = 1; index < 32; ++index)
    result.edges[0].push_back(index);
  result.edges.resize(32);
  return result;
}

Workload makeDeep() {
  Workload result{.name = "deep", .requests = 1,
                  .edges = std::vector<std::vector<std::size_t>>(32)};
  for (std::size_t index = 0; index + 1 < 32; ++index)
    result.edges[index].push_back(index + 1);
  return result;
}

Workload makeShared() {
  Workload result{.name = "shared", .requests = 16,
                  .edges = std::vector<std::vector<std::size_t>>(33)};
  for (std::size_t index = 0; index < 16; ++index) {
    result.edges[index].push_back(16 + index);
    result.edges[index].push_back(32);
    result.edges[16 + index].push_back(32);
  }
  return result;
}

Workload makeScc() {
  Workload result{.name = "scc", .requests = 8,
                  .edges = std::vector<std::vector<std::size_t>>(8)};
  for (std::size_t index = 0; index < 8; ++index)
    result.edges[index].push_back((index + 1) % 8);
  return result;
}

std::uint64_t consumeClosure(const Workload &workload, std::size_t root) {
  std::vector<bool> visiting(workload.edges.size());
  std::vector<bool> visited(workload.edges.size());
  const auto visit = [&](const auto &self, std::size_t node) -> std::uint64_t {
    if (node >= workload.edges.size() || visited[node] || visiting[node])
      return 0;
    visiting[node] = true;
    std::uint64_t value = node + 0x9e3779b97f4a7c15ULL;
    for (std::size_t iteration = 0; iteration < 20000; ++iteration)
      value = (value ^ (value >> 27U)) * 0x3c79ac492ba7b653ULL + iteration;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    for (const auto dependency : workload.edges[node])
      value ^= self(self, dependency);
    visiting[node] = false;
    visited[node] = true;
    return value;
  };
  return visit(visit, root % workload.edges.size());
}

std::uint64_t run(const Workload &workload, std::size_t jobs) {
  const auto workers = jobs <= 1 ? 0 : std::min<std::size_t>(4, jobs);
  auto metrics = std::make_shared<chtholly::CompilerArtifactLoadMetrics>(
      jobs, workers, std::max<std::size_t>(1, workers * 4));
  chtholly::CompilerArtifactLoadExecutor executor(workers, {}, metrics);
  std::vector<chtholly::compiler::ObjectArtifactLoadRequest> requests;
  requests.reserve(workload.requests);
  for (std::size_t index = 0; index < workload.requests; ++index) {
    requests.push_back(
        {.fingerprint = chtholly::compiler::StableFingerprint::fromCanonicalBytes(
             std::string(workload.name) + '-' + std::to_string(index)),
         .specific_fingerprint =
             chtholly::compiler::StableFingerprint::fromCanonicalBytes(
                 "artifact-load-benchmark-v1")});
  }
  const auto started = Clock::now();
  const auto results = executor.loadObjects(
      requests, [&](const auto &fingerprint, const auto &) {
        const auto found = std::ranges::find(
            requests, fingerprint, [](const auto &request) {
              return request.fingerprint;
            });
        const auto index = static_cast<std::size_t>(found - requests.begin());
        const auto value = consumeClosure(workload, index);
        return chtholly::compiler::ObjectArtifactLoadResult{
            .status = chtholly::compiler::ObjectArtifactLoadStatus::Found,
            .bytes = std::to_string(value)};
      });
  executor.drain();
  if (results.size() != requests.size())
    std::abort();
  return elapsedNanoseconds(started);
}

void writeObservations(const Workload &workload, std::size_t jobs,
                       std::size_t repetitions, bool &first) {
  std::vector<std::uint64_t> values;
  values.reserve(repetitions);
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
    values.push_back(run(workload, jobs));
  auto sorted = values;
  std::ranges::sort(sorted);
  if (!first)
    std::cout << ",\n";
  first = false;
  std::cout << "    {\"workload\":\"" << workload.name
            << "\",\"jobs\":" << jobs << ",\"raw-nanoseconds\":[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0)
      std::cout << ',';
    std::cout << values[index];
  }
  std::cout << "],\"minimum-nanoseconds\":" << sorted.front()
            << ",\"median-nanoseconds\":" << sorted[sorted.size() / 2]
            << ",\"maximum-nanoseconds\":" << sorted.back() << '}';
}

} // namespace

int main(int argc, char **argv) {
  std::size_t repetitions = 7;
  if (argc == 3 && std::string_view(argv[1]) == "--repetitions")
    repetitions = std::max<std::size_t>(1, std::strtoull(argv[2], nullptr, 10));
  else if (argc != 1) {
    std::cerr << "usage: chtholly_artifact_load_benchmark "
                 "[--repetitions N]\n";
    return 1;
  }
  const std::vector<Workload> workloads = {
      makeIndependent(), makeWide(), makeDeep(), makeShared(), makeScc()};
  std::cout << "{\n  \"schema\": "
               "\"chtholly-compiler-artifact-load-benchmark-v1\",\n"
               "  \"cache-mode\": \"process-warm-after-first-pass\",\n"
               "  \"observations\": [\n";
  bool first = true;
  for (const auto &workload : workloads)
    for (const auto jobs : {1U, 2U, 4U, 8U})
      writeObservations(workload, jobs, repetitions, first);
  std::cout << "\n  ]\n}\n";
}
