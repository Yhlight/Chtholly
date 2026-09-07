// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
RegistryPublicationStore::archivePath(std::string_view sha256,
                                      std::string &error) const {
  if (!impl_ || !isHexDigest(sha256)) {
    error = "registry archive digest is invalid";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  Statement statement(impl_->database,
                      "SELECT size, path FROM blobs WHERE sha256 = ?", error);
  if (!statement || !statement.bind(1, sha256, error) ||
      statement.step() != SQLITE_ROW) {
    if (error.empty())
      error = "registry archive was not found";
    return std::nullopt;
  }
  const auto path = statement.text(1);
  std::error_code file_error;
  const auto size = std::filesystem::file_size(path, file_error);
  const auto digest =
      file_error ? std::optional<std::string>{} : sha256File(path);
  if (file_error || size != statement.integer(0) || !digest ||
      *digest != sha256) {
    error = "registry CAS archive failed integrity verification";
    return std::nullopt;
  }
  return path;
}

std::optional<RegistryAuditCheckpoint>
RegistryPublicationStore::latestCheckpoint(std::string &error) const {
  if (!impl_)
    return std::nullopt;
  std::lock_guard lock(impl_->mutex);
  Statement statement(
      impl_->database,
      "SELECT receipt FROM mutations ORDER BY rowid DESC LIMIT 1", error);
  if (!statement || statement.step() != SQLITE_ROW) {
    if (error.empty())
      error = "registry audit log is empty";
    return std::nullopt;
  }
  auto receipt = parseRegistryAuditReceipt(statement.text(0), error);
  if (receipt)
    return receipt->checkpoint;
  error.clear();
  auto lifecycle = parseRegistryLifecycleReceipt(statement.text(0), error);
  if (!lifecycle)
    return std::nullopt;
  return lifecycle->checkpoint;
}

std::optional<std::string>
RegistryPublicationStore::auditLeaf(std::uint64_t index,
                                    std::string &error) const {
  if (!impl_)
    return std::nullopt;
  std::lock_guard lock(impl_->mutex);
  Statement statement(
      impl_->database,
      "SELECT canonical_leaf FROM audit_leaves WHERE leaf_index = ?", error);
  if (!statement || !statement.bind(1, index, error) ||
      statement.step() != SQLITE_ROW) {
    if (error.empty())
      error = "registry audit leaf was not found";
    return std::nullopt;
  }
  return statement.text(0);
}

std::vector<std::string> RegistryPublicationStore::inclusionProof(
    std::uint64_t index, std::uint64_t tree_size, std::string &error) const {
  if (!impl_ || tree_size == 0 || index >= tree_size) {
    error = "registry audit inclusion proof request is invalid";
    return {};
  }
  std::lock_guard lock(impl_->mutex);
  auto leaves = impl_->leafHashes(tree_size, error);
  return error.empty() ? registryMerkleInclusionProof(leaves, index)
                       : std::vector<std::string>{};
}

std::vector<std::string>
RegistryPublicationStore::consistencyProof(std::uint64_t old_tree_size,
                                           std::uint64_t new_tree_size,
                                           std::string &error) const {
  if (!impl_ || old_tree_size == 0 || old_tree_size > new_tree_size) {
    error = "registry audit consistency proof request is invalid";
    return {};
  }
  std::lock_guard lock(impl_->mutex);
  auto leaves = impl_->leafHashes(new_tree_size, error);
  return error.empty() ? registryMerkleConsistencyProof(leaves, old_tree_size)
                       : std::vector<std::string>{};
}

