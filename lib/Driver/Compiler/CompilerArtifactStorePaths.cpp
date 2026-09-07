#include "CompilerArtifactStoreInternal.h"

namespace chtholly {
namespace {

std::string shardedPath(const std::filesystem::path &root,
                        std::string_view directory,
                        const compiler::StableFingerprint &fingerprint,
                        std::string_view suffix) {
  const auto hex = fingerprint.hex();
  return (root / std::string(directory) / hex.substr(0, 2) /
          (hex + std::string(suffix)))
      .string();
}

} // namespace

std::string
CompilerArtifactPathService::reference(const std::filesystem::path &root,
                                       std::string_view session_key) {
  return (root / "refs" / (std::string(session_key) + ".ref")).string();
}

std::string
CompilerArtifactPathService::lease(const std::filesystem::path &root,
                                   std::string_view lease_id) {
  return (root / "leases" / (std::string(lease_id) + ".lease")).string();
}

std::string CompilerArtifactPathService::manifest(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "manifests", fingerprint, ".manifest");
}

std::string CompilerArtifactPathService::object(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint,
    std::string_view extension) {
  const auto hex = fingerprint.hex();
  return (root / "objects" / hex.substr(0, 2) /
          (hex + "." + std::string(extension)))
      .string();
}

std::string CompilerArtifactPathService::specialization(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "specializations", fingerprint, ".specific");
}

std::string CompilerArtifactPathService::specializationIndex(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "specialization-index", fingerprint, ".ref");
}

std::string CompilerArtifactPathService::typeSpecific(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "type-specifics", fingerprint, ".type");
}

std::string CompilerArtifactPathService::typeSpecificIndex(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "type-specific-index", fingerprint, ".ref");
}

std::string CompilerArtifactPathService::nominalWitness(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "nominal-semantic-witnesses", fingerprint,
                     ".witness");
}

std::string CompilerArtifactPathService::nominalWitnessIndex(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "nominal-semantic-witness-index", fingerprint,
                     ".ref");
}

std::string CompilerArtifactPathService::typeLayout(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "type-layouts", fingerprint, ".layout");
}

std::string CompilerArtifactPathService::typeLayoutIndex(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return shardedPath(root, "type-layout-index", fingerprint, ".ref");
}

} // namespace chtholly
