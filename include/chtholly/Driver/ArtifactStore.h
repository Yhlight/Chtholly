#pragma once

#include "chtholly/Driver/PackageArtifactArchive.h"

#include <optional>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

struct ArtifactStoreLocator {
  std::string artifact_identity;
  std::string closure_digest;
};

struct ArtifactStoreInstallObservation {
  bool closure_hit = false;
  std::uint64_t archive_bytes = 0;
};

std::string defaultArtifactStorePath();
std::string resolveArtifactStorePath(std::string_view cli_path);
std::optional<ArtifactStoreLocator>
parseArtifactStoreLocator(std::string_view locator, std::string &error);
std::string renderArtifactStoreLocator(const ArtifactStoreLocator &locator);

class ArtifactStore {
public:
  explicit ArtifactStore(std::string root);

  const std::string &root() const { return root_; }

  std::optional<PackageArtifactArchiveInfo>
  install(const std::string &archive_path, std::string &error,
          ArtifactStoreInstallObservation *observation = nullptr) const;
  bool uninstall(std::string_view locator, std::string &error) const;
  std::optional<PackageArtifactArchiveInfo>
  inspect(std::string_view locator, std::string &error) const;
  std::optional<std::vector<std::string>> list(std::string &error) const;
  std::optional<std::size_t> garbageCollect(std::string &error) const;

  std::optional<std::string>
  materialize(std::string_view locator, const std::string &destination_root,
              std::string &error) const;
  std::optional<std::string>
  filePath(std::string_view locator, std::string_view relative_path,
           std::string &error) const;

private:
  std::string root_;
};

} // namespace chtholly
