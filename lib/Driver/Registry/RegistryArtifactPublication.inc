// Included by RegistryArtifact.cpp; this fragment retains registry artifact
// canonicalization, publication, lifecycle, gossip, or fetch state.
std::optional<RegistryPublishReceipt>
publishRegistryArtifact(const RegistryPublishRequest &request,
                        std::string &error) {
  const auto authority_end = request.publish_url.find('/', 8);
  const auto authority =
      std::string_view(request.publish_url)
          .substr(8, authority_end == std::string::npos ? std::string_view::npos
                                                        : authority_end - 8);
  if (request.registry_name.empty() ||
      !request.publish_url.starts_with("https://") || authority.empty() ||
      hasUrlCredentials(request.publish_url) ||
      request.publish_url.find_first_of("\t\r\n ") != std::string::npos ||
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
    error =
        "registry publish requires an HTTPS endpoint, bootstrap root trust, "
        "and an identity store";
    return std::nullopt;
  }
  auto token = loadPublishToken(request, error);
  if (!token)
    return std::nullopt;

  auto entry_text = readTextFile(request.entry_path, error);
  if (!entry_text)
    return std::nullopt;
  auto entry =
      parseSignedRegistryEntryText(*entry_text, request.entry_path, error);
  if (!entry)
    return std::nullopt;
  auto archive = inspectPackageArtifactArchive(request.archive_path, error);
  if (!archive)
    return std::nullopt;
  std::error_code size_error;
  const auto archive_size =
      std::filesystem::file_size(request.archive_path, size_error);
  if (size_error) {
    error = "failed to measure registry publish archive";
    return std::nullopt;
  }
  const auto variant = std::find_if(
      entry->variants.begin(), entry->variants.end(),
      [&](const auto &candidate) {
        return candidate.archive_size == archive_size &&
               candidate.archive_sha256 == archive->archive_sha256 &&
               candidate.artifact_identity == archive->artifact_identity &&
               candidate.closure_digest == archive->closure_digest;
      });
  if (variant == entry->variants.end() ||
      entry->package_name != archive->package_name) {
    error = "registry publish archive does not match a signed entry variant";
    return std::nullopt;
  }
  if (!initializeCurl(error))
    return std::nullopt;

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    error = "failed to initialize registry publish HTTPS request";
    return std::nullopt;
  }
  curl_mime *mime = curl_mime_init(curl);
  curl_slist *headers = nullptr;
  if (mime == nullptr) {
    curl_easy_cleanup(curl);
    error = "failed to initialize registry publish multipart body";
    return std::nullopt;
  }
  const auto add_file = [&](const char *name, const std::string &path) {
    curl_mimepart *part = curl_mime_addpart(mime);
    return part != nullptr && curl_mime_name(part, name) == CURLE_OK &&
           curl_mime_filedata(part, path.c_str()) == CURLE_OK;
  };
  if (!add_file("archive", request.archive_path) ||
      !add_file("entry", request.entry_path)) {
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    error = "failed to construct registry publish multipart body";
    return std::nullopt;
  }
  const std::string authorization = "Authorization: Bearer " + *token;
  const std::string idempotency =
      "Idempotency-Key: " +
      registryPublishMutationId(request.registry_name, *entry, *variant);
  headers = curl_slist_append(headers, authorization.c_str());
  headers = curl_slist_append(headers, idempotency.c_str());
  headers = curl_slist_append(headers, "Accept: text/plain");
  if (headers == nullptr) {
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    error = "failed to construct registry publish headers";
    return std::nullopt;
  }
  CurlTextState response;
  curl_easy_setopt(curl, CURLOPT_URL, request.publish_url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlText);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "chthollyc-registry-publish/1");
  if (!request.ca_bundle_path.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, request.ca_bundle_path.c_str());
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_mime_free(mime);
  curl_easy_cleanup(curl);
  token->assign(token->size(), '\0');
  if (result != CURLE_OK || response.exceeded_limit || status < 200 ||
      status >= 300) {
    error = response.exceeded_limit ? "registry publish receipt exceeded 64 KiB"
                                    : "registry publish HTTPS request failed";
    return std::nullopt;
  }

  auto audit_receipt = parseRegistryAuditReceipt(response.text, error);
  if (!audit_receipt)
    return std::nullopt;
  if (audit_receipt->registry_name != request.registry_name ||
      audit_receipt->package_name != entry->package_name ||
      audit_receipt->version != entry->version ||
      audit_receipt->variant_name != variant->name ||
      audit_receipt->archive_sha256 != archive->archive_sha256 ||
      audit_receipt->entry_sha256 !=
          sha256Hex(renderSignedRegistryEntry(*entry))) {
    error = "registry publish receipt does not match the uploaded artifact";
    return std::nullopt;
  }

  const auto origin = authority_end == std::string::npos
                          ? request.publish_url
                          : request.publish_url.substr(0, authority_end);
  const auto identity = sha256Hex(request.registry_name + "\n" + origin);
  const auto audit_state = std::filesystem::path(request.identity_store_path) /
                           "registry-audit" / identity;
  if (audit_receipt->checkpoint.root_version > 4096) {
    error = "registry audit receipt root version exceeds the client limit";
    return std::nullopt;
  }
  const auto chain_root =
      audit_state / "chains" /
      std::to_string(audit_receipt->checkpoint.root_version);
  const auto roots_directory = chain_root / "trust" / "root";
  const auto observed_roots_directory = audit_state / "observed-roots";
  std::error_code file_error;
  std::filesystem::create_directories(roots_directory, file_error);
  if (!file_error)
    std::filesystem::create_directories(observed_roots_directory, file_error);
  if (file_error) {
    error = "failed to create registry audit identity state: " +
            file_error.message();
    return std::nullopt;
  }
  const auto persist_root = [&](const std::filesystem::path &destination,
                                const std::string &text) {
    const auto exists = std::filesystem::exists(destination, file_error);
    if (file_error) {
      error =
          "failed to inspect persisted registry root: " + file_error.message();
      return false;
    }
    if (exists) {
      auto existing = readTextFile(destination.string(), error);
      if (!existing)
        return false;
      if (*existing != text) {
        error = "registry root version changed after it was observed";
        return false;
      }
      return true;
    }
    const auto temporary = destination.string() + ".tmp";
    if (writeTextFile(temporary, text, error) &&
        replaceFile(temporary, destination.string(), file_error))
      return true;
    if (error.empty())
      error = "failed to persist registry root chain: " + file_error.message();
    removeFile(temporary, file_error);
    return false;
  };
  std::vector<std::pair<std::filesystem::path, std::string>> roots_to_observe;
  std::vector<std::string> root_chain_texts;
  for (std::uint64_t version = 1;
       version <= audit_receipt->checkpoint.root_version; ++version) {
    auto fetched =
        registryHttpsGet(origin + "/v1/trust/root/" + std::to_string(version),
                         request.ca_bundle_path, 1024 * 1024, error);
    if (!fetched || fetched->first != 200) {
      if (fetched)
        error = "registry did not provide required root version " +
                std::to_string(version);
      return std::nullopt;
    }
    const auto root_path =
        roots_directory / (std::to_string(version) + ".toml");
    const auto observed_root_path =
        observed_roots_directory / (std::to_string(version) + ".toml");
    const auto observed =
        std::filesystem::exists(observed_root_path, file_error);
    if (file_error) {
      error =
          "failed to inspect observed registry root: " + file_error.message();
      return std::nullopt;
    }
    if (observed) {
      auto existing = readTextFile(observed_root_path.string(), error);
      if (!existing)
        return std::nullopt;
      if (*existing != fetched->second) {
        error = "registry root version changed after it was observed";
        return std::nullopt;
      }
    } else {
      roots_to_observe.emplace_back(observed_root_path, fetched->second);
    }
    root_chain_texts.push_back(fetched->second);
    if (!persist_root(root_path, fetched->second))
      return std::nullopt;
  }
  RegistryRootChainVerificationRequest root_request;
  root_request.registry_name = request.registry_name;
  root_request.checkout_root = chain_root.string();
  root_request.bootstrap_root_keys = request.bootstrap_root_keys;
  root_request.bootstrap_root_threshold = request.bootstrap_root_threshold;
  auto verified_root = verifyRegistryRootChain(root_request, error);
  if (!verified_root ||
      !verifyRegistryAuditReceipt(*audit_receipt, verified_root->root, error))
    return std::nullopt;
  for (const auto &[path, text] : roots_to_observe) {
    if (!persist_root(path, text))
      return std::nullopt;
  }

  const auto checkpoint_path = audit_state / "last-receipt.txt";
  if (std::filesystem::exists(checkpoint_path, file_error)) {
    auto previous_text = readTextFile(checkpoint_path.string(), error);
    auto previous = previous_text
                        ? parseRegistryAuditReceipt(*previous_text, error)
                        : std::optional<RegistryAuditReceipt>{};
    error.clear();
    auto previous_lifecycle =
        !previous && previous_text
            ? parseRegistryLifecycleReceipt(*previous_text, error)
            : std::optional<RegistryLifecycleReceipt>{};
    if (!previous && !previous_lifecycle)
      return std::nullopt;
    const auto previous_checkpoint =
        previous ? previous->checkpoint : previous_lifecycle->checkpoint;
    if (previous_checkpoint.root_version >
        audit_receipt->checkpoint.root_version) {
      error = "registry audit checkpoint rolled back to an older root";
      return std::nullopt;
    }
    if (previous_checkpoint.tree_size > audit_receipt->checkpoint.tree_size) {
      error = "registry audit checkpoint rolled back to an older tree";
      return std::nullopt;
    }
    if (previous_checkpoint.tree_size == audit_receipt->checkpoint.tree_size) {
      if (previous_checkpoint.root_hash !=
          audit_receipt->checkpoint.root_hash) {
        error = "registry audit checkpoint equivocated at the same tree size";
        return std::nullopt;
      }
    } else {
      const auto old_size = previous_checkpoint.tree_size;
      const auto new_size = audit_receipt->checkpoint.tree_size;
      auto fetched =
          registryHttpsGet(origin + "/v1/audit/consistency?old_size=" +
                               std::to_string(old_size) +
                               "&new_size=" + std::to_string(new_size),
                           request.ca_bundle_path, 1024 * 1024, error);
      auto proof =
          fetched && fetched->first == 200
              ? parseRegistryProof(fetched->second,
                                   "chtholly-registry-audit-consistency-v1",
                                   old_size, new_size, error)
              : std::optional<std::vector<std::string>>{};
      if (!proof || !verifyRegistryMerkleConsistency(
                        old_size, new_size, previous_checkpoint.root_hash,
                        audit_receipt->checkpoint.root_hash, *proof)) {
        if (error.empty())
          error = "registry audit consistency proof is invalid";
        return std::nullopt;
      }
    }
  }
  std::filesystem::create_directories(audit_state, file_error);
  const auto checkpoint_temporary = checkpoint_path.string() + ".tmp";
  if (file_error ||
      !writeTextFile(checkpoint_temporary, response.text, error) ||
      !replaceFile(checkpoint_temporary, checkpoint_path.string(),
                   file_error)) {
    if (error.empty())
      error = "failed to persist registry audit checkpoint: " +
              file_error.message();
    removeFile(checkpoint_temporary, file_error);
    return std::nullopt;
  }
  if (!request.receipt_output_path.empty()) {
    const auto temporary = request.receipt_output_path + ".tmp";
    if (!writeTextFile(temporary, response.text, error) ||
        !replaceFile(temporary, request.receipt_output_path, file_error)) {
      if (error.empty())
        error = "failed to write registry publication receipt: " +
                file_error.message();
      removeFile(temporary, file_error);
      return std::nullopt;
    }
  }
  if (request.witness_threshold != 0) {
    const auto pending_directory = audit_state / "pending-gossip";
    std::filesystem::create_directories(pending_directory, file_error);
    const auto pending_path =
        pending_directory / (sha256Hex(response.text) + ".receipt");
    if (file_error ||
        !writeTextFile(pending_path.string(), response.text, error)) {
      if (error.empty())
        error = "failed to persist pending registry witness gossip: " +
                file_error.message();
      return std::nullopt;
    }
    RegistryWitnessObservation observation;
    observation.registry_origin = origin;
    observation.checkpoint = audit_receipt->checkpoint;
    observation.root_chain = std::move(root_chain_texts);
    const auto statement_state = audit_state / "witness";
    auto witnessed = submitWitnessObservation(
        observation, request.witness_urls, request.witness_keys,
        request.witness_threshold,
        request.witness_ca_bundle_path.empty() ? request.ca_bundle_path
                                               : request.witness_ca_bundle_path,
        statement_state, error);
    if (!witnessed)
      return std::nullopt;
    std::filesystem::remove(pending_path, file_error);
    if (file_error) {
      error = "failed to clear completed registry witness gossip: " +
              file_error.message();
      return std::nullopt;
    }
  }
  return RegistryPublishReceipt{
      audit_receipt->registry_name, audit_receipt->package_name,
      audit_receipt->version, audit_receipt->variant_name,
      audit_receipt->archive_sha256};
}
