// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
RegistryServerLifecycleResult RegistryPublicationStore::changeReleaseState(
    const RegistryServerLifecycleRequest &request) {
  RegistryServerLifecycleResult result;
  std::string error;
  std::string version_error;
  if (!impl_ || request.bearer_token.empty() ||
      !isSafeRegistryPackageName(request.package_name) ||
      !parseSemVer(request.version, version_error) ||
      !isRegistryReleaseReasonCode(request.reason_code) ||
      request.idempotency_key.empty() || request.idempotency_key.size() > 256 ||
      request.idempotency_key.find_first_of("\r\n\0") != std::string::npos) {
    result.status = 400;
    result.message = "registry lifecycle request is incomplete or invalid";
    return result;
  }
  const auto mutation =
      "lifecycle:sha256:" +
      sha256Hex("chtholly-registry-lifecycle-idempotency-v1\n" +
                request.idempotency_key);
  const auto now = effectiveNow(request.now_unix_seconds);
  std::lock_guard lock(impl_->mutex);

  std::string actor_type = "publisher";
  std::string actor_principal;
  auto publisher =
      impl_->authenticate(request.bearer_token, request.package_name, error);
  if (publisher) {
    actor_principal = publisher->first;
  } else {
    error.clear();
    auto principal = impl_->authenticateOperator(request.bearer_token,
                                                 "release:yank", error);
    if (!principal) {
      result.status = 401;
      result.message = "registry lifecycle authentication failed";
      return result;
    }
    actor_type = "operator";
    actor_principal = std::move(*principal);
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
    auto receipt = parseRegistryLifecycleReceipt(retry.text(0), error);
    if (!receipt) {
      result.status = 409;
      result.message = "registry lifecycle idempotency key is already in use";
      return result;
    }
    if (receipt->package_name != request.package_name ||
        receipt->version != request.version || receipt->state != request.state ||
        receipt->reason_code != request.reason_code) {
      result.status = 409;
      result.message = "registry lifecycle idempotency key was reused for a "
                       "different request";
      return result;
    }
    if (!retry.boolean(1) &&
        !impl_->materialize(mutation, request.package_name, request.version,
                            now, error)) {
      result.status = 503;
      result.message = error;
      return result;
    }
    result.status = 200;
    result.receipt = std::move(receipt);
    return result;
  }

  Statement state(impl_->database,
                  "SELECT yanked, state_leaf_index, state_leaf_hash FROM "
                  "release_states WHERE package_name = ? AND version = ?",
                  error);
  if (!state || !state.bind(1, request.package_name, error) ||
      !state.bind(2, request.version, error) || state.step() != SQLITE_ROW) {
    result.status = 404;
    result.message = "registry package version was not found";
    return result;
  }
  const bool desired_yanked = request.state == RegistryReleaseState::Yanked;
  if (state.boolean(0) == desired_yanked) {
    result.status = 409;
    result.message = desired_yanked ? "registry release is already yanked"
                                    : "registry release is already active";
    return result;
  }

  auto root_request = rootRequest(impl_->config, now, error);
  auto verified_root = root_request
                           ? verifyRegistryRootChain(*root_request, error)
                           : std::optional<VerifiedRegistryRoot>{};
  if (!verified_root || verified_root->root.format != RegistryRootFormatV2) {
    result.status = 503;
    result.message = error.empty() ? "registry root v2 is unavailable" : error;
    return result;
  }
  Statement count(impl_->database, "SELECT COUNT(*) FROM audit_leaves", error);
  if (!count || count.step() != SQLITE_ROW) {
    result.status = 500;
    result.message = "failed to count registry audit leaves";
    return result;
  }
  RegistryLifecycleReceipt receipt;
  receipt.registry_name = impl_->config.registry_name;
  receipt.package_name = request.package_name;
  receipt.version = request.version;
  receipt.state = request.state;
  receipt.previous_state_leaf_index = state.integer(1);
  receipt.previous_state_leaf_hash = state.text(2);
  receipt.actor_type = actor_type;
  receipt.actor_principal = actor_principal;
  receipt.accepted_at = rfc3339(now);
  receipt.reason_code = request.reason_code;
  receipt.leaf_index = count.integer(0);
  receipt.checkpoint.registry_name = impl_->config.registry_name;
  receipt.checkpoint.root_version = verified_root->root.version;
  receipt.checkpoint.root_sha256 = verified_root->root_sha256;
  const auto canonical_leaf = registryLifecycleCanonicalLeaf(receipt);
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
  const auto rendered = renderRegistryLifecycleReceipt(receipt);

  Transaction transaction(impl_->database, error);
  Statement leaf(impl_->database,
                 "INSERT INTO audit_leaves(leaf_index, mutation_id, "
                 "canonical_leaf, leaf_hash) VALUES(?, ?, ?, ?)",
                 error);
  Statement update(
      impl_->database,
      "UPDATE release_states SET yanked = ?, state_leaf_index = ?, "
      "state_leaf_hash = ?, changed_at = ?, reason_code = ? "
      "WHERE package_name = ? AND version = ? AND state_leaf_hash = ?",
      error);
  Statement mutation_statement(
      impl_->database,
      "INSERT INTO mutations(mutation_id, package_name, version, receipt) "
      "VALUES(?, ?, ?, ?)",
      error);
  if (!transaction || !leaf || !update || !mutation_statement ||
      !leaf.bind(1, receipt.leaf_index, error) ||
      !leaf.bind(2, mutation, error) || !leaf.bind(3, canonical_leaf, error) ||
      !leaf.bind(4, receipt.leaf_hash, error) || leaf.step() != SQLITE_DONE ||
      !update.bind(1, desired_yanked, error) ||
      !update.bind(2, receipt.leaf_index, error) ||
      !update.bind(3, receipt.leaf_hash, error) ||
      !update.bind(4, receipt.accepted_at, error) ||
      !update.bind(5, receipt.reason_code, error) ||
      !update.bind(6, request.package_name, error) ||
      !update.bind(7, request.version, error) ||
      !update.bind(8, receipt.previous_state_leaf_hash, error) ||
      update.step() != SQLITE_DONE || sqlite3_changes(impl_->database) != 1 ||
      !mutation_statement.bind(1, mutation, error) ||
      !mutation_statement.bind(2, request.package_name, error) ||
      !mutation_statement.bind(3, request.version, error) ||
      !mutation_statement.bind(4, rendered, error) ||
      mutation_statement.step() != SQLITE_DONE || !transaction.commit(error)) {
    result.status = 500;
    result.message = error.empty() ? "failed to append registry lifecycle event"
                                   : error;
    return result;
  }
  if (!impl_->materialize(mutation, request.package_name, request.version, now,
                          error)) {
    result.status = 503;
    result.message = error;
    return result;
  }
  result.status = 201;
  result.receipt = std::move(receipt);
  return result;
}

