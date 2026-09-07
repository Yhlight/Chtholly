#include "chtholly/Driver/CompilerDaemon.h"

#include "chtholly/Driver/CompilerBuildControlSnapshot.h"
#include "chtholly/Compiler/CFDL.h"

#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

namespace chtholly {

struct CompilerCancellationState {
  std::atomic_bool cancelled = false;
};

bool CompilerCancellationToken::isCancelled() const {
  return state_ && state_->cancelled.load(std::memory_order_acquire);
}

CompilerCancellationSource::CompilerCancellationSource()
    : state_(std::make_shared<CompilerCancellationState>()) {}

CompilerCancellationToken CompilerCancellationSource::token() const {
  return CompilerCancellationToken(state_);
}

void CompilerCancellationSource::cancel() const {
  state_->cancelled.store(true, std::memory_order_release);
}

struct CompilerPackageQueryCache::Impl {
  struct Entry {
    std::shared_ptr<const compiler::CompilationSession> value;
    std::list<std::string>::iterator recency;
  };

  explicit Impl(std::size_t requested_capacity)
      : capacity(requested_capacity) {}

  std::size_t capacity;
  mutable std::mutex mutex;
  std::list<std::string> recency;
  std::unordered_map<std::string, Entry> entries;
  std::size_t hits = 0;
  std::size_t misses = 0;
};

CompilerPackageQueryCache::CompilerPackageQueryCache(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
CompilerPackageQueryCache::~CompilerPackageQueryCache() = default;

std::shared_ptr<const compiler::CompilationSession>
CompilerPackageQueryCache::lookup(std::string_view key) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->entries.find(std::string(key));
  if (found == impl_->entries.end()) {
    ++impl_->misses;
    return {};
  }
  impl_->recency.splice(impl_->recency.begin(), impl_->recency,
                        found->second.recency);
  ++impl_->hits;
  return found->second.value;
}

void CompilerPackageQueryCache::insert(
    std::string key, std::shared_ptr<const compiler::CompilationSession> result) {
  if (!result)
    return;
  std::lock_guard lock(impl_->mutex);
  if (impl_->capacity == 0)
    return;
  if (const auto found = impl_->entries.find(key);
      found != impl_->entries.end()) {
    found->second.value = std::move(result);
    impl_->recency.splice(impl_->recency.begin(), impl_->recency,
                          found->second.recency);
    return;
  }
  impl_->recency.push_front(key);
  impl_->entries.emplace(std::move(key),
                         Impl::Entry{.value = std::move(result),
                                     .recency = impl_->recency.begin()});
  while (impl_->entries.size() > impl_->capacity) {
    const auto evicted = std::move(impl_->recency.back());
    impl_->recency.pop_back();
    impl_->entries.erase(evicted);
  }
}

CompilerPackageQueryCacheStats CompilerPackageQueryCache::stats() const {
  std::lock_guard lock(impl_->mutex);
  return {.hits = impl_->hits,
          .misses = impl_->misses,
          .entries = impl_->entries.size()};
}

void CompilerPackageQueryCache::clear() {
  std::lock_guard lock(impl_->mutex);
  impl_->entries.clear();
  impl_->recency.clear();
}

struct CompilerDaemonRequestCoordinator::Impl {
  struct Pending {
    CompilerRequestId id = 0;
    CompilerDaemonRequest request;
    CompilerDaemonCompletion completion;
  };

  explicit Impl(std::size_t cache_capacity)
      : cache(cache_capacity), worker([this] { run(); }) {}

  ~Impl() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      if (active_cancellation)
        active_cancellation->cancel();
      pending.reset();
    }
    changed.notify_all();
    if (worker.joinable())
      worker.join();
  }

  void run() {
    while (true) {
      Pending work;
      CompilerCancellationSource cancellation;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || pending.has_value(); });
        if (stopping)
          return;
        work = std::move(*pending);
        pending.reset();
        active_cancellation = cancellation;
      }

      CompilerCheckExecutionResult result;
      std::string error;
      auto prepared = prepareNextCompilerRequest(
          work.request.invocation,
          CompilerCompilerEnvironment{.input_files = work.request.input_files,
                                  .update_lockfile = false},
          error);
      if (cancellation.token().isCancelled()) {
        result.status = CompilerDaemonRequestStatus::Cancelled;
      } else if (!prepared) {
        result.status = CompilerDaemonRequestStatus::Failed;
        result.error = std::move(error);
      } else {
        result =
            executeNextCheckRequest(*prepared, cache, cancellation.token());
      }

      bool publish = false;
      {
        std::lock_guard lock(mutex);
        publish = !stopping && !cancellation.token().isCancelled() &&
                  work.id == latest_id &&
                  work.request.overlay_generation == latest_generation;
        if (active_cancellation && active_cancellation->token().isCancelled() ==
                                       cancellation.token().isCancelled())
          active_cancellation.reset();
      }
      if (publish && work.completion)
        work.completion(work.id, work.request.overlay_generation,
                        std::move(result));
    }
  }

  mutable std::mutex mutex;
  std::condition_variable changed;
  bool stopping = false;
  CompilerRequestId latest_id = 0;
  CompilerOverlayGeneration latest_generation = 0;
  std::optional<Pending> pending;
  std::optional<CompilerCancellationSource> active_cancellation;
  CompilerPackageQueryCache cache;
  std::thread worker;
};

CompilerDaemonRequestCoordinator::CompilerDaemonRequestCoordinator(
    std::size_t cache_capacity)
    : impl_(std::make_unique<Impl>(cache_capacity)) {}
CompilerDaemonRequestCoordinator::~CompilerDaemonRequestCoordinator() = default;

CompilerRequestId
CompilerDaemonRequestCoordinator::submit(CompilerDaemonRequest request,
                                     CompilerDaemonCompletion completion) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->active_cancellation)
    impl_->active_cancellation->cancel();
  const auto id = ++impl_->latest_id;
  impl_->latest_generation = request.overlay_generation;
  impl_->pending = Impl::Pending{.id = id,
                                 .request = std::move(request),
                                 .completion = std::move(completion)};
  impl_->changed.notify_all();
  return id;
}

void CompilerDaemonRequestCoordinator::cancelCurrent() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->active_cancellation)
    impl_->active_cancellation->cancel();
  impl_->pending.reset();
  ++impl_->latest_id;
  impl_->changed.notify_all();
}

CompilerPackageQueryCacheStats CompilerDaemonRequestCoordinator::cacheStats() const {
  return impl_->cache.stats();
}

struct CompilerLanguageService::Impl {
  struct Document {
    CompilerDocumentVersion version = 0;
    std::string text;
  };

  struct PendingQuery {
    CompilerRequestId id = 0;
    CompilerOverlayGeneration generation = 0;
    CompilerLanguageQueryKind kind = CompilerLanguageQueryKind::Hover;
    std::string path;
    CompilerTextPosition position;
    bool include_declaration = false;
    std::string new_name;
    std::string completion_prefix;
    std::vector<CompilerCompletionItem> direct_completion_items;
  };

  Impl(CompilerInvocation request_invocation,
       std::shared_ptr<const CompilerInputFileSystem> files,
       CompilerLanguageServiceCallbacks service_callbacks,
       std::size_t cache_capacity)
      : invocation(std::move(request_invocation)), base_files(std::move(files)),
        callbacks(std::move(service_callbacks)), coordinator(cache_capacity),
        completion_coordinator(0) {}

  void complete(PendingQuery query,
                std::shared_ptr<const CompilerWorkspaceSymbolIndex> index,
                CompilerDaemonRequestStatus status, std::string error = {}) {
    CompilerLanguageQueryResult result{.request_id = query.id,
                                   .overlay_generation = query.generation,
                                   .status = status,
                                   .kind = query.kind,
                                   .error = std::move(error)};
    if (status == CompilerDaemonRequestStatus::Succeeded &&
        query.kind == CompilerLanguageQueryKind::Completion &&
        !query.direct_completion_items.empty()) {
      result.completion_items = std::move(query.direct_completion_items);
    } else if (status == CompilerDaemonRequestStatus::Succeeded && index) {
      switch (query.kind) {
      case CompilerLanguageQueryKind::Hover:
        result.hover = index->hover(query.path, query.position);
        break;
      case CompilerLanguageQueryKind::Definition:
        result.locations = index->definition(query.path, query.position);
        break;
      case CompilerLanguageQueryKind::References:
        result.locations = index->references(query.path, query.position,
                                             query.include_declaration);
        break;
      case CompilerLanguageQueryKind::Completion:
        result.completion_items = index->completion(query.path, query.position,
                                                    query.completion_prefix);
        break;
      case CompilerLanguageQueryKind::DocumentSymbols:
        result.document_symbols = index->documentSymbols(query.path);
        break;
      case CompilerLanguageQueryKind::PrepareRename:
        result.rename = index->prepareRename(query.path, query.position);
        break;
      case CompilerLanguageQueryKind::Rename:
        result.rename = index->rename(query.path, query.position,
                                      query.new_name, result.error);
        result.rename_text = std::move(query.new_name);
        if (!result.error.empty())
          result.status = CompilerDaemonRequestStatus::Failed;
        break;
      }
    }
    if (callbacks.complete_query)
      callbacks.complete_query(std::move(result));
  }

  bool apply(std::string path, CompilerDocumentVersion version,
             std::span<const CompilerTextDocumentContentChange> text_changes,
             bool opening, std::string &error) {
    path = normalizeCompilerInputPath(path);
    if (path.empty()) {
      error = "compiler language service requires a valid document path";
      return false;
    }
    std::vector<PendingQuery> stale;
    {
      std::lock_guard lock(mutex);
      const auto found = documents.find(path);
      if (opening ? found != documents.end() : found == documents.end()) {
        error = opening ? "compiler document is already open"
                        : "compiler document is not open";
        return false;
      }
      if (!opening && version <= found->second.version) {
        error = "compiler document version must increase monotonically";
        return false;
      }
      std::string compiler_text;
      if (!applyNextTextChanges(opening ? std::string_view{}
                                        : std::string_view(found->second.text),
                                text_changes, compiler_text, error))
        return false;
      const auto overlay_changes =
          std::vector{CompilerOverlayChange::replace(path, compiler_text)};
      auto compiler_overlay = overlay ? overlay->withChanges(overlay_changes, error)
                                  : CompilerOverlayInputFileSystem::create(
                                        base_files, overlay_changes, error);
      if (!compiler_overlay)
        return false;
      overlay = std::move(compiler_overlay);
      documents[path] = {.version = version, .text = std::move(compiler_text)};
      ++generation;
      analysis_pending = false;
      for (auto &[unused, query] : pending_queries)
        stale.push_back(std::move(query));
      pending_queries.clear();
      for (auto &[unused, query] : completion_queries)
        stale.push_back(std::move(query));
      completion_queries.clear();
      coordinator.cancelCurrent();
      completion_coordinator.cancelCurrent();
    }
    for (auto &query : stale)
      complete(std::move(query), {}, CompilerDaemonRequestStatus::Stale,
               "compiler document changed before the query completed");
    return true;
  }

  std::pair<CompilerRequestId, bool> enqueue(CompilerLanguageQueryKind kind,
                                         std::string path,
                                         CompilerTextPosition position,
                                         bool include_declaration,
                                         std::string new_name = {}) {
    path = normalizeCompilerInputPath(path);
    PendingQuery query;
    std::shared_ptr<const CompilerWorkspaceSymbolIndex> index;
    {
      std::lock_guard lock(mutex);
      query = {.id = ++compiler_query_id,
               .generation = generation,
               .kind = kind,
               .path = std::move(path),
               .position = position,
               .include_declaration = include_declaration,
               .new_name = std::move(new_name)};
      if (latest_symbol_index && latest_analysis_generation == generation)
        index = latest_symbol_index;
      else
        pending_queries.emplace(query.id, query);
    }
    if (index) {
      const auto id = query.id;
      complete(std::move(query), std::move(index),
               CompilerDaemonRequestStatus::Succeeded);
      return {id, false};
    }
    return {query.id, true};
  }

  CompilerInvocation invocation;
  std::shared_ptr<const CompilerInputFileSystem> base_files;
  CompilerLanguageServiceCallbacks callbacks;
  mutable std::mutex mutex;
  std::mutex completion_request_mutex;
  std::shared_ptr<const CompilerOverlayInputFileSystem> overlay;
  std::unordered_map<std::string, Document> documents;
  CompilerOverlayGeneration generation = 0;
  std::shared_ptr<const CompilerRequestSnapshot> latest_snapshot;
  std::shared_ptr<const CompilerWorkspaceSymbolIndex> latest_symbol_index;
  CompilerOverlayGeneration latest_analysis_generation = 0;
  bool analysis_pending = false;
  CompilerOverlayGeneration pending_analysis_generation = 0;
  CompilerRequestId pending_analysis_request = 0;
  CompilerRequestId compiler_query_id = (std::uint64_t{1} << 63U);
  std::unordered_map<CompilerRequestId, PendingQuery> pending_queries;
  std::unordered_map<CompilerRequestId, PendingQuery> completion_queries;
  CompilerDaemonRequestCoordinator coordinator;
  CompilerDaemonRequestCoordinator completion_coordinator;
};

CompilerLanguageService::CompilerLanguageService(
    CompilerInvocation invocation,
    std::shared_ptr<const CompilerInputFileSystem> base_files,
    CompilerLanguageServiceCallbacks callbacks, std::size_t cache_capacity)
    : impl_(std::make_unique<Impl>(std::move(invocation), std::move(base_files),
                                   std::move(callbacks), cache_capacity)) {}

CompilerLanguageService::~CompilerLanguageService() {
  impl_->coordinator.cancelCurrent();
  impl_->completion_coordinator.cancelCurrent();
}

bool CompilerLanguageService::openDocument(std::string path,
                                       CompilerDocumentVersion version,
                                       std::string text, std::string &error) {
  const auto changes = std::vector{CompilerTextDocumentContentChange{
      .range = std::nullopt, .text = std::move(text)}};
  return impl_->apply(std::move(path), version, changes, true, error);
}

bool CompilerLanguageService::changeDocument(std::string path,
                                         CompilerDocumentVersion version,
                                         std::string text, std::string &error) {
  const auto changes = std::vector{CompilerTextDocumentContentChange{
      .range = std::nullopt, .text = std::move(text)}};
  return impl_->apply(std::move(path), version, changes, false, error);
}

bool CompilerLanguageService::changeDocument(
    std::string path, CompilerDocumentVersion version,
    std::span<const CompilerTextDocumentContentChange> changes,
    std::string &error) {
  return impl_->apply(std::move(path), version, changes, false, error);
}

bool CompilerLanguageService::closeDocument(std::string path, std::string &error) {
  path = normalizeCompilerInputPath(path);
  CompilerLanguageServiceCallbacks callbacks;
  CompilerOverlayGeneration generation = 0;
  std::vector<CompilerLanguageService::Impl::PendingQuery> stale;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->documents.contains(path)) {
      error = "compiler document is not open";
      return false;
    }
    const auto changes = std::vector{CompilerOverlayChange::removeOverride(path)};
    auto compiler_overlay = impl_->overlay->withChanges(changes, error);
    if (!compiler_overlay)
      return false;
    impl_->overlay = std::move(compiler_overlay);
    impl_->documents.erase(path);
    generation = ++impl_->generation;
    impl_->analysis_pending = false;
    for (auto &[unused, query] : impl_->pending_queries)
      stale.push_back(std::move(query));
    impl_->pending_queries.clear();
    for (auto &[unused, query] : impl_->completion_queries)
      stale.push_back(std::move(query));
    impl_->completion_queries.clear();
    callbacks = impl_->callbacks;
    impl_->coordinator.cancelCurrent();
    impl_->completion_coordinator.cancelCurrent();
  }
  for (auto &query : stale)
    impl_->complete(std::move(query), {}, CompilerDaemonRequestStatus::Stale,
                    "compiler document closed before the query completed");
  if (callbacks.publish_diagnostics)
    callbacks.publish_diagnostics(
        {.overlay_generation = generation, .path = std::move(path)});
  return true;
}

CompilerRequestId CompilerLanguageService::requestDiagnostics() {
  CompilerDaemonRequest request;
  CompilerRequestId request_id = 0;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->analysis_pending &&
        impl_->pending_analysis_generation == impl_->generation)
      return impl_->pending_analysis_request;
    request.invocation = impl_->invocation;
    request.input_files = impl_->overlay ? impl_->overlay : impl_->base_files;
    request.overlay_generation = impl_->generation;
    for (const auto &[path, document] : impl_->documents)
      request.document_versions.emplace(path, document.version);
    auto versions = request.document_versions;
    impl_->analysis_pending = true;
    impl_->pending_analysis_generation = impl_->generation;
    request_id = impl_->coordinator.submit(
        std::move(request),
        [this, versions = std::move(versions)](
            CompilerRequestId id, CompilerOverlayGeneration generation,
            CompilerCheckExecutionResult result) mutable {
          CompilerLanguageServiceCallbacks callbacks;
          std::vector<CompilerLanguageService::Impl::PendingQuery> queries;
          std::shared_ptr<const CompilerWorkspaceSymbolIndex> symbol_index;
          {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->analysis_pending ||
                impl_->pending_analysis_request != id)
              return;
            if (generation != impl_->generation)
              return;
            for (const auto &[path, version] : versions) {
              const auto found = impl_->documents.find(path);
              if (found == impl_->documents.end() ||
                  found->second.version != version)
                return;
            }
            impl_->analysis_pending = false;
            impl_->pending_analysis_request = 0;
            if (result.snapshot)
              impl_->latest_snapshot = result.snapshot;
            if (result.symbol_index) {
              impl_->latest_symbol_index = result.symbol_index;
              impl_->latest_analysis_generation = generation;
            }
            symbol_index = result.symbol_index;
            for (auto position = impl_->pending_queries.begin();
                 position != impl_->pending_queries.end();) {
              if (position->second.generation == generation) {
                queries.push_back(std::move(position->second));
                position = impl_->pending_queries.erase(position);
              } else {
                ++position;
              }
            }
            callbacks = impl_->callbacks;
          }
          if (result.status == CompilerDaemonRequestStatus::Cancelled ||
              result.status == CompilerDaemonRequestStatus::Stale)
            return;
          std::unordered_map<std::string, std::vector<CompilerSourceDiagnostic>>
              by_path;
          for (auto &diagnostic : result.diagnostics)
            by_path[diagnostic.path].push_back(std::move(diagnostic));
          if (callbacks.publish_diagnostics) {
            for (const auto &[path, version] : versions) {
              CompilerDiagnosticBatch batch;
              batch.request_id = id;
              batch.overlay_generation = generation;
              if (result.snapshot)
                batch.request_fingerprint = result.snapshot->fingerprint();
              batch.path = path;
              batch.version = version;
              if (auto found = by_path.find(path); found != by_path.end())
                batch.diagnostics = std::move(found->second);
              callbacks.publish_diagnostics(std::move(batch));
            }
          }
          const auto query_error = result.error;
          if (!result.error.empty() && result.diagnostics.empty() &&
              callbacks.publish_workspace_diagnostic)
            callbacks.publish_workspace_diagnostic(id, std::move(result.error));
          for (auto &query : queries) {
            const auto query_status = symbol_index
                                          ? CompilerDaemonRequestStatus::Succeeded
                                          : result.status;
            impl_->complete(std::move(query), symbol_index, query_status,
                            symbol_index ? std::string{} : query_error);
          }
        });
    impl_->pending_analysis_request = request_id;
  }
  return request_id;
}

CompilerRequestId CompilerLanguageService::requestHover(std::string path,
                                                CompilerTextPosition position) {
  const auto [id, needs_analysis] = impl_->enqueue(
      CompilerLanguageQueryKind::Hover, std::move(path), position, false);
  if (needs_analysis)
    (void)requestDiagnostics();
  return id;
}

CompilerRequestId
CompilerLanguageService::requestDefinition(std::string path,
                                       CompilerTextPosition position) {
  const auto [id, needs_analysis] = impl_->enqueue(
      CompilerLanguageQueryKind::Definition, std::move(path), position, false);
  if (needs_analysis)
    (void)requestDiagnostics();
  return id;
}

CompilerRequestId CompilerLanguageService::requestReferences(std::string path,
                                                     CompilerTextPosition position,
                                                     bool include_declaration) {
  const auto [id, needs_analysis] =
      impl_->enqueue(CompilerLanguageQueryKind::References, std::move(path),
                     position, include_declaration);
  if (needs_analysis)
    (void)requestDiagnostics();
  return id;
}

CompilerRequestId
CompilerLanguageService::requestCompletion(std::string path,
                                       CompilerTextPosition position) {
  std::unique_lock request_lock(impl_->completion_request_mutex);
  path = normalizeCompilerInputPath(path);
  CompilerLanguageService::Impl::PendingQuery query;
  CompilerDaemonRequest request;
  bool submit = false;
  std::vector<CompilerLanguageService::Impl::PendingQuery> superseded;
  {
    std::lock_guard lock(impl_->mutex);
    for (auto &[unused, pending] : impl_->completion_queries)
      superseded.push_back(std::move(pending));
    impl_->completion_queries.clear();
    query = {.id = ++impl_->compiler_query_id,
             .generation = impl_->generation,
             .kind = CompilerLanguageQueryKind::Completion,
             .path = path,
             .position = position};
    const auto document = impl_->documents.find(path);
    if (document != impl_->documents.end()) {
      std::uint32_t offset = 0;
      std::string probe_error;
      if (nextTextPositionToOffset(document->second.text, position, offset,
                                   probe_error)) {
        auto prefix_start = static_cast<std::size_t>(offset);
        const auto identifier_continue = [](unsigned char value) {
          return std::isalnum(value) != 0 || value == '_';
        };
        while (prefix_start != 0 &&
               identifier_continue(static_cast<unsigned char>(
                   document->second.text[prefix_start - 1])))
          --prefix_start;
        auto prefix_end = static_cast<std::size_t>(offset);
        while (prefix_end < document->second.text.size() &&
               identifier_continue(static_cast<unsigned char>(
                   document->second.text[prefix_end])))
          ++prefix_end;
        query.completion_prefix = document->second.text.substr(
            prefix_start, static_cast<std::size_t>(offset) - prefix_start);
        if (path.ends_with(".cfdl")) {
          compiler::SourceBuffer source(
              compiler::SourceInput(path, std::string(document->second.text)));
          compiler::CFDLSyntaxFile syntax;
          std::vector<compiler::CFDLDiagnostic> diagnostics;
          (void)compiler::parseCFDL(source, syntax, diagnostics);
          for (auto candidate : compiler::cfdlCompletionCandidates(syntax)) {
            if (candidate.starts_with(query.completion_prefix))
              query.direct_completion_items.push_back(
                  {.label = std::move(candidate),
                   .detail = "CFDL operation capability",
                   .kind = CompilerCompletionItemKind::Function});
          }
        }
        const auto before =
            std::string_view(document->second.text).substr(0, prefix_start);
        const auto member_context =
            (!before.empty() && before.back() == '.') ||
            (before.size() >= 2 && before.substr(before.size() - 2) == "::");
        if (member_context && query.direct_completion_items.empty()) {
          auto probe_text = document->second.text;
          probe_text.replace(prefix_start, prefix_end - prefix_start,
                             "__chtholly_tooling_probe()");
          const std::array probe_changes{
              CompilerOverlayChange::replace(path, probe_text)};
          auto probe_overlay =
              impl_->overlay->withChanges(probe_changes, probe_error);
          if (probe_overlay) {
            request.invocation = impl_->invocation;
            request.input_files = std::move(probe_overlay);
            request.overlay_generation = impl_->generation;
            for (const auto &[document_path, open_document] : impl_->documents)
              request.document_versions.emplace(document_path,
                                                open_document.version);
            impl_->completion_queries.emplace(query.id, query);
            submit = true;
          }
        }
      }
    }
  }
  impl_->completion_coordinator.cancelCurrent();
  const auto query_id = query.id;
  if (submit) {
    (void)impl_->completion_coordinator.submit(
        std::move(request),
        [this, query_id](CompilerRequestId, CompilerOverlayGeneration generation,
                         CompilerCheckExecutionResult result) mutable {
          std::optional<CompilerLanguageService::Impl::PendingQuery> pending;
          {
            std::lock_guard lock(impl_->mutex);
            const auto found = impl_->completion_queries.find(query_id);
            if (found == impl_->completion_queries.end())
              return;
            pending = std::move(found->second);
            impl_->completion_queries.erase(found);
          }
          if (generation != pending->generation) {
            impl_->complete(std::move(*pending), {},
                            CompilerDaemonRequestStatus::Stale,
                            "compiler document changed before completion finished");
            return;
          }
          impl_->complete(std::move(*pending), result.symbol_index,
                          result.symbol_index
                              ? CompilerDaemonRequestStatus::Succeeded
                              : result.status,
                          result.symbol_index ? std::string{} : result.error);
        });
  }
  request_lock.unlock();
  for (auto &pending : superseded)
    impl_->complete(std::move(pending), {}, CompilerDaemonRequestStatus::Cancelled,
                    "compiler completion query was superseded");
  if (!submit) {
    impl_->complete(std::move(query), {}, CompilerDaemonRequestStatus::Succeeded);
    return query_id;
  }
  return query_id;
}

CompilerRequestId
CompilerLanguageService::requestDocumentSymbols(std::string path) {
  const auto [id, needs_analysis] = impl_->enqueue(
      CompilerLanguageQueryKind::DocumentSymbols, std::move(path), {}, false);
  if (needs_analysis)
    (void)requestDiagnostics();
  return id;
}

CompilerRequestId
CompilerLanguageService::requestPrepareRename(std::string path,
                                          CompilerTextPosition position) {
  const auto [id, needs_analysis] = impl_->enqueue(
      CompilerLanguageQueryKind::PrepareRename, std::move(path), position, false);
  if (needs_analysis)
    (void)requestDiagnostics();
  return id;
}

CompilerRequestId CompilerLanguageService::requestRename(std::string path,
                                                 CompilerTextPosition position,
                                                 std::string new_name) {
  const auto [id, needs_analysis] = impl_->enqueue(
      CompilerLanguageQueryKind::Rename, std::move(path), position, false,
      std::move(new_name));
  if (needs_analysis)
    (void)requestDiagnostics();
  return id;
}

void CompilerLanguageService::cancelRequest(CompilerRequestId request_id) {
  std::optional<CompilerLanguageService::Impl::PendingQuery> cancelled;
  {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->pending_queries.find(request_id);
    if (found != impl_->pending_queries.end()) {
      cancelled = std::move(found->second);
      impl_->pending_queries.erase(found);
    } else if (const auto completion =
                   impl_->completion_queries.find(request_id);
               completion != impl_->completion_queries.end()) {
      cancelled = std::move(completion->second);
      impl_->completion_queries.erase(completion);
      impl_->completion_coordinator.cancelCurrent();
    }
  }
  if (!cancelled)
    return;
  impl_->complete(std::move(*cancelled), {}, CompilerDaemonRequestStatus::Cancelled,
                  "compiler language query was cancelled");
}

void CompilerLanguageService::cancelCurrent() {
  std::vector<CompilerLanguageService::Impl::PendingQuery> cancelled;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->analysis_pending = false;
    impl_->pending_analysis_request = 0;
    for (auto &[unused, query] : impl_->pending_queries)
      cancelled.push_back(std::move(query));
    impl_->pending_queries.clear();
    for (auto &[unused, query] : impl_->completion_queries)
      cancelled.push_back(std::move(query));
    impl_->completion_queries.clear();
    impl_->coordinator.cancelCurrent();
    impl_->completion_coordinator.cancelCurrent();
  }
  for (auto &query : cancelled)
    impl_->complete(std::move(query), {}, CompilerDaemonRequestStatus::Cancelled,
                    "compiler language analysis was cancelled");
}

CompilerOverlayGeneration CompilerLanguageService::overlayGeneration() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->generation;
}

std::shared_ptr<const CompilerRequestSnapshot>
CompilerLanguageService::latestPublishedSnapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->latest_snapshot;
}

} // namespace chtholly
