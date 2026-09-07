#pragma once

#include "chtholly/Driver/RegistryTrust.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

inline constexpr std::string_view RegistrySignerProtocolFormat =
    "chtholly-registry-signer-v1";
inline constexpr std::string_view RegistryFileSignerConfigFormat =
    "chtholly-registry-file-signer-v1";
inline constexpr std::string_view RegistryFileSignerConfigFormatV2 =
    "chtholly-registry-file-signer-v2";

enum class RegistrySigningRole { Snapshot, Timestamp, Audit, Witness };
enum class RegistrySignerOperation { Keys, Sign };

std::string_view registrySigningRoleSpelling(RegistrySigningRole role);
std::optional<RegistrySigningRole>
parseRegistrySigningRole(std::string_view value);

struct RegistrySignerRequest {
  RegistrySignerOperation operation = RegistrySignerOperation::Keys;
  std::string registry_name;
  RegistrySigningRole role = RegistrySigningRole::Snapshot;
  std::uint64_t root_version = 0;
  std::uint32_t threshold = 0;
  std::vector<std::string> authorized_key_ids;
  std::string payload;
};

struct RegistrySignerResponse {
  std::string payload_sha256;
  std::vector<std::string> key_ids;
  std::vector<RegistryMetadataSignature> signatures;
};

class RegistrySigningProvider {
public:
  virtual ~RegistrySigningProvider();
  virtual std::optional<RegistrySignerResponse>
  execute(const RegistrySignerRequest &request, std::string &error) const = 0;
};

struct RegistryCommandSignerConfig {
  std::string command;
  std::string config_path;
  std::uint64_t timeout_milliseconds = 30000;
};

struct RegistryFileSignerConfig {
  std::string registry_name;
  std::vector<std::string> snapshot_secret_key_paths;
  std::vector<std::string> timestamp_secret_key_paths;
  std::vector<std::string> audit_secret_key_paths;
  std::vector<std::string> witness_secret_key_paths;
};

std::optional<RegistryFileSignerConfig>
loadRegistryFileSignerConfig(const std::string &path, std::string &error);

std::unique_ptr<RegistrySigningProvider>
createRegistryCommandSigningProvider(RegistryCommandSignerConfig config);
std::unique_ptr<RegistrySigningProvider>
createRegistryFileSigningProvider(RegistryFileSignerConfig config);

std::string renderRegistrySignerRequest(const RegistrySignerRequest &request);
std::optional<RegistrySignerRequest>
parseRegistrySignerRequest(std::string_view text, std::string &error);
std::string
renderRegistrySignerResponse(const RegistrySignerRequest &request,
                             const RegistrySignerResponse &response);
std::optional<RegistrySignerResponse>
parseRegistrySignerResponse(std::string_view text,
                            const RegistrySignerRequest &request,
                            std::string &error);

bool validateRegistrySigningPayload(const RegistrySignerRequest &request,
                                    std::string &error);

} // namespace chtholly
