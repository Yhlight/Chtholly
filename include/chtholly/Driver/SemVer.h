#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string prerelease;
  std::string build;
};

struct VersionRequirement {
  std::string text;
};

std::optional<SemVer> parseSemVer(std::string_view text, std::string &error);
int compareSemVer(const SemVer &lhs, const SemVer &rhs);
std::optional<VersionRequirement> parseVersionRequirement(std::string_view text,
                                                          std::string &error);
bool versionRequirementAllowsPrerelease(const VersionRequirement &requirement);
bool versionSatisfiesRequirement(const SemVer &version,
                                 const VersionRequirement &requirement);
std::optional<std::string> selectHighestVersion(
    const std::vector<std::string> &versions, const VersionRequirement &requirement,
    std::string &error);

} // namespace chtholly
