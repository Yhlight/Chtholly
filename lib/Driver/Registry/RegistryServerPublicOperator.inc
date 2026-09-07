// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
bool RegistryPublicationStore::revokeOperatorToken(std::string_view token_id,
                                                   std::string &error) {
  if (!impl_ || token_id.empty()) {
    error = "registry operator token ID is missing";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  Statement statement(impl_->database,
                      "UPDATE operators SET revoked = 1 WHERE token_id = ?",
                      error);
  if (!statement || !statement.bind(1, token_id, error) ||
      statement.step() != SQLITE_DONE || sqlite3_changes(impl_->database) != 1) {
    if (error.empty())
      error = "registry operator token ID was not found";
    return false;
  }
  return true;
}

std::optional<std::vector<RegistryOperatorTokenInfo>>
RegistryPublicationStore::listOperatorTokens(std::string &error) const {
  if (!impl_) {
    error = "registry publication store is not open";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  Statement tokens(impl_->database,
                   "SELECT token_id, principal, revoked FROM operators "
                   "ORDER BY token_id",
                   error);
  if (!tokens)
    return std::nullopt;
  std::vector<RegistryOperatorTokenInfo> result;
  while (tokens.step() == SQLITE_ROW) {
    RegistryOperatorTokenInfo info;
    info.token_id = tokens.text(0);
    info.principal = tokens.text(1);
    info.revoked = tokens.boolean(2);
    Statement capabilities(
        impl_->database,
        "SELECT capability FROM operator_capabilities WHERE token_id = ? "
        "ORDER BY capability",
        error);
    if (!capabilities || !capabilities.bind(1, info.token_id, error))
      return std::nullopt;
    while (capabilities.step() == SQLITE_ROW)
      info.capabilities.push_back(capabilities.text(0));
    result.push_back(std::move(info));
  }
  return result;
}

bool RegistryPublicationStore::authenticateOperatorToken(
    std::string_view token, std::string_view capability,
    std::string &error) const {
  if (!impl_ || token.empty() || capability.empty()) {
    error = "registry operator authentication request is incomplete";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  return impl_->authenticateOperator(token, capability, error).has_value();
}

