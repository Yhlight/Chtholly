// Included by RegistryArtifact.cpp; this fragment retains registry artifact
// canonicalization, publication, lifecycle, gossip, or fetch state.
bool generateRegistrySigningKeyFiles(const std::string &secret_key_path,
                                     const std::string &public_key_path,
                                     std::string &error) {
  return registry_crypto::generateSigningKeyFiles(secret_key_path,
                                                  public_key_path, error);
}

bool signRegistryArtifactEntry(const RegistrySigningRequest &request,
                               RegistryArtifactVariant &variant,
                               std::string &error) {
  if (!registry_crypto::initialize(error)) {
    return false;
  }
  std::string version_error;
  if (request.registry_name.empty() || request.package_name.empty() ||
      request.url.empty() || request.secret_key_paths.empty() ||
      !parseSemVer(request.version, version_error)) {
    error = "registry signing request requires registry, package, semantic "
            "version, and artifact URL";
    return false;
  }
  auto archive = inspectPackageArtifactArchive(request.archive_path, error);
  if (!archive) {
    return false;
  }
  if (archive->package_name != request.package_name) {
    error = "registry signing package does not match archive root package";
    return false;
  }
  auto requested_features = request.requested_features;
  std::sort(requested_features.begin(), requested_features.end());
  if (std::adjacent_find(requested_features.begin(),
                         requested_features.end()) !=
      requested_features.end()) {
    error = "registry signing request contains duplicate features";
    return false;
  }
  for (const auto &feature : requested_features) {
    if (!std::binary_search(archive->resolved_features.begin(),
                            archive->resolved_features.end(), feature)) {
      error = "registry signing requested feature is absent from archive: '" +
              feature + "'";
      return false;
    }
  }
  if (request.default_features != archive->default_features) {
    error = "registry signing default feature mode does not match archive";
    return false;
  }
  std::error_code ec;
  const auto archive_size =
      std::filesystem::file_size(request.archive_path, ec);
  if (ec || archive->archive_sha256.empty()) {
    error = "failed to measure signed artifact archive";
    return false;
  }
  variant.target = archive->target;
  variant.abi_version = archive->abi_version;
  variant.runtime_abi = archive->runtime_abi;
  variant.requested_features = std::move(requested_features);
  variant.default_features = request.default_features;
  variant.url = request.url;
  variant.archive_size = archive_size;
  variant.archive_sha256 = archive->archive_sha256;
  variant.artifact_identity = archive->artifact_identity;
  variant.closure_digest = archive->closure_digest;
  variant.name = registryArtifactVariantName(variant);

  SignedRegistryEntry entry;
  if (std::filesystem::exists(request.output_path)) {
    auto text = readTextFile(request.output_path, error);
    auto parsed =
        text ? parseSignedRegistryEntryText(*text, request.output_path, error)
             : std::optional<SignedRegistryEntry>{};
    if (!parsed) {
      return false;
    }
    entry = std::move(*parsed);
    if (entry.package_name != request.package_name ||
        entry.version != request.version) {
      error = "registry entry output belongs to a different package version";
      return false;
    }
  } else {
    entry.package_name = request.package_name;
    entry.version = request.version;
  }
  const auto same_variant = [&](const RegistryArtifactVariant &existing) {
    return existing.target.triple == variant.target.triple &&
           existing.target.pointer_width_bits ==
               variant.target.pointer_width_bits &&
           existing.abi_version == variant.abi_version &&
           existing.runtime_abi == variant.runtime_abi &&
           existing.requested_features == variant.requested_features &&
           existing.default_features == variant.default_features;
  };
  const auto conflict =
      std::find_if(entry.variants.begin(), entry.variants.end(), same_variant);
  if (conflict != entry.variants.end()) {
    if (registryArtifactVariantName(*conflict) != variant.name) {
      error = "registry entry already contains a conflicting target/feature "
              "variant";
      return false;
    }
    variant.signatures = conflict->signatures;
  }

  std::set<std::string> request_key_ids;
  for (const auto &secret_path : request.secret_key_paths) {
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
    if (!registry_crypto::loadSecretKeyFile(secret_path, secret, public_key,
                                            error)) {
      return false;
    }
    const auto key_id = registry_crypto::publicKeyId(public_key);
    if (!request_key_ids.insert(key_id).second) {
      sodium_memzero(secret.data(), secret.size());
      error = "registry entry signing request contains a duplicate secret key";
      return false;
    }
    const auto message = canonicalRegistryArtifactSignatureInput(
        request.registry_name, request.package_name, request.version, variant,
        key_id);
    registry_crypto::Signature signature{};
    crypto_sign_detached(
        signature.data(), nullptr,
        reinterpret_cast<const unsigned char *>(message.data()),
        static_cast<unsigned long long>(message.size()), secret.data());
    sodium_memzero(secret.data(), secret.size());
    RegistryArtifactSignature signed_value{
        key_id, registry_crypto::encodeSignature(signature)};
    const auto existing = std::find_if(
        variant.signatures.begin(), variant.signatures.end(),
        [&](const auto &candidate) { return candidate.key_id == key_id; });
    if (existing == variant.signatures.end()) {
      variant.signatures.push_back(std::move(signed_value));
    } else {
      *existing = std::move(signed_value);
    }
  }
  std::sort(
      variant.signatures.begin(), variant.signatures.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.key_id < rhs.key_id; });
  if (conflict == entry.variants.end()) {
    entry.variants.push_back(variant);
  } else {
    *conflict = variant;
  }
  for (auto &existing : entry.variants) {
    existing.name = registryArtifactVariantName(existing);
  }
  std::sort(
      entry.variants.begin(), entry.variants.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.name < rhs.name; });
  if (std::adjacent_find(entry.variants.begin(), entry.variants.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.name == rhs.name;
                         }) != entry.variants.end()) {
    error = "registry entry contains duplicate deterministic artifact variants";
    return false;
  }
  const auto output = renderSignedRegistryEntry(entry);
  const auto temporary = request.output_path + ".tmp";
  if (!writeTextFile(temporary, output, error)) {
    return false;
  }
  if (!replaceFile(temporary, request.output_path, ec)) {
    error = "failed to publish signed registry entry: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}
