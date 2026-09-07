#pragma once

#include "chtholly/Driver/PackageArtifactArchive.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

inline constexpr std::string_view RegistryArtifactEntryFormatV2 =
    "chtholly-registry-entry-v2";
inline constexpr std::string_view RegistryArtifactEntryFormat =
    "chtholly-registry-entry-v3";
inline constexpr std::string_view RegistryIndexEntryFormat =
    "chtholly-registry-index-entry-v1";

bool isValidRegistryPublicKey(std::string_view key);

struct RegistryArtifactSignature {
  std::string key_id;
  std::string signature;
};

struct RegistryArtifactVariant {
  std::string name;
  TargetInfo target;
  AbiVersion abi_version = AbiVersion::V2;
  std::string runtime_abi;
  std::vector<std::string> requested_features;
  bool default_features = true;
  std::string url;
  std::uint64_t archive_size = 0;
  std::string archive_sha256;
  std::string artifact_identity;
  std::string closure_digest;
  std::vector<RegistryArtifactSignature> signatures;
};

struct RegistryArtifactVerification {
  std::uint32_t required_threshold = 0;
  std::vector<std::string> valid_signing_key_ids;
};

struct SignedRegistryEntry {
  std::string package_name;
  std::string version;
  std::vector<RegistryArtifactVariant> variants;
};

enum class RegistryReleaseState { Active, Yanked };

struct RegistryIndexEntry {
  SignedRegistryEntry artifact;
  RegistryReleaseState state = RegistryReleaseState::Active;
  std::uint64_t state_leaf_index = 0;
  std::string state_leaf_hash;
  std::string changed_at;
  std::string reason_code = "unspecified";
};

struct RegistrySigningRequest {
  std::string archive_path;
  std::string registry_name;
  std::string package_name;
  std::string version;
  std::string url;
  std::vector<std::string> requested_features;
  bool default_features = true;
  std::vector<std::string> secret_key_paths;
  std::string output_path;
};

struct RegistryPublishRequest {
  std::string registry_name;
  std::string publish_url;
  std::string archive_path;
  std::string entry_path;
  std::string token_environment;
  std::string token_file_path;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::string identity_store_path;
  std::string ca_bundle_path;
  std::string receipt_output_path;
  std::vector<std::string> witness_urls;
  std::vector<std::string> witness_keys;
  std::uint32_t witness_threshold = 0;
  std::string witness_ca_bundle_path;
};

struct RegistryPublishReceipt {
  std::string registry_name;
  std::string package_name;
  std::string version;
  std::string variant_name;
  std::string archive_sha256;
};

struct RegistryReleaseRequest {
  std::string registry_name;
  std::string publish_url;
  std::string package_name;
  std::string version;
  RegistryReleaseState state = RegistryReleaseState::Active;
  std::string reason_code;
  std::string token_environment;
  std::string token_file_path;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::string identity_store_path;
  std::string ca_bundle_path;
  std::string receipt_output_path;
  std::vector<std::string> witness_urls;
  std::vector<std::string> witness_keys;
  std::uint32_t witness_threshold = 0;
  std::string witness_ca_bundle_path;
};

struct RegistryReleaseReceipt {
  std::string registry_name;
  std::string package_name;
  std::string version;
  RegistryReleaseState state = RegistryReleaseState::Active;
  std::uint64_t leaf_index = 0;
};

struct RegistryAuditGossipRequest {
  std::string registry_name;
  std::string registry_origin;
  std::string receipt_path;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::string identity_store_path;
  std::string registry_ca_bundle_path;
  std::vector<std::string> witness_urls;
  std::vector<std::string> witness_keys;
  std::uint32_t witness_threshold = 0;
  std::string witness_ca_bundle_path;
};

std::optional<std::string>
loadRegistryBearerToken(const std::string &environment,
                        const std::string &file_path, std::string &error);

std::optional<SignedRegistryEntry>
parseSignedRegistryEntryText(std::string_view text, const std::string &path,
                             std::string &error);
std::string renderSignedRegistryEntry(const SignedRegistryEntry &entry);
std::optional<RegistryIndexEntry>
parseRegistryIndexEntryText(std::string_view text, const std::string &path,
                            std::string &error);
std::string renderRegistryIndexEntry(const RegistryIndexEntry &entry);
bool isRegistryReleaseReasonCode(std::string_view reason_code);
std::string canonicalRegistryArtifactSignatureInput(
    std::string_view registry_name, std::string_view package_name,
    std::string_view version, const RegistryArtifactVariant &variant,
    std::string_view signing_key_id);
std::string registryArtifactVariantName(const RegistryArtifactVariant &variant);

std::optional<RegistryArtifactVerification> verifyRegistryArtifactVariant(
    std::string_view registry_name, std::string_view package_name,
    std::string_view version, const RegistryArtifactVariant &variant,
    const std::vector<std::string> &trusted_keys,
    std::uint32_t required_threshold, std::string &error);

bool generateRegistrySigningKeyFiles(const std::string &secret_key_path,
                                     const std::string &public_key_path,
                                     std::string &error);
bool signRegistryArtifactEntry(const RegistrySigningRequest &request,
                               RegistryArtifactVariant &variant,
                               std::string &error);
std::optional<RegistryPublishReceipt>
publishRegistryArtifact(const RegistryPublishRequest &request,
                        std::string &error);
std::optional<RegistryReleaseReceipt>
changeRegistryReleaseState(const RegistryReleaseRequest &request,
                           std::string &error);
std::optional<std::size_t>
gossipRegistryAuditReceipt(const RegistryAuditGossipRequest &request,
                           std::string &error);

std::optional<std::string>
fetchRegistryArtifactArchive(const RegistryArtifactVariant &variant,
                             const std::string &registry_index_checkout,
                             const std::string &registry_cache_root,
                             const std::string &ca_bundle_path, bool offline,
                             std::string &error);

} // namespace chtholly
