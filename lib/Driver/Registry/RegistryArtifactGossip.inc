// Included by RegistryArtifact.cpp; this fragment retains registry artifact
// canonicalization, publication, lifecycle, gossip, or fetch state.
std::optional<std::size_t>
gossipRegistryAuditReceipt(const RegistryAuditGossipRequest &request,
                           std::string &error) {
  const auto authority_end = request.registry_origin.find('/', 8);
  const auto authority =
      std::string_view(request.registry_origin)
          .substr(8, authority_end == std::string::npos ? std::string_view::npos
                                                        : authority_end - 8);
  if (request.registry_name.empty() ||
      !request.registry_origin.starts_with("https://") || authority.empty() ||
      authority_end != std::string::npos ||
      hasUrlCredentials(request.registry_origin) ||
      request.bootstrap_root_keys.empty() ||
      request.bootstrap_root_threshold == 0 ||
      request.bootstrap_root_threshold > request.bootstrap_root_keys.size() ||
      request.identity_store_path.empty() ||
      ((!request.witness_urls.empty() || !request.witness_keys.empty() ||
        request.witness_threshold != 0) &&
       (request.witness_urls.empty() || request.witness_keys.empty() ||
        request.witness_threshold == 0 ||
        request.witness_threshold > request.witness_urls.size() ||
        request.witness_threshold > request.witness_keys.size()))) {
    error = "registry audit gossip request is incomplete";
    return std::nullopt;
  }
  auto receipt_text = readTextFile(request.receipt_path, error);
  auto receipt = receipt_text ? parseRegistryAuditReceipt(*receipt_text, error)
                              : std::optional<RegistryAuditReceipt>{};
  error.clear();
  auto lifecycle = !receipt && receipt_text
                       ? parseRegistryLifecycleReceipt(*receipt_text, error)
                       : std::optional<RegistryLifecycleReceipt>{};
  const auto registry_name = receipt ? receipt->registry_name
                                     : lifecycle ? lifecycle->registry_name : "";
  const auto checkpoint = receipt ? receipt->checkpoint
                                  : lifecycle ? lifecycle->checkpoint
                                              : RegistryAuditCheckpoint{};
  if ((!receipt && !lifecycle) || registry_name != request.registry_name ||
      checkpoint.root_version > 4096) {
    if (error.empty())
      error = "registry audit receipt does not match the gossip request";
    return std::nullopt;
  }
  const auto identity =
      sha256Hex(request.registry_name + "\n" + request.registry_origin);
  const auto state = std::filesystem::path(request.identity_store_path) /
                     "registry-audit" / identity;
  const auto chain_root = state / "gossip-chains" /
                          std::to_string(checkpoint.root_version);
  const auto roots_directory = chain_root / "trust" / "root";
  const auto observed_roots_directory = state / "observed-roots";
  std::error_code ec;
  std::filesystem::create_directories(roots_directory, ec);
  if (!ec)
    std::filesystem::create_directories(observed_roots_directory, ec);
  if (ec) {
    error = "failed to create registry gossip root chain: " + ec.message();
    return std::nullopt;
  }
  std::vector<std::string> roots;
  std::vector<std::pair<std::filesystem::path, std::string>> roots_to_observe;
  for (std::uint64_t version = 1; version <= checkpoint.root_version;
       ++version) {
    auto fetched = registryHttpsGet(
        request.registry_origin + "/v1/trust/root/" + std::to_string(version),
        request.registry_ca_bundle_path, 1024u * 1024u, error);
    if (!fetched || fetched->first != 200) {
      if (error.empty())
        error = "registry did not provide the root chain required for gossip";
      return std::nullopt;
    }
    auto root =
        parseRegistryRootMetadata(fetched->second, "gossip root", error);
    const auto root_path =
        roots_directory / (std::to_string(version) + ".toml");
    const auto observed_path =
        observed_roots_directory / (std::to_string(version) + ".toml");
    if (!root || root->registry_name != request.registry_name ||
        root->version != version ||
        renderRegistryRootMetadata(*root) != fetched->second)
      return std::nullopt;
    if (std::filesystem::exists(root_path, ec)) {
      auto observed = readTextFile(root_path.string(), error);
      if (!observed || *observed != fetched->second) {
        if (error.empty())
          error = "registry root version changed after it was observed";
        return std::nullopt;
      }
    } else if (ec ||
               !writeTextFile(root_path.string(), fetched->second, error)) {
      if (error.empty())
        error = "failed to persist registry gossip root";
      return std::nullopt;
    }
    if (std::filesystem::exists(observed_path, ec)) {
      auto observed = readTextFile(observed_path.string(), error);
      if (!observed || *observed != fetched->second) {
        if (error.empty())
          error = "registry root version changed after it was observed";
        return std::nullopt;
      }
    } else if (ec) {
      error = "failed to inspect observed registry root: " + ec.message();
      return std::nullopt;
    } else {
      roots_to_observe.emplace_back(observed_path, fetched->second);
    }
    roots.push_back(std::move(fetched->second));
  }
  RegistryRootChainVerificationRequest verify;
  verify.registry_name = request.registry_name;
  verify.checkout_root = chain_root.string();
  verify.bootstrap_root_keys = request.bootstrap_root_keys;
  verify.bootstrap_root_threshold = request.bootstrap_root_threshold;
  auto root = verifyRegistryRootChain(verify, error);
  if (!root ||
      (receipt && !verifyRegistryAuditReceipt(*receipt, root->root, error)) ||
      (lifecycle &&
       !verifyRegistryLifecycleReceipt(*lifecycle, root->root, error)))
    return std::nullopt;
  for (const auto &[path, text] : roots_to_observe) {
    if (std::filesystem::exists(path, ec)) {
      auto observed = readTextFile(path.string(), error);
      if (!observed || *observed != text) {
        if (error.empty())
          error = "registry root version changed while it was observed";
        return std::nullopt;
      }
      continue;
    }
    const auto temporary = path.string() + ".tmp." + sha256Hex(text);
    if (ec || !writeTextFile(temporary, text, error) ||
        !replaceFile(temporary, path.string(), ec)) {
      removeFile(temporary, ec);
      if (error.empty())
        error = "failed to persist observed registry root";
      return std::nullopt;
    }
  }
  const auto last_path = state / "last-receipt.txt";
  if (std::filesystem::exists(last_path, ec)) {
    auto previous_text = readTextFile(last_path.string(), error);
    auto previous_publish =
        previous_text ? parseRegistryAuditReceipt(*previous_text, error)
                      : std::optional<RegistryAuditReceipt>{};
    error.clear();
    auto previous_lifecycle =
        !previous_publish && previous_text
            ? parseRegistryLifecycleReceipt(*previous_text, error)
            : std::optional<RegistryLifecycleReceipt>{};
    if (!previous_publish && !previous_lifecycle)
      return std::nullopt;
    const auto previous = previous_publish
                              ? previous_publish->checkpoint
                              : previous_lifecycle->checkpoint;
    if (previous.root_version > checkpoint.root_version ||
        previous.tree_size > checkpoint.tree_size) {
      error = "registry audit checkpoint rolled back";
      return std::nullopt;
    }
    if (previous.tree_size == checkpoint.tree_size) {
      if (previous.root_hash != checkpoint.root_hash) {
        error = "registry audit checkpoint equivocated at the same tree size";
        return std::nullopt;
      }
    } else {
      auto fetched = registryHttpsGet(
          request.registry_origin + "/v1/audit/consistency?old_size=" +
              std::to_string(previous.tree_size) +
              "&new_size=" + std::to_string(checkpoint.tree_size),
          request.registry_ca_bundle_path, 1024 * 1024, error);
      auto proof =
          fetched && fetched->first == 200
              ? parseRegistryProof(fetched->second,
                                   "chtholly-registry-audit-consistency-v1",
                                   previous.tree_size, checkpoint.tree_size,
                                   error)
              : std::optional<std::vector<std::string>>{};
      if (!proof || !verifyRegistryMerkleConsistency(
                        previous.tree_size, checkpoint.tree_size,
                        previous.root_hash, checkpoint.root_hash, *proof)) {
        if (error.empty())
          error = "registry audit consistency proof is invalid";
        return std::nullopt;
      }
    }
  }
  std::filesystem::create_directories(state, ec);
  const auto last_temporary = last_path.string() + ".tmp";
  if (ec || !writeTextFile(last_temporary, *receipt_text, error) ||
      !replaceFile(last_temporary, last_path.string(), ec)) {
    if (error.empty())
      error = "failed to persist registry audit checkpoint";
    removeFile(last_temporary, ec);
    return std::nullopt;
  }
  if (request.witness_threshold == 0)
    return std::size_t{0};
  RegistryWitnessObservation observation{request.registry_origin, checkpoint,
                                         std::move(roots)};
  const auto pending_directory = state / "pending-gossip";
  std::filesystem::create_directories(pending_directory, ec);
  const auto digest = sha256Hex(*receipt_text);
  const auto pending = pending_directory / (digest + ".receipt");
  if (ec || !writeTextFile(pending.string(), *receipt_text, error)) {
    if (error.empty())
      error = "failed to persist pending registry witness gossip";
    return std::nullopt;
  }
  auto result = submitWitnessObservation(
      observation, request.witness_urls, request.witness_keys,
      request.witness_threshold,
      request.witness_ca_bundle_path.empty() ? request.registry_ca_bundle_path
                                             : request.witness_ca_bundle_path,
      state / "witness", error);
  if (!result)
    return std::nullopt;
  std::filesystem::remove(pending, ec);
  if (ec) {
    error =
        "failed to clear completed registry witness gossip: " + ec.message();
    return std::nullopt;
  }
  return result;
}
