#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chtholly {

struct ToolchainReleaseInfo {
  std::string release_id;
  std::string version;
  std::string source_commit;
  std::string host;
  std::string archive_sha256;
  std::size_t file_count = 0;
  // Populated by install/upgrade after the pre-extraction filesystem check.
  // These are evidence fields only; they do not participate in release or ABI
  // identities and remain zero/empty for package and verify operations.
  std::uint64_t space_payload_bytes = 0;
  std::uint64_t space_index_bytes = 0;
  std::uint64_t space_required_bytes = 0;
  std::uint64_t space_available_bytes = 0;
  std::string space_path;
  bool space_sufficient = false;
};

struct ToolchainTrustRootRequest {
  std::string output_path;
  std::uint64_t version = 0;
  std::uint32_t threshold = 0;
  std::vector<std::string> public_key_paths;
  std::vector<std::string> revoked_key_ids;
  std::vector<std::string> secret_key_paths;
};

struct ToolchainPackageRequest {
  std::string install_tree;
  std::string archive_path;
  std::string version;
  std::string source_commit;
  std::string host;
  std::vector<std::string> secret_key_paths;
};

bool generateToolchainSigningKeyFiles(const std::string &secret_key_path,
                                      const std::string &public_key_path,
                                      std::string &error);

bool createToolchainTrustRoot(const ToolchainTrustRootRequest &request,
                              std::string &error);
bool installToolchainTrustRoot(const std::string &manager_root,
                               const std::string &root_file, bool initialize,
                               std::string &error);
std::optional<std::string>
inspectToolchainTrustRoot(const std::string &manager_root, std::string &error);

std::optional<ToolchainReleaseInfo>
packageToolchainRelease(const ToolchainPackageRequest &request,
                        std::string &error);
std::optional<ToolchainReleaseInfo>
verifyToolchainRelease(const std::string &archive_path,
                       const std::string &manager_root,
                       const std::string &expected_host, std::string &error);
std::optional<ToolchainReleaseInfo> installToolchainRelease(
    const std::string &archive_path, const std::string &manager_root,
    const std::string &expected_host, bool upgrade, std::string &error);

bool activateToolchainRelease(const std::string &manager_root,
                              const std::string &release_id,
                              std::string &error);
std::optional<std::string>
rollbackToolchainRelease(const std::string &manager_root, std::string &error);
std::optional<std::vector<std::string>>
listToolchainReleases(const std::string &manager_root, std::string &error);
bool removeToolchainRelease(const std::string &manager_root,
                            const std::string &release_id, std::string &error);

} // namespace chtholly
