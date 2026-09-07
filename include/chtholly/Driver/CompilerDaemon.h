#pragma once

#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/CompilerDiagnostics.h"
#include "chtholly/Driver/CompilerPipeline.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"
#include "chtholly/Driver/CompilerLanguageSupport.h"
#include "chtholly/Compiler/CompilationUnit.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chtholly {

struct CompilerCancellationState;

using CompilerRequestId = std::uint64_t;
using CompilerOverlayGeneration = std::uint64_t;
using CompilerDocumentVersion = std::int64_t;

class CompilerCancellationToken {
public:
  [[nodiscard]] bool isCancelled() const;

private:
  explicit CompilerCancellationToken(
      std::shared_ptr<const CompilerCancellationState> state)
      : state_(std::move(state)) {}
  std::shared_ptr<const CompilerCancellationState> state_;
  friend class CompilerCancellationSource;
};

class CompilerCancellationSource {
public:
  CompilerCancellationSource();
  [[nodiscard]] CompilerCancellationToken token() const;
  void cancel() const;

private:
  std::shared_ptr<CompilerCancellationState> state_;
};

struct CompilerDiagnosticBatch {
  CompilerRequestId request_id = 0;
  CompilerOverlayGeneration overlay_generation = 0;
  compiler::StableFingerprint request_fingerprint;
  std::string path;
  std::optional<CompilerDocumentVersion> version;
  std::vector<CompilerSourceDiagnostic> diagnostics;
};

enum class CompilerDaemonRequestStatus : std::uint8_t {
  Succeeded,
  Failed,
  Cancelled,
  Stale,
};

struct CompilerPackageQueryCacheStats {
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t entries = 0;
};

class CompilerPackageQueryCache {
public:
  explicit CompilerPackageQueryCache(std::size_t capacity = 128);
  ~CompilerPackageQueryCache();

  [[nodiscard]] std::shared_ptr<const compiler::CompilationSession>
  lookup(std::string_view key);
  void insert(std::string key,
              std::shared_ptr<const compiler::CompilationSession> result);
  [[nodiscard]] CompilerPackageQueryCacheStats stats() const;
  void clear();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct CompilerCheckExecutionResult {
  CompilerDaemonRequestStatus status = CompilerDaemonRequestStatus::Failed;
  std::shared_ptr<const CompilerRequestSnapshot> snapshot;
  std::vector<CompilerSourceDiagnostic> diagnostics;
  std::vector<std::shared_ptr<const compiler::CompilationSession>> package_sessions;
  std::shared_ptr<const CompilerWorkspaceSymbolIndex> symbol_index;
  std::string error;
  std::size_t cache_hits = 0;
  std::size_t cache_misses = 0;
};

[[nodiscard]] CompilerCheckExecutionResult
executeNextCheckRequest(const CompilerPreparedRequest &request,
                        CompilerPackageQueryCache &cache,
                        const CompilerCancellationToken &cancellation);

struct CompilerDaemonRequest {
  CompilerInvocation invocation;
  std::shared_ptr<const CompilerInputFileSystem> input_files;
  CompilerOverlayGeneration overlay_generation = 0;
  std::unordered_map<std::string, CompilerDocumentVersion> document_versions;
};

using CompilerDaemonCompletion = std::function<void(
    CompilerRequestId, CompilerOverlayGeneration, CompilerCheckExecutionResult)>;

class CompilerDaemonRequestCoordinator {
public:
  explicit CompilerDaemonRequestCoordinator(std::size_t cache_capacity = 128);
  ~CompilerDaemonRequestCoordinator();

  [[nodiscard]] CompilerRequestId submit(CompilerDaemonRequest request,
                                     CompilerDaemonCompletion completion);
  void cancelCurrent();
  [[nodiscard]] CompilerPackageQueryCacheStats cacheStats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct CompilerLanguageServiceCallbacks {
  std::function<void(CompilerDiagnosticBatch)> publish_diagnostics;
  std::function<void(CompilerRequestId, std::string)> publish_workspace_diagnostic;
  std::function<void(struct CompilerLanguageQueryResult)> complete_query;
};

enum class CompilerLanguageQueryKind : std::uint8_t {
  Hover,
  Definition,
  References,
  Completion,
  DocumentSymbols,
  PrepareRename,
  Rename,
};

struct CompilerLanguageQueryResult {
  CompilerRequestId request_id = 0;
  CompilerOverlayGeneration overlay_generation = 0;
  CompilerDaemonRequestStatus status = CompilerDaemonRequestStatus::Failed;
  CompilerLanguageQueryKind kind = CompilerLanguageQueryKind::Hover;
  std::optional<CompilerHoverResult> hover;
  std::vector<CompilerSourceLocation> locations;
  std::vector<CompilerCompletionItem> completion_items;
  std::vector<CompilerDocumentSymbol> document_symbols;
  std::optional<CompilerRenameResult> rename;
  std::string rename_text;
  std::string error;
};

class CompilerLanguageService {
public:
  CompilerLanguageService(CompilerInvocation invocation,
                      std::shared_ptr<const CompilerInputFileSystem> base_files,
                      CompilerLanguageServiceCallbacks callbacks,
                      std::size_t cache_capacity = 128);
  ~CompilerLanguageService();

  [[nodiscard]] bool openDocument(std::string path, CompilerDocumentVersion version,
                                  std::string text, std::string &error);
  [[nodiscard]] bool changeDocument(std::string path,
                                    CompilerDocumentVersion version,
                                    std::string text, std::string &error);
  [[nodiscard]] bool
  changeDocument(std::string path, CompilerDocumentVersion version,
                 std::span<const CompilerTextDocumentContentChange> changes,
                 std::string &error);
  [[nodiscard]] bool closeDocument(std::string path, std::string &error);
  [[nodiscard]] CompilerRequestId requestDiagnostics();
  [[nodiscard]] CompilerRequestId requestHover(std::string path,
                                           CompilerTextPosition position);
  [[nodiscard]] CompilerRequestId requestDefinition(std::string path,
                                                CompilerTextPosition position);
  [[nodiscard]] CompilerRequestId requestReferences(std::string path,
                                                CompilerTextPosition position,
                                                bool include_declaration);
  [[nodiscard]] CompilerRequestId requestCompletion(std::string path,
                                                CompilerTextPosition position);
  [[nodiscard]] CompilerRequestId requestDocumentSymbols(std::string path);
  [[nodiscard]] CompilerRequestId requestPrepareRename(std::string path,
                                                   CompilerTextPosition position);
  [[nodiscard]] CompilerRequestId requestRename(std::string path,
                                            CompilerTextPosition position,
                                            std::string new_name);
  void cancelRequest(CompilerRequestId request_id);
  void cancelCurrent();
  [[nodiscard]] CompilerOverlayGeneration overlayGeneration() const;
  [[nodiscard]] std::shared_ptr<const CompilerRequestSnapshot>
  latestPublishedSnapshot() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly
