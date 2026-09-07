// Included by RegistryArtifact.cpp; this fragment retains registry artifact
// canonicalization, publication, lifecycle, gossip, or fetch state.
std::optional<std::string>
fetchRegistryArtifactArchive(const RegistryArtifactVariant &variant,
                             const std::string &registry_index_checkout,
                             const std::string &registry_cache_root,
                             const std::string &ca_bundle_path, bool offline,
                             std::string &error) {
  if (!isHexDigest(variant.archive_sha256) || variant.archive_size == 0) {
    error = "registry artifact has invalid cache identity facts";
    return std::nullopt;
  }
  const auto cache_root = normalizedAbsolute(registry_cache_root);
  const auto cache_path = archiveCachePath(cache_root, variant);
  if (!pathWithin(cache_path, cache_root)) {
    error = "registry artifact cache path escapes its cache root";
    return std::nullopt;
  }
  if (verifyCachedArchive(cache_path, variant)) {
    return cache_path.string();
  }
  std::error_code ec;
  if (std::filesystem::exists(cache_path, ec)) {
    std::filesystem::remove(cache_path, ec);
  }
  if (offline) {
    error = "offline registry artifact requires a valid cached archive: " +
            variant.archive_sha256;
    return std::nullopt;
  }

  std::filesystem::create_directories(cache_path.parent_path(), ec);
  if (ec) {
    error =
        "failed to create registry artifact cache directory: " + ec.message();
    return std::nullopt;
  }
  const auto temporary =
      cache_path.string() + ".tmp-" + uniqueTemporarySuffix();
  TemporaryFileCleanup cleanup(temporary);

  const auto scheme = variant.url.find("://");
  if (scheme == std::string::npos) {
    auto source = std::filesystem::path(registry_index_checkout) /
                  std::filesystem::path(variant.url);
    source = normalizedAbsolute(source);
    const auto checkout = normalizedAbsolute(registry_index_checkout);
    if (!pathWithin(source, checkout)) {
      error = "registry artifact relative URL escapes the registry checkout";
      return std::nullopt;
    }
    if (!verifyCachedArchive(source, variant)) {
      error = "registry artifact local archive does not match its signed size "
              "and SHA-256";
      return std::nullopt;
    }
    std::filesystem::copy_file(
        source, temporary, std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
      error = "failed to cache registry artifact archive: " + ec.message();
      return std::nullopt;
    }
  } else {
    if (!variant.url.starts_with("https://") ||
        hasUrlCredentials(variant.url)) {
      error = "registry artifact remote URL must be HTTPS and must not contain "
              "credentials";
      return std::nullopt;
    }
    if (!initializeCurl(error)) {
      return std::nullopt;
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "failed to create registry artifact cache temporary file";
      return std::nullopt;
    }
    CurlDownloadState state{&output, variant.archive_size, 0, false};
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
      error = "failed to initialize registry artifact HTTPS request";
      return std::nullopt;
    }
    curl_easy_setopt(curl, CURLOPT_URL, variant.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "chthollyc-registry-artifact/1");
    if (!ca_bundle_path.empty())
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle_path.c_str());
    const auto result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    output.close();
    if (result != CURLE_OK || state.exceeded_size ||
        state.received_size != variant.archive_size) {
      error = state.exceeded_size
                  ? "registry artifact download exceeded its signed size"
                  : "registry artifact HTTPS download failed: " +
                        std::string(curl_easy_strerror(result));
      return std::nullopt;
    }
  }

  if (!verifyCachedArchive(temporary, variant)) {
    error = "downloaded registry artifact does not match its signed size and "
            "SHA-256";
    return std::nullopt;
  }
  if (!replaceFile(temporary, cache_path.string(), ec)) {
    if (verifyCachedArchive(cache_path, variant)) {
      return cache_path.string();
    }
    error = "failed to publish registry artifact cache entry: " + ec.message();
    return std::nullopt;
  }
  cleanup.release();
  return cache_path.string();
}
