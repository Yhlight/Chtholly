#pragma once

#include "chtholly/Driver/RegistryArtifact.h"
#include "chtholly/Driver/RegistryServer.h"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::registry_internal {

[[nodiscard]] bool isHexDigest(std::string_view value);
[[nodiscard]] bool isSafeRegistryPackageName(std::string_view value);
[[nodiscard]] bool isValidHttpsBaseUrl(std::string_view value);
[[nodiscard]] bool pathIsWithin(const std::filesystem::path &path,
                                const std::filesystem::path &root);
void appendMutationField(std::ostringstream &out, std::string_view name,
                         std::string_view value);
[[nodiscard]] std::string mutationDigest(
    std::string_view registry_name, const SignedRegistryEntry &entry,
    const RegistryArtifactVariant &variant);
[[nodiscard]] std::string rfc3339(std::int64_t seconds);
[[nodiscard]] std::int64_t effectiveNow(std::int64_t requested);
[[nodiscard]] bool runGit(const std::string &worktree,
                          const std::vector<std::string> &arguments,
                          std::string &error);
[[nodiscard]] std::optional<RegistryRootChainVerificationRequest> rootRequest(
    const RegistryServerConfig &config, std::int64_t now, std::string &error);
[[nodiscard]] bool copyIntoCas(const std::string &source,
                               const std::filesystem::path &target,
                               std::uint64_t expected_size,
                               std::string_view expected_digest,
                               std::string &error);
[[nodiscard]] std::string resolveConfigPath(
    const std::filesystem::path &base, std::string value);

} // namespace chtholly::registry_internal
