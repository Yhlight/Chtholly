#pragma once

#include "chtholly/Driver/RegistrySigner.h"
#include "chtholly/Driver/RegistryTransparency.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

inline constexpr std::string_view RegistryWitnessObservationFormat =
    "chtholly-registry-witness-observation-v1";
inline constexpr std::string_view RegistryWitnessStatementFormat =
    "chtholly-registry-witness-statement-v1";
inline constexpr std::string_view RegistryWitnessServerConfigFormat =
    "chtholly-registry-witness-server-v1";

struct RegistryWitnessObservation {
  std::string registry_origin;
  RegistryAuditCheckpoint checkpoint;
  std::vector<std::string> root_chain;
};

struct RegistryWitnessStatement {
  std::string witness_name;
  std::string registry_name;
  std::string registry_origin;
  std::string checkpoint_sha256;
  std::uint64_t tree_size = 0;
  std::string root_hash;
  std::uint64_t root_version = 0;
  std::string root_sha256;
  std::string observed_at;
  std::uint64_t head_tree_size = 0;
  std::string head_root_hash;
  RegistryMetadataSignature signature;
};

struct RegistryWitnessIncident {
  std::uint64_t sequence = 0;
  std::string kind;
  std::uint64_t known_tree_size = 0;
  std::string known_root_hash;
  std::uint64_t observed_tree_size = 0;
  std::string observed_root_hash;
  std::string detected_at;
};

struct RegistryWitnessConfig {
  std::string witness_name;
  std::string state_directory;
  std::string registry_name;
  std::string registry_origin;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::string witness_public_key;
  RegistryCommandSignerConfig signer;
};

struct RegistryWitnessDaemonConfig {
  RegistryWitnessConfig witness;
  std::string listen_address = "127.0.0.1";
  std::uint16_t listen_port = 9443;
  std::string tls_certificate_path;
  std::string tls_private_key_path;
  std::string registry_ca_bundle_path;
};

using RegistryConsistencyProofFetcher =
    std::function<std::optional<std::vector<std::string>>(
        std::uint64_t, std::uint64_t, std::string &)>;

std::string
renderRegistryWitnessObservation(const RegistryWitnessObservation &observation);
std::optional<RegistryWitnessObservation>
parseRegistryWitnessObservation(std::string_view text, std::string &error);
std::string registryWitnessStatementSignatureInput(
    const RegistryWitnessStatement &statement);
std::string
renderRegistryWitnessStatement(const RegistryWitnessStatement &statement);
std::optional<RegistryWitnessStatement>
parseRegistryWitnessStatement(std::string_view text, std::string &error);
bool verifyRegistryWitnessStatement(
    const RegistryWitnessStatement &statement,
    const RegistryWitnessObservation &observation,
    const std::vector<std::string> &authorized_keys, std::string &error);
bool verifyRegistryWitnessStatementSignature(
    const RegistryWitnessStatement &statement,
    const std::vector<std::string> &authorized_keys, std::string &error);

std::optional<RegistryWitnessDaemonConfig>
loadRegistryWitnessDaemonConfig(const std::string &path, std::string &error);

class RegistryWitnessStore {
public:
  RegistryWitnessStore();
  ~RegistryWitnessStore();
  RegistryWitnessStore(RegistryWitnessStore &&) noexcept;
  RegistryWitnessStore &operator=(RegistryWitnessStore &&) noexcept;
  RegistryWitnessStore(const RegistryWitnessStore &) = delete;
  RegistryWitnessStore &operator=(const RegistryWitnessStore &) = delete;

  static std::optional<RegistryWitnessStore> open(RegistryWitnessConfig config,
                                                  std::string &error);

  std::optional<RegistryWitnessStatement>
  observe(const RegistryWitnessObservation &observation,
          std::int64_t now_unix_seconds,
          const RegistryConsistencyProofFetcher &fetch_proof,
          std::string &error);
  std::optional<RegistryWitnessStatement> latest(std::string &error) const;
  std::optional<std::vector<RegistryWitnessIncident>>
  incidents(std::string &error) const;

private:
  struct Impl;
  explicit RegistryWitnessStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly
