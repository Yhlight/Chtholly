// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
RegistryPublicationStore::planGarbageCollection(
    std::int64_t now_unix_seconds, std::string &error) {
  if (!impl_) {
    error = "registry publication store is not open";
    return std::nullopt;
  }
  const auto now = effectiveNow(now_unix_seconds);
  std::lock_guard lock(impl_->mutex);
  const auto cas_root =
      std::filesystem::path(impl_->config.state_directory) / "blobs" / "sha256";
  std::error_code discovery_error;
  if (std::filesystem::exists(cas_root, discovery_error)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(cas_root,
                                                       discovery_error)) {
      if (discovery_error) {
        error = "failed to discover registry CAS objects: " +
                discovery_error.message();
        return std::nullopt;
      }
      const auto path = entry.path();
      const auto status = std::filesystem::symlink_status(path, discovery_error);
      if (discovery_error ||
          status.type() == std::filesystem::file_type::symlink) {
        error = "registry CAS discovery found an invalid object path";
        return std::nullopt;
      }
      if (status.type() == std::filesystem::file_type::directory)
        continue;
      if (status.type() != std::filesystem::file_type::regular) {
        error = "registry CAS discovery found a non-regular object";
        return std::nullopt;
      }
      const auto filename = path.filename().string();
      const auto digest = filename.ends_with(".cpa")
                              ? filename.substr(0, filename.size() - 4)
                              : std::string{};
      const auto measured = discovery_error
                                ? std::optional<std::string>{}
                                : sha256File(path.string());
      const auto size = discovery_error
                            ? 0
                            : std::filesystem::file_size(path, discovery_error);
      if (discovery_error ||
          !isHexDigest(digest) ||
          path.parent_path().filename() != digest.substr(0, 2) ||
          !pathIsWithin(path, cas_root) || !measured || *measured != digest) {
        error = "registry CAS discovery found an invalid object path or digest";
        return std::nullopt;
      }
      Statement discover(
          impl_->database,
          "INSERT OR IGNORE INTO blobs(sha256, size, path, created_at, "
          "last_audit_reference) VALUES(?, ?, ?, ?, 0)",
          error);
      if (!discover || !discover.bind(1, digest, error) ||
          !discover.bind(2, static_cast<std::uint64_t>(size), error) ||
          !discover.bind(3, path.string(), error) ||
          !discover.bind(4, now, error) || discover.step() != SQLITE_DONE)
        return std::nullopt;
    }
    if (discovery_error) {
      error = "failed to discover registry CAS objects: " +
              discovery_error.message();
      return std::nullopt;
    }
  } else if (discovery_error) {
    error = "failed to inspect registry CAS root: " + discovery_error.message();
    return std::nullopt;
  }
  Statement blobs(impl_->database,
                  "SELECT sha256, size, path, created_at, "
                  "last_audit_reference FROM blobs ORDER BY sha256",
                  error);
  if (!blobs)
    return std::nullopt;
  struct Candidate {
    std::string sha256;
    std::uint64_t size = 0;
    std::string path;
  };
  std::vector<Candidate> candidates;
  while (blobs.step() == SQLITE_ROW) {
    const auto digest = blobs.text(0);
    Statement variants(impl_->database,
                       "SELECT 1 FROM variants WHERE archive_sha256 = ? LIMIT 1",
                       error);
    Statement recovery(
        impl_->database,
        "SELECT 1 FROM recovery_point_blobs b JOIN recovery_points p ON "
        "p.recovery_id = b.recovery_id WHERE b.sha256 = ? AND "
        "p.released = 0 AND p.retain_until > ? LIMIT 1",
        error);
    if (!variants || !recovery || !variants.bind(1, digest, error) ||
        !recovery.bind(1, digest, error) || !recovery.bind(2, now, error))
      return std::nullopt;
    const bool referenced = variants.step() == SQLITE_ROW;
    const bool protected_by_recovery = recovery.step() == SQLITE_ROW;
    const auto created = static_cast<std::int64_t>(blobs.integer(3));
    const auto audited = static_cast<std::int64_t>(blobs.integer(4));
    const bool creation_old_enough =
        impl_->config.unreferenced_blob_seconds == 0 ||
        (now >= static_cast<std::int64_t>(
                    impl_->config.unreferenced_blob_seconds) &&
         created <= now - static_cast<std::int64_t>(
                             impl_->config.unreferenced_blob_seconds));
    const bool audit_old_enough =
        impl_->config.audit_blob_seconds == 0 ||
        (now >= static_cast<std::int64_t>(impl_->config.audit_blob_seconds) &&
         audited <= now - static_cast<std::int64_t>(
                             impl_->config.audit_blob_seconds));
    if (!referenced && !protected_by_recovery && creation_old_enough &&
        audit_old_enough)
      candidates.push_back({digest, blobs.integer(1), blobs.text(2)});
  }
  std::ostringstream identity;
  identity << "chtholly-registry-gc-plan-v1\n" << impl_->config.registry_name
           << '\n' << now << '\n'
           << std::chrono::steady_clock::now().time_since_epoch().count()
           << '\n';
  for (const auto &candidate : candidates)
    identity << candidate.sha256 << '\t' << candidate.size << '\t'
             << candidate.path << '\n';
  RegistryGcPlanInfo result;
  result.id = sha256Hex(identity.str());
  result.created_at = rfc3339(now);
  Transaction transaction(impl_->database, error);
  Statement plan(impl_->database,
                 "INSERT INTO gc_plans(plan_id, created_at) VALUES(?, ?)",
                 error);
  if (!transaction || !plan || !plan.bind(1, result.id, error) ||
      !plan.bind(2, now, error) || plan.step() != SQLITE_DONE)
    return std::nullopt;
  for (const auto &candidate : candidates) {
    Statement insert(
        impl_->database,
        "INSERT INTO gc_candidates(plan_id, sha256, size, path) "
        "VALUES(?, ?, ?, ?)",
        error);
    if (!insert || !insert.bind(1, result.id, error) ||
        !insert.bind(2, candidate.sha256, error) ||
        !insert.bind(3, candidate.size, error) ||
        !insert.bind(4, candidate.path, error) ||
        insert.step() != SQLITE_DONE)
      return std::nullopt;
    result.blob_sha256.push_back(candidate.sha256);
  }
  if (!transaction.commit(error))
    return std::nullopt;
  return result;
}

bool RegistryPublicationStore::applyGarbageCollection(
    std::string_view plan_id, std::int64_t now_unix_seconds,
    std::string &error) {
  if (!impl_ || !isHexDigest(plan_id)) {
    error = "registry GC plan ID is invalid";
    return false;
  }
  if (impl_->config.audit_blob_seconds == 0 ||
      impl_->config.recovery_point_seconds == 0 ||
      impl_->config.unreferenced_blob_seconds == 0) {
    error = "registry GC apply is disabled until retention is configured";
    return false;
  }
  const auto now = effectiveNow(now_unix_seconds);
  std::lock_guard lock(impl_->mutex);
  Statement plan(impl_->database,
                 "SELECT applied FROM gc_plans WHERE plan_id = ?", error);
  if (!plan || !plan.bind(1, plan_id, error) || plan.step() != SQLITE_ROW ||
      plan.boolean(0)) {
    if (error.empty())
      error = "registry GC plan was not found or was already applied";
    return false;
  }
  struct Candidate {
    std::string sha256;
    std::uint64_t size = 0;
    std::string path;
  };
  Statement rows(impl_->database,
                 "SELECT sha256, size, path FROM gc_candidates "
                 "WHERE plan_id = ? ORDER BY sha256",
                 error);
  if (!rows || !rows.bind(1, plan_id, error))
    return false;
  std::vector<Candidate> candidates;
  while (rows.step() == SQLITE_ROW)
    candidates.push_back({rows.text(0), rows.integer(1), rows.text(2)});

  const auto cas_root =
      std::filesystem::path(impl_->config.state_directory) / "blobs" / "sha256";
  const auto trash = std::filesystem::path(impl_->config.state_directory) /
                     "gc-trash" / std::string(plan_id);
  std::error_code ec;
  std::filesystem::create_directories(trash, ec);
  if (ec) {
    error = "failed to create registry GC trash: " + ec.message();
    return false;
  }
  std::vector<Candidate> renamed;
  const auto restore = [&] {
    for (auto it = renamed.rbegin(); it != renamed.rend(); ++it) {
      std::error_code ignored;
      std::filesystem::create_directories(
          std::filesystem::path(it->path).parent_path(), ignored);
      std::filesystem::rename(trash / (it->sha256 + ".cpa"), it->path,
                              ignored);
    }
  };
  for (const auto &candidate : candidates) {
    Statement current(
        impl_->database,
        "SELECT created_at, last_audit_reference FROM blobs WHERE sha256 = ? "
        "AND size = ? AND path = ? AND NOT EXISTS(SELECT 1 FROM variants "
        "WHERE archive_sha256 = ?) AND NOT EXISTS(SELECT 1 FROM "
        "recovery_point_blobs b JOIN recovery_points p ON p.recovery_id = "
        "b.recovery_id WHERE b.sha256 = ? AND p.released = 0 AND "
        "p.retain_until > ?)",
        error);
    if (!current || !current.bind(1, candidate.sha256, error) ||
        !current.bind(2, candidate.size, error) ||
        !current.bind(3, candidate.path, error) ||
        !current.bind(4, candidate.sha256, error) ||
        !current.bind(5, candidate.sha256, error) ||
        !current.bind(6, now, error) || current.step() != SQLITE_ROW) {
      restore();
      if (error.empty())
        error = "registry GC candidate is no longer eligible";
      return false;
    }
    const auto created = static_cast<std::int64_t>(current.integer(0));
    const auto audited = static_cast<std::int64_t>(current.integer(1));
    if (now < static_cast<std::int64_t>(
                  impl_->config.unreferenced_blob_seconds) ||
        now < static_cast<std::int64_t>(impl_->config.audit_blob_seconds) ||
        created > now - static_cast<std::int64_t>(
                            impl_->config.unreferenced_blob_seconds) ||
        audited > now -
                      static_cast<std::int64_t>(impl_->config.audit_blob_seconds)) {
      restore();
      error = "registry GC candidate no longer satisfies retention";
      return false;
    }
    const auto path = std::filesystem::path(candidate.path);
    const auto status = std::filesystem::symlink_status(path, ec);
    const auto digest =
        ec ? std::optional<std::string>{} : sha256File(path.string());
    const auto size = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec || status.type() == std::filesystem::file_type::symlink ||
        !pathIsWithin(path, cas_root) || !digest ||
        *digest != candidate.sha256 || size != candidate.size) {
      restore();
      error = "registry GC candidate failed path or integrity validation";
      return false;
    }
    std::filesystem::rename(path, trash / (candidate.sha256 + ".cpa"), ec);
    if (ec) {
      restore();
      error = "failed to stage registry GC candidate: " + ec.message();
      return false;
    }
    renamed.push_back(candidate);
  }
  Transaction transaction(impl_->database, error);
  if (!transaction) {
    restore();
    return false;
  }
  for (const auto &candidate : candidates) {
    Statement remove(impl_->database,
                     "DELETE FROM blobs WHERE sha256 = ?", error);
    if (!remove || !remove.bind(1, candidate.sha256, error) ||
        remove.step() != SQLITE_DONE || sqlite3_changes(impl_->database) != 1) {
      restore();
      return false;
    }
  }
  Statement applied(impl_->database,
                    "UPDATE gc_plans SET applied = 1 WHERE plan_id = ?",
                    error);
  if (!applied || !applied.bind(1, plan_id, error) ||
      applied.step() != SQLITE_DONE || sqlite3_changes(impl_->database) != 1 ||
      !transaction.commit(error)) {
    restore();
    return false;
  }
  std::filesystem::remove_all(trash, ec);
  if (ec) {
    error = "registry GC committed but trash cleanup failed: " + ec.message();
    return false;
  }
  return true;
}

std::optional<std::string>
