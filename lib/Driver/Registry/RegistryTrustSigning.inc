// Included by RegistryTrust.cpp; this fragment retains the original trust
// metadata stores and verification/signing state.
bool signRegistryRootMetadataFile(const std::string &input_path,
                                  const std::string &secret_key_path,
                                  const std::string &output_path,
                                  std::string &error) {
  auto text = readTextFile(input_path, error);
  auto root = text ? parseRegistryRootMetadata(*text, input_path, error)
                   : std::optional<RegistryRootMetadata>{};
  if (!root) return false;
  if (!addSignature(*root, rootPayload(*root), secret_key_path, error))
    return false;
  const auto output = renderRegistryRootMetadata(*root);
  const auto temporary = output_path + ".tmp";
  if (!writeTextFile(temporary, output, error)) return false;
  std::error_code ec;
  if (!replaceFile(temporary, output_path, ec)) {
    error = "failed to publish signed registry root metadata: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}

bool sealRegistryIndex(const RegistryIndexSealRequest &request,
                       const RegistrySigningProvider &signer,
                       VerifiedRegistryTrust &sealed, std::string &error) {
  if (!validExpiry(request.snapshot_expires) ||
      !validExpiry(request.timestamp_expires)) {
    error = "registry index seal request is incomplete";
    return false;
  }
  const auto trust = std::filesystem::path(request.index_root) / "trust";
  const auto root_dir = trust / "root";
  RegistryRootMetadata root;
  RegistryTrustRole previous_role;
  std::vector<std::string> previous_revoked;
  for (std::uint64_t version = 1;; ++version) {
    const auto path = root_dir / (std::to_string(version) + ".toml");
    if (!std::filesystem::exists(path)) break;
    auto text = readTextFile(path.string(), error);
    auto parsed = text ? parseRegistryRootMetadata(*text, path.string(), error)
                       : std::optional<RegistryRootMetadata>{};
    if (!parsed || parsed->version != version ||
        parsed->registry_name != request.registry_name) return false;
    if (version == 1) previous_role = parsed->root;
    if (!std::includes(parsed->revoked_key_ids.begin(),
                       parsed->revoked_key_ids.end(),
                       previous_revoked.begin(), previous_revoked.end()) ||
        !verifyThreshold(rootPayload(*parsed), parsed->signatures,
                         previous_role, previous_revoked, error) ||
        !verifyThreshold(rootPayload(*parsed), parsed->signatures,
                         parsed->root, parsed->revoked_key_ids, error)) {
      if (error.empty()) error = "registry root rotation chain is invalid";
      return false;
    }
    previous_role = parsed->root;
    previous_revoked = parsed->revoked_key_ids;
    root = std::move(*parsed);
  }
  if (root.version == 0) {
    if (error.empty()) error = "registry index has no valid current root";
    return false;
  }
  std::optional<std::string> checkpoint_text;
  if (request.audit_checkpoint) {
    if (root.format != RegistryRootFormatV2 ||
        !verifyRegistryAuditCheckpoint(*request.audit_checkpoint, root,
                                       error)) {
      if (error.empty())
        error = "registry index checkpoint cannot be verified by current root";
      return false;
    }
    checkpoint_text = renderRegistryAuditCheckpoint(*request.audit_checkpoint);
  }
  auto packages = computeRegistryPackagesDigest(request.index_root, error);
  if (!packages) return false;
  RegistrySnapshotMetadata snapshot;
  if (checkpoint_text) {
    snapshot.format = std::string(RegistrySnapshotFormatV2);
    snapshot.audit_checkpoint_sha256 = sha256Hex(*checkpoint_text);
  }
  snapshot.registry_name = request.registry_name;
  snapshot.version = 1;
  snapshot.root_version = root.version;
  snapshot.expires = request.snapshot_expires;
  snapshot.packages_sha256 = *packages;
  const auto snapshot_path = trust / "snapshot.toml";
  std::optional<std::string> previous_snapshot_text;
  if (std::filesystem::exists(snapshot_path)) {
    previous_snapshot_text = readTextFile(snapshot_path.string(), error);
    auto previous = previous_snapshot_text
                        ? parseRegistrySnapshotMetadata(*previous_snapshot_text,
                                                        snapshot_path.string(),
                                                        error)
                        : std::optional<RegistrySnapshotMetadata>{};
    if (!previous) return false;
    snapshot.version = previous->version + 1;
  }
  if (!signWithProvider(signer, RegistrySigningRole::Snapshot, root.snapshot,
                        root.revoked_key_ids, request.registry_name,
                        root.version, snapshotSignatureInput(snapshot),
                        snapshot.signatures, error))
    return false;
  const auto snapshot_text = renderRegistrySnapshotMetadata(snapshot);

  RegistryTimestampMetadata timestamp;
  timestamp.registry_name = request.registry_name;
  timestamp.version = 1;
  timestamp.root_version = root.version;
  timestamp.expires = request.timestamp_expires;
  timestamp.snapshot_version = snapshot.version;
  timestamp.snapshot_sha256 = sha256Hex(snapshot_text);
  const auto timestamp_path = trust / "timestamp.toml";
  if (std::filesystem::exists(timestamp_path)) {
    auto previous_timestamp_text = readTextFile(timestamp_path.string(), error);
    auto previous = previous_timestamp_text
                        ? parseRegistryTimestampMetadata(*previous_timestamp_text,
                                                         timestamp_path.string(),
                                                         error)
                        : std::optional<RegistryTimestampMetadata>{};
    if (!previous) return false;
    timestamp.version = previous->version + 1;
  }
  if (!signWithProvider(signer, RegistrySigningRole::Timestamp, root.timestamp,
                        root.revoked_key_ids, request.registry_name,
                        root.version, timestampSignatureInput(timestamp),
                        timestamp.signatures, error))
    return false;
  const auto timestamp_text = renderRegistryTimestampMetadata(timestamp);
  std::error_code ec;
  std::filesystem::create_directories(trust, ec);
  if (ec) {
    error = "failed to create registry trust directory: " + ec.message();
    return false;
  }
  const auto checkpoint_path = trust / "audit-checkpoint.txt";
  std::optional<std::string> previous_checkpoint_text;
  if (checkpoint_text && std::filesystem::exists(checkpoint_path)) {
    previous_checkpoint_text = readTextFile(checkpoint_path.string(), error);
    if (!previous_checkpoint_text)
      return false;
  }
  const auto checkpoint_temp = checkpoint_path.string() + ".tmp";
  const auto snapshot_temp = snapshot_path.string() + ".tmp";
  const auto timestamp_temp = timestamp_path.string() + ".tmp";
  if ((checkpoint_text &&
       !writeTextFile(checkpoint_temp, *checkpoint_text, error)) ||
      !writeTextFile(snapshot_temp, snapshot_text, error) ||
      !writeTextFile(timestamp_temp, timestamp_text, error)) {
    if (error.empty())
      error = "failed to publish sealed registry index metadata";
    removeFile(checkpoint_temp, ec);
    removeFile(snapshot_temp, ec);
    removeFile(timestamp_temp, ec);
    return false;
  }
  bool checkpoint_published = false;
  const auto rollback_checkpoint = [&](std::string &rollback_error) {
    if (!checkpoint_published)
      return true;
    std::error_code rollback_ec;
    if (previous_checkpoint_text) {
      const auto rollback = checkpoint_path.string() + ".rollback";
      if (!writeTextFile(rollback, *previous_checkpoint_text, rollback_error))
        return false;
      const bool restored =
          replaceFile(rollback, checkpoint_path.string(), rollback_ec);
      removeFile(rollback, rollback_ec);
      if (!restored && rollback_error.empty())
        rollback_error = rollback_ec.message();
      return restored;
    }
    const bool removed = removeFile(checkpoint_path.string(), rollback_ec);
    if ((!removed || rollback_ec) && rollback_error.empty())
      rollback_error = rollback_ec.message();
    return removed && !rollback_ec;
  };
  if (checkpoint_text) {
    if (!replaceFile(checkpoint_temp, checkpoint_path.string(), ec)) {
      error = "failed to publish sealed registry checkpoint: " + ec.message();
      removeFile(checkpoint_temp, ec);
      removeFile(snapshot_temp, ec);
      removeFile(timestamp_temp, ec);
      return false;
    }
    checkpoint_published = true;
  }
  if (!replaceFile(snapshot_temp, snapshot_path.string(), ec)) {
    error = "failed to publish sealed registry snapshot: " + ec.message();
    std::string rollback_error;
    if (!rollback_checkpoint(rollback_error))
      error += "; checkpoint rollback failed: " + rollback_error;
    removeFile(snapshot_temp, ec);
    removeFile(timestamp_temp, ec);
    return false;
  }
  if (!replaceFile(timestamp_temp, timestamp_path.string(), ec)) {
    const auto publish_error = ec.message();
    const auto rollback = snapshot_path.string() + ".rollback";
    bool rollback_succeeded = false;
    std::string rollback_error;
    if (previous_snapshot_text) {
      if (writeTextFile(rollback, *previous_snapshot_text, rollback_error)) {
        std::error_code rollback_ec;
        rollback_succeeded =
            replaceFile(rollback, snapshot_path.string(), rollback_ec);
        if (!rollback_succeeded) rollback_error = rollback_ec.message();
      }
    } else {
      std::error_code rollback_ec;
      rollback_succeeded =
          removeFile(snapshot_path.string(), rollback_ec) && !rollback_ec;
      if (!rollback_succeeded) rollback_error = rollback_ec.message();
    }
    std::error_code cleanup_ec;
    removeFile(rollback, cleanup_ec);
    removeFile(timestamp_temp, cleanup_ec);
    error = "failed to publish sealed registry timestamp: " + publish_error;
    std::string checkpoint_rollback_error;
    const bool checkpoint_rollback_succeeded =
        rollback_checkpoint(checkpoint_rollback_error);
    if (rollback_succeeded) {
      error += "; snapshot was rolled back";
    } else {
      error += "; snapshot rollback failed";
      if (!rollback_error.empty()) error += ": " + rollback_error;
      error += "; registry metadata requires repair";
    }
    if (!checkpoint_rollback_succeeded) {
      error += "; checkpoint rollback failed";
      if (!checkpoint_rollback_error.empty())
        error += ": " + checkpoint_rollback_error;
    }
    return false;
  }
  sealed = VerifiedRegistryTrust{};
  sealed.root = root;
  sealed.snapshot = snapshot;
  sealed.timestamp = timestamp;
  sealed.root_sha256 = sha256Hex(renderRegistryRootMetadata(root));
  sealed.snapshot_sha256 = sha256Hex(snapshot_text);
  sealed.timestamp_sha256 = sha256Hex(timestamp_text);
  sealed.packages_sha256 = *packages;
  sealed.audit_checkpoint = request.audit_checkpoint;
  return true;
}
