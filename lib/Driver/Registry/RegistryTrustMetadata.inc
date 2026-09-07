// Included by RegistryTrust.cpp; this fragment retains the original trust
// metadata stores and verification/signing state.
bool parseRegistryUtcTimestamp(std::string_view value,
                               std::int64_t &unix_seconds) {
  return parseRfc3339(value, unix_seconds);
}

std::string
registrySnapshotSignatureInput(const RegistrySnapshotMetadata &snapshot) {
  return snapshotSignatureInput(snapshot);
}

std::string
registryTimestampSignatureInput(const RegistryTimestampMetadata &timestamp) {
  return timestampSignatureInput(timestamp);
}

std::optional<std::vector<std::string>>
registryActiveRoleKeyIds(const RegistryTrustRole &role,
                         const std::vector<std::string> &revoked_key_ids,
                         std::string &error) {
  std::vector<std::string> result;
  for (const auto &key_text : role.keys) {
    auto key = registry_crypto::parsePublicKey(key_text);
    if (!key)
      return error = "registry signing role contains an invalid public key",
             std::nullopt;
    const auto id = registry_crypto::publicKeyId(*key);
    if (std::find(revoked_key_ids.begin(), revoked_key_ids.end(), id) ==
        revoked_key_ids.end())
      result.push_back(id);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool verifyRegistryRoleSignatures(
    std::string_view payload,
    const std::vector<RegistryMetadataSignature> &signatures,
    const RegistryTrustRole &role,
    const std::vector<std::string> &revoked_key_ids, std::string &error) {
  return verifyThreshold(payload, signatures, role, revoked_key_ids, error);
}

std::optional<RegistryRootMetadata>
parseRegistryRootMetadata(std::string_view text, const std::string &path,
                          std::string &error) {
  auto assignments = manifest_toml::parseAssignments(
      text, {"roles", "signatures"}, "registry root metadata", error);
  if (!assignments) return std::nullopt;
  RegistryRootMetadata root;
  std::string format;
  for (const auto &assignment : *assignments) {
    if (assignment.table.empty() && assignment.key == "format") {
      if (!format.empty() ||
          !manifest_toml::parseString(assignment.value, format) ||
          (format != RegistryRootFormat && format != RegistryRootFormatV2)) {
        error = "invalid registry metadata format";
        return std::nullopt;
      }
    }
  }
  if (format.empty()) {
    error = "registry root metadata is missing its format";
    return std::nullopt;
  }
  root.format = format;
  std::set<std::string> seen;
  if (!parseCommon(*assignments, format, root, seen, error))
    return std::nullopt;
  for (const auto &assignment : *assignments) {
    if (assignment.table.empty() || assignment.table == "signatures") continue;
    if (assignment.table != "roles" ||
        !seen.insert(assignment.fullKey()).second) {
      error = "invalid or duplicate registry root role field";
      return std::nullopt;
    }
    RegistryTrustRole *role = nullptr;
    std::string_view suffix;
    if (assignment.key.starts_with("root_")) {
      role = &root.root; suffix = std::string_view(assignment.key).substr(5);
    } else if (assignment.key.starts_with("targets_")) {
      role = &root.targets; suffix = std::string_view(assignment.key).substr(8);
    } else if (assignment.key.starts_with("snapshot_")) {
      role = &root.snapshot; suffix = std::string_view(assignment.key).substr(9);
    } else if (assignment.key.starts_with("timestamp_")) {
      role = &root.timestamp; suffix = std::string_view(assignment.key).substr(10);
    } else if (assignment.key.starts_with("audit_")) {
      role = &root.audit;
      suffix = std::string_view(assignment.key).substr(6);
    } else if (assignment.key == "revoked_key_ids") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           root.revoked_key_ids)) {
        error = "invalid registry root revoked key list";
        return std::nullopt;
      }
      continue;
    }
    if (role == nullptr) {
      error = "unknown registry root role field '" + assignment.key + "'";
      return std::nullopt;
    }
    if (suffix == "keys") {
      if (!manifest_toml::parseStringArray(assignment.value, role->keys)) {
        error = "invalid registry root role key list";
        return std::nullopt;
      }
    } else if (suffix == "threshold") {
      std::uint64_t threshold = 0;
      if (!manifest_toml::parseUnsigned(assignment.value, threshold) ||
          threshold > UINT32_MAX) {
        error = "invalid registry root role threshold";
        return std::nullopt;
      }
      role->threshold = static_cast<std::uint32_t>(threshold);
    } else {
      error = "unknown registry root role field '" + assignment.key + "'";
      return std::nullopt;
    }
  }
  const std::set<std::string> required{
      "format", "registry", "version", "expires", "roles.root_keys",
      "roles.root_threshold", "roles.targets_keys",
      "roles.targets_threshold", "roles.snapshot_keys",
      "roles.snapshot_threshold", "roles.timestamp_keys",
      "roles.timestamp_threshold", "roles.revoked_key_ids"};
  const bool audit_complete =
      format == RegistryRootFormat ||
      (seen.contains("roles.audit_keys") &&
       seen.contains("roles.audit_threshold") && validRole(root.audit));
  if (!std::all_of(required.begin(), required.end(),
                   [&](const auto &key) { return seen.contains(key); }) ||
      !validRole(root.root) || !validRole(root.targets) ||
      !validRole(root.snapshot) || !validRole(root.timestamp) ||
      !audit_complete ||
      !validSignatures(root.signatures)) {
    error = "registry root metadata is incomplete or invalid: " + path;
    return std::nullopt;
  }
  std::sort(root.revoked_key_ids.begin(), root.revoked_key_ids.end());
  if (std::adjacent_find(root.revoked_key_ids.begin(),
                         root.revoked_key_ids.end()) !=
          root.revoked_key_ids.end() ||
      !std::all_of(root.revoked_key_ids.begin(), root.revoked_key_ids.end(),
                   [](const auto &id) {
                     return id.starts_with("sha256:") &&
                            isHexDigest(std::string_view(id).substr(7));
                   })) {
    error = "registry root metadata has invalid revoked key IDs";
    return std::nullopt;
  }
  const auto active_not_revoked = [&](const RegistryTrustRole &role) {
    return std::all_of(role.keys.begin(), role.keys.end(), [&](const auto &key) {
      const auto parsed = registry_crypto::parsePublicKey(key);
      return parsed &&
             !std::binary_search(root.revoked_key_ids.begin(),
                                 root.revoked_key_ids.end(),
                                 registry_crypto::publicKeyId(*parsed));
    });
  };
  if (!active_not_revoked(root.root) || !active_not_revoked(root.targets) ||
      !active_not_revoked(root.snapshot) ||
      !active_not_revoked(root.timestamp) ||
      (format == RegistryRootFormatV2 && !active_not_revoked(root.audit))) {
    error = "registry root metadata authorizes a revoked key";
    return std::nullopt;
  }
  return root;
}

std::optional<RegistrySnapshotMetadata>
parseRegistrySnapshotMetadata(std::string_view text, const std::string &path,
                              std::string &error) {
  auto assignments = manifest_toml::parseAssignments(
      text, {"signatures"}, "registry snapshot metadata", error);
  if (!assignments) return std::nullopt;
  RegistrySnapshotMetadata metadata;
  std::set<std::string> seen;
  std::string format;
  const auto format_assignment = std::find_if(
      assignments->begin(), assignments->end(), [](const auto &assignment) {
        return assignment.table.empty() && assignment.key == "format";
      });
  if (format_assignment == assignments->end() ||
      !manifest_toml::parseString(format_assignment->value, format) ||
      (format != RegistrySnapshotFormat &&
       format != RegistrySnapshotFormatV2)) {
    error = "invalid registry metadata format";
    return std::nullopt;
  }
  metadata.format = format;
  if (!parseCommon(*assignments, format, metadata, seen, error))
    return std::nullopt;
  for (const auto &assignment : *assignments) {
    if (!assignment.table.empty()) continue;
    if (assignment.key == "root_version") {
      if (!manifest_toml::parseUnsigned(assignment.value,
                                        metadata.root_version) ||
          metadata.root_version == 0) {
        error = "invalid registry snapshot root version";
        return std::nullopt;
      }
    } else if (assignment.key == "packages_sha256") {
      if (!manifest_toml::parseString(assignment.value,
                                      metadata.packages_sha256) ||
          !isHexDigest(metadata.packages_sha256)) {
        error = "invalid registry snapshot packages digest";
        return std::nullopt;
      }
    } else if (assignment.key == "audit_checkpoint_sha256") {
      if (metadata.format != RegistrySnapshotFormatV2 ||
          !manifest_toml::parseString(assignment.value,
                                      metadata.audit_checkpoint_sha256) ||
          !isHexDigest(metadata.audit_checkpoint_sha256)) {
        error = "invalid registry snapshot audit checkpoint digest";
        return std::nullopt;
      }
    } else if (!std::set<std::string>{"format", "registry", "version",
                                      "expires"}.contains(assignment.key)) {
      error = "unknown registry snapshot metadata field '" + assignment.key + "'";
      return std::nullopt;
    }
  }
  if (!seen.contains("format") || !seen.contains("registry") ||
      !seen.contains("version") || !seen.contains("expires") ||
      !seen.contains("root_version") || !seen.contains("packages_sha256") ||
      (metadata.format == RegistrySnapshotFormatV2 &&
       !seen.contains("audit_checkpoint_sha256")) ||
      !validSignatures(metadata.signatures)) {
    error = "registry snapshot metadata is incomplete or invalid: " + path;
    return std::nullopt;
  }
  return metadata;
}

std::optional<RegistryTimestampMetadata>
parseRegistryTimestampMetadata(std::string_view text, const std::string &path,
                               std::string &error) {
  auto assignments = manifest_toml::parseAssignments(
      text, {"signatures"}, "registry timestamp metadata", error);
  if (!assignments) return std::nullopt;
  RegistryTimestampMetadata metadata;
  std::set<std::string> seen;
  if (!parseCommon(*assignments, RegistryTimestampFormat, metadata, seen, error))
    return std::nullopt;
  for (const auto &assignment : *assignments) {
    if (!assignment.table.empty()) continue;
    if (assignment.key == "root_version") {
      if (!manifest_toml::parseUnsigned(assignment.value,
                                        metadata.root_version) ||
          metadata.root_version == 0) {
        error = "invalid registry timestamp root version";
        return std::nullopt;
      }
    } else if (assignment.key == "snapshot_version") {
      if (!manifest_toml::parseUnsigned(assignment.value,
                                        metadata.snapshot_version) ||
          metadata.snapshot_version == 0) {
        error = "invalid registry timestamp snapshot version";
        return std::nullopt;
      }
    } else if (assignment.key == "snapshot_sha256") {
      if (!manifest_toml::parseString(assignment.value,
                                      metadata.snapshot_sha256) ||
          !isHexDigest(metadata.snapshot_sha256)) {
        error = "invalid registry timestamp snapshot digest";
        return std::nullopt;
      }
    } else if (!std::set<std::string>{"format", "registry", "version",
                                      "expires"}.contains(assignment.key)) {
      error = "unknown registry timestamp metadata field '" + assignment.key + "'";
      return std::nullopt;
    }
  }
  if (!seen.contains("format") || !seen.contains("registry") ||
      !seen.contains("version") || !seen.contains("expires") ||
      !seen.contains("root_version") || !seen.contains("snapshot_version") ||
      !seen.contains("snapshot_sha256") ||
      !validSignatures(metadata.signatures)) {
    error = "registry timestamp metadata is incomplete or invalid: " + path;
    return std::nullopt;
  }
  return metadata;
}

std::string renderRegistryRootMetadata(const RegistryRootMetadata &root) {
  std::ostringstream out;
  out << "format = " << quote(root.format) << '\n'
      << "registry = " << quote(root.registry_name) << '\n'
      << "version = " << root.version << '\n'
      << "expires = " << quote(root.expires) << "\n\n[roles]\n"
      << "root_keys = " << renderArray(root.root.keys) << '\n'
      << "root_threshold = " << root.root.threshold << '\n'
      << "targets_keys = " << renderArray(root.targets.keys) << '\n'
      << "targets_threshold = " << root.targets.threshold << '\n'
      << "snapshot_keys = " << renderArray(root.snapshot.keys) << '\n'
      << "snapshot_threshold = " << root.snapshot.threshold << '\n'
      << "timestamp_keys = " << renderArray(root.timestamp.keys) << '\n'
      << "timestamp_threshold = " << root.timestamp.threshold << '\n';
  if (root.format == RegistryRootFormatV2) {
    out << "audit_keys = " << renderArray(root.audit.keys) << '\n'
        << "audit_threshold = " << root.audit.threshold << '\n';
  }
  out << "revoked_key_ids = " << renderArray(root.revoked_key_ids) << '\n'
      << renderSignatures(root.signatures);
  return out.str();
}

std::string renderRegistrySnapshotMetadata(
    const RegistrySnapshotMetadata &metadata) {
  std::ostringstream out;
  out << "format = " << quote(metadata.format) << '\n'
      << "registry = " << quote(metadata.registry_name) << '\n'
      << "version = " << metadata.version << '\n'
      << "root_version = " << metadata.root_version << '\n'
      << "expires = " << quote(metadata.expires) << '\n'
      << "packages_sha256 = " << quote(metadata.packages_sha256) << '\n';
  if (metadata.format == RegistrySnapshotFormatV2)
    out << "audit_checkpoint_sha256 = "
        << quote(metadata.audit_checkpoint_sha256) << '\n';
  out << renderSignatures(metadata.signatures);
  return out.str();
}

std::string renderRegistryTimestampMetadata(
    const RegistryTimestampMetadata &metadata) {
  std::ostringstream out;
  out << "format = " << quote(RegistryTimestampFormat) << '\n'
      << "registry = " << quote(metadata.registry_name) << '\n'
      << "version = " << metadata.version << '\n'
      << "root_version = " << metadata.root_version << '\n'
      << "expires = " << quote(metadata.expires) << '\n'
      << "snapshot_version = " << metadata.snapshot_version << '\n'
      << "snapshot_sha256 = " << quote(metadata.snapshot_sha256) << '\n'
      << renderSignatures(metadata.signatures);
  return out.str();
}
