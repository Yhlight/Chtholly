#pragma once

#include "chtholly/Driver/DeploymentManifest.h"
#include <array>
#include <cstdint>

inline std::string telemetryDigestHex(const std::array<std::uint8_t, 32> &digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) { result.push_back(hex[byte >> 4U]); result.push_back(hex[byte & 0x0fU]); }
  return result;
}

using TelemetryDeploymentManifest = chtholly::DeploymentManifest;

inline bool loadTelemetryDeploymentManifest(
    const std::filesystem::path &path, TelemetryDeploymentManifest &result,
    std::string &error) {
  return chtholly::loadDeploymentManifest(path, result, error);
}
