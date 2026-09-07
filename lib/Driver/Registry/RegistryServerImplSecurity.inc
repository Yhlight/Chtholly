// Included inside RegistryPublicationStore::Impl; all methods alias its database and config state.
  std::vector<std::string> leafHashes(std::uint64_t limit,
                                      std::string &error) const {
    std::vector<std::string> hashes;
    Statement statement(database,
                        "SELECT leaf_hash FROM audit_leaves "
                        "WHERE leaf_index < ? ORDER BY leaf_index",
                        error);
    if (!statement || !statement.bind(1, limit, error))
      return hashes;
    while (statement.step() == SQLITE_ROW)
      hashes.push_back(statement.text(0));
    if (hashes.size() != limit) {
      error = "registry audit log is non-contiguous";
      hashes.clear();
    }
    return hashes;
  }

  std::optional<RegistrySignerResponse>
  executeSigner(RegistrySignerOperation operation, RegistrySigningRole role,
                const RegistryTrustRole &trust_role,
                const std::vector<std::string> &revoked_key_ids,
                std::uint64_t root_version, std::string payload,
                std::string &error) const {
    auto authorized =
        registryActiveRoleKeyIds(trust_role, revoked_key_ids, error);
    if (!signer || !authorized || trust_role.threshold == 0 ||
        authorized->size() < trust_role.threshold) {
      if (error.empty())
        error = "registry signing role has insufficient active keys";
      return std::nullopt;
    }
    RegistrySignerRequest request;
    request.operation = operation;
    request.registry_name = config.registry_name;
    request.role = role;
    request.root_version = root_version;
    request.threshold = trust_role.threshold;
    request.authorized_key_ids = *authorized;
    request.payload = std::move(payload);
    auto response = signer->execute(request, error);
    if (!response)
      return std::nullopt;
    const auto outside_role = [&](const auto &id) {
      return !std::binary_search(authorized->begin(), authorized->end(), id);
    };
    if (response->payload_sha256 != sha256Hex(request.payload) ||
        response->key_ids.size() < trust_role.threshold ||
        std::any_of(response->key_ids.begin(), response->key_ids.end(),
                    outside_role) ||
        std::any_of(response->signatures.begin(), response->signatures.end(),
                    [&](const auto &signature) {
                      return outside_role(signature.key_id) ||
                             !std::binary_search(response->key_ids.begin(),
                                                 response->key_ids.end(),
                                                 signature.key_id);
                    })) {
      error = "registry signer response is outside the current trust role";
      return std::nullopt;
    }
    if (operation == RegistrySignerOperation::Sign &&
        !verifyRegistryRoleSignatures(request.payload, response->signatures,
                                      trust_role, revoked_key_ids, error))
      return std::nullopt;
    return response;
  }

  bool signerCapabilitiesMatch(const RegistryRootMetadata &root,
                               std::string &error) const {
    return executeSigner(RegistrySignerOperation::Keys,
                         RegistrySigningRole::Snapshot, root.snapshot,
                         root.revoked_key_ids, root.version, {}, error)
               .has_value() &&
           executeSigner(RegistrySignerOperation::Keys,
                         RegistrySigningRole::Timestamp, root.timestamp,
                         root.revoked_key_ids, root.version, {}, error)
               .has_value() &&
           executeSigner(RegistrySignerOperation::Keys,
                         RegistrySigningRole::Audit, root.audit,
                         root.revoked_key_ids, root.version, {}, error)
               .has_value();
  }

  bool signCheckpoint(RegistryAuditCheckpoint &checkpoint,
                       const RegistryRootMetadata &root,
                       std::string &error) const {
    const auto payload = registryAuditCheckpointSignatureInput(checkpoint);
    auto response = executeSigner(
        RegistrySignerOperation::Sign, RegistrySigningRole::Audit, root.audit,
        root.revoked_key_ids, root.version, payload, error);
    if (!response)
      return false;
    checkpoint.signatures = std::move(response->signatures);
    return true;
  }

  std::optional<std::pair<std::string, std::string>>
  authenticate(std::string_view token, std::string_view package,
               std::string &error) const {
    const auto separator = token.find('.');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == token.size()) {
      error = "registry publisher token is malformed";
      return std::nullopt;
    }
    const auto id = token.substr(0, separator);
    const auto secret = token.substr(separator + 1);
    Statement statement(database,
                        "SELECT principal, secret_hash, all_packages, revoked "
                        "FROM publishers WHERE token_id = ?",
                        error);
    if (!statement || !statement.bind(1, id, error) ||
        statement.step() != SQLITE_ROW || statement.boolean(3) ||
        crypto_pwhash_str_verify(statement.text(1).c_str(), secret.data(),
                                 secret.size()) != 0) {
      error = "registry publisher authentication failed";
      return std::nullopt;
    }
    const auto principal = statement.text(0);
    if (!statement.boolean(2)) {
      Statement scope(database,
                      "SELECT 1 FROM publisher_packages "
                      "WHERE token_id = ? AND package_name = ?",
                      error);
      if (!scope || !scope.bind(1, id, error) ||
          !scope.bind(2, package, error) || scope.step() != SQLITE_ROW) {
        error = "registry publisher is not authorized for package '" +
                std::string(package) + "'";
        return std::nullopt;
      }
    }
    return std::pair{principal, std::string(id)};
  }

  std::optional<std::string>
  authenticateOperator(std::string_view token, std::string_view capability,
                       std::string &error) const {
    const auto separator = token.find('.');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == token.size()) {
      error = "registry operator token is malformed";
      return std::nullopt;
    }
    const auto id = token.substr(0, separator);
    const auto secret = token.substr(separator + 1);
    Statement statement(
        database,
        "SELECT principal, secret_hash, revoked FROM operators "
        "WHERE token_id = ?",
        error);
    if (!statement || !statement.bind(1, id, error) ||
        statement.step() != SQLITE_ROW || statement.boolean(2) ||
        crypto_pwhash_str_verify(statement.text(1).c_str(), secret.data(),
                                 secret.size()) != 0) {
      error = "registry operator authentication failed";
      return std::nullopt;
    }
    Statement permission(
        database,
        "SELECT 1 FROM operator_capabilities WHERE token_id = ? "
        "AND capability = ?",
        error);
    if (!permission || !permission.bind(1, id, error) ||
        !permission.bind(2, capability, error) ||
        permission.step() != SQLITE_ROW) {
      error = "registry operator token lacks capability '" +
              std::string(capability) + "'";
      return std::nullopt;
    }
    return statement.text(0);
  }

