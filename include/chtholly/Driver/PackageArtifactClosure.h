#pragma once

#include "chtholly/Driver/PackageArtifact.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chtholly {

struct PackageArtifactClosureFile {
  std::string relative_path;
  std::string absolute_path;
  std::string sha256;
  std::uint64_t size = 0;
};

struct PackageArtifactClosure {
  std::string root_directory;
  std::string root_manifest_relative_path;
  std::string root_artifact_identity;
  std::vector<PackageArtifactManifest> manifests;
  std::vector<PackageArtifactClosureFile> files;
};

std::optional<PackageArtifactClosure>
loadPackageArtifactClosure(const std::string &root_manifest_path,
                           std::string &error);

std::string
canonicalPackageArtifactClosureIndex(const PackageArtifactClosure &closure);
std::string packageArtifactClosureDigest(const PackageArtifactClosure &closure);

} // namespace chtholly
