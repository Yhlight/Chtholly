// Included at namespace scope; this fragment aliases RegistryPublicationStore
// and its single Impl state without changing public behavior.
RegistryPublicationStore::RegistryPublicationStore() = default;
RegistryPublicationStore::~RegistryPublicationStore() = default;
RegistryPublicationStore::RegistryPublicationStore(
    RegistryPublicationStore &&) noexcept = default;
RegistryPublicationStore &RegistryPublicationStore::operator=(
    RegistryPublicationStore &&) noexcept = default;
RegistryPublicationStore::RegistryPublicationStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

std::optional<RegistryPublicationStore>
RegistryPublicationStore::open(RegistryServerConfig config,
                               std::string &error) {
  if (!registry_crypto::initialize(error))
    return std::nullopt;
  if (config.registry_name.empty() || config.state_directory.empty() ||
      config.index_worktree.empty() ||
      !isValidHttpsBaseUrl(config.public_artifact_base_url) ||
      config.bootstrap_root_threshold == 0 ||
      config.signer.command.empty() || config.signer.config_path.empty() ||
      config.signer.timeout_milliseconds == 0 ||
      config.snapshot_validity_seconds == 0 ||
      config.timestamp_validity_seconds == 0 ||
      config.snapshot_validity_seconds >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)()) ||
      config.timestamp_validity_seconds >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)()) ||
      config.audit_blob_seconds > static_cast<std::uint64_t>(
                                      (std::numeric_limits<std::int64_t>::max)()) ||
      config.recovery_point_seconds >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)()) ||
      config.unreferenced_blob_seconds >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)()) ||
      config.max_archive_bytes == 0) {
    error = "registry server configuration is incomplete";
    return std::nullopt;
  }
  while (config.public_artifact_base_url.ends_with('/'))
    config.public_artifact_base_url.pop_back();
  auto root_request = rootRequest(config, effectiveNow(-1), error);
  auto root = root_request ? verifyRegistryRootChain(*root_request, error)
                           : std::optional<VerifiedRegistryRoot>{};
  if (!root || root->root.format != RegistryRootFormatV2) {
    if (error.empty())
      error = "registry server requires a verified root v2";
    return std::nullopt;
  }
  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->signer = createRegistryCommandSigningProvider(impl->config.signer);
  if (!impl->initialize(error) ||
      !impl->signerCapabilitiesMatch(root->root, error) ||
      !impl->recover(error)) {
    return std::nullopt;
  }
  return RegistryPublicationStore(std::move(impl));
}

std::optional<std::string> RegistryPublicationStore::createPublisherToken(
    const RegistryPublisherTokenRequest &request, std::string &error) {
  if (!impl_ || request.principal.empty() ||
      (!request.all_packages && request.packages.empty()) ||
      (request.all_packages && !request.packages.empty())) {
    error = "registry publisher token request is incomplete";
    return std::nullopt;
  }
  auto packages = request.packages;
  std::sort(packages.begin(), packages.end());
  if (std::adjacent_find(packages.begin(), packages.end()) != packages.end() ||
      std::any_of(packages.begin(), packages.end(), [](const auto &name) {
        return !isSafeRegistryPackageName(name);
      })) {
    error = "registry publisher token contains invalid package scopes";
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
    error = "failed to hash registry publisher token";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  Transaction transaction(impl_->database, error);
  Statement publisher(
      impl_->database,
      "INSERT INTO publishers(token_id, principal, secret_hash, all_packages) "
      "VALUES(?, ?, ?, ?)",
      error);
  bool ok = transaction && publisher && publisher.bind(1, token_id, error) &&
            publisher.bind(2, request.principal, error) &&
            publisher.bind(3, std::string_view(hash), error) &&
            publisher.bind(4, request.all_packages, error) &&
            publisher.step() == SQLITE_DONE;
  sodium_memzero(hash, sizeof(hash));
  for (const auto &package : packages) {
    Statement scope(impl_->database,
                    "INSERT INTO publisher_packages(token_id, package_name) "
                    "VALUES(?, ?)",
                    error);
    ok = ok && scope && scope.bind(1, token_id, error) &&
         scope.bind(2, package, error) && scope.step() == SQLITE_DONE;
  }
  if (!ok || !transaction.commit(error)) {
    sodium_memzero(encoded.data(), encoded.size());
    if (error.empty())
      error = "failed to store registry publisher token";
    return std::nullopt;
  }
  auto token = token_id + "." + encoded;
  sodium_memzero(encoded.data(), encoded.size());
  return token;
}

