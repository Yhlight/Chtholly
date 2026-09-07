#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

class RegistrySigningProvider;

inline constexpr std::string_view RegistryRootFormat =
    "chtholly-registry-root-v1";
inline constexpr std::string_view RegistryRootFormatV2 =
    "chtholly-registry-root-v2";
inline constexpr std::string_view RegistrySnapshotFormat =
    "chtholly-registry-snapshot-v1";
inline constexpr std::string_view RegistrySnapshotFormatV2 =
    "chtholly-registry-snapshot-v2";
inline constexpr std::string_view RegistryTimestampFormat =
    "chtholly-registry-timestamp-v1";

struct RegistryTrustRole {
  std::vector<std::string> keys;
  std::uint32_t threshold = 0;
};

struct RegistryMetadataSignature {
  std::string key_id;
  std::string signature;
};

struct RegistryAuditCheckpoint {
  std::string registry_name;
  std::uint64_t tree_size = 0;
  std::string root_hash;
  std::uint64_t root_version = 0;
  std::string root_sha256;
  std::string issued_at;
  std::vector<RegistryMetadataSignature> signatures;
};

struct RegistryRootMetadata {
  std::string format = std::string(RegistryRootFormat);
  std::string registry_name;
  std::uint64_t version = 0;
  std::string expires;
  RegistryTrustRole root;
  RegistryTrustRole targets;
  RegistryTrustRole snapshot;
  RegistryTrustRole timestamp;
  RegistryTrustRole audit;
  std::vector<std::string> revoked_key_ids;
  std::vector<RegistryMetadataSignature> signatures;
};

struct VerifiedRegistryRoot {
  RegistryRootMetadata root;
  std::string root_sha256;
};

struct RegistryRootChainVerificationRequest {
  std::string registry_name;
  std::string checkout_root;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::int64_t now_unix_seconds = -1;
};

struct RegistrySnapshotMetadata {
  std::string format = std::string(RegistrySnapshotFormat);
  std::string registry_name;
  std::uint64_t version = 0;
  std::uint64_t root_version = 0;
  std::string expires;
  std::string packages_sha256;
  std::string audit_checkpoint_sha256;
  std::vector<RegistryMetadataSignature> signatures;
};

struct RegistryTimestampMetadata {
  std::string registry_name;
  std::uint64_t version = 0;
  std::uint64_t root_version = 0;
  std::string expires;
  std::uint64_t snapshot_version = 0;
  std::string snapshot_sha256;
  std::vector<RegistryMetadataSignature> signatures;
};

struct VerifiedRegistryTrust {
  RegistryRootMetadata root;
  RegistrySnapshotMetadata snapshot;
  RegistryTimestampMetadata timestamp;
  std::string root_sha256;
  std::string snapshot_sha256;
  std::string timestamp_sha256;
  std::string packages_sha256;
  std::optional<RegistryAuditCheckpoint> audit_checkpoint;
};

struct RegistryTrustVerificationRequest {
  std::string registry_name;
  std::string registry_index;
  std::string checkout_root;
  std::string identity_store_root;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::int64_t now_unix_seconds = -1;
  bool persist_state = true;
};

struct RegistryIndexSealRequest {
  std::string registry_name;
  std::string index_root;
  std::string snapshot_expires;
  std::string timestamp_expires;
  std::optional<RegistryAuditCheckpoint> audit_checkpoint;
};

bool parseRegistryUtcTimestamp(std::string_view value,
                               std::int64_t &unix_seconds);

std::optional<RegistryRootMetadata>
parseRegistryRootMetadata(std::string_view text, const std::string &path,
                          std::string &error);
std::optional<RegistrySnapshotMetadata>
parseRegistrySnapshotMetadata(std::string_view text, const std::string &path,
                              std::string &error);
std::optional<RegistryTimestampMetadata>
parseRegistryTimestampMetadata(std::string_view text, const std::string &path,
                               std::string &error);

std::string renderRegistryRootMetadata(const RegistryRootMetadata &metadata);
std::string
renderRegistrySnapshotMetadata(const RegistrySnapshotMetadata &metadata);
std::string
renderRegistryTimestampMetadata(const RegistryTimestampMetadata &metadata);

std::optional<VerifiedRegistryTrust>
verifyRegistryTrust(const RegistryTrustVerificationRequest &request,
                    std::string &error);
bool commitVerifiedRegistryTrust(
    const RegistryTrustVerificationRequest &request,
    const VerifiedRegistryTrust &trust, std::string &error);
std::optional<VerifiedRegistryRoot>
verifyRegistryRootChain(const RegistryRootChainVerificationRequest &request,
                        std::string &error);

bool signRegistryRootMetadataFile(const std::string &input_path,
                                  const std::string &secret_key_path,
                                  const std::string &output_path,
                                  std::string &error);
bool sealRegistryIndex(const RegistryIndexSealRequest &request,
                       const RegistrySigningProvider &signer,
                       VerifiedRegistryTrust &sealed, std::string &error);

std::string
registrySnapshotSignatureInput(const RegistrySnapshotMetadata &snapshot);
std::string
registryTimestampSignatureInput(const RegistryTimestampMetadata &timestamp);
std::optional<std::vector<std::string>>
registryActiveRoleKeyIds(const RegistryTrustRole &role,
                         const std::vector<std::string> &revoked_key_ids,
                         std::string &error);
bool verifyRegistryRoleSignatures(
    std::string_view payload,
    const std::vector<RegistryMetadataSignature> &signatures,
    const RegistryTrustRole &role,
    const std::vector<std::string> &revoked_key_ids, std::string &error);

std::string registryTrustStatePath(std::string_view identity_store_root,
                                   std::string_view registry_name,
                                   std::string_view registry_index);
std::optional<std::string>
computeRegistryPackagesDigest(const std::string &checkout_root,
                              std::string &error);

} // namespace chtholly
