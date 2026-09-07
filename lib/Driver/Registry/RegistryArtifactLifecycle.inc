// Included by RegistryArtifact.cpp; this fragment retains registry artifact
// canonicalization, publication, lifecycle, gossip, or fetch state.
std::optional<RegistryReleaseReceipt>
changeRegistryReleaseState(const RegistryReleaseRequest &request,
                           std::string &error) {
  const auto authority_end = request.publish_url.find('/', 8);
  const auto authority =
      std::string_view(request.publish_url)
          .substr(8, authority_end == std::string::npos ? std::string_view::npos
                                                        : authority_end - 8);
  const auto origin = authority_end == std::string::npos
                          ? request.publish_url
                          : request.publish_url.substr(0, authority_end);
  if (request.registry_name.empty() || request.package_name.empty() ||
      request.version.empty() || !request.publish_url.starts_with("https://") ||
      authority.empty() || hasUrlCredentials(request.publish_url) ||
      request.publish_url.find_first_of("\t\r\n ") != std::string::npos ||
      !isRegistryReleaseReasonCode(request.reason_code) ||
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
    error = "registry release change request is incomplete";
    return std::nullopt;
  }
  auto token = loadRegistryBearerToken(request.token_environment,
                                       request.token_file_path, error);
  if (!token || !initializeCurl(error))
    return std::nullopt;
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "failed to initialize registry lifecycle HTTPS request";
    return std::nullopt;
  }
  const auto escape = [&](const std::string &value) {
    char *encoded = curl_easy_escape(curl, value.c_str(),
                                     static_cast<int>(value.size()));
    std::string result = encoded == nullptr ? std::string{} : encoded;
    curl_free(encoded);
    return result;
  };
  const auto package = escape(request.package_name);
  const auto version = escape(request.version);
  const auto action = request.state == RegistryReleaseState::Yanked
                          ? std::string("yank")
                          : std::string("unyank");
  if (package.empty() || version.empty()) {
    curl_easy_cleanup(curl);
    error = "failed to encode registry lifecycle URL";
    return std::nullopt;
  }
  const auto url = origin + "/v1/releases/" + package + "/" + version +
                   "/" + action;
  const auto mutation = sha256Hex(
      "chtholly-registry-lifecycle-request-v1\n" + request.registry_name +
      "\n" + request.package_name + "\n" + request.version + "\n" + action +
      "\n" + request.reason_code);
  const std::string authorization = "Authorization: Bearer " + *token;
  const std::string idempotency = "Idempotency-Key: sha256:" + mutation;
  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, authorization.c_str());
  headers = curl_slist_append(headers, idempotency.c_str());
  headers = curl_slist_append(headers, "Content-Type: text/plain");
  headers = curl_slist_append(headers, "Accept: text/plain");
  CurlTextState response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.reason_code.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request.reason_code.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlText);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "chthollyc-registry-lifecycle/1");
  if (!request.ca_bundle_path.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, request.ca_bundle_path.c_str());
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  token->assign(token->size(), '\0');
  if (result != CURLE_OK || response.exceeded_limit || status < 200 ||
      status >= 300) {
    error = response.text.empty() ? "registry lifecycle HTTPS request failed"
                                  : response.text;
    return std::nullopt;
  }
  auto receipt = parseRegistryLifecycleReceipt(response.text, error);
  if (!receipt || receipt->registry_name != request.registry_name ||
      receipt->package_name != request.package_name ||
      receipt->version != request.version || receipt->state != request.state ||
      receipt->reason_code != request.reason_code)
    return std::nullopt;

  std::error_code ec;
  const auto directory = std::filesystem::path(request.identity_store_path) /
                         "registry-audit" / "pending-client";
  std::filesystem::create_directories(directory, ec);
  const auto receipt_path =
      (directory / (sha256Hex(response.text) + ".receipt")).string();
  const auto temporary = receipt_path + ".tmp";
  if (ec || !writeTextFile(temporary, response.text, error) ||
      !replaceFile(temporary, receipt_path, ec)) {
    if (error.empty())
      error = "failed to persist registry lifecycle receipt";
    removeFile(temporary, ec);
    return std::nullopt;
  }
  RegistryAuditGossipRequest gossip;
  gossip.registry_name = request.registry_name;
  gossip.registry_origin = origin;
  gossip.receipt_path = receipt_path;
  gossip.bootstrap_root_keys = request.bootstrap_root_keys;
  gossip.bootstrap_root_threshold = request.bootstrap_root_threshold;
  gossip.identity_store_path = request.identity_store_path;
  gossip.registry_ca_bundle_path = request.ca_bundle_path;
  auto verified = gossipRegistryAuditReceipt(gossip, error);
  if (!verified) {
    removeFile(receipt_path, ec);
    return std::nullopt;
  }
  if (!request.receipt_output_path.empty()) {
    const auto output_temporary = request.receipt_output_path + ".tmp";
    if (!writeTextFile(output_temporary, response.text, error) ||
        !replaceFile(output_temporary, request.receipt_output_path, ec)) {
      if (error.empty())
        error = "failed to persist verified registry lifecycle receipt";
      removeFile(output_temporary, ec);
      removeFile(receipt_path, ec);
      return std::nullopt;
    }
  }
  if (request.witness_threshold != 0) {
    gossip.witness_urls = request.witness_urls;
    gossip.witness_keys = request.witness_keys;
    gossip.witness_threshold = request.witness_threshold;
    gossip.witness_ca_bundle_path = request.witness_ca_bundle_path;
    if (!gossipRegistryAuditReceipt(gossip, error)) {
      removeFile(receipt_path, ec);
      return std::nullopt;
    }
  }
  removeFile(receipt_path, ec);
  return RegistryReleaseReceipt{receipt->registry_name, receipt->package_name,
                                receipt->version, receipt->state,
                                receipt->leaf_index};
}
