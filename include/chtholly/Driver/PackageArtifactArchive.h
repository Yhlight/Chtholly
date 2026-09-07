#pragma once

#include "chtholly/Driver/PackageArtifact.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

inline constexpr const char *PackageArchiveIndexHeader =
    "chtholly-archive-index-v1";

struct PackageArtifactArchiveFile {
  std::string relative_path;
  std::string sha256;
  std::uint64_t size = 0;
};

struct PackageArtifactArchiveInfo {
  std::string archive_path;
  std::string root_manifest_relative_path;
  std::string artifact_identity;
  std::string closure_digest;
  std::string archive_sha256;
  std::string canonical_index;
  std::string package_name;
  TargetInfo target;
  AbiVersion abi_version = AbiVersion::V2;
  std::string runtime_abi;
  bool default_features = true;
  std::vector<std::string> resolved_features;
  std::vector<PackageArtifactArchiveFile> files;
};

std::optional<PackageArtifactArchiveInfo>
packPackageArtifactArchive(const std::string &root_manifest_path,
                           const std::string &archive_path,
                           std::string &error);

std::optional<PackageArtifactArchiveInfo>
inspectPackageArtifactArchive(const std::string &archive_path,
                              std::string &error);

bool extractPackageArtifactArchive(const std::string &archive_path,
                                   const std::string &destination,
                                   PackageArtifactArchiveInfo &info,
                                   std::string &error);

std::optional<PackageArtifactArchiveInfo>
parsePackageArtifactArchiveIndex(std::string_view index,
                                 std::string &error);

} // namespace chtholly
