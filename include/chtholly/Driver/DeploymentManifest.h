#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace chtholly {

struct DeploymentManifest {
  std::filesystem::path root;
  std::filesystem::path library;
  std::filesystem::path contract;
  std::string identity;
  std::string version;
  std::string target;
  std::string runtime;
  std::string contract_digest;
};

[[nodiscard]] bool loadDeploymentManifest(const std::filesystem::path &path,
                                          DeploymentManifest &result,
                                          std::string &error);
[[nodiscard]] std::optional<std::array<std::uint8_t, 32>>
parseDeploymentDigest(std::string_view text);
[[nodiscard]] bool validateDeploymentFiles(const DeploymentManifest &manifest,
                                           std::string &error);

} // namespace chtholly
