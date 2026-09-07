#pragma once

#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Driver/CompilerArtifactStore.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace chtholly {

struct ArtifactReadResult {
  CompilerArtifactReadStatus status = CompilerArtifactReadStatus::Missing;
  std::string bytes;
  std::string error;
  std::uint64_t elapsed_nanoseconds = 0;
};

struct CompilerArtifactReadService {
  [[nodiscard]] static ArtifactReadResult
  read(const std::string &path, CompilerArtifactLoadMetrics *metrics);
};

struct CompilerArtifactWriteService {
  [[nodiscard]] static bool
  atomic(const std::string &path, const std::string &bytes, std::string &error);
};

struct CompilerArtifactLeaseSnapshot {
  std::string session_key;
  std::string target_triple;
  std::string root_package;
  compiler::StableFingerprint root_manifest;
};

struct CompilerArtifactCodecService {
  enum class NominalReferenceKind {
    TypeSpecific,
    SemanticWitness,
    TypeLayout,
  };
  [[nodiscard]] static bool hasInvalidFieldCharacter(std::string_view value);
  [[nodiscard]] static bool validHexKey(std::string_view value);
  [[nodiscard]] static std::optional<compiler::StableFingerprint>
  parseFingerprint(std::string_view hex);
  [[nodiscard]] static std::string
  encodeNominalReference(NominalReferenceKind kind,
                         const compiler::StableFingerprint &result_fingerprint);
  [[nodiscard]] static std::optional<compiler::StableFingerprint>
  decodeNominalReference(std::string_view bytes, NominalReferenceKind kind);
  [[nodiscard]] static std::string encodeSpecializationReference(
      const compiler::StableFingerprint &component_fingerprint);
  [[nodiscard]] static std::optional<compiler::StableFingerprint>
  decodeSpecializationReference(std::string_view bytes);
  [[nodiscard]] static bool
  verifyLease(const CompilerArtifactLeaseSnapshot &lease, std::string &error);
  [[nodiscard]] static std::string
  encodeLease(const CompilerArtifactLeaseSnapshot &lease, std::string &error);
  [[nodiscard]] static std::optional<CompilerArtifactLeaseSnapshot>
  decodeLease(std::string_view bytes, std::string &error);
};

struct CompilerArtifactPathService {
  [[nodiscard]] static std::string reference(const std::filesystem::path &root,
                                             std::string_view session_key);
  [[nodiscard]] static std::string lease(const std::filesystem::path &root,
                                         std::string_view lease_id);
  [[nodiscard]] static std::string
  manifest(const std::filesystem::path &root,
           const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  object(const std::filesystem::path &root,
         const compiler::StableFingerprint &fingerprint,
         std::string_view extension);
  [[nodiscard]] static std::string
  specialization(const std::filesystem::path &root,
                 const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  specializationIndex(const std::filesystem::path &root,
                      const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  typeSpecific(const std::filesystem::path &root,
               const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  typeSpecificIndex(const std::filesystem::path &root,
                    const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  nominalWitness(const std::filesystem::path &root,
                 const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  nominalWitnessIndex(const std::filesystem::path &root,
                      const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  typeLayout(const std::filesystem::path &root,
             const compiler::StableFingerprint &fingerprint);
  [[nodiscard]] static std::string
  typeLayoutIndex(const std::filesystem::path &root,
                  const compiler::StableFingerprint &fingerprint);
};

enum class CompilerArtifactLeaseProbe { Active, Stale, Error };

struct CompilerArtifactGCState {
  const std::filesystem::path &root;
  std::function<CompilerArtifactLeaseProbe(const std::filesystem::path &,
                                           std::string &)>
      probe_lease;
};

struct CompilerArtifactGCService {
  [[nodiscard]] static bool verifyReferenceClosure(
      const std::filesystem::path &root,
      const CompilerSessionArtifactReference &reference,
      std::map<std::string, compiler::CompilerPackageArtifactManifest>
          *manifests,
      std::string &error);
  [[nodiscard]] static bool collect(bool force,
                                    std::chrono::seconds minimum_interval,
                                    CompilerGarbageCollectionReport &report,
                                    std::string &error,
                                    CompilerArtifactGCState &state);
};

struct CompilerArtifactPublishState {
  const std::filesystem::path &root;
  std::string_view session_key;
  std::string_view expected_target;
  const std::optional<CompilerSessionArtifactReference> &observed_reference;
  std::function<compiler::ObjectArtifactLoadResult(
      const compiler::StableFingerprint &, const compiler::StableFingerprint &,
      std::string_view, std::string_view)>
      load_object;
  std::function<bool(std::string_view,
                     std::optional<CompilerSessionArtifactReference> &,
                     std::string &)>
      load_current_reference;
  std::function<bool(const std::function<bool()> &, std::string &)>
      with_store_lock;
  std::function<void()> retire_lease;
};

struct CompilerArtifactPublishService {
  [[nodiscard]] static bool publish(
      const compiler::CompilerPackageArtifactManifest &root_manifest,
      std::span<const compiler::CompilerPackageArtifactManifest *const>
          manifests,
      std::span<const CompilerPublishedObject> objects,
      std::span<const CompilerPublishedSpecialization> specializations,
      std::span<const CompilerPublishedNominalTypeSpecific> nominal_specifics,
      std::span<const CompilerPublishedNominalSemanticWitness>
          nominal_semantic_witnesses,
      std::span<const CompilerPublishedNominalTypeLayout> nominal_layouts,
      std::string &error, CompilerArtifactPublishState &state);
};

struct CompilerArtifactAcquireState {
  const std::filesystem::path &root;
  std::function<bool(std::string_view,
                     std::optional<CompilerSessionArtifactReference> &,
                     std::string &)>
      load_current_reference;
};

struct CompilerArtifactLeasePlan {
  std::optional<CompilerSessionArtifactReference> reference;
  std::map<std::string, compiler::CompilerPackageArtifactManifest> manifests;
  std::string lease_path;
};

struct CompilerArtifactAcquireService {
  [[nodiscard]] static std::optional<CompilerArtifactLeasePlan>
  prepare(std::string_view session_key, std::string_view expected_target,
          std::string_view expected_root, std::string &error,
          CompilerArtifactAcquireState &state);
};

} // namespace chtholly
