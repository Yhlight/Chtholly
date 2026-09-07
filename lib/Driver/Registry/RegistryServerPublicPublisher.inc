// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
bool RegistryPublicationStore::revokePublisherToken(std::string_view token_id,
                                                    std::string &error) {
  if (!impl_ || token_id.empty()) {
    error = "registry publisher token ID is missing";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  Statement statement(impl_->database,
                      "UPDATE publishers SET revoked = 1 WHERE token_id = ?",
                      error);
  if (!statement || !statement.bind(1, token_id, error) ||
      statement.step() != SQLITE_DONE ||
      sqlite3_changes(impl_->database) != 1) {
    if (error.empty())
      error = "registry publisher token ID was not found";
    return false;
  }
  return true;
}

std::optional<std::vector<RegistryPublisherTokenInfo>>
RegistryPublicationStore::listPublisherTokens(std::string &error) const {
  if (!impl_) {
    error = "registry publication store is not open";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  Statement publishers(
      impl_->database,
      "SELECT token_id, principal, all_packages, revoked FROM publishers "
      "ORDER BY token_id",
      error);
  if (!publishers)
    return std::nullopt;
  std::vector<RegistryPublisherTokenInfo> result;
  while (publishers.step() == SQLITE_ROW) {
    RegistryPublisherTokenInfo info;
    info.token_id = publishers.text(0);
    info.principal = publishers.text(1);
    info.all_packages = publishers.boolean(2);
    info.revoked = publishers.boolean(3);
    Statement scopes(impl_->database,
                     "SELECT package_name FROM publisher_packages "
                     "WHERE token_id = ? ORDER BY package_name",
                     error);
    if (!scopes || !scopes.bind(1, info.token_id, error))
      return std::nullopt;
    while (scopes.step() == SQLITE_ROW)
      info.packages.push_back(scopes.text(0));
    result.push_back(std::move(info));
  }
  return result;
}

std::optional<std::string> RegistryPublicationStore::createOperatorToken(
    const RegistryOperatorTokenRequest &request, std::string &error) {
  if (!impl_ || request.principal.empty() || request.capabilities.empty()) {
    error = "registry operator token request is incomplete";
    return std::nullopt;
  }
  auto capabilities = request.capabilities;
  std::sort(capabilities.begin(), capabilities.end());
  if (std::adjacent_find(capabilities.begin(), capabilities.end()) !=
          capabilities.end() ||
      std::any_of(capabilities.begin(), capabilities.end(), [](const auto &item) {
        return item != "backup:create" && item != "release:yank" &&
               item != "recovery:manage" && item != "gc:plan" &&
               item != "gc:apply";
      })) {
    error = "registry operator token contains an unsupported capability";
    return std::nullopt;
  }
  std::array<unsigned char, 32> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  std::string encoded(
      sodium_base64_encoded_len(bytes.size(),
                                sodium_base64_VARIANT_URLSAFE_NO_PADDING),
      '\0');
  sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  encoded.resize(encoded.find('\0'));
  const auto token_id =
      sha256Hex(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                                 bytes.size()))
          .substr(0, 24);
  sodium_memzero(bytes.data(), bytes.size());
  char hash[crypto_pwhash_STRBYTES]{};
  if (crypto_pwhash_str(hash, encoded.data(), encoded.size(),
                        crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    sodium_memzero(encoded.data(), encoded.size());
    error = "failed to hash registry operator token";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  Transaction transaction(impl_->database, error);
  Statement token(impl_->database,
                  "INSERT INTO operators(token_id, principal, secret_hash) "
                  "VALUES(?, ?, ?)",
                  error);
  bool ok = transaction && token && token.bind(1, token_id, error) &&
            token.bind(2, request.principal, error) &&
            token.bind(3, std::string_view(hash), error) &&
            token.step() == SQLITE_DONE;
  sodium_memzero(hash, sizeof(hash));
  for (const auto &capability : capabilities) {
    Statement item(impl_->database,
                   "INSERT INTO operator_capabilities(token_id, capability) "
                   "VALUES(?, ?)",
                   error);
    ok = ok && item && item.bind(1, token_id, error) &&
         item.bind(2, capability, error) && item.step() == SQLITE_DONE;
  }
  if (!ok || !transaction.commit(error)) {
    sodium_memzero(encoded.data(), encoded.size());
    if (error.empty())
      error = "failed to store registry operator token";
    return std::nullopt;
  }
  auto result = token_id + "." + encoded;
  sodium_memzero(encoded.data(), encoded.size());
  return result;
}

