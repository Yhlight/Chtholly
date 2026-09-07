#pragma once

#include "chtholly/Driver/WorkspaceArtifactTypes.h"
#include "chtholly/Driver/CompilerDiagnostics.h"

#include <memory>
#include <string>
#include <vector>

namespace chtholly {

struct CompilerInvocation;
class CompilerInputFileSystem;
class CompilerRequestSnapshot;

struct CompilerCompilerEnvironment {
  std::shared_ptr<const CompilerInputFileSystem> input_files;
  bool update_lockfile = true;
};

class CompilerPreparedRequest {
public:
  struct Impl;

  CompilerPreparedRequest(const CompilerPreparedRequest &) = delete;
  CompilerPreparedRequest &operator=(const CompilerPreparedRequest &) = delete;
  ~CompilerPreparedRequest();

  [[nodiscard]] const CompilerRequestSnapshot &snapshot() const;

private:
  explicit CompilerPreparedRequest(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend std::shared_ptr<const CompilerPreparedRequest>
  prepareNextCompilerRequest(const CompilerInvocation &,
                             const CompilerCompilerEnvironment &, std::string &);
  friend int runCompilerPipeline(
      const CompilerInvocation &, const CompilerCompilerEnvironment &,
      std::string &,
      std::vector<WorkspaceArtifactResult::InvalidationExplanation> *,
      std::vector<CompilerSourceDiagnostic> *);
  friend struct CompilerPreparedRequestAccess;
};

[[nodiscard]] std::shared_ptr<const CompilerPreparedRequest>
prepareNextCompilerRequest(const CompilerInvocation &invocation,
                           const CompilerCompilerEnvironment &environment,
                           std::string &error);

int runCompilerPipeline(
    const CompilerInvocation &invocation,
    const CompilerCompilerEnvironment &environment, std::string &error,
    std::vector<WorkspaceArtifactResult::InvalidationExplanation>
        *invalidation_explanations = nullptr,
    std::vector<CompilerSourceDiagnostic> *diagnostics = nullptr);

int runCompilerPipeline(
    const CompilerInvocation &invocation, std::string &error,
    std::vector<WorkspaceArtifactResult::InvalidationExplanation>
        *invalidation_explanations = nullptr,
    std::vector<CompilerSourceDiagnostic> *diagnostics = nullptr);

} // namespace chtholly
