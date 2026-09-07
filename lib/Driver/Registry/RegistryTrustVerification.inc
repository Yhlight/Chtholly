// Included by RegistryTrust.cpp; this fragment retains the original trust
// metadata stores and verification/signing state.
std::string registryTrustStatePath(std::string_view identity_store_root,
                                   std::string_view registry_name,
                                   std::string_view registry_index) {
  std::ostringstream identity;
  appendField(identity, "registry", registry_name);
  appendField(identity, "index", registry_index);
  return (std::filesystem::path(identity_store_root) / "trust" / "registries" /
          sha256Hex(identity.str()) / "state.toml")
      .string();
}

std::optional<std::string>
computeRegistryPackagesDigest(const std::string &checkout_root,
                              std::string &error) {
  const auto packages = std::filesystem::path(checkout_root) / "packages";
  std::error_code ec;
  if (!std::filesystem::is_directory(packages, ec) || ec) {
    error = "registry packages directory is missing";
    return std::nullopt;
  }
  struct File { std::string path; std::uint64_t size; std::string digest; };
  std::vector<File> files;
  for (std::filesystem::recursive_directory_iterator it(packages, ec), end;
       !ec && it != end; it.increment(ec)) {
    const auto status = it->symlink_status(ec);
    if (ec || std::filesystem::is_symlink(status)) {
      error = "registry packages tree contains a symlink";
      return std::nullopt;
    }
    if (std::filesystem::is_directory(status)) continue;
    if (!std::filesystem::is_regular_file(status)) {
      error = "registry packages tree contains a non-regular file";
      return std::nullopt;
    }
    const auto relative = std::filesystem::relative(it->path(), packages, ec)
                              .generic_string();
    const auto digest = sha256File(it->path().string());
    const auto size = std::filesystem::file_size(it->path(), ec);
    if (ec || !digest || relative.empty()) {
      error = "failed to inspect registry packages tree";
      return std::nullopt;
    }
    files.push_back({relative, size, *digest});
  }
  if (ec) {
    error = "failed to enumerate registry packages tree: " + ec.message();
    return std::nullopt;
  }
  std::sort(files.begin(), files.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.path < rhs.path; });
  std::ostringstream canonical;
  canonical << "chtholly-registry-packages-tree-v1\n";
  for (const auto &file : files) {
    appendField(canonical, "path", file.path);
    appendField(canonical, "size", std::to_string(file.size));
    appendField(canonical, "sha256", file.digest);
  }
  return sha256Hex(canonical.str());
}

std::optional<VerifiedRegistryRoot>
verifyRegistryRootChain(const RegistryRootChainVerificationRequest &request,
                    std::string &error) {
  if (request.registry_name.empty() || request.checkout_root.empty() ||
      request.bootstrap_root_threshold == 0 ||
      request.bootstrap_root_threshold > request.bootstrap_root_keys.size()) {
    error = "registry root chain verification request is incomplete";
    return std::nullopt;
  }
  RegistryTrustRole bootstrap{request.bootstrap_root_keys,
                              request.bootstrap_root_threshold};
  if (!validRole(bootstrap)) {
    error = "registry bootstrap root role is invalid";
    return std::nullopt;
  }

  const auto root_dir = std::filesystem::path(request.checkout_root) / "trust" /
                        "root";
  RegistryRootMetadata current;
  RegistryTrustRole previous_role = bootstrap;
  std::vector<std::string> previous_revoked;
  for (std::uint64_t version = 1;; ++version) {
    const auto path = root_dir / (std::to_string(version) + ".toml");
    if (!std::filesystem::exists(path)) break;
    auto text = readTextFile(path.string(), error);
    auto root = text ? parseRegistryRootMetadata(*text, path.string(), error)
                     : std::optional<RegistryRootMetadata>{};
    if (!root) return std::nullopt;
    if (root->registry_name != request.registry_name ||
        root->version != version ||
        !std::includes(root->revoked_key_ids.begin(),
                       root->revoked_key_ids.end(), previous_revoked.begin(),
                       previous_revoked.end())) {
      error = "registry root rotation is non-contiguous or revocation regressed";
      return std::nullopt;
    }
    const auto payload = rootPayload(*root);
    if (!verifyThreshold(payload, root->signatures, previous_role,
                         previous_revoked, error) ||
        !verifyThreshold(payload, root->signatures, root->root,
                         root->revoked_key_ids, error)) {
      error = "registry root " + std::to_string(version) + ": " + error;
      return std::nullopt;
    }
    previous_role = root->root;
    previous_revoked = root->revoked_key_ids;
    current = std::move(*root);
  }
  if (current.version == 0) {
    error = "registry trust root metadata is missing";
    return std::nullopt;
  }
  const auto now =
      request.now_unix_seconds >= 0
          ? request.now_unix_seconds
          : static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()));
  std::int64_t expiry = 0;
  if (!parseRfc3339(current.expires, expiry) || now >= expiry) {
    error = "registry root metadata is expired";
    return std::nullopt;
  }
  const auto digest = sha256Hex(renderRegistryRootMetadata(current));
  return VerifiedRegistryRoot{std::move(current), digest};
}

std::optional<VerifiedRegistryTrust>
verifyRegistryTrust(const RegistryTrustVerificationRequest &request,
                    std::string &error) {
  if (request.registry_name.empty() || request.checkout_root.empty() ||
      request.identity_store_root.empty() ||
      request.bootstrap_root_threshold == 0) {
    error = "registry trust verification request is incomplete";
    return std::nullopt;
  }
  const auto state_path = std::filesystem::path(
      registryTrustStatePath(request.identity_store_root, request.registry_name,
                             request.registry_index));
  TrustStateLock lock(state_path.parent_path() / ".lock", error);
  if (!lock.locked()) return std::nullopt;
  auto state = loadState(state_path, error);
  if (!state) return std::nullopt;

  RegistryRootChainVerificationRequest root_request;
  root_request.registry_name = request.registry_name;
  root_request.checkout_root = request.checkout_root;
  root_request.bootstrap_root_keys = request.bootstrap_root_keys;
  root_request.bootstrap_root_threshold = request.bootstrap_root_threshold;
  root_request.now_unix_seconds = request.now_unix_seconds;
  auto verified_root = verifyRegistryRootChain(root_request, error);
  if (!verified_root) return std::nullopt;
  auto current = std::move(verified_root->root);
  const auto snapshot_path = std::filesystem::path(request.checkout_root) /
                             "trust" / "snapshot.toml";
  const auto timestamp_path = std::filesystem::path(request.checkout_root) /
                              "trust" / "timestamp.toml";
  auto snapshot_text = readTextFile(snapshot_path.string(), error);
  auto timestamp_text = readTextFile(timestamp_path.string(), error);
  if (!snapshot_text || !timestamp_text) return std::nullopt;
  auto snapshot = parseRegistrySnapshotMetadata(*snapshot_text,
                                                snapshot_path.string(), error);
  auto timestamp = parseRegistryTimestampMetadata(*timestamp_text,
                                                  timestamp_path.string(), error);
  if (!snapshot || !timestamp) return std::nullopt;
  const auto root_digest = verified_root->root_sha256;
  const auto snapshot_digest =
      sha256Hex(renderRegistrySnapshotMetadata(*snapshot));
  const auto timestamp_digest =
      sha256Hex(renderRegistryTimestampMetadata(*timestamp));
  if (snapshot->registry_name != request.registry_name ||
      timestamp->registry_name != request.registry_name ||
      snapshot->root_version != current.version ||
      timestamp->root_version != current.version ||
      timestamp->snapshot_version != snapshot->version ||
      timestamp->snapshot_sha256 != snapshot_digest) {
    error = "registry freshness metadata does not form one trusted chain";
    return std::nullopt;
  }
  if (!verifyThreshold(snapshotSignatureInput(*snapshot), snapshot->signatures,
                       current.snapshot, current.revoked_key_ids, error) ||
      !verifyThreshold(timestampSignatureInput(*timestamp), timestamp->signatures,
                       current.timestamp, current.revoked_key_ids, error)) {
    return std::nullopt;
  }
  std::optional<RegistryAuditCheckpoint> audit_checkpoint;
  if (snapshot->format == RegistrySnapshotFormatV2) {
    const auto checkpoint_path = std::filesystem::path(request.checkout_root) /
                                 "trust" / "audit-checkpoint.txt";
    auto checkpoint_text = readTextFile(checkpoint_path.string(), error);
    auto checkpoint =
        checkpoint_text ? parseRegistryAuditCheckpoint(*checkpoint_text, error)
                        : std::optional<RegistryAuditCheckpoint>{};
    if (!checkpoint ||
        sha256Hex(*checkpoint_text) != snapshot->audit_checkpoint_sha256 ||
        !verifyRegistryAuditCheckpoint(*checkpoint, current, error)) {
      if (error.empty())
        error = "registry snapshot audit checkpoint is invalid";
      return std::nullopt;
    }
    audit_checkpoint = std::move(*checkpoint);
  }
  const auto now =
      request.now_unix_seconds >= 0
          ? request.now_unix_seconds
          : static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()));
  const auto effective_now = std::max(now, state->last_verified_time);
  std::int64_t root_expiry = 0, snapshot_expiry = 0, timestamp_expiry = 0;
  if (!parseRfc3339(current.expires, root_expiry) ||
      !parseRfc3339(snapshot->expires, snapshot_expiry) ||
      !parseRfc3339(timestamp->expires, timestamp_expiry) ||
      effective_now >= root_expiry || effective_now >= snapshot_expiry ||
      effective_now >= timestamp_expiry) {
    error = "registry trust metadata is expired";
    return std::nullopt;
  }
  if (!checkRollback("root", current.version, root_digest,
                     state->root_version, state->root_sha256, error) ||
      !checkRollback("snapshot", snapshot->version, snapshot_digest,
                     state->snapshot_version, state->snapshot_sha256, error) ||
      !checkRollback("timestamp", timestamp->version, timestamp_digest,
                     state->timestamp_version, state->timestamp_sha256, error)) {
    return std::nullopt;
  }
  auto packages_digest =
      computeRegistryPackagesDigest(request.checkout_root, error);
  if (!packages_digest || *packages_digest != snapshot->packages_sha256) {
    if (error.empty()) error = "registry packages tree does not match snapshot";
    return std::nullopt;
  }
  TrustState next{current.version, root_digest, snapshot->version,
                  snapshot_digest, timestamp->version, timestamp_digest,
                  effective_now};
  if (request.persist_state && !writeState(state_path, next, error))
    return std::nullopt;
  VerifiedRegistryTrust result;
  result.root = std::move(current);
  result.snapshot = std::move(*snapshot);
  result.timestamp = std::move(*timestamp);
  result.root_sha256 = root_digest;
  result.snapshot_sha256 = snapshot_digest;
  result.timestamp_sha256 = timestamp_digest;
  result.packages_sha256 = *packages_digest;
  result.audit_checkpoint = std::move(audit_checkpoint);
  return result;
}

bool commitVerifiedRegistryTrust(
    const RegistryTrustVerificationRequest &request,
    const VerifiedRegistryTrust &trust, std::string &error) {
  if (request.registry_name.empty() || request.registry_index.empty() ||
      request.identity_store_root.empty() ||
      trust.root.registry_name != request.registry_name ||
      trust.snapshot.registry_name != request.registry_name ||
      trust.timestamp.registry_name != request.registry_name) {
    error = "registry verified trust commit request is incomplete";
    return false;
  }
  const auto state_path = std::filesystem::path(
      registryTrustStatePath(request.identity_store_root, request.registry_name,
                             request.registry_index));
  TrustStateLock lock(state_path.parent_path() / ".lock", error);
  if (!lock.locked())
    return false;
  auto state = loadState(state_path, error);
  if (!state)
    return false;
  if (!checkRollback("root", trust.root.version, trust.root_sha256,
                     state->root_version, state->root_sha256, error) ||
      !checkRollback("snapshot", trust.snapshot.version, trust.snapshot_sha256,
                     state->snapshot_version, state->snapshot_sha256, error) ||
      !checkRollback("timestamp", trust.timestamp.version,
                     trust.timestamp_sha256, state->timestamp_version,
                     state->timestamp_sha256, error))
    return false;
  const auto now =
      request.now_unix_seconds >= 0
          ? request.now_unix_seconds
          : static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()));
  TrustState next{trust.root.version,
                  trust.root_sha256,
                  trust.snapshot.version,
                  trust.snapshot_sha256,
                  trust.timestamp.version,
                  trust.timestamp_sha256,
                  std::max(now, state->last_verified_time)};
  return writeState(state_path, next, error);
}
