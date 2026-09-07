#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chtholly {

inline constexpr const char *RegistryBackupFormatV1 =
    "chtholly-registry-backup-v1";
inline constexpr const char *RegistryBackupFormat =
    "chtholly-registry-backup-v2";

struct RegistryBackupFile {
  std::string relative_path;
  std::uint64_t size = 0;
  std::string sha256;
};

struct RegistryBackupInfo {
  std::string registry_name;
  std::string created_at;
  std::uint64_t database_schema = 0;
  std::string git_head;
  std::uint64_t tree_size = 0;
  std::string tree_root;
  std::uint64_t root_version = 0;
  std::string root_sha256;
  std::string recovery_point_id;
  std::string retain_until;
  std::vector<RegistryBackupFile> files;
};

struct RegistryBackupSource {
  std::string registry_name;
  std::string state_directory;
  std::string index_worktree;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::int64_t now_unix_seconds = -1;
  std::string recovery_point_id;
  std::string retain_until;
};

struct RegistryBackupRestoreRequest {
  std::string archive_path;
  std::string registry_name;
  std::string state_directory;
  std::string index_worktree;
  std::vector<std::string> bootstrap_root_keys;
  std::uint32_t bootstrap_root_threshold = 0;
  std::int64_t now_unix_seconds = -1;
};

std::optional<RegistryBackupInfo>
createRegistryBackupArchive(const RegistryBackupSource &source,
                            const std::string &archive_path,
                            std::string &error);

std::optional<RegistryBackupInfo>
verifyRegistryBackupArchive(const RegistryBackupRestoreRequest &request,
                            std::string &error);

std::optional<RegistryBackupInfo>
restoreRegistryBackupArchive(const RegistryBackupRestoreRequest &request,
                             std::string &error);

} // namespace chtholly
