#pragma once

#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Driver/CompilerArtifactLoadMetrics.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

struct CompilerSessionArtifactReference {
  std::string target_triple;
  std::string root_package;
  compiler::StableFingerprint root_manifest;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode(std::string &error) const;
  [[nodiscard]] static std::optional<CompilerSessionArtifactReference>
  decode(std::string_view bytes, std::string &error);

  friend bool operator==(const CompilerSessionArtifactReference &,
                         const CompilerSessionArtifactReference &) = default;
};

struct CompilerPublishedObject {
  compiler::StableFingerprint fingerprint;
  compiler::StableFingerprint specific_fingerprint;
  std::string target_triple;
  std::string extension;
  std::string bytes;
};

struct CompilerPublishedSpecialization {
  compiler::ConcreteSpecializationComponentArtifact component;
};

struct CompilerPublishedNominalTypeSpecific {
  compiler::NominalTypeSpecificArtifact artifact;
};

struct CompilerPublishedNominalSemanticWitness {
  compiler::NominalSemanticWitnessArtifact artifact;
};

struct CompilerPublishedNominalTypeLayout {
  compiler::NominalTypeLayoutArtifact artifact;
};

struct CompilerGarbageCollectionReport {
  bool ran = false;
  // A failed collection never mutates the namespace.  Keep this explicit so
  // callers can distinguish a skipped/failed observation from a completed GC.
  bool valid = true;
  std::string recovery_instruction;
  std::size_t manifest_count = 0;
  std::size_t object_count = 0;
  std::size_t specialization_component_count = 0;
  std::size_t specialization_index_count = 0;
  std::size_t nominal_type_specific_count = 0;
  std::size_t nominal_type_specific_index_count = 0;
  std::size_t nominal_semantic_witness_count = 0;
  std::size_t nominal_semantic_witness_index_count = 0;
  std::size_t nominal_type_layout_count = 0;
  std::size_t nominal_type_layout_index_count = 0;
  std::size_t active_lease_count = 0;
  std::size_t stale_lease_count = 0;
  std::uintmax_t manifest_total_bytes = 0;
  std::uintmax_t manifest_reachable_bytes = 0;
  std::uintmax_t manifest_unreachable_bytes = 0;
  std::uintmax_t object_total_bytes = 0;
  std::uintmax_t object_reachable_bytes = 0;
  std::uintmax_t object_unreachable_bytes = 0;
  std::uintmax_t specialization_component_total_bytes = 0;
  std::uintmax_t specialization_component_reachable_bytes = 0;
  std::uintmax_t specialization_component_unreachable_bytes = 0;
  std::uintmax_t specialization_index_total_bytes = 0;
  std::uintmax_t specialization_index_reachable_bytes = 0;
  std::uintmax_t specialization_index_unreachable_bytes = 0;
  std::uintmax_t nominal_type_specific_total_bytes = 0;
  std::uintmax_t nominal_type_specific_reachable_bytes = 0;
  std::uintmax_t nominal_type_specific_unreachable_bytes = 0;
  std::uintmax_t nominal_type_specific_index_total_bytes = 0;
  std::uintmax_t nominal_type_specific_index_reachable_bytes = 0;
  std::uintmax_t nominal_type_specific_index_unreachable_bytes = 0;
  std::uintmax_t nominal_semantic_witness_total_bytes = 0;
  std::uintmax_t nominal_semantic_witness_reachable_bytes = 0;
  std::uintmax_t nominal_semantic_witness_unreachable_bytes = 0;
  std::uintmax_t nominal_semantic_witness_index_total_bytes = 0;
  std::uintmax_t nominal_semantic_witness_index_reachable_bytes = 0;
  std::uintmax_t nominal_semantic_witness_index_unreachable_bytes = 0;
  std::uintmax_t nominal_type_layout_total_bytes = 0;
  std::uintmax_t nominal_type_layout_reachable_bytes = 0;
  std::uintmax_t nominal_type_layout_unreachable_bytes = 0;
  std::uintmax_t nominal_type_layout_index_total_bytes = 0;
  std::uintmax_t nominal_type_layout_index_reachable_bytes = 0;
  std::uintmax_t nominal_type_layout_index_unreachable_bytes = 0;
  std::uintmax_t quarantine_bytes = 0;
  std::uintmax_t reclaimed_bytes = 0;
};

class CompilerArtifactLease {
public:
  CompilerArtifactLease(CompilerArtifactLease &&) noexcept;
  CompilerArtifactLease &operator=(CompilerArtifactLease &&) noexcept;
  ~CompilerArtifactLease();

  [[nodiscard]] bool valid() const;
  [[nodiscard]] const std::map<std::string, compiler::CompilerPackageArtifactManifest> &
  previousManifests() const;
  [[nodiscard]] compiler::ObjectArtifactLoadResult
  loadObject(const compiler::StableFingerprint &fingerprint,
             const compiler::StableFingerprint &specific_fingerprint,
             std::string_view target_triple, std::string_view extension,
             CompilerArtifactLoadMetrics *metrics = nullptr) const;
  [[nodiscard]] compiler::ConcreteSpecializationLoadResult
  loadSpecialization(const compiler::StableFingerprint &request_fingerprint,
                     CompilerArtifactLoadMetrics *metrics = nullptr) const;
  [[nodiscard]] std::optional<compiler::NominalTypeSpecificArtifact>
  loadNominalTypeSpecific(const compiler::StableFingerprint &request_fingerprint,
                          std::string &error) const;
  [[nodiscard]] std::optional<compiler::NominalSemanticWitnessArtifact>
  loadNominalSemanticWitness(const compiler::StableFingerprint &request_fingerprint,
                             std::string &error) const;
  [[nodiscard]] std::optional<compiler::NominalTypeLayoutArtifact>
  loadNominalTypeLayout(const compiler::StableFingerprint &request_fingerprint,
                        std::string &error) const;

private:
  struct Impl;
  explicit CompilerArtifactLease(std::unique_ptr<Impl> impl);
  friend class CompilerArtifactStore;
  std::unique_ptr<Impl> impl_;
};

class CompilerArtifactStore {
public:
  explicit CompilerArtifactStore(std::string root);

  [[nodiscard]] std::unique_ptr<CompilerArtifactLease>
  acquireLease(std::string_view session_key, std::string_view expected_target,
               std::string_view expected_root, std::string &error) const;
  [[nodiscard]] bool
  publish(CompilerArtifactLease &lease,
          const compiler::CompilerPackageArtifactManifest &root_manifest,
          std::span<const compiler::CompilerPackageArtifactManifest *const> manifests,
          std::span<const CompilerPublishedObject> objects,
          std::span<const CompilerPublishedSpecialization> specializations,
          std::span<const CompilerPublishedNominalTypeSpecific> nominal_specifics,
          std::span<const CompilerPublishedNominalSemanticWitness>
              nominal_semantic_witnesses,
          std::span<const CompilerPublishedNominalTypeLayout> nominal_layouts,
          std::string &error) const;
  [[nodiscard]] bool
  publish(CompilerArtifactLease &lease,
          const compiler::CompilerPackageArtifactManifest &root_manifest,
          std::span<const compiler::CompilerPackageArtifactManifest *const> manifests,
          std::span<const CompilerPublishedObject> objects,
          std::span<const CompilerPublishedSpecialization> specializations,
          std::string &error) const {
    return publish(lease, root_manifest, manifests, objects, specializations,
                   {}, {}, {}, error);
  }
  [[nodiscard]] bool collectGarbage(bool force,
                                    std::chrono::seconds minimum_interval,
                                    CompilerGarbageCollectionReport &report,
                                    std::string &error) const;
  [[nodiscard]] std::string
  objectPath(const compiler::StableFingerprint &fingerprint,
             std::string_view extension) const;

private:
  std::string root_;
};

} // namespace chtholly
