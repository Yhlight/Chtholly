// Included by RegistryArtifact.cpp; this fragment retains registry artifact
// canonicalization, publication, lifecycle, gossip, or fetch state.
std::optional<std::string>
loadRegistryBearerToken(const std::string &environment,
                        const std::string &file_path, std::string &error) {
  RegistryPublishRequest request;
  request.token_environment = environment;
  request.token_file_path = file_path;
  return loadPublishToken(request, error);
}

bool isValidRegistryPublicKey(std::string_view key) {
  return registry_crypto::parsePublicKey(key).has_value();
}

std::optional<SignedRegistryEntry>
parseSignedRegistryEntryText(std::string_view text, const std::string &path,
                             std::string &error) {
  auto assignments = manifest_toml::parseAssignments(
      text, {"package", "artifacts"}, "signed registry entry", error);
  if (!assignments) {
    return std::nullopt;
  }
  SignedRegistryEntry entry;
  std::string format;
  for (const auto &assignment : *assignments) {
    if (assignment.table.empty() && assignment.key == "format") {
      if (!format.empty() ||
          !manifest_toml::parseString(assignment.value, format) ||
          (format != RegistryArtifactEntryFormatV2 &&
           format != RegistryArtifactEntryFormat)) {
        error = "registry entry '" + path + "' has an invalid format";
        return std::nullopt;
      }
    }
  }
  if (format.empty()) {
    error = "registry entry '" + path + "' is missing its format";
    return std::nullopt;
  }
  bool name_seen = false;
  bool version_seen = false;
  std::set<std::string> variant_names;
  for (const auto &assignment : *assignments) {
    if (assignment.table.empty() && assignment.key == "format") {
      continue;
    } else if (assignment.fullKey() == "package.name") {
      if (name_seen ||
          !manifest_toml::parseString(assignment.value, entry.package_name) ||
          entry.package_name.empty()) {
        error = "registry entry '" + path + "' has an invalid package.name";
        return std::nullopt;
      }
      name_seen = true;
    } else if (assignment.fullKey() == "package.version") {
      std::string version_error;
      if (version_seen ||
          !manifest_toml::parseString(assignment.value, entry.version) ||
          !parseSemVer(entry.version, version_error)) {
        error = "registry entry '" + path + "' has an invalid package.version";
        return std::nullopt;
      }
      version_seen = true;
    } else if (assignment.table == "artifacts") {
      if (!variant_names.insert(assignment.key).second) {
        error = "registry entry '" + path +
                "' contains a duplicate artifact variant name";
        return std::nullopt;
      }
      RegistryArtifactVariant variant;
      if (!parseVariant(assignment.key, assignment.value, format, variant,
                        error)) {
        return std::nullopt;
      }
      entry.variants.push_back(std::move(variant));
    } else {
      error =
          "unknown signed registry entry key '" + assignment.fullKey() + "'";
      return std::nullopt;
    }
  }
  if (!name_seen || !version_seen || entry.variants.empty()) {
    error = "registry entry '" + path + "' is incomplete";
    return std::nullopt;
  }
  std::sort(
      entry.variants.begin(), entry.variants.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.name < rhs.name; });
  return entry;
}

std::string renderSignedRegistryEntry(const SignedRegistryEntry &entry) {
  std::ostringstream out;
  out << "format = " << quote(RegistryArtifactEntryFormat) << "\n\n"
      << "[package]\nname = " << quote(entry.package_name)
      << "\nversion = " << quote(entry.version) << "\n\n[artifacts]\n";
  auto variants = entry.variants;
  std::sort(
      variants.begin(), variants.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.name < rhs.name; });
  for (const auto &variant : variants) {
    out << renderVariant(variant);
  }
  return out.str();
}

bool isRegistryReleaseReasonCode(std::string_view reason_code) {
  static constexpr std::array<std::string_view, 7> codes{
      "defective", "security", "policy", "superseded", "restored",
      "mistake", "unspecified"};
  return std::find(codes.begin(), codes.end(), reason_code) != codes.end();
}

std::optional<RegistryIndexEntry>
parseRegistryIndexEntryText(std::string_view text, const std::string &path,
                            std::string &error) {
  const auto canonical_header =
      "format = \"" + std::string(RegistryIndexEntryFormat) + "\"";
  if (!text.starts_with(canonical_header)) {
    auto artifact = parseSignedRegistryEntryText(text, path, error);
    if (!artifact)
      return std::nullopt;
    RegistryIndexEntry legacy;
    legacy.artifact = std::move(*artifact);
    return legacy;
  }
  auto assignments = manifest_toml::parseAssignments(
      text, {"package", "lifecycle", "artifacts"},
      "registry index entry", error);
  if (!assignments)
    return std::nullopt;
  RegistryIndexEntry result;
  std::string format;
  std::string state;
  std::set<std::string> seen;
  std::set<std::string> variants;
  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if (!seen.insert(key).second) {
      error = "registry index entry '" + path +
              "' contains a duplicate key";
      return std::nullopt;
    }
    if (key == "format") {
      if (!manifest_toml::parseString(assignment.value, format))
        format.clear();
    } else if (key == "package.name") {
      if (!manifest_toml::parseString(assignment.value,
                                      result.artifact.package_name))
        result.artifact.package_name.clear();
    } else if (key == "package.version") {
      if (!manifest_toml::parseString(assignment.value,
                                      result.artifact.version))
        result.artifact.version.clear();
    } else if (key == "lifecycle.state") {
      if (!manifest_toml::parseString(assignment.value, state))
        state.clear();
    } else if (key == "lifecycle.event_leaf_index") {
      if (!manifest_toml::parseUnsigned(assignment.value,
                                        result.state_leaf_index))
        result.state_leaf_index = UINT64_MAX;
    } else if (key == "lifecycle.event_leaf_hash") {
      if (!manifest_toml::parseString(assignment.value,
                                      result.state_leaf_hash))
        result.state_leaf_hash.clear();
    } else if (key == "lifecycle.changed_at") {
      if (!manifest_toml::parseString(assignment.value, result.changed_at))
        result.changed_at.clear();
    } else if (key == "lifecycle.reason") {
      if (!manifest_toml::parseString(assignment.value, result.reason_code))
        result.reason_code.clear();
    } else if (assignment.table == "artifacts") {
      RegistryArtifactVariant variant;
      if (!variants.insert(assignment.key).second ||
          !parseVariant(assignment.key, assignment.value,
                        std::string(RegistryArtifactEntryFormat), variant,
                        error))
        return std::nullopt;
      result.artifact.variants.push_back(std::move(variant));
    } else {
      error = "unknown registry index entry key '" + key + "'";
      return std::nullopt;
    }
  }
  std::string version_error;
  std::int64_t changed_at = 0;
  const bool valid_state = state == "active" || state == "yanked";
  if (format != RegistryIndexEntryFormat ||
      result.artifact.package_name.empty() ||
      !parseSemVer(result.artifact.version, version_error) ||
      result.artifact.variants.empty() || !valid_state ||
      !isHexDigest(result.state_leaf_hash) ||
      !parseRegistryUtcTimestamp(result.changed_at, changed_at) ||
      !isRegistryReleaseReasonCode(result.reason_code)) {
    error = "registry index entry '" + path + "' is incomplete or invalid";
    return std::nullopt;
  }
  result.state = state == "yanked" ? RegistryReleaseState::Yanked
                                    : RegistryReleaseState::Active;
  std::sort(result.artifact.variants.begin(), result.artifact.variants.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.name < rhs.name;
            });
  return result;
}

std::string renderRegistryIndexEntry(const RegistryIndexEntry &entry) {
  std::ostringstream out;
  out << "format = " << quote(RegistryIndexEntryFormat) << "\n\n"
      << "[package]\nname = " << quote(entry.artifact.package_name)
      << "\nversion = " << quote(entry.artifact.version) << "\n\n"
      << "[lifecycle]\nstate = "
      << quote(entry.state == RegistryReleaseState::Yanked ? "yanked"
                                                           : "active")
      << "\nevent_leaf_index = " << entry.state_leaf_index
      << "\nevent_leaf_hash = " << quote(entry.state_leaf_hash)
      << "\nchanged_at = " << quote(entry.changed_at)
      << "\nreason = " << quote(entry.reason_code) << "\n\n"
      << "[artifacts]\n";
  auto variants = entry.artifact.variants;
  std::sort(variants.begin(), variants.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.name < rhs.name;
            });
  for (const auto &variant : variants)
    out << renderVariant(variant);
  return out.str();
}

std::string canonicalRegistryArtifactSignatureInput(
    std::string_view registry_name, std::string_view package_name,
    std::string_view version, const RegistryArtifactVariant &variant,
    std::string_view signing_key_id) {
  std::ostringstream out;
  out << "chtholly-registry-artifact-signature-v1\n";
  appendField(out, "registry", registry_name);
  appendField(out, "package", package_name);
  appendField(out, "version", version);
  appendRegistryArtifactFacts(out, variant);
  appendField(out, "key-id", signing_key_id);
  return out.str();
}

std::string
registryArtifactVariantName(const RegistryArtifactVariant &variant) {
  std::ostringstream facts;
  facts << "chtholly-registry-artifact-facts-v3\n";
  appendRegistryArtifactFacts(facts, variant);
  return "variant-" + sha256Hex(facts.str());
}

std::optional<RegistryArtifactVerification> verifyRegistryArtifactVariant(
    std::string_view registry_name, std::string_view package_name,
    std::string_view version, const RegistryArtifactVariant &variant,
    const std::vector<std::string> &trusted_keys,
    std::uint32_t required_threshold, std::string &error) {
  if (required_threshold == 0 || required_threshold > trusted_keys.size()) {
    error = "registry artifact targets signature threshold is invalid";
    return std::nullopt;
  }
  if (!registry_crypto::initialize(error)) {
    return std::nullopt;
  }
  std::map<std::string, std::array<unsigned char, crypto_sign_PUBLICKEYBYTES>>
      keys;
  for (const auto &trusted : trusted_keys) {
    auto candidate = registry_crypto::parsePublicKey(trusted);
    if (!candidate) {
      error = "registry contains an invalid trusted Ed25519 public key";
      return std::nullopt;
    }
    keys.emplace(registry_crypto::publicKeyId(*candidate), *candidate);
  }
  RegistryArtifactVerification verification;
  verification.required_threshold = required_threshold;
  for (const auto &candidate : variant.signatures) {
    const auto key = keys.find(candidate.key_id);
    if (key == keys.end())
      continue;
    registry_crypto::Signature signature{};
    if (!registry_crypto::decodeSignature(candidate.signature, signature)) {
      continue;
    }
    const auto message = canonicalRegistryArtifactSignatureInput(
        registry_name, package_name, version, variant, candidate.key_id);
    if (crypto_sign_verify_detached(
            signature.data(),
            reinterpret_cast<const unsigned char *>(message.data()),
            static_cast<unsigned long long>(message.size()),
            key->second.data()) == 0) {
      verification.valid_signing_key_ids.push_back(candidate.key_id);
    }
  }
  std::sort(verification.valid_signing_key_ids.begin(),
            verification.valid_signing_key_ids.end());
  verification.valid_signing_key_ids.erase(
      std::unique(verification.valid_signing_key_ids.begin(),
                  verification.valid_signing_key_ids.end()),
      verification.valid_signing_key_ids.end());
  if (verification.valid_signing_key_ids.size() < required_threshold) {
    error =
        "registry artifact targets signature threshold was not satisfied (" +
        std::to_string(verification.valid_signing_key_ids.size()) + "/" +
        std::to_string(required_threshold) + ")";
    return std::nullopt;
  }
  return verification;
}
