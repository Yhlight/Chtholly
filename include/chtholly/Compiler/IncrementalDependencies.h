#pragma once

#include "chtholly/Basic/LanguageVersion.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Compiler/CFFIIdentity.h"
#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Compiler/ModuleEmission.h"
#include "chtholly/Compiler/Source.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

enum class PackageProvenanceKind : std::uint8_t {
  Workspace,
  ToolchainStandardLibrary,
  Count,
};

struct PackageProvenance {
  PackageProvenanceKind kind = PackageProvenanceKind::Workspace;
  StableFingerprint contract_fingerprint;

  friend bool operator==(const PackageProvenance &,
                         const PackageProvenance &) = default;
};

struct ModuleIdentity {
  std::string package_name;
  std::string module_name;

  friend bool operator==(const ModuleIdentity &,
                         const ModuleIdentity &) = default;
  friend auto operator<=>(const ModuleIdentity &,
                          const ModuleIdentity &) = default;
};

enum class DependencyObservationKind : std::uint8_t {
  ModulePresence,
  EntityBinding,
  ExportSet,
  LifecycleCallable,
  NominalBinding,
  RelocationClosure,
  Count,
};

enum class LifecycleObservationRole : std::uint8_t {
  Copy,
  Drop,
  Count,
};

struct DependencyObservation {
  DependencyObservationKind kind = DependencyObservationKind::Count;
  ModuleIdentity provider;
  std::string binding_name;
  StableFingerprint expected_fingerprint;
  LifecycleObservationRole lifecycle_role = LifecycleObservationRole::Count;
  StableFingerprint witness_fingerprint;
  StableFingerprint specific_closure_fingerprint;
  ModuleIdentity canonical_provider;
  std::string canonical_name;

  friend bool operator==(const DependencyObservation &,
                         const DependencyObservation &) = default;
};

[[nodiscard]] StableFingerprint
fingerprintRelocationClosure(const PublicNominalTypeArtifact &nominal);

struct NominalArtifactReference {
  StableFingerprint request_fingerprint;
  StableFingerprint result_fingerprint;

  friend bool operator==(const NominalArtifactReference &,
                         const NominalArtifactReference &) = default;
};

struct ForeignSymbolRequirement {
  std::string logical_name;
  std::string external_symbol;
  ForeignCallingConvention calling_convention = ForeignCallingConvention::C;
  StableFingerprint signature_fingerprint;

  friend bool operator==(const ForeignSymbolRequirement &,
                         const ForeignSymbolRequirement &) = default;
  friend auto operator<=>(const ForeignSymbolRequirement &lhs,
                          const ForeignSymbolRequirement &rhs) {
    return std::tuple(lhs.external_symbol, lhs.logical_name,
                      lhs.calling_convention,
                      lhs.signature_fingerprint.hex()) <=>
           std::tuple(rhs.external_symbol, rhs.logical_name,
                      rhs.calling_convention, rhs.signature_fingerprint.hex());
  }
};

struct PackageModuleArtifact {
  StableFingerprint source_fingerprint;
  CompilationUnitKind unit_kind = CompilationUnitKind::ChthollySource;
  ModuleEmissionRole emission_role = ModuleEmissionRole::Library;
  PublicInterfaceArtifact public_interface;
  // Canonical module-level link closure. These are persisted so an
  // artifact-only consumer never has to rediscover source imports.
  std::vector<ModuleIdentity> module_dependencies;
  std::vector<ForeignSymbolRequirement> required_foreign_symbols;
  std::vector<DependencyObservation> observations;
  std::vector<ConcreteSpecializationReference> specializations;
  std::vector<NominalArtifactReference> nominal_type_specifics;
  std::vector<NominalArtifactReference> nominal_semantic_witnesses;
  std::vector<NominalArtifactReference> nominal_type_layouts;
  StableFingerprint specific_fingerprint;
  StableFingerprint object_fingerprint;

  [[nodiscard]] std::string_view moduleName() const {
    return public_interface.moduleName();
  }
  [[nodiscard]] bool verify(std::string &error) const;
};

struct PackageManifestDependency {
  std::string package_name;
  StableFingerprint manifest_fingerprint;

  friend bool operator==(const PackageManifestDependency &,
                         const PackageManifestDependency &) = default;
};

struct PackageCheckDependency {
  std::string package_name;
  StableFingerprint artifact_fingerprint;

  friend bool operator==(const PackageCheckDependency &,
                         const PackageCheckDependency &) = default;
};

struct PackageModuleCheckArtifact {
  StableFingerprint source_fingerprint;
  CompilationUnitKind unit_kind = CompilationUnitKind::ChthollySource;
  PublicInterfaceArtifact public_interface;
  std::vector<ModuleIdentity> module_dependencies;
  std::vector<ForeignSymbolRequirement> required_foreign_symbols;
  std::vector<DependencyObservation> observations;
  StableFingerprint specific_fingerprint;

  [[nodiscard]] std::string_view moduleName() const {
    return public_interface.moduleName();
  }
  [[nodiscard]] bool verify(std::string &error) const;
};

class CompilerPackageCheckArtifact {
public:
  CompilerPackageCheckArtifact() = default;
  CompilerPackageCheckArtifact(
      std::string package_name, std::string target_triple,
      StableFingerprint compile_toolchain_fingerprint,
      std::vector<std::string> resolved_features,
      std::vector<PackageCheckDependency> direct_dependencies,
      std::vector<PackageModuleCheckArtifact> modules,
      PackageProvenance provenance = {},
      LanguageContract language_contract = CurrentLanguageContract,
      std::optional<CFFIReceiptIdentity> cffi_identity = std::nullopt);

  [[nodiscard]] std::string_view packageName() const {
    return package_name_;
  }
  [[nodiscard]] std::string_view targetTriple() const {
    return target_triple_;
  }
  [[nodiscard]] const StableFingerprint &compileToolchainFingerprint() const {
    return compile_toolchain_fingerprint_;
  }
  [[nodiscard]] const PackageProvenance &provenance() const {
    return provenance_;
  }
  [[nodiscard]] const LanguageContract &languageContract() const {
    return language_contract_;
  }
  [[nodiscard]] const std::optional<CFFIReceiptIdentity> &cffiIdentity() const {
    return cffi_identity_;
  }
  [[nodiscard]] std::span<const std::string> resolvedFeatures() const {
    return resolved_features_;
  }
  [[nodiscard]] std::span<const PackageCheckDependency>
  directDependencies() const {
    return direct_dependencies_;
  }
  [[nodiscard]] std::span<const PackageModuleCheckArtifact> modules() const {
    return modules_;
  }
  [[nodiscard]] const PackageModuleCheckArtifact *
  findModule(std::string_view name) const;
  [[nodiscard]] StableFingerprint fingerprint() const;
  [[nodiscard]] bool verify(std::string &error) const;

private:
  std::string package_name_;
  std::string target_triple_;
  StableFingerprint compile_toolchain_fingerprint_;
  PackageProvenance provenance_;
  LanguageContract language_contract_;
  std::optional<CFFIReceiptIdentity> cffi_identity_;
  std::vector<std::string> resolved_features_;
  std::vector<PackageCheckDependency> direct_dependencies_;
  std::vector<PackageModuleCheckArtifact> modules_;
};

class CompilerPackageArtifactManifest {
public:
  CompilerPackageArtifactManifest() = default;
  CompilerPackageArtifactManifest(
      std::string package_name, std::string target_triple,
      StableFingerprint compile_toolchain_fingerprint,
      std::vector<std::string> resolved_features,
      std::vector<PackageManifestDependency> direct_dependencies,
      std::vector<PackageModuleArtifact> modules,
      PackageProvenance provenance = {},
      LanguageContract language_contract = CurrentLanguageContract,
      std::optional<CFFIReceiptIdentity> cffi_identity = std::nullopt);

  [[nodiscard]] std::string_view packageName() const {
    return package_name_;
  }

  [[nodiscard]] std::string_view targetTriple() const {
    return target_triple_;
  }
  [[nodiscard]] const StableFingerprint &configurationFingerprint() const {
    return configuration_fingerprint_;
  }
  [[nodiscard]] const StableFingerprint &compileToolchainFingerprint() const {
    return compile_toolchain_fingerprint_;
  }
  [[nodiscard]] const PackageProvenance &provenance() const {
    return provenance_;
  }
  [[nodiscard]] const LanguageContract &languageContract() const {
    return language_contract_;
  }
  [[nodiscard]] const std::optional<CFFIReceiptIdentity> &cffiIdentity() const {
    return cffi_identity_;
  }
  [[nodiscard]] std::span<const std::string> resolvedFeatures() const {
    return resolved_features_;
  }
  [[nodiscard]] std::span<const PackageManifestDependency>
  directDependencies() const {
    return direct_dependencies_;
  }
  [[nodiscard]] const PackageManifestDependency *
  findDependency(std::string_view package_name) const;
  [[nodiscard]] std::span<const PackageModuleArtifact> modules() const {
    return modules_;
  }
  [[nodiscard]] const PackageModuleArtifact *
  findModule(std::string_view name) const;
  [[nodiscard]] StableFingerprint fingerprint() const;
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] bool verifyDependencies(
      std::span<const CompilerPackageArtifactManifest *const> dependencies,
      std::string &error) const;
  [[nodiscard]] std::string encode(std::string &error) const;
  [[nodiscard]] static std::optional<CompilerPackageArtifactManifest>
  decode(std::string_view bytes, std::string &error);
  [[nodiscard]] static std::optional<CompilerPackageArtifactManifest>
  load(const std::string &path, std::string &error);
  [[nodiscard]] bool save(const std::string &path, std::string &error) const;
  [[nodiscard]] std::string print() const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  std::string package_name_;
  std::string target_triple_;
  StableFingerprint compile_toolchain_fingerprint_;
  PackageProvenance provenance_;
  LanguageContract language_contract_;
  std::optional<CFFIReceiptIdentity> cffi_identity_;
  StableFingerprint configuration_fingerprint_;
  std::vector<std::string> resolved_features_;
  std::vector<PackageManifestDependency> direct_dependencies_;
  std::vector<PackageModuleArtifact> modules_;
};

enum class UnitCompilationAction : std::uint8_t {
  Rebuild,
  Reuse,
  Removed,
  Count,
};

enum class UnitInvalidationReason : std::uint8_t {
  ModuleAdded,
  ModuleRemoved,
  SourceChanged,
  SourceKindChanged,
  ObjectEmissionRoleChanged,
  CompilationConfigurationChanged,
  RequestedOutput,
  ObjectArtifactMissing,
  ObjectArtifactCorrupt,
  ImportedModuleRemoved,
  EntityBindingChanged,
  LifecycleCallableChanged,
  ExportSetChanged,
  NominalBindingChanged,
  RelocationClosureChanged,
  Count,
};

struct UnitInvalidation {
  UnitInvalidationReason reason = UnitInvalidationReason::Count;
  ModuleIdentity provider;
  std::string binding_name;

  friend bool operator==(const UnitInvalidation &,
                         const UnitInvalidation &) = default;
};

struct UnitCompilationDecision {
  std::string module_name;
  UnitCompilationAction action = UnitCompilationAction::Count;
  std::vector<UnitInvalidation> invalidations;

  friend bool operator==(const UnitCompilationDecision &,
                         const UnitCompilationDecision &) = default;
};

class IncrementalCompilationPlan {
public:
  IncrementalCompilationPlan() = default;
  explicit IncrementalCompilationPlan(
      std::vector<UnitCompilationDecision> decisions);

  [[nodiscard]] std::span<const UnitCompilationDecision> decisions() const {
    return decisions_;
  }
  [[nodiscard]] const UnitCompilationDecision *
  find(std::string_view module_name) const;
  [[nodiscard]] bool rebuilds(std::string_view module_name) const;
  [[nodiscard]] bool reuses(std::string_view module_name) const;
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string print() const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  std::vector<UnitCompilationDecision> decisions_;
};

[[nodiscard]] StableFingerprint fingerprintSource(std::string_view package_name,
                                                  std::string_view module_name,
                                                  CompilationUnitKind unit_kind,
                                                  std::string_view source_text);
[[nodiscard]] StableFingerprint
fingerprintModuleIdentity(std::string_view package_name,
                          std::string_view module_name);
[[nodiscard]] StableFingerprint fingerprintCompilationConfiguration(
    std::string_view package_name, std::string_view target_triple,
    const StableFingerprint &compile_toolchain_fingerprint,
    std::span<const std::string> resolved_features = {},
    PackageProvenance provenance = {},
    LanguageContract language_contract = CurrentLanguageContract,
    const std::optional<CFFIReceiptIdentity> &cffi_identity = std::nullopt);
[[nodiscard]] StableFingerprint defaultCompileToolchainFingerprint();
[[nodiscard]] StableFingerprint
fingerprintObject(std::string_view target_triple, std::string_view object_bytes,
                  StableFingerprint specific_fingerprint);
[[nodiscard]] std::string_view
dependencyObservationKindName(DependencyObservationKind kind);
[[nodiscard]] std::string_view
unitCompilationActionName(UnitCompilationAction action);
[[nodiscard]] std::string_view
unitInvalidationReasonName(UnitInvalidationReason reason);

} // namespace chtholly::compiler
