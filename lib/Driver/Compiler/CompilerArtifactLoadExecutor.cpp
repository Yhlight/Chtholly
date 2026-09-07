#include "chtholly/Driver/CompilerArtifactLoadExecutor.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace chtholly {
namespace {

compiler::ObjectArtifactLoadResult cancelledObjectLoad() {
  return {.status = compiler::ObjectArtifactLoadStatus::Error,
          .error = "compiler artifact loading cancelled"};
}

compiler::NominalSemanticWitnessLoadResult cancelledWitnessLoad() {
  return {.error = "compiler artifact loading cancelled"};
}

compiler::ConcreteSpecializationLoadResult cancelledSpecializationLoad() {
  return {.status = compiler::ConcreteSpecializationLoadStatus::Error,
          .error = "compiler artifact loading cancelled"};
}

} // namespace

struct CompilerArtifactLoadExecutor::Impl {
  struct Task {
    std::function<void(std::size_t)> run;
    std::function<void()> cancel;
  };

  Impl(std::size_t requested_workers,
       std::function<bool()> cancellation_callback,
       std::shared_ptr<CompilerArtifactLoadMetrics> load_metrics)
      : worker_count(requested_workers),
        queue_limit(std::max<std::size_t>(1, requested_workers * 4)),
        is_cancelled(std::move(cancellation_callback)),
        metrics(std::move(load_metrics)) {
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index)
      workers.emplace_back([this] { work(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex);
      accepting = false;
    }
    changed.notify_all();
    space.notify_all();
    for (auto &worker : workers)
      worker.join();
  }

  [[nodiscard]] bool cancelled() const {
    return cancellation_requested.load(std::memory_order_acquire) ||
           (is_cancelled && is_cancelled());
  }

  template <typename Result, typename Work, typename Cancel>
  std::shared_future<Result> submit(CompilerArtifactLoadFamily family, Work work,
                                    Cancel cancel) {
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future().share();
    auto completed = std::make_shared<std::atomic<bool>>(false);
    const auto complete = [promise, completed](Result result) {
      if (!completed->exchange(true, std::memory_order_acq_rel))
        promise->set_value(std::move(result));
    };
    if (metrics)
      metrics->recordSubmitted(family);
    if (worker_count == 0) {
      if (cancelled()) {
        complete(cancel());
        if (metrics)
          metrics->recordCancelled(family);
      } else {
        if (metrics)
          metrics->recordStarted(family, 0, 0);
        const auto started_at = metrics ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
        auto result = work();
        const auto execution = metrics ? elapsedNanoseconds(started_at) : 0;
        if (cancelled()) {
          complete(cancel());
          if (metrics)
            metrics->recordCancelled(family, execution);
        } else {
          complete(std::move(result));
          if (metrics)
            metrics->recordCompleted(family, execution);
        }
      }
      return future;
    }
    std::unique_lock lock(mutex);
    const bool backpressured = tasks.size() >= queue_limit;
    const auto wait_started = metrics && backpressured
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    space.wait(lock, [&] {
      return !accepting || cancelled() || tasks.size() < queue_limit;
    });
    if (metrics && backpressured)
      metrics->recordBackpressure(elapsedNanoseconds(wait_started));
    if (!accepting || cancelled()) {
      lock.unlock();
      complete(cancel());
      if (metrics)
        metrics->recordCancelled(family);
      return future;
    }
    const auto queued_at = metrics ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    auto terminal_recorded = std::make_shared<std::atomic<bool>>(false);
    tasks.push_back(
        {.run =
             [this, complete, work = std::move(work), cancel, family,
              queued_at, terminal_recorded](std::size_t active_workers) mutable {
               if (metrics)
                 metrics->recordStarted(family, elapsedNanoseconds(queued_at),
                                        active_workers);
               const auto started_at = metrics
                                           ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
               auto result = work();
               const auto execution =
                   metrics ? elapsedNanoseconds(started_at) : 0;
               if (cancelled()) {
                 complete(cancel());
                 if (metrics && !terminal_recorded->exchange(true))
                   metrics->recordCancelled(family, execution);
               } else {
                 complete(std::move(result));
                 if (metrics && !terminal_recorded->exchange(true))
                   metrics->recordCompleted(family, execution);
               }
             },
         .cancel = [this, complete, cancel = std::move(cancel), family,
                    terminal_recorded]() mutable {
           complete(cancel());
           if (metrics && !terminal_recorded->exchange(true))
             metrics->recordCancelled(family);
         }});
    const auto queue_depth = tasks.size();
    lock.unlock();
    if (metrics)
      metrics->recordQueueDepth(queue_depth);
    changed.notify_one();
    return future;
  }

  void work() {
    while (true) {
      Task task;
      std::size_t active_workers = 0;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return !accepting || !tasks.empty(); });
        if (tasks.empty()) {
          if (!accepting)
            return;
          continue;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
        ++active;
        active_workers = active;
      }
      space.notify_all();
      try {
        if (cancelled())
          task.cancel();
        else
          task.run(active_workers);
      } catch (...) {
        try {
          task.cancel();
        } catch (...) {}
      }
      {
        std::lock_guard lock(mutex);
        --active;
      }
      drained.notify_all();
    }
  }

  void cancelPending() {
    cancellation_requested.store(true, std::memory_order_release);
    std::deque<Task> pending;
    {
      std::lock_guard lock(mutex);
      pending.swap(tasks);
    }
    for (auto &task : pending)
      task.cancel();
    changed.notify_all();
    space.notify_all();
    drained.notify_all();
  }

  void drain() {
    std::unique_lock lock(mutex);
    drained.wait(lock, [&] { return tasks.empty() && active == 0; });
  }

  const std::size_t worker_count;
  const std::size_t queue_limit;
  std::function<bool()> is_cancelled;
  std::shared_ptr<CompilerArtifactLoadMetrics> metrics;
  std::atomic<bool> cancellation_requested = false;
  std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable space;
  std::condition_variable drained;
  std::deque<Task> tasks;
  std::vector<std::thread> workers;
  std::size_t active = 0;
  bool accepting = true;

  std::mutex specialization_mutex;
  std::unordered_map<std::string,
                     std::shared_future<compiler::ConcreteSpecializationLoadResult>>
      specializations;

  static std::uint64_t elapsedNanoseconds(
      std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }
};

CompilerArtifactLoadExecutor::CompilerArtifactLoadExecutor(
    std::size_t worker_count, std::function<bool()> is_cancelled,
    std::shared_ptr<CompilerArtifactLoadMetrics> metrics)
    : impl_(std::make_unique<Impl>(worker_count, std::move(is_cancelled),
                                  std::move(metrics))) {}

CompilerArtifactLoadExecutor::~CompilerArtifactLoadExecutor() = default;

std::vector<compiler::ObjectArtifactLoadResult>
CompilerArtifactLoadExecutor::loadObjects(
    std::span<const compiler::ObjectArtifactLoadRequest> requests,
    const compiler::ObjectArtifactLoader &loader) {
  if (impl_->worker_count != 0 && requests.size() <= impl_->worker_count) {
    std::vector<compiler::ObjectArtifactLoadResult> results;
    results.reserve(requests.size());
    for (const auto &request : requests) {
      if (impl_->metrics) {
        impl_->metrics->recordSubmitted(CompilerArtifactLoadFamily::Object);
        impl_->metrics->recordStarted(CompilerArtifactLoadFamily::Object, 0, 0);
      }
      const auto started_at = impl_->metrics
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
      if (impl_->cancelled()) {
        results.push_back(cancelledObjectLoad());
        if (impl_->metrics)
          impl_->metrics->recordCancelled(CompilerArtifactLoadFamily::Object);
        continue;
      }
      compiler::ObjectArtifactLoadResult result;
      bool loader_failed = false;
      try {
        result = loader(request.fingerprint, request.specific_fingerprint);
      } catch (...) {
        loader_failed = true;
        result = cancelledObjectLoad();
      }
      const auto execution =
          impl_->metrics ? Impl::elapsedNanoseconds(started_at) : 0;
      if (loader_failed || impl_->cancelled()) {
        results.push_back(cancelledObjectLoad());
        if (impl_->metrics)
          impl_->metrics->recordCancelled(CompilerArtifactLoadFamily::Object,
                                          execution);
      } else {
        results.push_back(std::move(result));
        if (impl_->metrics)
          impl_->metrics->recordCompleted(CompilerArtifactLoadFamily::Object,
                                          execution);
      }
    }
    return results;
  }
  std::vector<std::shared_future<compiler::ObjectArtifactLoadResult>> pending;
  pending.reserve(requests.size());
  for (const auto request : requests) {
    pending.push_back(impl_->submit<compiler::ObjectArtifactLoadResult>(
        CompilerArtifactLoadFamily::Object,
        [loader, request] {
          return loader(request.fingerprint, request.specific_fingerprint);
        },
        cancelledObjectLoad));
  }
  std::vector<compiler::ObjectArtifactLoadResult> results;
  results.reserve(pending.size());
  for (auto &future : pending)
    {
      const auto wait_started = impl_->metrics
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
      results.push_back(future.get());
      if (impl_->metrics)
        impl_->metrics->recordConsumerWait(
            CompilerArtifactLoadFamily::Object,
            Impl::elapsedNanoseconds(wait_started));
    }
  return results;
}

std::vector<compiler::NominalSemanticWitnessLoadResult>
CompilerArtifactLoadExecutor::loadNominalSemanticWitnesses(
    std::span<const compiler::StableFingerprint> request_fingerprints,
    const compiler::NominalSemanticWitnessLoader &loader) {
  std::vector<std::shared_future<compiler::NominalSemanticWitnessLoadResult>>
      pending;
  pending.reserve(request_fingerprints.size());
  for (const auto fingerprint : request_fingerprints) {
    pending.push_back(impl_->submit<compiler::NominalSemanticWitnessLoadResult>(
        CompilerArtifactLoadFamily::NominalWitness,
        [loader, fingerprint] {
          compiler::NominalSemanticWitnessLoadResult result;
          result.artifact = loader(fingerprint, result.error);
          return result;
        },
        cancelledWitnessLoad));
  }
  std::vector<compiler::NominalSemanticWitnessLoadResult> results;
  results.reserve(pending.size());
  for (auto &future : pending)
    {
      const auto wait_started = impl_->metrics
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
      results.push_back(future.get());
      if (impl_->metrics)
        impl_->metrics->recordConsumerWait(
            CompilerArtifactLoadFamily::NominalWitness,
            Impl::elapsedNanoseconds(wait_started));
    }
  return results;
}

compiler::ConcreteSpecializationLoadResult
CompilerArtifactLoadExecutor::loadSpecialization(
    const compiler::StableFingerprint &request_fingerprint,
    const compiler::ConcreteSpecializationLoader &loader) {
  std::shared_future<compiler::ConcreteSpecializationLoadResult> future;
  {
    std::lock_guard lock(impl_->specialization_mutex);
    const auto key = request_fingerprint.hex();
    if (const auto found = impl_->specializations.find(key);
        found != impl_->specializations.end()) {
      future = found->second;
      if (impl_->metrics)
        impl_->metrics->recordSpecializationDedup(
            future.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready);
    } else {
      future = impl_->submit<compiler::ConcreteSpecializationLoadResult>(
          CompilerArtifactLoadFamily::Specialization,
          [loader, request_fingerprint] { return loader(request_fingerprint); },
          cancelledSpecializationLoad);
      impl_->specializations.emplace(key, future);
    }
  }
  const auto wait_started = impl_->metrics
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  auto result = future.get();
  if (impl_->metrics)
    impl_->metrics->recordConsumerWait(CompilerArtifactLoadFamily::Specialization,
                                       Impl::elapsedNanoseconds(wait_started));
  return result;
}

void CompilerArtifactLoadExecutor::cancelPending() {
  impl_->cancelPending();
}

void CompilerArtifactLoadExecutor::drain() {
  impl_->drain();
}

bool CompilerArtifactLoadExecutor::isCancelled() const {
  return impl_->cancelled();
}

std::size_t CompilerArtifactLoadExecutor::workerCount() const {
  return impl_->worker_count;
}

std::size_t CompilerArtifactLoadExecutor::queueCapacity() const {
  return impl_->queue_limit;
}

} // namespace chtholly
