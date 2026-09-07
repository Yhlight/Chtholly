#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace chtholly {

struct ManifestDiscovery {
  std::optional<std::filesystem::path> project_manifest;
  std::optional<std::filesystem::path> workspace_manifest;
};

std::filesystem::path normalizeDiscoveryPath(const std::filesystem::path &path);

std::optional<std::filesystem::path>
findAncestorManifest(std::filesystem::path cursor, std::string_view name);

ManifestDiscovery discoverManifests(std::filesystem::path cursor);

} // namespace chtholly
