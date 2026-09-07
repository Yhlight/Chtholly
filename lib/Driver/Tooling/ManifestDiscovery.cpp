#include "chtholly/Driver/ManifestDiscovery.h"

#include <string>

namespace chtholly {

std::filesystem::path
normalizeDiscoveryPath(const std::filesystem::path &path) {
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  return (ec ? path : canonical).lexically_normal();
}

std::optional<std::filesystem::path>
findAncestorManifest(std::filesystem::path cursor, std::string_view name) {
  cursor = normalizeDiscoveryPath(cursor.empty() ? std::filesystem::path(".")
                                                 : cursor);
  while (!cursor.empty()) {
    const auto candidate = cursor / std::filesystem::path(std::string(name));
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return normalizeDiscoveryPath(candidate);
    }
    const auto parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }
  return std::nullopt;
}

ManifestDiscovery discoverManifests(std::filesystem::path cursor) {
  ManifestDiscovery discovery;
  discovery.project_manifest = findAncestorManifest(cursor, "chtholly.toml");
  discovery.workspace_manifest =
      findAncestorManifest(cursor, "chtholly.workspace.toml");
  return discovery;
}

} // namespace chtholly
