#include "chtholly/Driver/RegistryServer.h"

#include "ManifestToml.h"
#include "RegistryServerSupportInternal.h"
#include "chtholly/Driver/SemVer.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>

namespace chtholly {

using registry_internal::isHexDigest;
using registry_internal::isSafeRegistryPackageName;
using registry_internal::isValidHttpsBaseUrl;
using registry_internal::resolveConfigPath;

std::optional<RegistryDaemonConfig>
loadRegistryDaemonConfig(const std::string &path, std::string &error) {
  auto text = readTextFile(path, error);
  if (!text)
    return std::nullopt;
  auto assignments = manifest_toml::parseAssignments(
      *text,
      {"", "server", "storage", "trust", "signer", "validity", "limits",
       "retention", "git"},
      "registry server config", error);
  if (!assignments)
    return std::nullopt;

  RegistryDaemonConfig config;
  std::unordered_set<std::string> seen;
  bool has_format = false;
  bool has_registry = false;
  bool has_certificate = false;
  bool has_private_key = false;
  bool has_state = false;
  bool has_index = false;
  bool has_public_url = false;
  bool has_root_keys = false;
  bool has_root_threshold = false;
  bool has_signer_command = false;
  bool has_signer_config = false;
  bool config_v3 = false;
  const auto parse_string = [&](const manifest_toml::Assignment &assignment,
                                std::string &output) {
    if (!manifest_toml::parseString(assignment.value, output) ||
        output.empty()) {
      error = "registry server config '" + assignment.fullKey() +
              "' expects a non-empty string";
      return false;
    }
    return true;
  };
  const auto parse_array = [&](const manifest_toml::Assignment &assignment,
                               std::vector<std::string> &output) {
    if (!manifest_toml::parseStringArray(assignment.value, output) ||
        output.empty() ||
        std::any_of(output.begin(), output.end(),
                    [](const auto &item) { return item.empty(); }) ||
        std::set<std::string>(output.begin(), output.end()).size() !=
            output.size()) {
      error = "registry server config '" + assignment.fullKey() +
              "' expects a non-empty array of unique strings";
      return false;
    }
    return true;
  };
  const auto parse_unsigned = [&](const manifest_toml::Assignment &assignment,
                                  std::uint64_t &output) {
    if (!manifest_toml::parseUnsigned(assignment.value, output) ||
        output == 0) {
      error = "registry server config '" + assignment.fullKey() +
              "' expects a positive integer";
      return false;
    }
    return true;
  };

  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if (!seen.insert(key).second) {
      error = "duplicate registry server config key '" + key + "'";
      return std::nullopt;
    }
    if (key == "format") {
      std::string format;
      if (!parse_string(assignment, format))
        return std::nullopt;
      if (format != RegistryServerConfigFormat &&
          format != RegistryServerConfigFormatV3) {
        error = "unsupported registry server config format '" + format + "'";
        return std::nullopt;
      }
      config_v3 = format == RegistryServerConfigFormatV3;
      has_format = true;
    } else if (key == "server.registry") {
      if (!parse_string(assignment, config.publication.registry_name))
        return std::nullopt;
      has_registry = true;
    } else if (key == "server.listen") {
      if (!parse_string(assignment, config.listen_address))
        return std::nullopt;
    } else if (key == "server.port") {
      std::uint64_t value = 0;
      if (!parse_unsigned(assignment, value))
        return std::nullopt;
      if (value > UINT16_MAX) {
        error = "registry server config 'server.port' exceeds 65535";
        return std::nullopt;
      }
      config.listen_port = static_cast<std::uint16_t>(value);
    } else if (key == "server.tls_certificate") {
      if (!parse_string(assignment, config.tls_certificate_path))
        return std::nullopt;
      has_certificate = true;
    } else if (key == "server.tls_private_key") {
      if (!parse_string(assignment, config.tls_private_key_path))
        return std::nullopt;
      has_private_key = true;
    } else if (key == "server.public_artifact_base_url") {
      if (!parse_string(assignment,
                        config.publication.public_artifact_base_url))
        return std::nullopt;
      has_public_url = true;
    } else if (key == "storage.state_directory") {
      if (!parse_string(assignment, config.publication.state_directory))
        return std::nullopt;
      has_state = true;
    } else if (key == "storage.index_worktree") {
      if (!parse_string(assignment, config.publication.index_worktree))
        return std::nullopt;
      has_index = true;
    } else if (key == "trust.bootstrap_root_keys") {
      if (!parse_array(assignment, config.publication.bootstrap_root_keys))
        return std::nullopt;
      has_root_keys = true;
    } else if (key == "trust.bootstrap_root_threshold") {
      std::uint64_t value = 0;
      if (!parse_unsigned(assignment, value))
        return std::nullopt;
      if (value > UINT32_MAX) {
        error = "registry server bootstrap root threshold exceeds 32 bits";
        return std::nullopt;
      }
      config.publication.bootstrap_root_threshold =
          static_cast<std::uint32_t>(value);
      has_root_threshold = true;
    } else if (key == "signer.command") {
      if (!parse_string(assignment, config.publication.signer.command))
        return std::nullopt;
      has_signer_command = true;
    } else if (key == "signer.config") {
      if (!parse_string(assignment, config.publication.signer.config_path))
        return std::nullopt;
      has_signer_config = true;
    } else if (key == "signer.timeout_seconds") {
      std::uint64_t value = 0;
      if (!parse_unsigned(assignment, value) || value > 300) {
        if (error.empty())
          error = "registry server signer timeout exceeds 300 seconds";
        return std::nullopt;
      }
      config.publication.signer.timeout_milliseconds = value * 1000;
    } else if (key == "validity.snapshot_seconds") {
      if (!parse_unsigned(assignment,
                          config.publication.snapshot_validity_seconds))
        return std::nullopt;
    } else if (key == "validity.timestamp_seconds") {
      if (!parse_unsigned(assignment,
                          config.publication.timestamp_validity_seconds))
        return std::nullopt;
    } else if (key == "limits.max_archive_bytes") {
      if (!parse_unsigned(assignment, config.publication.max_archive_bytes))
        return std::nullopt;
    } else if (key == "retention.audit_blob_seconds") {
      if (!parse_unsigned(assignment, config.publication.audit_blob_seconds))
        return std::nullopt;
    } else if (key == "retention.recovery_point_seconds") {
      if (!parse_unsigned(assignment,
                          config.publication.recovery_point_seconds))
        return std::nullopt;
    } else if (key == "retention.unreferenced_blob_seconds") {
      if (!parse_unsigned(assignment,
                          config.publication.unreferenced_blob_seconds))
        return std::nullopt;
    } else if (key == "git.remote") {
      if (!manifest_toml::parseString(assignment.value,
                                      config.publication.git_remote)) {
        error = "registry server config 'git.remote' expects a string";
        return std::nullopt;
      }
    } else if (key == "git.branch") {
      if (!parse_string(assignment, config.publication.git_branch))
        return std::nullopt;
    } else {
      error = "unknown registry server config key '" + key + "'";
      return std::nullopt;
    }
  }

  if (!has_format || !has_registry || !has_certificate || !has_private_key ||
      !has_state || !has_index || !has_public_url || !has_root_keys ||
      !has_root_threshold || !has_signer_command || !has_signer_config ||
      config.publication.bootstrap_root_threshold >
          config.publication.bootstrap_root_keys.size()) {
    error = "registry server config is incomplete or has an unsatisfied "
            "bootstrap root threshold";
    return std::nullopt;
  }
  const bool any_retention = config.publication.audit_blob_seconds != 0 ||
                             config.publication.recovery_point_seconds != 0 ||
                             config.publication.unreferenced_blob_seconds != 0;
  const bool all_retention = config.publication.audit_blob_seconds != 0 &&
                             config.publication.recovery_point_seconds != 0 &&
                             config.publication.unreferenced_blob_seconds != 0;
  if ((!config_v3 && any_retention) || (config_v3 && !all_retention)) {
    error = config_v3
                ? "registry server v3 requires all retention intervals"
                : "registry server v2 cannot configure retention intervals";
    return std::nullopt;
  }
  const auto base = std::filesystem::absolute(path).parent_path();
  config.tls_certificate_path =
      resolveConfigPath(base, std::move(config.tls_certificate_path));
  config.tls_private_key_path =
      resolveConfigPath(base, std::move(config.tls_private_key_path));
  config.publication.state_directory =
      resolveConfigPath(base, std::move(config.publication.state_directory));
  config.publication.index_worktree =
      resolveConfigPath(base, std::move(config.publication.index_worktree));
  auto signer_command = std::filesystem::path(config.publication.signer.command);
  if (signer_command.is_absolute() || signer_command.has_parent_path())
    config.publication.signer.command =
        resolveConfigPath(base, std::move(config.publication.signer.command));
  config.publication.signer.config_path =
      resolveConfigPath(base, std::move(config.publication.signer.config_path));
  return config;
}



} // namespace chtholly
