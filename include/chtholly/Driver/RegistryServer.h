#pragma once

#include "chtholly/Driver/RegistryArtifact.h"
#include "chtholly/Driver/RegistrySigner.h"
#include "chtholly/Driver/RegistryTransparency.h"
#include "chtholly/Driver/RegistryTrust.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

inline constexpr std::string_view RegistryServerConfigFormat =
    "chtholly-registry-server-v2";
inline constexpr std::string_view RegistryServerConfigFormatV3 =
    "chtholly-registry-server-v3";
struct RegistryServerConfig {
  std::string registry_name;
  std::string state_directory;
  std::string index_worktree;
  std::string public_artifact_base_url;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  RegistryCommandSignerConfig signer;
  std::uint64_t snapshot_validity_seconds = 86400;
  std::uint64_t timestamp_validity_seconds = 3600;
  std::uint64_t max_archive_bytes = 1024ULL * 1024ULL * 1024ULL;
  std::uint64_t audit_blob_seconds = 0;
  std::uint64_t recovery_point_seconds = 0;
  std::uint64_t unreferenced_blob_seconds = 0;
  std::string git_remote;
  std::string git_branch = "main";
};

struct RegistryPublisherTokenRequest {
  std::string principal;
  std::vector<std::string> packages;
  bool all_packages = false;
};

struct RegistryPublisherTokenInfo {
  std::string token_id;
  std::string principal;
  std::vector<std::string> packages;
  bool all_packages = false;
  bool revoked = false;
};

struct RegistryOperatorTokenRequest {
  std::string principal;
  std::vector<std::string> capabilities;
};

struct RegistryOperatorTokenInfo {
  std::string token_id;
  std::string principal;
  std::vector<std::string> capabilities;
  bool revoked = false;
};

struct RegistryDaemonConfig {
  RegistryServerConfig publication;
  std::string listen_address = "127.0.0.1";
  std::uint16_t listen_port = 8443;
  std::string tls_certificate_path;
  std::string tls_private_key_path;
};

std::optional<RegistryDaemonConfig>
loadRegistryDaemonConfig(const std::string &path, std::string &error);

struct RegistryServerPublishRequest {
  std::string bearer_token;
  std::string archive_path;
  std::string entry_path;
  std::string idempotency_key;
  std::int64_t now_unix_seconds = -1;
};

struct RegistryServerPublishResult {
  unsigned status = 500;
  std::string message;
  std::optional<RegistryAuditReceipt> receipt;
};

struct RegistryServerLifecycleRequest {
  std::string bearer_token;
  std::string package_name;
  std::string version;
  RegistryReleaseState state = RegistryReleaseState::Active;
  std::string reason_code;
  std::string idempotency_key;
  std::int64_t now_unix_seconds = -1;
};

struct RegistryServerLifecycleResult {
  unsigned status = 500;
  std::string message;
  std::optional<RegistryLifecycleReceipt> receipt;
};

struct RegistryRecoveryPointInfo {
  std::string id;
  std::string created_at;
  std::string retain_until;
  bool released = false;
  std::uint64_t blob_count = 0;
};

struct RegistryGcPlanInfo {
  std::string id;
  std::string created_at;
  std::vector<std::string> blob_sha256;
  bool applied = false;
};

class RegistryPublicationStore {
public:
  RegistryPublicationStore();
  ~RegistryPublicationStore();
  RegistryPublicationStore(RegistryPublicationStore &&) noexcept;
  RegistryPublicationStore &operator=(RegistryPublicationStore &&) noexcept;
  RegistryPublicationStore(const RegistryPublicationStore &) = delete;
  RegistryPublicationStore &
  operator=(const RegistryPublicationStore &) = delete;

  static std::optional<RegistryPublicationStore>
  open(RegistryServerConfig config, std::string &error);

  std::optional<std::string>
  createPublisherToken(const RegistryPublisherTokenRequest &request,
                       std::string &error);
  bool revokePublisherToken(std::string_view token_id, std::string &error);
  std::optional<std::vector<RegistryPublisherTokenInfo>>
  listPublisherTokens(std::string &error) const;
  std::optional<std::string>
  createOperatorToken(const RegistryOperatorTokenRequest &request,
                      std::string &error);
  bool revokeOperatorToken(std::string_view token_id, std::string &error);
  std::optional<std::vector<RegistryOperatorTokenInfo>>
  listOperatorTokens(std::string &error) const;
  bool authenticateOperatorToken(std::string_view token,
                                 std::string_view capability,
                                 std::string &error) const;
  RegistryServerPublishResult
  publish(const RegistryServerPublishRequest &request);
  RegistryServerLifecycleResult
  changeReleaseState(const RegistryServerLifecycleRequest &request);
  bool resealIndex(std::int64_t now_unix_seconds, std::string &error);

  bool createBackup(const std::string &archive_path,
                    std::int64_t now_unix_seconds, std::string &error) const;
  std::optional<RegistryRecoveryPointInfo>
  createRecoveryPoint(const std::string &archive_path,
                      std::int64_t now_unix_seconds, std::string &error);
  bool extendRecoveryPoint(std::string_view id, std::int64_t retain_until,
                           std::string &error);
  bool releaseRecoveryPoint(std::string_view id, std::string &error);
  std::optional<std::vector<RegistryRecoveryPointInfo>>
  listRecoveryPoints(std::string &error) const;
  std::optional<RegistryGcPlanInfo>
  planGarbageCollection(std::int64_t now_unix_seconds, std::string &error);
  bool applyGarbageCollection(std::string_view plan_id,
                              std::int64_t now_unix_seconds,
                              std::string &error);

  std::optional<std::string> archivePath(std::string_view sha256,
                                         std::string &error) const;
  std::optional<RegistryAuditCheckpoint>
  latestCheckpoint(std::string &error) const;
  std::optional<std::string> auditLeaf(std::uint64_t index,
                                       std::string &error) const;
  std::vector<std::string> inclusionProof(std::uint64_t index,
                                          std::uint64_t tree_size,
                                          std::string &error) const;
  std::vector<std::string> consistencyProof(std::uint64_t old_tree_size,
                                            std::uint64_t new_tree_size,
                                            std::string &error) const;

private:
  struct Impl;
  explicit RegistryPublicationStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly
