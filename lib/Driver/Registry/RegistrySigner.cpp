#include "chtholly/Driver/RegistrySigner.h"

#include "ManifestToml.h"
#include "RegistryCrypto.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

namespace chtholly {
namespace {

constexpr std::size_t kMaximumSignerPayload = 1024u * 1024u;
constexpr std::size_t kMaximumSignerResponse = 2u * 1024u * 1024u;
constexpr std::size_t kMaximumSignerSignatures = 64;

std::string base64Url(std::string_view value) {
  std::string result(
      sodium_base64_encoded_len(value.size(),
                                sodium_base64_VARIANT_URLSAFE_NO_PADDING),
      '\0');
  sodium_bin2base64(result.data(), result.size(),
                    reinterpret_cast<const unsigned char *>(value.data()),
                    value.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  result.resize(result.find('\0'));
  return result;
}

std::optional<std::string> decodeBase64Url(std::string_view value) {
  std::string result(value.size(), '\0');
  std::size_t size = 0;
  if (sodium_base642bin(reinterpret_cast<unsigned char *>(result.data()),
                        result.size(), value.data(), value.size(), nullptr,
                        &size, nullptr,
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
    return std::nullopt;
  result.resize(size);
  return result;
}

template <typename Integer>
bool parseUnsigned(std::string_view value, Integer &output) {
  const auto [end, status] =
      std::from_chars(value.data(), value.data() + value.size(), output);
  return status == std::errc{} && end == value.data() + value.size();
}

std::vector<std::string_view> lines(std::string_view text) {
  std::vector<std::string_view> result;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    result.push_back(line);
    if (newline == std::string_view::npos)
      break;
    text.remove_prefix(newline + 1);
  }
  return result;
}

bool splitField(std::string_view line, std::string_view &name,
                std::string_view &value) {
  const auto tab = line.find('\t');
  if (tab == std::string_view::npos || tab == 0 || tab + 1 > line.size())
    return false;
  name = line.substr(0, tab);
  value = line.substr(tab + 1);
  return true;
}

bool validKeyId(std::string_view value) {
  constexpr std::string_view prefix = "sha256:";
  return value.starts_with(prefix) && value.size() == prefix.size() + 64 &&
         std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                     value.end(), [](unsigned char ch) {
                       return (ch >= '0' && ch <= '9') ||
                              (ch >= 'a' && ch <= 'f');
                     });
}

bool lowerHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool utcTimestamp(std::string_view value) {
  std::int64_t unix_seconds = 0;
  return parseRegistryUtcTimestamp(value, unix_seconds);
}

bool validHttpsOrigin(std::string_view value) {
  if (!value.starts_with("https://") ||
      value.find_first_of("\t\r\n ") != std::string_view::npos ||
      value.find('@') != std::string_view::npos || value.ends_with('/'))
    return false;
  const auto authority = value.substr(8);
  return !authority.empty() && authority.find('/') == std::string_view::npos;
}

bool sortedUniqueKeys(const std::vector<std::string> &keys) {
  return keys.size() <= kMaximumSignerSignatures &&
         std::is_sorted(keys.begin(), keys.end()) &&
         std::adjacent_find(keys.begin(), keys.end()) == keys.end() &&
         std::all_of(keys.begin(), keys.end(), validKeyId);
}

std::optional<std::map<std::string, std::string>>
parseCanonicalPayload(std::string_view payload, std::string_view magic,
                      const std::vector<std::string> &expected,
                      std::string &error) {
  const auto newline = payload.find('\n');
  if (newline == std::string_view::npos || payload.substr(0, newline) != magic)
    return error = "registry signer payload has an invalid format",
           std::nullopt;
  payload.remove_prefix(newline + 1);
  std::map<std::string, std::string> fields;
  while (!payload.empty()) {
    const auto line_end = payload.find('\n');
    if (line_end == std::string_view::npos)
      return error = "registry signer payload is not newline terminated",
             std::nullopt;
    const auto line = payload.substr(0, line_end);
    payload.remove_prefix(line_end + 1);
    const auto tab = line.find('\t');
    const auto colon = tab == std::string_view::npos ? std::string_view::npos
                                                     : line.find(':', tab + 1);
    std::size_t size = 0;
    if (tab == std::string_view::npos || tab == 0 ||
        colon == std::string_view::npos ||
        !parseUnsigned(line.substr(tab + 1, colon - tab - 1), size) ||
        line.size() - colon - 1 != size ||
        !fields
             .emplace(std::string(line.substr(0, tab)),
                      std::string(line.substr(colon + 1)))
             .second) {
      return error = "registry signer payload contains an invalid field",
             std::nullopt;
    }
  }
  if (fields.size() != expected.size() ||
      !std::all_of(expected.begin(), expected.end(),
                   [&](const auto &field) { return fields.contains(field); }))
    return error = "registry signer payload fields are incomplete or unknown",
           std::nullopt;
  return fields;
}

const std::vector<std::string> &
rolePaths(const RegistryFileSignerConfig &config, RegistrySigningRole role) {
  if (role == RegistrySigningRole::Snapshot)
    return config.snapshot_secret_key_paths;
  if (role == RegistrySigningRole::Timestamp)
    return config.timestamp_secret_key_paths;
  if (role == RegistrySigningRole::Audit)
    return config.audit_secret_key_paths;
  return config.witness_secret_key_paths;
}

class FileSigningProvider final : public RegistrySigningProvider {
public:
  explicit FileSigningProvider(RegistryFileSignerConfig config)
      : config_(std::move(config)) {}

  std::optional<RegistrySignerResponse>
  execute(const RegistrySignerRequest &request,
          std::string &error) const override {
    if (request.registry_name != config_.registry_name ||
        request.root_version == 0 || request.threshold == 0 ||
        request.threshold > request.authorized_key_ids.size() ||
        !sortedUniqueKeys(request.authorized_key_ids)) {
      error = "registry signer request is not authorized by its policy";
      return std::nullopt;
    }
    if (request.operation == RegistrySignerOperation::Sign &&
        !validateRegistrySigningPayload(request, error))
      return std::nullopt;
    RegistrySignerResponse response;
    response.payload_sha256 = sha256Hex(request.payload);
    for (const auto &path : rolePaths(config_, request.role)) {
      registry_crypto::SecretKey secret{};
      registry_crypto::PublicKey public_key{};
      if (!registry_crypto::loadSecretKeyFile(path, secret, public_key, error))
        return std::nullopt;
      const auto id = registry_crypto::publicKeyId(public_key);
      if (!std::binary_search(request.authorized_key_ids.begin(),
                              request.authorized_key_ids.end(), id)) {
        sodium_memzero(secret.data(), secret.size());
        continue;
      }
      response.key_ids.push_back(id);
      if (request.operation == RegistrySignerOperation::Sign) {
        registry_crypto::Signature signature{};
        crypto_sign_detached(
            signature.data(), nullptr,
            reinterpret_cast<const unsigned char *>(request.payload.data()),
            static_cast<unsigned long long>(request.payload.size()),
            secret.data());
        response.signatures.push_back(
            {id, registry_crypto::encodeSignature(signature)});
      }
      sodium_memzero(secret.data(), secret.size());
    }
    std::sort(response.key_ids.begin(), response.key_ids.end());
    response.key_ids.erase(
        std::unique(response.key_ids.begin(), response.key_ids.end()),
        response.key_ids.end());
    std::sort(response.signatures.begin(), response.signatures.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.key_id < rhs.key_id;
              });
    if (std::adjacent_find(response.signatures.begin(),
                           response.signatures.end(),
                           [](const auto &lhs, const auto &rhs) {
                             return lhs.key_id == rhs.key_id;
                           }) != response.signatures.end()) {
      error = "registry file signer contains duplicate key material";
      return std::nullopt;
    }
    if (response.key_ids.size() < request.threshold ||
        (request.operation == RegistrySignerOperation::Sign &&
         response.signatures.size() < request.threshold)) {
      error = "registry signer cannot satisfy the requested role threshold";
      return std::nullopt;
    }
    return response;
  }

private:
  RegistryFileSignerConfig config_;
};

class CommandSigningProvider final : public RegistrySigningProvider {
public:
  explicit CommandSigningProvider(RegistryCommandSignerConfig config)
      : config_(std::move(config)) {}

  std::optional<RegistrySignerResponse>
  execute(const RegistrySignerRequest &request,
          std::string &error) const override {
    ProcessRunOptions options;
    options.stdin_text = renderRegistrySignerRequest(request);
    options.timeout_milliseconds = config_.timeout_milliseconds;
    options.max_stdout_bytes = kMaximumSignerResponse;
    options.max_stderr_bytes = 64u * 1024u;
    auto result = runProcess(config_.command,
                             {"request", "--config", config_.config_path},
                             options, error);
    if (!result) {
      error = error == "process timed out"
                  ? "external registry signer timed out"
                  : "external registry signer was unavailable";
      return std::nullopt;
    }
    if (result->exit_code != 0) {
      error = "external registry signer failed with exit code " +
              std::to_string(result->exit_code);
      return std::nullopt;
    }
    auto response =
        parseRegistrySignerResponse(result->stdout_text, request, error);
    if (!response)
      error = "external registry signer returned an invalid response: " + error;
    return response;
  }

private:
  RegistryCommandSignerConfig config_;
};

} // namespace

RegistrySigningProvider::~RegistrySigningProvider() = default;

std::string_view registrySigningRoleSpelling(RegistrySigningRole role) {
  switch (role) {
  case RegistrySigningRole::Snapshot:
    return "snapshot";
  case RegistrySigningRole::Timestamp:
    return "timestamp";
  case RegistrySigningRole::Audit:
    return "audit";
  case RegistrySigningRole::Witness:
    return "witness";
  }
  return {};
}

std::optional<RegistrySigningRole>
parseRegistrySigningRole(std::string_view value) {
  if (value == "snapshot")
    return RegistrySigningRole::Snapshot;
  if (value == "timestamp")
    return RegistrySigningRole::Timestamp;
  if (value == "audit")
    return RegistrySigningRole::Audit;
  if (value == "witness")
    return RegistrySigningRole::Witness;
  return std::nullopt;
}

std::optional<RegistryFileSignerConfig>
loadRegistryFileSignerConfig(const std::string &path, std::string &error) {
  auto text = readTextFile(path, error);
  auto assignments =
      text ? manifest_toml::parseAssignments(
                 *text, {"", "roles"}, "registry file signer config", error)
           : std::nullopt;
  if (!assignments)
    return std::nullopt;
  RegistryFileSignerConfig config;
  std::set<std::string> seen;
  bool format = false;
  bool format_v2 = false;
  const auto array = [&](const manifest_toml::Assignment &assignment,
                         std::vector<std::string> &output) {
    return manifest_toml::parseStringArray(assignment.value, output) &&
           !output.empty() &&
           std::all_of(output.begin(), output.end(),
                       [](const auto &item) { return !item.empty(); }) &&
           std::set<std::string>(output.begin(), output.end()).size() ==
               output.size();
  };
  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if (!seen.insert(key).second)
      return error =
                 "duplicate registry file signer config field '" + key + "'",
             std::nullopt;
    if (key == "format") {
      std::string value;
      format = manifest_toml::parseString(assignment.value, value) &&
               (value == RegistryFileSignerConfigFormat ||
                value == RegistryFileSignerConfigFormatV2);
      format_v2 = value == RegistryFileSignerConfigFormatV2;
      if (!format)
        return error = "unsupported registry file signer config format",
               std::nullopt;
    } else if (key == "registry") {
      if (!manifest_toml::parseString(assignment.value, config.registry_name) ||
          config.registry_name.empty())
        return error = "registry file signer config has an invalid registry",
               std::nullopt;
    } else if (key == "roles.snapshot") {
      if (!array(assignment, config.snapshot_secret_key_paths))
        return error = "registry file signer snapshot role is invalid",
               std::nullopt;
    } else if (key == "roles.timestamp") {
      if (!array(assignment, config.timestamp_secret_key_paths))
        return error = "registry file signer timestamp role is invalid",
               std::nullopt;
    } else if (key == "roles.audit") {
      if (!array(assignment, config.audit_secret_key_paths))
        return error = "registry file signer audit role is invalid",
               std::nullopt;
    } else if (key == "roles.witness") {
      if (!array(assignment, config.witness_secret_key_paths))
        return error = "registry file signer witness role is invalid",
               std::nullopt;
    } else {
      return error = "unknown registry file signer config field '" + key + "'",
             std::nullopt;
    }
  }
  const bool any_role = !config.snapshot_secret_key_paths.empty() ||
                        !config.timestamp_secret_key_paths.empty() ||
                        !config.audit_secret_key_paths.empty() ||
                        !config.witness_secret_key_paths.empty();
  if (!format || config.registry_name.empty() || !any_role ||
      (!format_v2 && (config.snapshot_secret_key_paths.empty() ||
                      config.timestamp_secret_key_paths.empty() ||
                      config.audit_secret_key_paths.empty() ||
                      !config.witness_secret_key_paths.empty())))
    return error = "registry file signer config is incomplete", std::nullopt;
  const auto base = std::filesystem::absolute(path).parent_path();
  for (auto *paths :
       {&config.snapshot_secret_key_paths, &config.timestamp_secret_key_paths,
        &config.audit_secret_key_paths, &config.witness_secret_key_paths})
    for (auto &key_path : *paths) {
      auto resolved = std::filesystem::path(key_path);
      if (resolved.is_relative())
        resolved = base / resolved;
      key_path = resolved.lexically_normal().string();
    }
  return config;
}

std::unique_ptr<RegistrySigningProvider>
createRegistryCommandSigningProvider(RegistryCommandSignerConfig config) {
  return std::make_unique<CommandSigningProvider>(std::move(config));
}

std::unique_ptr<RegistrySigningProvider>
createRegistryFileSigningProvider(RegistryFileSignerConfig config) {
  return std::make_unique<FileSigningProvider>(std::move(config));
}

std::string renderRegistrySignerRequest(const RegistrySignerRequest &request) {
  std::ostringstream out;
  out << RegistrySignerProtocolFormat << '\n'
      << "operation\t"
      << (request.operation == RegistrySignerOperation::Keys ? "keys" : "sign")
      << '\n'
      << "registry\t" << base64Url(request.registry_name) << '\n'
      << "role\t" << registrySigningRoleSpelling(request.role) << '\n'
      << "root-version\t" << request.root_version << '\n'
      << "threshold\t" << request.threshold << '\n'
      << "authorized-count\t" << request.authorized_key_ids.size() << '\n';
  for (const auto &key : request.authorized_key_ids)
    out << "authorized\t" << key << '\n';
  out << "payload-size\t" << request.payload.size() << '\n'
      << "payload-sha256\t" << sha256Hex(request.payload) << '\n'
      << "payload\t" << base64Url(request.payload) << '\n';
  return out.str();
}

std::optional<RegistrySignerRequest>
parseRegistrySignerRequest(std::string_view text, std::string &error) {
  if (text.size() > 2u * 1024u * 1024u)
    return error = "registry signer request exceeds its size limit",
           std::nullopt;
  auto input = lines(text);
  if (input.empty() || input.front() != RegistrySignerProtocolFormat)
    return error = "registry signer request has an invalid format",
           std::nullopt;
  RegistrySignerRequest request;
  std::map<std::string, std::string> fields;
  for (std::size_t index = 1; index < input.size(); ++index) {
    std::string_view name, value;
    if (!splitField(input[index], name, value))
      return error = "registry signer request contains an invalid field",
             std::nullopt;
    if (name == "authorized") {
      request.authorized_key_ids.emplace_back(value);
    } else if (!fields.emplace(std::string(name), std::string(value)).second) {
      return error = "registry signer request contains a duplicate field",
             std::nullopt;
    }
  }
  const std::set<std::string> expected{
      "operation",    "registry",       "role",
      "root-version", "threshold",      "authorized-count",
      "payload-size", "payload-sha256", "payload"};
  if (fields.size() != expected.size() ||
      !std::all_of(expected.begin(), expected.end(),
                   [&](const auto &field) { return fields.contains(field); }))
    return error = "registry signer request fields are incomplete or unknown",
           std::nullopt;
  auto registry = decodeBase64Url(fields["registry"]);
  auto payload = decodeBase64Url(fields["payload"]);
  auto role = parseRegistrySigningRole(fields["role"]);
  std::size_t authorized_count = 0;
  std::size_t payload_size = 0;
  if (fields["operation"] == "keys")
    request.operation = RegistrySignerOperation::Keys;
  else if (fields["operation"] == "sign")
    request.operation = RegistrySignerOperation::Sign;
  else
    return error = "registry signer request operation is invalid", std::nullopt;
  if (!registry || registry->empty() || !payload || !role ||
      !parseUnsigned(fields["root-version"], request.root_version) ||
      !parseUnsigned(fields["threshold"], request.threshold) ||
      !parseUnsigned(fields["authorized-count"], authorized_count) ||
      !parseUnsigned(fields["payload-size"], payload_size) ||
      request.root_version == 0 || request.threshold == 0 ||
      authorized_count != request.authorized_key_ids.size() ||
      request.threshold > authorized_count ||
      !sortedUniqueKeys(request.authorized_key_ids) ||
      payload_size != payload->size() || payload_size > kMaximumSignerPayload ||
      fields["payload-sha256"] != sha256Hex(*payload) ||
      (request.operation == RegistrySignerOperation::Keys &&
       !payload->empty()) ||
      (request.operation == RegistrySignerOperation::Sign && payload->empty()))
    return error = "registry signer request values are invalid", std::nullopt;
  request.registry_name = std::move(*registry);
  request.payload = std::move(*payload);
  request.role = *role;
  return request;
}

std::string
renderRegistrySignerResponse(const RegistrySignerRequest &request,
                             const RegistrySignerResponse &response) {
  std::ostringstream out;
  out << RegistrySignerProtocolFormat << '\n'
      << "operation\t"
      << (request.operation == RegistrySignerOperation::Keys ? "keys" : "sign")
      << '\n'
      << "payload-sha256\t" << response.payload_sha256 << '\n'
      << "key-count\t" << response.key_ids.size() << '\n';
  for (const auto &key : response.key_ids)
    out << "key\t" << key << '\n';
  out << "signature-count\t" << response.signatures.size() << '\n';
  for (const auto &signature : response.signatures)
    out << "signature\t" << signature.key_id << '\t' << signature.signature
        << '\n';
  return out.str();
}

std::optional<RegistrySignerResponse>
parseRegistrySignerResponse(std::string_view text,
                            const RegistrySignerRequest &request,
                            std::string &error) {
  if (text.size() > kMaximumSignerResponse)
    return error = "registry signer response exceeds its size limit",
           std::nullopt;
  auto input = lines(text);
  if (input.empty() || input.front() != RegistrySignerProtocolFormat)
    return error = "registry signer response has an invalid format",
           std::nullopt;
  RegistrySignerResponse response;
  std::map<std::string, std::string> fields;
  for (std::size_t index = 1; index < input.size(); ++index) {
    std::string_view name, value;
    if (!splitField(input[index], name, value))
      return error = "registry signer response contains an invalid field",
             std::nullopt;
    if (name == "key") {
      response.key_ids.emplace_back(value);
    } else if (name == "signature") {
      const auto tab = value.find('\t');
      if (tab == std::string_view::npos)
        return error = "registry signer response signature is malformed",
               std::nullopt;
      response.signatures.push_back({std::string(value.substr(0, tab)),
                                     std::string(value.substr(tab + 1))});
    } else if (!fields.emplace(std::string(name), std::string(value)).second) {
      return error = "registry signer response contains a duplicate field",
             std::nullopt;
    }
  }
  const std::set<std::string> expected{"operation", "payload-sha256",
                                       "key-count", "signature-count"};
  std::size_t key_count = 0;
  std::size_t signature_count = 0;
  const auto operation =
      request.operation == RegistrySignerOperation::Keys ? "keys" : "sign";
  if (fields.size() != expected.size() ||
      !std::all_of(expected.begin(), expected.end(),
                   [&](const auto &field) { return fields.contains(field); }) ||
      fields["operation"] != operation ||
      fields["payload-sha256"] != sha256Hex(request.payload) ||
      !parseUnsigned(fields["key-count"], key_count) ||
      !parseUnsigned(fields["signature-count"], signature_count) ||
      key_count != response.key_ids.size() ||
      signature_count != response.signatures.size() ||
      !sortedUniqueKeys(response.key_ids) ||
      response.key_ids.size() < request.threshold ||
      response.signatures.size() > kMaximumSignerSignatures ||
      (request.operation == RegistrySignerOperation::Keys &&
       !response.signatures.empty()) ||
      (request.operation == RegistrySignerOperation::Sign &&
       response.signatures.size() < request.threshold))
    return error = "registry signer response values are invalid", std::nullopt;
  response.payload_sha256 = fields["payload-sha256"];
  std::string previous;
  for (const auto &signature : response.signatures) {
    registry_crypto::Signature decoded{};
    if (!validKeyId(signature.key_id) ||
        (!previous.empty() && previous >= signature.key_id) ||
        !std::binary_search(response.key_ids.begin(), response.key_ids.end(),
                            signature.key_id) ||
        !registry_crypto::decodeSignature(signature.signature, decoded))
      return error = "registry signer response contains an invalid signature",
             std::nullopt;
    previous = signature.key_id;
  }
  return response;
}

bool validateRegistrySigningPayload(const RegistrySignerRequest &request,
                                    std::string &error) {
  std::string_view magic;
  std::vector<std::string> expected;
  if (request.role == RegistrySigningRole::Snapshot) {
    if (request.payload.starts_with(
            "chtholly-registry-snapshot-signature-v2\n")) {
      magic = "chtholly-registry-snapshot-signature-v2";
      expected = {"registry", "version",         "root-version",
                  "expires",  "packages-sha256", "audit-checkpoint-sha256"};
    } else {
      magic = "chtholly-registry-snapshot-signature-v1";
      expected = {"registry", "version", "root-version", "expires",
                  "packages-sha256"};
    }
  } else if (request.role == RegistrySigningRole::Timestamp) {
    magic = "chtholly-registry-timestamp-signature-v1";
    expected = {"registry", "version",          "root-version",
                "expires",  "snapshot-version", "snapshot-sha256"};
  } else if (request.role == RegistrySigningRole::Audit) {
    magic = "chtholly-registry-audit-checkpoint-v1";
    expected = {"registry",     "tree-size",   "root-hash",
                "root-version", "root-sha256", "issued-at"};
  } else {
    magic = "chtholly-registry-witness-statement-v1";
    expected = {"witness",           "registry",      "registry-origin",
                "checkpoint-sha256", "tree-size",     "root-hash",
                "root-version",      "root-sha256",   "observed-at",
                "head-tree-size",    "head-root-hash"};
  }
  auto fields = parseCanonicalPayload(request.payload, magic, expected, error);
  std::uint64_t root_version = 0;
  if (!fields || (*fields)["registry"] != request.registry_name ||
      !parseUnsigned((*fields)["root-version"], root_version) ||
      root_version != request.root_version) {
    if (error.empty())
      error = "registry signer payload does not match its request envelope";
    return false;
  }
  std::uint64_t version = 0;
  if (request.role == RegistrySigningRole::Snapshot) {
    if (!parseUnsigned((*fields)["version"], version) || version == 0 ||
        !utcTimestamp((*fields)["expires"]) ||
        !lowerHexDigest((*fields)["packages-sha256"]) ||
        (magic == "chtholly-registry-snapshot-signature-v2" &&
         !lowerHexDigest((*fields)["audit-checkpoint-sha256"]))) {
      error = "registry snapshot signer payload values are invalid";
      return false;
    }
  } else if (request.role == RegistrySigningRole::Timestamp) {
    std::uint64_t snapshot_version = 0;
    if (!parseUnsigned((*fields)["version"], version) || version == 0 ||
        !parseUnsigned((*fields)["snapshot-version"], snapshot_version) ||
        snapshot_version == 0 || !utcTimestamp((*fields)["expires"]) ||
        !lowerHexDigest((*fields)["snapshot-sha256"])) {
      error = "registry timestamp signer payload values are invalid";
      return false;
    }
  } else if (request.role == RegistrySigningRole::Audit) {
    std::uint64_t tree_size = 0;
    if (!parseUnsigned((*fields)["tree-size"], tree_size) || tree_size == 0 ||
        !lowerHexDigest((*fields)["root-hash"]) ||
        !lowerHexDigest((*fields)["root-sha256"]) ||
        !utcTimestamp((*fields)["issued-at"])) {
      error = "registry audit signer payload values are invalid";
      return false;
    }
  } else {
    std::uint64_t tree_size = 0;
    std::uint64_t head_tree_size = 0;
    if ((*fields)["witness"].empty() ||
        !validHttpsOrigin((*fields)["registry-origin"]) ||
        !lowerHexDigest((*fields)["checkpoint-sha256"]) ||
        !parseUnsigned((*fields)["tree-size"], tree_size) || tree_size == 0 ||
        !lowerHexDigest((*fields)["root-hash"]) ||
        !lowerHexDigest((*fields)["root-sha256"]) ||
        !utcTimestamp((*fields)["observed-at"]) ||
        !parseUnsigned((*fields)["head-tree-size"], head_tree_size) ||
        head_tree_size < tree_size ||
        !lowerHexDigest((*fields)["head-root-hash"])) {
      error = "registry witness signer payload values are invalid";
      return false;
    }
  }
  return true;
}

} // namespace chtholly
