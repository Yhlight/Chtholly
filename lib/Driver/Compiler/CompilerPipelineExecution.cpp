#include "CompilerPipelineInternal.h"

#include "chtholly/Compiler/PackageQueryGraph.h"
#include "chtholly/Driver/CompilerArtifactLoadMetrics.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace chtholly {

bool CompilerPipelineExecutionService::packageQueryGraph(
    CompilerPackageQueryExecutionState &state,
    std::vector<PackageQueryResult> &results, std::string &error) {
  std::vector<compiler::PackageQueryNode> nodes;
  nodes.reserve(state.plan.packages.size());
  for (const auto &package : state.plan.packages) {
    std::vector<compiler::PackageQueryId> dependencies;
    dependencies.reserve(package.dependencies.size());
    for (const auto dependency : package.dependencies)
      dependencies.push_back(compiler::PackageQueryId(
          static_cast<std::uint32_t>(dependency)));
    nodes.emplace_back(package.package_name, std::move(dependencies));
  }
  compiler::CompilerPackageQueryGraph graph(std::move(nodes));
  if (!graph.initialize(error))
    return false;
  results.clear();
  results.resize(state.plan.packages.size());
  if (results.empty()) {
    error = "compiler package query graph is empty";
    return false;
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::size_t completed_count = 0;
  bool stop = false;
  std::string scheduler_error;
  const auto worker_count = std::min(
      {results.size(), std::max<std::size_t>(1, state.jobs),
       state.maximum_parallelism()});
  if (state.artifact_load_metrics)
    state.artifact_load_metrics->configurePackageScheduling(results.size(),
                                                             worker_count);
  const auto scheduling_started =
      state.artifact_load_metrics ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
  std::size_t active_workers = 0;
  const auto run_worker = [&] {
    while (true) {
      std::size_t index = 0;
      {
        std::unique_lock lock(mutex);
        const auto wait_started =
            state.artifact_load_metrics ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
        changed.wait(lock, [&] {
          return stop || completed_count == results.size() ||
                 graph.hasReadyQuery();
        });
        if (stop || completed_count == results.size())
          return;
        index = graph.takeReadyQuery()->index;
        ++active_workers;
        if (state.artifact_load_metrics) {
          const auto wait_nanoseconds = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - wait_started)
                  .count());
          state.artifact_load_metrics->recordPackageQueryStarted(
              wait_nanoseconds, active_workers);
        }
      }

      PackageQueryResult result;
      const auto query_started =
          state.artifact_load_metrics ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
      try {
        result = state.execute_query(
            index, std::span<const PackageQueryResult>(results));
      } catch (const std::exception &exception) {
        result.error =
            std::string("compiler package query failed: ") + exception.what();
      } catch (...) {
        result.error = "compiler package query failed with an unknown exception";
      }

      {
        std::lock_guard lock(mutex);
        const auto query_nanoseconds =
            state.artifact_load_metrics
                ? static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - query_started)
                          .count())
                : 0;
        results[index] = std::move(result);
        --active_workers;
        ++completed_count;
        if (state.artifact_load_metrics)
          state.artifact_load_metrics->recordPackageQueryCompleted(
              index, state.plan.packages[index].dependencies,
              query_nanoseconds, !results[index].error.empty());
        if (!results[index].error.empty()) {
          std::string transition_error;
          if (!graph.markFailed(
                  compiler::PackageQueryId(static_cast<std::uint32_t>(index)),
                  transition_error))
            scheduler_error = std::move(transition_error);
          stop = true;
        } else if (!graph.markSucceeded(
                       compiler::PackageQueryId(static_cast<std::uint32_t>(index)),
                       scheduler_error)) {
          stop = true;
        }
      }
      changed.notify_all();
    }
  };
  std::vector<std::thread> workers;
  if (worker_count == 1) {
    run_worker();
  } else {
    workers.reserve(worker_count - 1);
    for (std::size_t worker = 1; worker < worker_count; ++worker)
      workers.emplace_back(run_worker);
    changed.notify_all();
    run_worker();
    for (auto &worker : workers)
      worker.join();
  }
  if (state.artifact_load_metrics) {
    const auto wall_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - scheduling_started)
            .count());
    state.artifact_load_metrics->recordPackageSchedulingCompleted(
        wall_nanoseconds);
  }

  if (!scheduler_error.empty()) {
    error = std::move(scheduler_error);
    return false;
  }
  if (stop) {
    graph.blockUnfinished();
    for (std::size_t index = 0; index < results.size(); ++index) {
      if (graph.node(compiler::PackageQueryId(static_cast<std::uint32_t>(index)))
              .state() == compiler::PackageQueryState::Failed) {
        error = "package '" + state.plan.packages[index].package_name +
                "': " + results[index].error;
        return false;
      }
    }
  }
  return completed_count == results.size();
}

} // namespace chtholly
