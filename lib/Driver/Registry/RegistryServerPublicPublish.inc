// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
RegistryServerPublishResult
RegistryPublicationStore::publish(const RegistryServerPublishRequest &request) {
  RegistryServerPublishResult result;
  std::string error;
  if (!impl_ || request.bearer_token.empty() || request.archive_path.empty() ||
      request.entry_path.empty()) {
    result.status = 400;
    result.message = "registry publish request is incomplete";
    return result;
  }
  auto entry_text = readTextFile(request.entry_path, error);
  auto entry =
      entry_text
          ? parseSignedRegistryEntryText(*entry_text, request.entry_path, error)
          : std::optional<SignedRegistryEntry>{};
  if (!entry ||
      !entry_text->starts_with("format = \"chtholly-registry-entry-v3\"")) {
    result.status = 422;
    result.message =
        error.empty() ? "registry server requires entry v3" : error;
    return result;
  }
  if (!isSafeRegistryPackageName(entry->package_name)) {
    result.status = 422;
    result.message = "registry package name is not a safe index path segment";
    return result;
  }
  std::lock_guard lock(impl_->mutex);
  auto authentication =
      impl_->authenticate(request.bearer_token, entry->package_name, error);
  if (!authentication) {
    result.status =
        error.find("not authorized") == std::string::npos ? 401 : 403;
    result.message = error;
    return result;
  }
  auto archive = inspectPackageArtifactArchive(request.archive_path, error);
  std::error_code ec;
  const auto archive_size =
      std::filesystem::file_size(request.archive_path, ec);
  if (!archive || ec || archive_size == 0 ||
      archive_size > impl_->config.max_archive_bytes) {
    result.status = 422;
    result.message =
        error.empty() ? "registry archive is invalid or too large" : error;
    return result;
  }
  auto variant = std::find_if(
      entry->variants.begin(), entry->variants.end(),
      [&](const auto &candidate) {
        return candidate.archive_sha256 == archive->archive_sha256 &&
               candidate.archive_size == archive_size &&
               candidate.artifact_identity == archive->artifact_identity &&
               candidate.closure_digest == archive->closure_digest;
      });
  if (entry->package_name != archive->package_name ||
      variant == entry->variants.end()) {
    result.status = 422;
    result.message = "registry archive does not match an entry variant";
    return result;
  }
  const auto expected_url = impl_->config.public_artifact_base_url + "/" +
                            variant->archive_sha256 + ".cpa";
  if (variant->url != expected_url) {
    result.status = 422;
    result.message = "registry artifact URL is not the server canonical URL";
    return result;
  }
  const auto now = effectiveNow(request.now_unix_seconds);
  auto root_request = rootRequest(impl_->config, now, error);
  auto verified_root = root_request
                           ? verifyRegistryRootChain(*root_request, error)
                           : std::optional<VerifiedRegistryRoot>{};
  if (!verified_root || verified_root->root.format != RegistryRootFormatV2) {
    result.status = 503;
    result.message = error.empty() ? "registry root v2 is unavailable" : error;
    return result;
  }
  auto verification = verifyRegistryArtifactVariant(
      impl_->config.registry_name, entry->package_name, entry->version,
      *variant, verified_root->root.targets.keys,
      verified_root->root.targets.threshold, error);
  if (!verification) {
    result.status = 422;
    result.message = error;
    return result;
  }
  const auto canonical_entry = renderSignedRegistryEntry(*entry);
  const auto entry_digest = sha256Hex(canonical_entry);
  const auto mutation =
      mutationDigest(impl_->config.registry_name, *entry, *variant);
  if (request.idempotency_key != mutation) {
    result.status = 400;
    result.message = "registry publish Idempotency-Key does not match mutation";
    return result;
  }
  Statement retry(impl_->database,
                  "SELECT receipt, distributed FROM mutations "
                  "WHERE mutation_id = ?",
                  error);
  if (!retry || !retry.bind(1, mutation, error)) {
    result.status = 500;
    result.message = error;
    return result;
  }
  if (retry.step() == SQLITE_ROW) {
    auto receipt = parseRegistryAuditReceipt(retry.text(0), error);
    if (!receipt) {
      result.status = 500;
      result.message = error;
      return result;
    }
    if (!retry.boolean(1) && !impl_->materialize(mutation, entry->package_name,
                                                 entry->version, now, error)) {
      result.status = 503;
      result.message = error;
      return result;
    }
    result.status = 200;
    result.receipt = std::move(receipt);
    return result;
  }

  RegistryArtifactVariant merged = *variant;
  std::string event_kind = "publish";
  Statement existing(
      impl_->database,
      "SELECT archive_sha256, archive_size, entry_text FROM variants "
      "WHERE package_name = ? AND version = ? AND variant_name = ?",
      error);
  if (!existing || !existing.bind(1, entry->package_name, error) ||
      !existing.bind(2, entry->version, error) ||
      !existing.bind(3, variant->name, error)) {
    result.status = 500;
    result.message = error;
    return result;
  }
  if (existing.step() == SQLITE_ROW) {
    if (existing.text(0) != variant->archive_sha256 ||
        existing.integer(1) != variant->archive_size) {
      result.status = 409;
      result.message = "registry variant facts are immutable";
      return result;
    }
    auto old_entry =
        parseSignedRegistryEntryText(existing.text(2), "database", error);
    if (!old_entry || old_entry->variants.size() != 1) {
      result.status = 500;
      result.message = error;
      return result;
    }
    if (!sameVariantFacts(old_entry->variants.front(), *variant)) {
      result.status = 409;
      result.message = "registry variant facts are immutable";
      return result;
    }
    merged = std::move(old_entry->variants.front());
    bool added = false;
    for (const auto &signature : variant->signatures) {
      const auto found =
          std::find_if(merged.signatures.begin(), merged.signatures.end(),
                       [&](const auto &candidate) {
                         return candidate.key_id == signature.key_id;
                       });
      if (found == merged.signatures.end()) {
        merged.signatures.push_back(signature);
        added = true;
      } else if (found->signature != signature.signature) {
        result.status = 409;
        result.message = "registry signature attestations are immutable";
        return result;
      }
    }
    if (!added) {
      result.status = 409;
      result.message = "registry publication does not append new state";
      return result;
    }
    std::sort(merged.signatures.begin(), merged.signatures.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.key_id < rhs.key_id;
              });
    event_kind = "countersign";
  }

  const auto cas = std::filesystem::path(impl_->config.state_directory) /
                   "blobs" / "sha256" / variant->archive_sha256.substr(0, 2) /
                   (variant->archive_sha256 + ".cpa");
  if (!copyIntoCas(request.archive_path, cas, archive_size,
                   variant->archive_sha256, error)) {
    result.status = 500;
    result.message = error;
    return result;
  }
  SignedRegistryEntry stored_entry;
  stored_entry.package_name = entry->package_name;
  stored_entry.version = entry->version;
  stored_entry.variants.push_back(merged);

  Transaction transaction(impl_->database, error);
  Statement blob(
      impl_->database,
      "INSERT INTO blobs(sha256, size, path, created_at, "
      "last_audit_reference) VALUES(?, ?, ?, ?, ?) "
      "ON CONFLICT(sha256) DO UPDATE SET last_audit_reference = "
      "MAX(last_audit_reference, excluded.last_audit_reference)",
      error);
  Statement upsert(
      impl_->database,
      "INSERT INTO variants(package_name, version, variant_name, "
      "archive_sha256, archive_size, entry_text) VALUES(?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(package_name, version, variant_name) DO UPDATE SET "
      "entry_text = excluded.entry_text",
      error);
  if (!transaction || !blob || !upsert ||
      !blob.bind(1, variant->archive_sha256, error) ||
      !blob.bind(2, static_cast<std::uint64_t>(archive_size), error) ||
      !blob.bind(3, cas.string(), error) || !blob.bind(4, now, error) ||
      !blob.bind(5, now, error) || blob.step() != SQLITE_DONE ||
      !upsert.bind(1, entry->package_name, error) ||
      !upsert.bind(2, entry->version, error) ||
      !upsert.bind(3, variant->name, error) ||
      !upsert.bind(4, variant->archive_sha256, error) ||
      !upsert.bind(5, static_cast<std::uint64_t>(archive_size), error) ||
      !upsert.bind(6, renderSignedRegistryEntry(stored_entry), error) ||
      upsert.step() != SQLITE_DONE) {
    result.status = 500;
    result.message = error.empty() ? "failed to store registry variant" : error;
    return result;
  }
  Statement count(impl_->database, "SELECT COUNT(*) FROM audit_leaves", error);
  if (!count || count.step() != SQLITE_ROW) {
    result.status = 500;
    result.message = "failed to count registry audit leaves";
    return result;
  }
  RegistryAuditReceipt receipt;
  receipt.registry_name = impl_->config.registry_name;
  receipt.package_name = entry->package_name;
  receipt.version = entry->version;
  receipt.variant_name = variant->name;
  receipt.archive_sha256 = variant->archive_sha256;
  receipt.entry_sha256 = entry_digest;
  receipt.event_kind = event_kind;
  receipt.publisher_principal = authentication->first;
  receipt.accepted_at = rfc3339(now);
  receipt.targets_threshold = verification->required_threshold;
  receipt.valid_target_signer_ids = verification->valid_signing_key_ids;
  receipt.leaf_index = count.integer(0);
  receipt.checkpoint.registry_name = impl_->config.registry_name;
  receipt.checkpoint.root_version = verified_root->root.version;
  receipt.checkpoint.root_sha256 = verified_root->root_sha256;
  const auto canonical_leaf = registryAuditCanonicalLeaf(receipt);
  receipt.leaf_hash = registryAuditLeafHash(canonical_leaf);
  auto leaves = impl_->leafHashes(receipt.leaf_index, error);
  if (!error.empty()) {
    result.status = 500;
    result.message = error;
    return result;
  }
  leaves.push_back(receipt.leaf_hash);
  receipt.checkpoint.tree_size = leaves.size();
  receipt.checkpoint.root_hash = registryMerkleRoot(leaves);
  receipt.checkpoint.issued_at = receipt.accepted_at;
  if (!impl_->signCheckpoint(receipt.checkpoint, verified_root->root, error)) {
    result.status = 500;
    result.message = error;
    return result;
  }
  receipt.inclusion_proof =
      registryMerkleInclusionProof(leaves, receipt.leaf_index);
  const auto rendered_receipt = renderRegistryAuditReceipt(receipt);
  Statement leaf(impl_->database,
                 "INSERT INTO audit_leaves(leaf_index, mutation_id, "
                 "canonical_leaf, leaf_hash) VALUES(?, ?, ?, ?)",
                 error);
  Statement mutation_statement(
      impl_->database,
      "INSERT INTO mutations(mutation_id, package_name, version, receipt) "
      "VALUES(?, ?, ?, ?)",
      error);
  Statement release_state(
      impl_->database,
      "INSERT OR IGNORE INTO release_states(package_name, version, yanked, "
      "state_leaf_index, state_leaf_hash, changed_at, reason_code) "
      "VALUES(?, ?, 0, ?, ?, ?, 'unspecified')",
      error);
  if (!leaf || !mutation_statement || !release_state ||
      !leaf.bind(1, receipt.leaf_index, error) ||
      !leaf.bind(2, mutation, error) || !leaf.bind(3, canonical_leaf, error) ||
      !leaf.bind(4, receipt.leaf_hash, error) || leaf.step() != SQLITE_DONE ||
      !mutation_statement.bind(1, mutation, error) ||
      !mutation_statement.bind(2, entry->package_name, error) ||
      !mutation_statement.bind(3, entry->version, error) ||
      !mutation_statement.bind(4, rendered_receipt, error) ||
      mutation_statement.step() != SQLITE_DONE ||
      !release_state.bind(1, entry->package_name, error) ||
      !release_state.bind(2, entry->version, error) ||
      !release_state.bind(3, receipt.leaf_index, error) ||
      !release_state.bind(4, receipt.leaf_hash, error) ||
      !release_state.bind(5, receipt.accepted_at, error) ||
      release_state.step() != SQLITE_DONE || !transaction.commit(error)) {
    result.status = 500;
    result.message =
        error.empty() ? "failed to append registry audit event" : error;
    return result;
  }
  if (!impl_->materialize(mutation, entry->package_name, entry->version, now,
                          error)) {
    result.status = 503;
    result.message = error;
    return result;
  }
  result.status = 201;
  result.receipt = std::move(receipt);
  return result;
}

