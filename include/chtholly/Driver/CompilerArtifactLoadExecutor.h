#pragma once

#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Driver/CompilerArtifactLoadMetrics.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace chtholly {

class CompilerArtifactLoadExecutor {
public:
  explicit CompilerArtifactLoadExecutor(std::size_t worker_count,
                                    std::function<bool()> is_cancelled = {},
                                    std::shared_ptr<CompilerArtifactLoadMetrics>
                                        metrics = {});
  CompilerArtifactLoadExecutor(const CompilerArtifactLoadExecutor &) = delete;
  CompilerArtifactLoadExecutor &
  operator=(const CompilerArtifactLoadExecutor &) = delete;
  ~CompilerArtifactLoadExecutor();

  [[nodiscard]] std::vector<compiler::ObjectArtifactLoadResult>
  loadObjects(std::span<const compiler::ObjectArtifactLoadRequest> requests,
              const compiler::ObjectArtifactLoader &loader);
  [[nodiscard]]
  std::vector<compiler::NominalSemanticWitnessLoadResult>
  loadNominalSemanticWitnesses(
      std::span<const compiler::StableFingerprint> request_fingerprints,
      const compiler::NominalSemanticWitnessLoader &loader);
  [[nodiscard]] compiler::ConcreteSpecializationLoadResult
  loadSpecialization(const compiler::StableFingerprint &request_fingerprint,
                     const compiler::ConcreteSpecializationLoader &loader);

  void cancelPending();
  void drain();
  [[nodiscard]] bool isCancelled() const;
  [[nodiscard]] std::size_t workerCount() const;
  [[nodiscard]] std::size_t queueCapacity() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly
