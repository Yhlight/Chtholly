#pragma once

#include "chtholly/Compiler/AnalysisMetrics.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"
#include "chtholly/Compiler/ComponentABI.h"
#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/IncrementalDependencies.h"
#include "chtholly/Compiler/LLVM.h"
#include "chtholly/Compiler/ModuleEmission.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/Source.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly::compiler {

class LowIR;
class ParseTree;
class SemIR;
struct CFDLSyntaxFile;

enum class ObjectArtifactLoadStatus : std::uint8_t {
  Found,
  Missing,
  Corrupt,
  Error,
};

struct ObjectArtifactLoadResult {
  ObjectArtifactLoadStatus status = ObjectArtifactLoadStatus::Missing;
  std::string bytes;
  std::string error;
};

struct ObjectArtifactLoadRequest {
  StableFingerprint fingerprint;
  StableFingerprint specific_fingerprint;
};

using ObjectArtifactLoader = std::function<ObjectArtifactLoadResult(
    const StableFingerprint &fingerprint,
    const StableFingerprint &specific_fingerprint)>;

using ObjectArtifactBatchLoader =
    std::function<std::vector<ObjectArtifactLoadResult>(
        std::span<const ObjectArtifactLoadRequest> requests)>;

struct NominalSemanticWitnessLoadResult {
  std::optional<NominalSemanticWitnessArtifact> artifact;
  std::string error;
};

using NominalSemanticWitnessLoader =
    std::function<std::optional<NominalSemanticWitnessArtifact>(
        const StableFingerprint &request_fingerprint, std::string &error)>;

using NominalSemanticWitnessBatchLoader =
    std::function<std::vector<NominalSemanticWitnessLoadResult>(
        std::span<const StableFingerprint> request_fingerprints)>;

struct CompilationRequest {
  enum class Mode : std::uint8_t { Check, Build };

  Mode mode = Mode::Build;
  DebugInfoMode debug_info = DebugInfoMode::None;
  LLVMOptimizationLevel optimization = LLVMOptimizationLevel::O0;
  const CompilerPackageArtifactManifest *previous_manifest = nullptr;
  std::vector<const CompilerPackageArtifactManifest *> dependency_manifests;
  // Implementation-only transitive artifacts used to resolve exported
  // generic template callees. They do not participate in source import lookup.
  std::vector<const CompilerPackageArtifactManifest *> template_dependency_closure;
  std::vector<const CompilerPackageCheckArtifact *> dependency_check_artifacts;
  std::vector<const CompilerPackageCheckArtifact *>
      template_dependency_check_closure;
  std::vector<CheckIRId> required_transient_outputs;
  std::string component_identity;
  std::vector<std::string> component_exports;
  // Indexed by CheckIRId. Missing entries default to Library.
  std::vector<ModuleEmissionRole> unit_emission_roles;
  ObjectArtifactLoader load_object;
  ObjectArtifactBatchLoader load_objects;
  ConcreteSpecializationLoader load_specialization;
  NominalSemanticWitnessLoader load_nominal_semantic_witness;
  NominalSemanticWitnessBatchLoader load_nominal_semantic_witnesses;
  std::function<bool()> is_cancelled;
};

enum class CompilationOutcome : std::uint8_t { Success, Failed, Cancelled };

class CompilationUnit {
public:
  CompilationUnit(CompilationUnit &&) noexcept;
  CompilationUnit &operator=(CompilationUnit &&) noexcept;
  ~CompilationUnit();

  [[nodiscard]] bool success() const;
  [[nodiscard]] bool wasReused() const;
  [[nodiscard]] bool hasEntryPoint() const;
  [[nodiscard]] CompilationUnitKind kind() const;
  [[nodiscard]] CheckIRId checkIRId() const;
  [[nodiscard]] std::string_view sourcePath() const;
  [[nodiscard]] const SourceBuffer &source() const;
  [[nodiscard]] const ParseTree *parseTree() const;
  [[nodiscard]] const CFDLSyntaxFile *cfdlSyntax() const;
  [[nodiscard]] std::span<const Diagnostic> diagnostics() const;
  [[nodiscard]] std::string_view moduleName() const;
  [[nodiscard]] const SemIR *semIR() const;
  [[nodiscard]] PublicInterfaceId publicInterfaceId() const;
  [[nodiscard]] const PublicInterface *publicInterface() const;
  [[nodiscard]] StableFingerprint publicInterfaceFingerprint() const;
  [[nodiscard]] const LowIR *lowIR() const;
  [[nodiscard]] std::span<const NominalTypeSpecificArtifact>
  nominalTypeSpecificArtifacts() const;
  [[nodiscard]] std::span<const NominalSemanticWitnessArtifact>
  nominalSemanticWitnessArtifacts() const;
  [[nodiscard]] std::span<const NominalTypeLayoutArtifact>
  nominalTypeLayoutArtifacts() const;
  [[nodiscard]] std::span<const ComponentExportLoweringPlan>
  componentExports() const;
  [[nodiscard]] std::string printTokens() const;
  [[nodiscard]] std::string printParseTree() const;
  [[nodiscard]] std::string printSemIR() const;
  [[nodiscard]] std::string printPublicInterface() const;
  [[nodiscard]] std::string printLowIR() const;
  [[nodiscard]] std::string printForeignProtocols() const;
  [[nodiscard]] std::string printLLVM() const;
  [[nodiscard]] std::string emitObject(std::string &error) const;
  [[nodiscard]] std::string metricsJson() const;
  [[nodiscard]] std::string analysisMetricsJson() const;

private:
  struct Impl;
  explicit CompilationUnit(std::unique_ptr<Impl> impl);
  friend class CompilationSession;
  std::unique_ptr<Impl> impl_;
};

class CompilationSession {
public:
  explicit CompilationSession(
      std::string target_triple, std::string package_name = "main",
      std::vector<std::string> resolved_features = {},
      StableFingerprint compile_toolchain_fingerprint =
          defaultCompileToolchainFingerprint(),
      PackageProvenance provenance = {},
      LanguageContract language_contract = CurrentLanguageContract,
      std::vector<CompilerIntrinsicBinding> compiler_intrinsics = {},
      std::optional<CFFIReceiptIdentity> cffi_identity = std::nullopt);
  CompilationSession(CompilationSession &&) noexcept;
  CompilationSession &operator=(CompilationSession &&) noexcept;
  ~CompilationSession();

  [[nodiscard]] CheckIRId
  addUnit(SourceInput source,
          CompilationUnitKind kind = CompilationUnitKind::ChthollySource);
  [[nodiscard]] bool setRuntimeSymbolMappings(
      std::vector<std::pair<std::string, std::string>> mappings,
      std::string &error);
  [[nodiscard]] CompilationOutcome
  compileRequest(std::string &error, const CompilationRequest &request = {});
  [[nodiscard]] bool compile(std::string &error,
                             const CompilationRequest &request = {});
  [[nodiscard]] bool loadInteropBundle(const std::string &path,
                                       std::string_view expected_package,
                                       std::string &error);
  [[nodiscard]] bool exportInteropBundle(const std::string &path,
                                         std::string &error) const;
  [[nodiscard]] const CompilationUnit &unit(CheckIRId id) const;
  [[nodiscard]] std::size_t unitCount() const;
  [[nodiscard]] const PublicInterfaceRegistry &publicInterfaces() const;
  [[nodiscard]] const IncrementalCompilationPlan &compilationPlan() const;
  [[nodiscard]] const CompilerPackageCheckArtifact &packageCheckArtifact() const;
  [[nodiscard]] const CompilerPackageArtifactManifest &packageManifest() const;
  [[nodiscard]] std::string printForeignProtocols() const;
  [[nodiscard]] std::string metricsJson() const;
  [[nodiscard]] std::string analysisMetricsJson() const;
  [[nodiscard]] std::vector<UnitAnalysisMetrics> analysisMetricUnits() const;
  [[nodiscard]] std::optional<ComponentContractArtifact>
  componentContract(std::string &error) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly::compiler
