// Included inside RegistryPublicationStore::Impl; all methods alias its database and config state.
  bool materialize(std::string_view mutation_id, std::string_view package,
                   std::string_view version, std::int64_t now,
                   std::string &error) {
    Statement mutation_receipt(
        database, "SELECT receipt FROM mutations WHERE mutation_id = ?", error);
    if (!mutation_receipt || !mutation_receipt.bind(1, mutation_id, error) ||
        mutation_receipt.step() != SQLITE_ROW) {
      if (error.empty())
        error = "registry mutation receipt is unavailable";
      return false;
    }
    const auto receipt_text = mutation_receipt.text(0);
    auto publish_receipt = parseRegistryAuditReceipt(receipt_text, error);
    std::optional<RegistryLifecycleReceipt> lifecycle_receipt;
    if (!publish_receipt) {
      error.clear();
      lifecycle_receipt = parseRegistryLifecycleReceipt(receipt_text, error);
    }
    if (!publish_receipt && !lifecycle_receipt)
      return false;
    const auto checkpoint = publish_receipt ? publish_receipt->checkpoint
                                            : lifecycle_receipt->checkpoint;
    Statement variants(database,
                       "SELECT entry_text FROM variants WHERE package_name = ? "
                       "AND version = ? ORDER BY variant_name",
                       error);
    if (!variants || !variants.bind(1, package, error) ||
        !variants.bind(2, version, error))
      return false;
    SignedRegistryEntry combined;
    combined.package_name = std::string(package);
    combined.version = std::string(version);
    while (variants.step() == SQLITE_ROW) {
      auto parsed =
          parseSignedRegistryEntryText(variants.text(0), "database", error);
      if (!parsed || parsed->variants.size() != 1)
        return false;
      combined.variants.push_back(std::move(parsed->variants.front()));
    }
    if (combined.variants.empty()) {
      error = "registry publication has no materializable variants";
      return false;
    }
    Statement state(database,
                    "SELECT yanked, state_leaf_index, state_leaf_hash, "
                    "changed_at, reason_code FROM release_states "
                    "WHERE package_name = ? AND version = ?",
                    error);
    if (!state || !state.bind(1, package, error) ||
        !state.bind(2, version, error) || state.step() != SQLITE_ROW) {
      if (error.empty())
        error = "registry release lifecycle state is unavailable";
      return false;
    }
    RegistryIndexEntry index_entry;
    index_entry.artifact = std::move(combined);
    index_entry.state = state.boolean(0) ? RegistryReleaseState::Yanked
                                         : RegistryReleaseState::Active;
    index_entry.state_leaf_index = state.integer(1);
    index_entry.state_leaf_hash = state.text(2);
    index_entry.changed_at = state.text(3);
    index_entry.reason_code = state.text(4);
    const auto output = std::filesystem::path(config.index_worktree) /
                        "packages" / std::string(package) /
                        (std::string(version) + ".toml");
    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
      error =
          "failed to create registry package index directory: " + ec.message();
      return false;
    }
    const auto temporary = output.string() + ".tmp";
    if (!writeTextFile(temporary, renderRegistryIndexEntry(index_entry), error) ||
        !replaceFile(temporary, output.string(), ec)) {
      if (error.empty())
        error = "failed to publish registry package entry";
      removeFile(temporary, ec);
      return false;
    }
    RegistryIndexSealRequest seal;
    seal.registry_name = config.registry_name;
    seal.index_root = config.index_worktree;
    seal.audit_checkpoint = checkpoint;
    const auto max_time = (std::numeric_limits<std::int64_t>::max)();
    const auto snapshot_validity =
        static_cast<std::int64_t>(config.snapshot_validity_seconds);
    const auto timestamp_validity =
        static_cast<std::int64_t>(config.timestamp_validity_seconds);
    if (now > max_time - snapshot_validity ||
        now > max_time - timestamp_validity) {
      error = "registry metadata validity interval overflows time";
      return false;
    }
    seal.snapshot_expires = rfc3339(now + snapshot_validity);
    seal.timestamp_expires = rfc3339(now + timestamp_validity);
    if (seal.snapshot_expires.empty() || seal.timestamp_expires.empty()) {
      error = "registry metadata expiry is outside the supported UTC range";
      return false;
    }
    VerifiedRegistryTrust sealed;
    if (!signer || !sealRegistryIndex(seal, *signer, sealed, error))
      return false;

    if (std::filesystem::exists(std::filesystem::path(config.index_worktree) /
                                ".git")) {
      if (!runGit(config.index_worktree,
                  {"add", "--", "packages", "trust/audit-checkpoint.txt",
                   "trust/snapshot.toml", "trust/timestamp.toml"},
                  error) ||
          !runGit(
              config.index_worktree,
              {"commit", "-m", "registry mutation " + std::string(mutation_id)},
              error)) {
        return false;
      }
      if (!config.git_remote.empty() &&
          !runGit(config.index_worktree,
                  {"push", config.git_remote,
                   "HEAD:refs/heads/" + config.git_branch},
                  error)) {
        return false;
      }
    }
    Statement done(database,
                   "UPDATE mutations SET distributed = 1 WHERE mutation_id = ?",
                   error);
    return done && done.bind(1, mutation_id, error) &&
           done.step() == SQLITE_DONE;
  }

