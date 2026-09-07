// Included inside RegistryPublicationStore::Impl; all methods alias its database and config state.
  bool recoverGcTrash(std::string &error) {
    const auto trash_root =
        std::filesystem::path(config.state_directory) / "gc-trash";
    std::error_code ec;
    if (!std::filesystem::exists(trash_root, ec))
      return !ec;
    for (const auto &run : std::filesystem::directory_iterator(trash_root, ec)) {
      if (ec) {
        error = "failed to inspect registry GC trash: " + ec.message();
        return false;
      }
      if (!run.is_directory()) {
        error = "registry GC trash contains an unexpected entry";
        return false;
      }
      const auto plan_id = run.path().filename().string();
      Statement plan(database, "SELECT applied FROM gc_plans WHERE plan_id = ?",
                     error);
      if (!plan || !plan.bind(1, plan_id, error) ||
          plan.step() != SQLITE_ROW) {
        error = "registry GC trash has no matching persistent plan";
        return false;
      }
      if (plan.boolean(0)) {
        std::filesystem::remove_all(run.path(), ec);
        if (ec) {
          error = "failed to clean committed registry GC trash: " +
                  ec.message();
          return false;
        }
        continue;
      }
      Statement candidates(database,
                           "SELECT sha256, path FROM gc_candidates "
                           "WHERE plan_id = ? ORDER BY sha256",
                           error);
      if (!candidates || !candidates.bind(1, plan_id, error))
        return false;
      while (candidates.step() == SQLITE_ROW) {
        const auto staged = run.path() / (candidates.text(0) + ".cpa");
        const auto original = std::filesystem::path(candidates.text(1));
        if (!std::filesystem::exists(staged, ec)) {
          if (ec) {
            error = "failed to inspect uncommitted registry GC candidate";
            return false;
          }
          continue;
        }
        std::filesystem::create_directories(original.parent_path(), ec);
        if (ec) {
          error = "failed to restore registry GC candidate directory: " +
                  ec.message();
          return false;
        }
        std::filesystem::rename(staged, original, ec);
        if (ec) {
          error = "failed to restore uncommitted registry GC candidate: " +
                  ec.message();
          return false;
        }
      }
      std::filesystem::remove_all(run.path(), ec);
      if (ec) {
        error = "failed to clean recovered registry GC trash: " + ec.message();
        return false;
      }
    }
    return true;
  }

  bool initializeReleaseStates(std::string &error) {
    Statement missing(
        database,
        "SELECT DISTINCT v.package_name, v.version FROM variants v "
        "LEFT JOIN release_states s ON s.package_name = v.package_name AND "
        "s.version = v.version WHERE s.package_name IS NULL "
        "ORDER BY v.package_name, v.version",
        error);
    if (!missing)
      return false;
    std::vector<std::pair<std::string, std::string>> releases;
    while (missing.step() == SQLITE_ROW)
      releases.emplace_back(missing.text(0), missing.text(1));
    for (const auto &[package, version] : releases) {
      Statement latest(
          database,
          "SELECT a.leaf_index, a.leaf_hash, m.receipt FROM mutations m "
          "JOIN audit_leaves a ON a.mutation_id = m.mutation_id WHERE "
          "m.package_name = ? AND m.version = ? ORDER BY a.leaf_index DESC "
          "LIMIT 1",
          error);
      if (!latest || !latest.bind(1, package, error) ||
          !latest.bind(2, version, error) || latest.step() != SQLITE_ROW) {
        if (error.empty())
          error = "registry release is missing its publication audit event";
        return false;
      }
      auto receipt = parseRegistryAuditReceipt(latest.text(2), error);
      if (!receipt || receipt->package_name != package ||
          receipt->version != version || receipt->leaf_index != latest.integer(0) ||
          receipt->leaf_hash != latest.text(1)) {
        if (error.empty())
          error = "registry release publication audit event is invalid";
        return false;
      }
      Statement insert(
          database,
          "INSERT INTO release_states(package_name, version, yanked, "
          "state_leaf_index, state_leaf_hash, changed_at, reason_code) "
          "VALUES(?, ?, 0, ?, ?, ?, 'unspecified')",
          error);
      if (!insert || !insert.bind(1, package, error) ||
          !insert.bind(2, version, error) ||
          !insert.bind(3, receipt->leaf_index, error) ||
          !insert.bind(4, receipt->leaf_hash, error) ||
          !insert.bind(5, receipt->accepted_at, error) ||
          insert.step() != SQLITE_DONE)
        return false;
    }
    return true;
  }

  bool initialize(std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(config.state_directory, ec);
    if (ec) {
      error =
          "failed to create registry server state directory: " + ec.message();
      return false;
    }
    const auto database_path =
        std::filesystem::path(config.state_directory) / "registry.sqlite3";
    if (sqlite3_open_v2(database_path.string().c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      error = "failed to open registry server database";
      return false;
    }
    sqlite3_busy_timeout(database, 30000);
    constexpr const char *schema = R"SQL(
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
CREATE TABLE IF NOT EXISTS schema_info(version INTEGER NOT NULL);
INSERT INTO schema_info(version) SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM schema_info);
CREATE TABLE IF NOT EXISTS publishers(
  token_id TEXT PRIMARY KEY, principal TEXT NOT NULL, secret_hash TEXT NOT NULL,
  all_packages INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS publisher_packages(
  token_id TEXT NOT NULL REFERENCES publishers(token_id) ON DELETE CASCADE,
  package_name TEXT NOT NULL, PRIMARY KEY(token_id, package_name));
CREATE TABLE IF NOT EXISTS blobs(
  sha256 TEXT PRIMARY KEY, size INTEGER NOT NULL, path TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS variants(
  package_name TEXT NOT NULL, version TEXT NOT NULL, variant_name TEXT NOT NULL,
  archive_sha256 TEXT NOT NULL, archive_size INTEGER NOT NULL,
  entry_text TEXT NOT NULL, PRIMARY KEY(package_name, version, variant_name));
CREATE TABLE IF NOT EXISTS audit_leaves(
  leaf_index INTEGER PRIMARY KEY, mutation_id TEXT NOT NULL UNIQUE,
  canonical_leaf TEXT NOT NULL, leaf_hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS mutations(
  mutation_id TEXT PRIMARY KEY, package_name TEXT NOT NULL,
  version TEXT NOT NULL, receipt TEXT NOT NULL, distributed INTEGER NOT NULL DEFAULT 0);
)SQL";
    if (!execute(database, schema, error))
      return false;
    std::uint64_t schema_version = 0;
    {
      Statement version(database, "SELECT version FROM schema_info", error);
      if (!version || version.step() != SQLITE_ROW) {
        error = "registry database schema version is unavailable";
        return false;
      }
      schema_version = version.integer(0);
      if (version.step() != SQLITE_DONE) {
        error = "registry database schema version is ambiguous";
        return false;
      }
    }
    constexpr const char *operator_schema = R"SQL(
CREATE TABLE IF NOT EXISTS operators(
  token_id TEXT PRIMARY KEY, principal TEXT NOT NULL, secret_hash TEXT NOT NULL,
  revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS operator_capabilities(
  token_id TEXT NOT NULL REFERENCES operators(token_id) ON DELETE CASCADE,
  capability TEXT NOT NULL, PRIMARY KEY(token_id, capability));
)SQL";
    constexpr const char *lifecycle_schema = R"SQL(
ALTER TABLE blobs ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE blobs ADD COLUMN last_audit_reference INTEGER NOT NULL DEFAULT 0;
UPDATE blobs SET created_at = CAST(strftime('%s', 'now') AS INTEGER),
  last_audit_reference = CAST(strftime('%s', 'now') AS INTEGER)
  WHERE created_at = 0 OR last_audit_reference = 0;
CREATE TABLE release_states(
  package_name TEXT NOT NULL, version TEXT NOT NULL,
  yanked INTEGER NOT NULL, state_leaf_index INTEGER NOT NULL,
  state_leaf_hash TEXT NOT NULL, changed_at TEXT NOT NULL,
  reason_code TEXT NOT NULL,
  PRIMARY KEY(package_name, version));
CREATE TABLE recovery_points(
  recovery_id TEXT PRIMARY KEY, created_at INTEGER NOT NULL,
  retain_until INTEGER NOT NULL, released INTEGER NOT NULL DEFAULT 0,
  archive_path TEXT NOT NULL);
CREATE TABLE recovery_point_blobs(
  recovery_id TEXT NOT NULL REFERENCES recovery_points(recovery_id)
    ON DELETE CASCADE,
  sha256 TEXT NOT NULL, PRIMARY KEY(recovery_id, sha256));
CREATE TABLE gc_plans(
  plan_id TEXT PRIMARY KEY, created_at INTEGER NOT NULL,
  applied INTEGER NOT NULL DEFAULT 0);
CREATE TABLE gc_candidates(
  plan_id TEXT NOT NULL REFERENCES gc_plans(plan_id) ON DELETE CASCADE,
  sha256 TEXT NOT NULL, size INTEGER NOT NULL, path TEXT NOT NULL,
  PRIMARY KEY(plan_id, sha256));
)SQL";
    if (schema_version == 1) {
      Transaction migration(database, error);
      if (!migration || !execute(database, operator_schema, error) ||
          !execute(database, "UPDATE schema_info SET version = 2", error) ||
          !migration.commit(error))
        return false;
      schema_version = 2;
    } else if (schema_version == 2 &&
               !execute(database, operator_schema, error)) {
      return false;
    }
    if (schema_version == 2) {
      Transaction migration(database, error);
      if (!migration || !execute(database, lifecycle_schema, error) ||
          !execute(database, "UPDATE schema_info SET version = 3", error) ||
          !migration.commit(error))
        return false;
      schema_version = 3;
    }
    if (schema_version != 3) {
      error = "registry database schema version is unsupported";
      return false;
    }
    return initializeReleaseStates(error) && recoverGcTrash(error);
  }
