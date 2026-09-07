#pragma once

#include "chtholly/Driver/RegistryArtifact.h"
#include "chtholly/Driver/RegistryTrust.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

inline constexpr std::string_view RegistryAuditReceiptFormat =
    "chtholly-registry-publish-receipt-v2";
inline constexpr std::string_view RegistryLifecycleReceiptFormat =
    "chtholly-registry-lifecycle-receipt-v1";

struct RegistryAuditReceipt {
  std::string registry_name;
  std::string package_name;
  std::string version;
  std::string variant_name;
  std::string archive_sha256;
  std::string entry_sha256;
  std::string event_kind;
  std::string publisher_principal;
  std::string accepted_at;
  std::uint32_t targets_threshold = 0;
  std::vector<std::string> valid_target_signer_ids;
  std::uint64_t leaf_index = 0;
  std::string leaf_hash;
  RegistryAuditCheckpoint checkpoint;
  std::vector<std::string> inclusion_proof;
};

struct RegistryLifecycleReceipt {
  std::string registry_name;
  std::string package_name;
  std::string version;
  RegistryReleaseState state = RegistryReleaseState::Active;
  std::uint64_t previous_state_leaf_index = 0;
  std::string previous_state_leaf_hash;
  std::string actor_type;
  std::string actor_principal;
  std::string accepted_at;
  std::string reason_code;
  std::uint64_t leaf_index = 0;
  std::string leaf_hash;
  RegistryAuditCheckpoint checkpoint;
  std::vector<std::string> inclusion_proof;
};

std::string registryAuditLeafHash(std::string_view canonical_leaf);
std::string registryAuditCanonicalLeaf(const RegistryAuditReceipt &receipt);
std::string registryAuditCheckpointSignatureInput(
    const RegistryAuditCheckpoint &checkpoint);
std::string registryMerkleRoot(const std::vector<std::string> &leaf_hashes);
std::vector<std::string>
registryMerkleInclusionProof(const std::vector<std::string> &leaf_hashes,
                             std::uint64_t leaf_index);
bool verifyRegistryMerkleInclusion(std::string_view leaf_hash,
                                   std::uint64_t leaf_index,
                                   std::uint64_t tree_size,
                                   const std::vector<std::string> &proof,
                                   std::string_view expected_root);
std::vector<std::string>
registryMerkleConsistencyProof(const std::vector<std::string> &leaf_hashes,
                               std::uint64_t old_tree_size);
bool verifyRegistryMerkleConsistency(std::uint64_t old_tree_size,
                                     std::uint64_t new_tree_size,
                                     std::string_view old_root,
                                     std::string_view new_root,
                                     const std::vector<std::string> &proof);

std::string
renderRegistryAuditCheckpoint(const RegistryAuditCheckpoint &checkpoint);
std::optional<RegistryAuditCheckpoint>
parseRegistryAuditCheckpoint(std::string_view text, std::string &error);
std::string renderRegistryAuditReceipt(const RegistryAuditReceipt &receipt);
std::optional<RegistryAuditReceipt>
parseRegistryAuditReceipt(std::string_view text, std::string &error);
bool verifyRegistryAuditReceipt(const RegistryAuditReceipt &receipt,
                                const RegistryRootMetadata &root,
                                std::string &error);
bool verifyRegistryAuditCheckpoint(const RegistryAuditCheckpoint &checkpoint,
                                   const RegistryRootMetadata &root,
                                   std::string &error);
std::string
registryLifecycleCanonicalLeaf(const RegistryLifecycleReceipt &receipt);
std::string
renderRegistryLifecycleReceipt(const RegistryLifecycleReceipt &receipt);
std::optional<RegistryLifecycleReceipt>
parseRegistryLifecycleReceipt(std::string_view text, std::string &error);
bool verifyRegistryLifecycleReceipt(const RegistryLifecycleReceipt &receipt,
                                    const RegistryRootMetadata &root,
                                    std::string &error);
std::string registryPublishMutationId(std::string_view registry_name,
                                      const SignedRegistryEntry &entry,
                                      const RegistryArtifactVariant &variant);

struct RegistryCheckpointPin {
  std::uint64_t tree_size = 0;
  std::string root_hash;
  std::uint64_t root_version = 0;
  std::string root_sha256;
};

struct RegistryTransparencyPolicy {
  std::string registry_origin;
  std::vector<std::string> witness_urls;
  std::vector<std::string> witness_keys;
  std::uint32_t witness_threshold = 0;
  std::string registry_ca_bundle_path;
  std::string witness_ca_bundle_path;
};

struct RegistryClientViewVerificationRequest {
  RegistryTrustVerificationRequest trust;
  RegistryTransparencyPolicy transparency;
  std::optional<RegistryCheckpointPin> minimum_checkpoint;
  bool offline = false;
};

struct VerifiedRegistryClientView {
  VerifiedRegistryTrust trust;
  RegistryCheckpointPin checkpoint;
  std::vector<std::string> witness_key_ids;
};

std::optional<VerifiedRegistryClientView>
verifyRegistryClientView(const RegistryClientViewVerificationRequest &request,
                         std::string &error);

} // namespace chtholly
