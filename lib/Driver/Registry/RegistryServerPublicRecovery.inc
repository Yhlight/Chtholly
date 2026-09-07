// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
bool RegistryPublicationStore::resealIndex(std::int64_t now_unix_seconds,
                                           std::string &error) {
  if (!impl_) {
    error = "registry publication store is not open";
    return false;
  }
  const auto now = effectiveNow(now_unix_seconds);
  std::lock_guard lock(impl_->mutex);
  Statement latest(
      impl_->database,
      "SELECT m.receipt FROM mutations m JOIN audit_leaves a ON "
      "a.mutation_id = m.mutation_id ORDER BY a.leaf_index DESC LIMIT 1",
      error);
  if (!latest || latest.step() != SQLITE_ROW) {
    if (error.empty())
      error = "registry index cannot be resealed before its first audit event";
    return false;
  }
  const auto receipt_text = latest.text(0);
  auto publish = parseRegistryAuditReceipt(receipt_text, error);
  std::optional<RegistryLifecycleReceipt> lifecycle;
  if (!publish) {
    error.clear();
    lifecycle = parseRegistryLifecycleReceipt(receipt_text, error);
  }
  if (!publish && !lifecycle)
    return false;
  const auto checkpoint = publish ? publish->checkpoint : lifecycle->checkpoint;
  const auto max_time = (std::numeric_limits<std::int64_t>::max)();
  if (now < 0 ||
      impl_->config.snapshot_validity_seconds >
          static_cast<std::uint64_t>(max_time - now) ||
      impl_->config.timestamp_validity_seconds >
          static_cast<std::uint64_t>(max_time - now)) {
    error = "registry metadata validity interval overflows time";
    return false;
  }
  RegistryIndexSealRequest seal;
  seal.registry_name = impl_->config.registry_name;
  seal.index_root = impl_->config.index_worktree;
  seal.snapshot_expires = rfc3339(
      now + static_cast<std::int64_t>(impl_->config.snapshot_validity_seconds));
  seal.timestamp_expires =
      rfc3339(now + static_cast<std::int64_t>(
                        impl_->config.timestamp_validity_seconds));
  seal.audit_checkpoint = checkpoint;
  VerifiedRegistryTrust sealed;
  if (seal.snapshot_expires.empty() || seal.timestamp_expires.empty() ||
      !impl_->signer || !sealRegistryIndex(seal, *impl_->signer, sealed, error))
    return false;
  if (std::filesystem::exists(
          std::filesystem::path(impl_->config.index_worktree) / ".git")) {
    if (!runGit(impl_->config.index_worktree,
                {"add", "--", "trust/audit-checkpoint.txt",
                 "trust/snapshot.toml", "trust/timestamp.toml"},
                error) ||
        !runGit(impl_->config.index_worktree,
                {"commit", "-m",
                 "registry transparency reseal " +
                     std::to_string(checkpoint.tree_size)},
                error) ||
        (!impl_->config.git_remote.empty() &&
         !runGit(impl_->config.index_worktree,
                 {"push", impl_->config.git_remote,
                  "HEAD:refs/heads/" + impl_->config.git_branch},
                 error)))
      return false;
  }
  return true;
}

bool RegistryPublicationStore::createBackup(const std::string &archive_path,
                                            std::int64_t now_unix_seconds,
                                            std::string &error) const {
  if (!impl_ || archive_path.empty()) {
    error = "registry backup output path is missing";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  RegistryBackupSource source;
  source.registry_name = impl_->config.registry_name;
  source.state_directory = impl_->config.state_directory;
  source.index_worktree = impl_->config.index_worktree;
  source.bootstrap_root_keys = impl_->config.bootstrap_root_keys;
  source.bootstrap_root_threshold = impl_->config.bootstrap_root_threshold;
  source.now_unix_seconds = now_unix_seconds;
  return createRegistryBackupArchive(source, archive_path, error).has_value();
}

std::optional<RegistryRecoveryPointInfo>
RegistryPublicationStore::createRecoveryPoint(const std::string &archive_path,
                                              std::int64_t now_unix_seconds,
                                              std::string &error) {
  if (!impl_ || archive_path.empty() ||
      impl_->config.recovery_point_seconds == 0) {
    error = "registry recovery points require configured retention";
    return std::nullopt;
  }
  const auto now = effectiveNow(now_unix_seconds);
  if (now < 0 || impl_->config.recovery_point_seconds >
                     static_cast<std::uint64_t>(
                         (std::numeric_limits<std::int64_t>::max)() - now)) {
    error = "registry recovery point retention interval overflows time";
    return std::nullopt;
  }
  const auto retain_until =
      now + static_cast<std::int64_t>(impl_->config.recovery_point_seconds);
  std::lock_guard lock(impl_->mutex);
  RegistryBackupSource source;
  source.registry_name = impl_->config.registry_name;
  source.state_directory = impl_->config.state_directory;
  source.index_worktree = impl_->config.index_worktree;
  source.bootstrap_root_keys = impl_->config.bootstrap_root_keys;
  source.bootstrap_root_threshold = impl_->config.bootstrap_root_threshold;
  source.now_unix_seconds = now;
  const auto recovery_id = sha256Hex(
      "chtholly-registry-recovery-point-v1\n" + impl_->config.registry_name +
      "\n" + std::to_string(now) + "\n" + archive_path + "\n" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  source.recovery_point_id = recovery_id;
  source.retain_until = rfc3339(retain_until);
  auto backup = createRegistryBackupArchive(source, archive_path, error);
  if (!backup)
    return std::nullopt;
  Transaction transaction(impl_->database, error);
  Statement point(
      impl_->database,
      "INSERT INTO recovery_points(recovery_id, created_at, retain_until, "
      "archive_path) VALUES(?, ?, ?, ?)",
      error);
  Statement blobs(impl_->database, "SELECT sha256 FROM blobs ORDER BY sha256",
                  error);
  if (!transaction || !point || !blobs ||
      !point.bind(1, recovery_id, error) || !point.bind(2, now, error) ||
      !point.bind(3, retain_until, error) ||
      !point.bind(4, std::filesystem::absolute(archive_path).string(), error) ||
      point.step() != SQLITE_DONE)
    return std::nullopt;
  std::uint64_t blob_count = 0;
  while (blobs.step() == SQLITE_ROW) {
    Statement reference(
        impl_->database,
        "INSERT INTO recovery_point_blobs(recovery_id, sha256) VALUES(?, ?)",
        error);
    if (!reference || !reference.bind(1, recovery_id, error) ||
        !reference.bind(2, blobs.text(0), error) ||
        reference.step() != SQLITE_DONE)
      return std::nullopt;
    ++blob_count;
  }
  if (!transaction.commit(error))
    return std::nullopt;
  RegistryRecoveryPointInfo result;
  result.id = recovery_id;
  result.created_at = rfc3339(now);
  result.retain_until = rfc3339(retain_until);
  result.blob_count = blob_count;
  return result;
}

bool RegistryPublicationStore::extendRecoveryPoint(std::string_view id,
                                                   std::int64_t retain_until,
                                                   std::string &error) {
  if (!impl_ || !isHexDigest(id) || retain_until <= effectiveNow(-1)) {
    error = "registry recovery point extension is invalid";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  Statement statement(
      impl_->database,
      "UPDATE recovery_points SET retain_until = ? WHERE recovery_id = ? "
      "AND released = 0 AND retain_until < ?",
      error);
  if (!statement || !statement.bind(1, retain_until, error) ||
      !statement.bind(2, id, error) ||
      !statement.bind(3, retain_until, error) ||
      statement.step() != SQLITE_DONE || sqlite3_changes(impl_->database) != 1) {
    if (error.empty())
      error = "registry recovery point was not found or was not extended";
    return false;
  }
  return true;
}

bool RegistryPublicationStore::releaseRecoveryPoint(std::string_view id,
                                                    std::string &error) {
  if (!impl_ || !isHexDigest(id)) {
    error = "registry recovery point ID is invalid";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  Statement statement(
      impl_->database,
      "UPDATE recovery_points SET released = 1 WHERE recovery_id = ? "
      "AND released = 0",
      error);
  if (!statement || !statement.bind(1, id, error) ||
      statement.step() != SQLITE_DONE || sqlite3_changes(impl_->database) != 1) {
    if (error.empty())
      error = "active registry recovery point was not found";
    return false;
  }
  return true;
}

std::optional<std::vector<RegistryRecoveryPointInfo>>
RegistryPublicationStore::listRecoveryPoints(std::string &error) const {
  if (!impl_) {
    error = "registry publication store is not open";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  Statement points(
      impl_->database,
      "SELECT recovery_id, created_at, retain_until, released, "
      "(SELECT COUNT(*) FROM recovery_point_blobs b WHERE "
      "b.recovery_id = p.recovery_id) FROM recovery_points p "
      "ORDER BY created_at, recovery_id",
      error);
  if (!points)
    return std::nullopt;
  std::vector<RegistryRecoveryPointInfo> result;
  while (points.step() == SQLITE_ROW) {
    RegistryRecoveryPointInfo info;
    info.id = points.text(0);
    info.created_at = rfc3339(static_cast<std::int64_t>(points.integer(1)));
    info.retain_until = rfc3339(static_cast<std::int64_t>(points.integer(2)));
    info.released = points.boolean(3);
    info.blob_count = points.integer(4);
    result.push_back(std::move(info));
  }
  return result;
}

std::optional<RegistryGcPlanInfo>
