#pragma once

// Private registry-artifact parsing, transport, and canonical-render helpers.
// Included inside RegistryArtifact.cpp's anonymous namespace so no public
// symbols or alternate registry state are introduced.

bool initializeCurl(std::string &error) {
  static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (result != CURLE_OK) {
    error = "failed to initialize libcurl: " +
            std::string(curl_easy_strerror(result));
    return false;
  }
  return true;
}

bool isHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool parseAbi(std::string_view value, AbiVersion &abi) {
  if (value == "v0") {
    abi = AbiVersion::V0;
    return true;
  }
  if (value == "v1") {
    abi = AbiVersion::V1;
    return true;
  }
  if (value == "v2") {
    abi = AbiVersion::V2;
    return true;
  }
  return false;
}

std::string quote(std::string_view value) {
  std::string result = "\"";
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result.push_back(ch);
      break;
    }
  }
  result += '"';
  return result;
}

std::string renderStringArray(const std::vector<std::string> &values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << quote(values[i]);
  }
  out << ']';
  return out.str();
}

void appendField(std::ostringstream &out, std::string_view name,
                 std::string_view value) {
  out << name << '\t' << value.size() << ':' << value << '\n';
}

bool sortedUnique(const std::vector<std::string> &values) {
  return std::adjacent_find(values.begin(), values.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return lhs >= rhs;
                            }) == values.end();
}

std::filesystem::path normalizedAbsolute(const std::filesystem::path &path) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec)
    absolute = path;
  const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
  return (ec ? absolute : canonical).lexically_normal();
}

bool pathWithin(const std::filesystem::path &path,
                const std::filesystem::path &root) {
  const auto candidate = normalizedAbsolute(path);
  const auto boundary = normalizedAbsolute(root);
  auto candidate_it = candidate.begin();
  for (auto root_it = boundary.begin(); root_it != boundary.end();
       ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *root_it) {
      return false;
    }
  }
  return true;
}

bool verifyCachedArchive(const std::filesystem::path &path,
                         const RegistryArtifactVariant &variant) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec &&
         std::filesystem::file_size(path, ec) == variant.archive_size && !ec &&
         sha256File(path.string()) ==
             std::optional<std::string>(variant.archive_sha256);
}

struct CurlDownloadState {
  std::ofstream *output = nullptr;
  std::uint64_t expected_size = 0;
  std::uint64_t received_size = 0;
  bool exceeded_size = false;
};

std::size_t writeCurlData(char *data, std::size_t item_size,
                          std::size_t item_count, void *context) {
  auto &state = *static_cast<CurlDownloadState *>(context);
  if (item_size != 0 &&
      item_count > (std::numeric_limits<std::size_t>::max)() / item_size) {
    state.exceeded_size = true;
    return 0;
  }
  const auto bytes = item_size * item_count;
  if (bytes > state.expected_size - state.received_size) {
    state.exceeded_size = true;
    return 0;
  }
  state.output->write(data, static_cast<std::streamsize>(bytes));
  if (!*state.output)
    return 0;
  state.received_size += bytes;
  return bytes;
}

bool hasUrlCredentials(std::string_view url) {
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos)
    return false;
  const auto authority_start = scheme + 3;
  const auto authority_end = url.find('/', authority_start);
  return url.substr(authority_start, authority_end - authority_start)
             .find('@') != std::string_view::npos;
}

std::filesystem::path archiveCachePath(const std::filesystem::path &cache_root,
                                       const RegistryArtifactVariant &variant) {
  return cache_root / "artifacts" / "sha256" /
         variant.archive_sha256.substr(0, 2) /
         (variant.archive_sha256 + ".cpa");
}

class TemporaryFileCleanup {
public:
  explicit TemporaryFileCleanup(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~TemporaryFileCleanup() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
  void release() { path_.clear(); }

private:
  std::filesystem::path path_;
};

std::string uniqueTemporarySuffix() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(timestamp) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool validateOwnerOnlyTokenFile(const std::string &path, std::string &error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(pathForFileSystem(path), ec) || ec) {
    error = "registry token file must be a regular file";
    return false;
  }
#if defined(_WIN32)
  const auto attributes = GetFileAttributesW(pathForFileSystem(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    error = "registry token file cannot be a reparse point";
    return false;
  }
  auto native_path = pathForFileSystem(path).wstring();
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PSID owner = nullptr;
  PACL dacl = nullptr;
  const auto result = GetNamedSecurityInfoW(
      native_path.data(), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &descriptor);
  if (result != ERROR_SUCCESS || owner == nullptr || dacl == nullptr) {
    if (descriptor != nullptr)
      LocalFree(descriptor);
    error = "registry token file must have an owner-only DACL";
    return false;
  }
  bool owner_only = true;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void *raw_ace = nullptr;
    if (!GetAce(dacl, index, &raw_ace)) {
      owner_only = false;
      break;
    }
    const auto *header = static_cast<ACE_HEADER *>(raw_ace);
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
      const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
      const auto *sid = reinterpret_cast<const SID *>(&ace->SidStart);
      if (!EqualSid(owner, const_cast<SID *>(sid))) {
        owner_only = false;
        break;
      }
    } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
               header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
               header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
      owner_only = false;
      break;
    }
  }
  LocalFree(descriptor);
  if (!owner_only) {
    error =
        "registry token file grants access to an identity other than its owner";
    return false;
  }
#else
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
      (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    error = "registry token file permissions must be owner-only";
    return false;
  }
#endif
  return true;
}

std::optional<std::string>
loadPublishToken(const RegistryPublishRequest &request, std::string &error) {
  if (request.token_environment.empty() == request.token_file_path.empty()) {
    error = "registry publish requires exactly one token source";
    return std::nullopt;
  }
  std::string token;
  if (!request.token_environment.empty()) {
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, request.token_environment.c_str()) ==
            0 &&
        value != nullptr) {
      token.assign(value, value_size == 0 ? 0 : value_size - 1);
    }
    std::free(value);
#else
    const char *value = std::getenv(request.token_environment.c_str());
    if (value != nullptr)
      token = value;
#endif
  } else {
    if (!validateOwnerOnlyTokenFile(request.token_file_path, error)) {
      return std::nullopt;
    }
    auto text = readTextFile(request.token_file_path, error);
    if (!text)
      return std::nullopt;
    token = std::move(*text);
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n')) {
      token.pop_back();
    }
  }
  if (token.empty() || token.size() > 8192 ||
      token.find_first_of("\r\n") != std::string::npos) {
    error = "registry publish token is empty or contains a line break";
    return std::nullopt;
  }
  return token;
}

struct CurlTextState {
  std::string text;
  bool exceeded_limit = false;
  std::size_t limit = 64 * 1024;
};

std::size_t writeCurlText(char *data, std::size_t item_size,
                          std::size_t item_count, void *context) {
  auto &state = *static_cast<CurlTextState *>(context);
  if (item_size != 0 &&
      item_count > (std::numeric_limits<std::size_t>::max)() / item_size) {
    state.exceeded_limit = true;
    return 0;
  }
  const auto size = item_size * item_count;
  if (state.text.size() > state.limit ||
      size > state.limit - state.text.size()) {
    state.exceeded_limit = true;
    return 0;
  }
  state.text.append(data, size);
  return size;
}

std::optional<std::pair<long, std::string>>
registryHttpsGet(const std::string &url, const std::string &ca_bundle,
                 std::size_t response_limit, std::string &error) {
  if (!initializeCurl(error))
    return std::nullopt;
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    error = "failed to initialize registry audit HTTPS request";
    return std::nullopt;
  }
  CurlTextState response;
  response.limit = response_limit;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlText);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "chthollyc-registry-audit/1");
  if (!ca_bundle.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || response.exceeded_limit) {
    error = response.exceeded_limit
                ? "registry audit response exceeded its size limit"
                : "registry audit HTTPS request failed: " +
                      std::string(curl_easy_strerror(result));
    return std::nullopt;
  }
  return std::pair{status, std::move(response.text)};
}

std::optional<std::pair<long, std::string>>
registryHttpsPost(const std::string &url, const std::string &ca_bundle,
                  std::string_view body, std::size_t response_limit,
                  std::string &error) {
  if (!initializeCurl(error))
    return std::nullopt;
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    error = "failed to initialize registry witness HTTPS request";
    return std::nullopt;
  }
  CurlTextState response;
  response.limit = response_limit;
  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: text/plain");
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
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlText);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "chthollyc-registry-witness/1");
  if (!ca_bundle.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || response.exceeded_limit) {
    error = response.exceeded_limit
                ? "registry witness response exceeded its size limit"
                : "registry witness HTTPS request failed: " +
                      std::string(curl_easy_strerror(result));
    return std::nullopt;
  }
  return std::pair{status, std::move(response.text)};
}

std::optional<std::size_t> submitWitnessObservation(
    const RegistryWitnessObservation &observation,
    const std::vector<std::string> &urls, const std::vector<std::string> &keys,
    std::uint32_t threshold, const std::string &ca_bundle,
    const std::filesystem::path &state, std::string &error) {
  if (urls.empty() || keys.empty() || threshold == 0 ||
      threshold > urls.size() || threshold > keys.size()) {
    error = "registry witness policy is incomplete";
    return std::nullopt;
  }
  const auto body = renderRegistryWitnessObservation(observation);
  std::map<std::string, std::string> accepted;
  std::string first_failure;
  std::error_code ec;
  const auto statements = state / "statements";
  std::filesystem::create_directories(statements, ec);
  if (ec) {
    error = "failed to create registry witness state: " + ec.message();
    return std::nullopt;
  }
  const auto checkpoint = renderRegistryAuditCheckpoint(observation.checkpoint);
  const auto statement_path = [&](std::string_view key_id) {
    return statements /
           (sha256Hex(checkpoint + "\n" + std::string(key_id)) + ".txt");
  };
  for (const auto &key : keys) {
    const auto public_key = registry_crypto::parsePublicKey(key);
    if (!public_key) {
      error = "registry witness policy contains an invalid public key";
      return std::nullopt;
    }
    const auto key_id = registry_crypto::publicKeyId(*public_key);
    const auto path = statement_path(key_id);
    if (!std::filesystem::exists(path, ec)) {
      if (ec) {
        error = "failed to inspect persisted registry witness statement: " +
                ec.message();
        return std::nullopt;
      }
      continue;
    }
    if (std::filesystem::file_size(path, ec) > 128u * 1024u || ec) {
      if (first_failure.empty())
        first_failure = "persisted registry witness statement is invalid";
      ec.clear();
      continue;
    }
    std::string statement_error;
    auto text = readTextFile(path.string(), statement_error);
    auto parsed = text ? parseRegistryWitnessStatement(*text, statement_error)
                       : std::optional<RegistryWitnessStatement>{};
    if (parsed && parsed->signature.key_id == key_id &&
        verifyRegistryWitnessStatement(*parsed, observation, keys,
                                       statement_error)) {
      accepted.emplace(key_id, std::move(*text));
    } else if (first_failure.empty()) {
      first_failure = statement_error.empty()
                          ? "persisted registry witness statement is invalid"
                          : std::move(statement_error);
    }
  }
  if (accepted.size() >= threshold)
    return accepted.size();
  struct WitnessReply {
    std::optional<std::pair<long, std::string>> response;
    std::string error;
  };
  std::vector<std::future<WitnessReply>> pending;
  pending.reserve(urls.size());
  for (const auto &base : urls) {
    auto url = base;
    while (url.ends_with('/'))
      url.pop_back();
    try {
      pending.push_back(std::async(std::launch::async, [url = std::move(url),
                                                        &ca_bundle,
                                                        &body]() mutable {
        WitnessReply reply;
        reply.response = registryHttpsPost(url + "/v1/observations", ca_bundle,
                                           body, 128u * 1024u, reply.error);
        return reply;
      }));
    } catch (const std::exception &exception) {
      if (first_failure.empty())
        first_failure = "failed to start registry witness request: " +
                        std::string(exception.what());
    }
  }
  for (auto &request : pending) {
    WitnessReply reply;
    try {
      reply = request.get();
    } catch (const std::exception &exception) {
      if (first_failure.empty())
        first_failure =
            "registry witness request failed: " + std::string(exception.what());
      continue;
    }
    auto &response = reply.response;
    auto request_error = std::move(reply.error);
    if (response && response->first != 200 && request_error.empty())
      request_error = response->second;
    auto statement =
        response && response->first == 200
            ? parseRegistryWitnessStatement(response->second, request_error)
            : std::optional<RegistryWitnessStatement>{};
    if (statement && verifyRegistryWitnessStatement(*statement, observation,
                                                    keys, request_error)) {
      accepted.emplace(statement->signature.key_id, response->second);
    } else {
      if (first_failure.empty())
        first_failure = request_error.empty()
                            ? "witness rejected the observation"
                            : std::move(request_error);
    }
  }
  for (const auto &[key, statement] : accepted) {
    const auto path = statement_path(key);
    const auto temporary = path.string() + ".tmp";
    if (!writeTextFile(temporary, statement, error) ||
        !replaceFile(temporary, path.string(), ec)) {
      if (error.empty())
        error = "failed to persist registry witness statement: " + ec.message();
      removeFile(temporary, ec);
      return std::nullopt;
    }
  }
  if (accepted.size() < threshold) {
    error = "registry publication was accepted, but witness gossip reached " +
            std::to_string(accepted.size()) + "/" + std::to_string(threshold) +
            " required independent signatures" +
            (first_failure.empty() ? std::string{} : ": " + first_failure);
    return std::nullopt;
  }
  return accepted.size();
}

std::optional<std::vector<std::string>>
parseRegistryProof(std::string_view text, std::string_view expected_format,
                   std::uint64_t expected_first, std::uint64_t expected_second,
                   std::string &error) {
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != expected_format) {
    error = "registry audit proof has an invalid format";
    return std::nullopt;
  }
  bool saw_first = false;
  bool saw_second = false;
  std::vector<std::string> hashes;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto split = line.find('\t');
    if (split == std::string::npos || split == 0 || split + 1 == line.size()) {
      error = "registry audit proof is malformed";
      return std::nullopt;
    }
    const auto name = std::string_view(line).substr(0, split);
    const auto value = std::string_view(line).substr(split + 1);
    if (name == "first") {
      saw_first = !saw_first && value == std::to_string(expected_first);
      if (!saw_first) {
        error = "registry audit proof first index does not match the request";
        return std::nullopt;
      }
    } else if (name == "second") {
      saw_second = !saw_second && value == std::to_string(expected_second);
      if (!saw_second) {
        error = "registry audit proof second index does not match the request";
        return std::nullopt;
      }
    } else if (name == "hash" && isHexDigest(value)) {
      hashes.emplace_back(value);
    } else {
      error = "registry audit proof contains an invalid field";
      return std::nullopt;
    }
  }
  if (!saw_first || !saw_second) {
    error = "registry audit proof is incomplete";
    return std::nullopt;
  }
  return hashes;
}

bool parseVariant(std::string name, std::string_view value,
                  std::string_view format, RegistryArtifactVariant &variant,
                  std::string &error) {
  std::vector<std::pair<std::string, std::string>> fields;
  if (!manifest_toml::parseInlineTable(value, fields)) {
    error = "registry artifact variant '" + name + "' expects an inline table";
    return false;
  }
  std::map<std::string, std::string> unique;
  for (auto &[field, raw] : fields) {
    if (!unique.emplace(std::move(field), std::move(raw)).second) {
      error =
          "registry artifact variant '" + name + "' contains a duplicate field";
      return false;
    }
  }
  std::set<std::string> expected{
      "target",         "pointer_width",     "abi",           "runtime_abi",
      "features",       "default_features",  "url",           "size",
      "archive_sha256", "artifact_identity", "closure_digest"};
  if (format == RegistryArtifactEntryFormatV2) {
    expected.insert("key_id");
    expected.insert("signature");
  } else {
    expected.insert("signatures");
  }
  for (const auto &[field, raw] : unique) {
    (void)raw;
    if (!expected.contains(field)) {
      error = "unknown registry artifact variant field '" + field + "'";
      return false;
    }
  }
  if (unique.size() != expected.size()) {
    error =
        "registry artifact variant '" + name + "' is missing required fields";
    return false;
  }
  std::string abi;
  std::uint64_t pointer_width = 0;
  if (!manifest_toml::parseString(unique["target"], variant.target.triple) ||
      variant.target.triple.empty() ||
      !manifest_toml::parseUnsigned(unique["pointer_width"], pointer_width) ||
      pointer_width == 0 || pointer_width > 65535 ||
      !manifest_toml::parseString(unique["abi"], abi) ||
      !parseAbi(abi, variant.abi_version) ||
      !manifest_toml::parseString(unique["runtime_abi"], variant.runtime_abi) ||
      variant.runtime_abi.empty() ||
      !manifest_toml::parseStringArray(unique["features"],
                                       variant.requested_features) ||
      !sortedUnique(variant.requested_features) ||
      !manifest_toml::parseBool(unique["default_features"],
                                variant.default_features) ||
      !manifest_toml::parseString(unique["url"], variant.url) ||
      variant.url.empty() ||
      !manifest_toml::parseUnsigned(unique["size"], variant.archive_size) ||
      variant.archive_size == 0 ||
      !manifest_toml::parseString(unique["archive_sha256"],
                                  variant.archive_sha256) ||
      !isHexDigest(variant.archive_sha256) ||
      !manifest_toml::parseString(unique["artifact_identity"],
                                  variant.artifact_identity) ||
      !isHexDigest(variant.artifact_identity) ||
      !manifest_toml::parseString(unique["closure_digest"],
                                  variant.closure_digest) ||
      !isHexDigest(variant.closure_digest)) {
    error = "registry artifact variant '" + name + "' contains invalid fields";
    return false;
  }
  if (format == RegistryArtifactEntryFormatV2) {
    RegistryArtifactSignature signature;
    if (!manifest_toml::parseString(unique["key_id"], signature.key_id) ||
        !signature.key_id.starts_with("sha256:") ||
        !isHexDigest(std::string_view(signature.key_id).substr(7)) ||
        !manifest_toml::parseString(unique["signature"], signature.signature) ||
        !signature.signature.starts_with("ed25519:")) {
      error = "registry artifact variant '" + name +
              "' contains invalid signature fields";
      return false;
    }
    variant.signatures.push_back(std::move(signature));
  } else {
    std::vector<std::pair<std::string, std::string>> signatures;
    if (!manifest_toml::parseInlineTable(unique["signatures"], signatures) ||
        signatures.empty()) {
      error = "registry artifact variant '" + name +
              "' contains an invalid signature map";
      return false;
    }
    for (auto &[key, raw] : signatures) {
      RegistryArtifactSignature signature;
      signature.key_id = "sha256:" + key;
      if (!isHexDigest(key) ||
          !manifest_toml::parseString(raw, signature.signature) ||
          !signature.signature.starts_with("ed25519:")) {
        error = "registry artifact variant '" + name +
                "' contains an invalid signature map entry";
        return false;
      }
      variant.signatures.push_back(std::move(signature));
    }
    std::sort(variant.signatures.begin(), variant.signatures.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.key_id < rhs.key_id;
              });
    if (std::adjacent_find(variant.signatures.begin(), variant.signatures.end(),
                           [](const auto &lhs, const auto &rhs) {
                             return lhs.key_id == rhs.key_id;
                           }) != variant.signatures.end()) {
      error = "registry artifact variant '" + name +
              "' contains a duplicate signature key";
      return false;
    }
  }
  variant.name = std::move(name);
  variant.target.pointer_width_bits = static_cast<std::uint16_t>(pointer_width);
  if (format == RegistryArtifactEntryFormat &&
      variant.name != registryArtifactVariantName(variant)) {
    error = "registry artifact variant name does not match its artifact facts";
    return false;
  }
  return true;
}

void appendRegistryArtifactFacts(std::ostringstream &out,
                                 const RegistryArtifactVariant &variant) {
  appendField(out, "target", variant.target.triple);
  appendField(out, "pointer-width",
              std::to_string(variant.target.pointer_width_bits));
  appendField(out, "abi", abiVersionSpelling(variant.abi_version));
  appendField(out, "runtime-abi", variant.runtime_abi);
  out << "feature-count\t" << variant.requested_features.size() << '\n';
  for (const auto &feature : variant.requested_features) {
    appendField(out, "feature", feature);
  }
  appendField(out, "default-features",
              variant.default_features ? "true" : "false");
  appendField(out, "url", variant.url);
  appendField(out, "size", std::to_string(variant.archive_size));
  appendField(out, "archive-sha256", variant.archive_sha256);
  appendField(out, "artifact-identity", variant.artifact_identity);
  appendField(out, "closure-digest", variant.closure_digest);
}

std::string renderVariant(const RegistryArtifactVariant &variant) {
  auto signatures = variant.signatures;
  std::sort(
      signatures.begin(), signatures.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.key_id < rhs.key_id; });
  std::ostringstream out;
  out << registryArtifactVariantName(variant)
      << " = { target = " << quote(variant.target.triple)
      << ", pointer_width = " << variant.target.pointer_width_bits
      << ", abi = " << quote(abiVersionSpelling(variant.abi_version))
      << ", runtime_abi = " << quote(variant.runtime_abi)
      << ", features = " << renderStringArray(variant.requested_features)
      << ", default_features = "
      << (variant.default_features ? "true" : "false")
      << ", url = " << quote(variant.url) << ", size = " << variant.archive_size
      << ", archive_sha256 = " << quote(variant.archive_sha256)
      << ", artifact_identity = " << quote(variant.artifact_identity)
      << ", closure_digest = " << quote(variant.closure_digest)
      << ", signatures = { ";
  for (std::size_t i = 0; i < signatures.size(); ++i) {
    if (i != 0)
      out << ", ";
    out << std::string_view(signatures[i].key_id).substr(7) << " = "
        << quote(signatures[i].signature);
  }
  out << " } }\n";
  return out.str();
}

