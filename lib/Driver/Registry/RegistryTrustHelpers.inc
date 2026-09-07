// Included by RegistryTrust.cpp; this fragment retains the original trust
// metadata stores and verification/signing state.
bool isHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::string quote(std::string_view value) {
  std::string out = "\"";
  for (const char ch : value) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(ch); break;
    }
  }
  out.push_back('"');
  return out;
}

std::string renderArray(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << quote(values[i]);
  }
  out << ']';
  return out.str();
}

void appendField(std::ostringstream &out, std::string_view name,
                 std::string_view value) {
  out << name << '\t' << value.size() << ':' << value << '\n';
}

bool sortedUniqueNonempty(std::vector<std::string> &values) {
  std::sort(values.begin(), values.end());
  return !values.empty() &&
         std::adjacent_find(values.begin(), values.end()) == values.end() &&
         std::all_of(values.begin(), values.end(),
                     [](const auto &value) { return !value.empty(); });
}

bool parseRfc3339(std::string_view value, std::int64_t &seconds) {
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z') {
    return false;
  }
  auto number = [&](std::size_t offset, std::size_t count, int &out) {
    out = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const char ch = value[offset + i];
      if (ch < '0' || ch > '9') return false;
      out = out * 10 + (ch - '0');
    }
    return true;
  };
  int year, month, day, hour, minute, second;
  if (!number(0, 4, year) || !number(5, 2, month) ||
      !number(8, 2, day) || !number(11, 2, hour) ||
      !number(14, 2, minute) || !number(17, 2, second) || year < 1970 ||
      month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 59) {
    return false;
  }
  std::tm time{};
  time.tm_year = year - 1900;
  time.tm_mon = month - 1;
  time.tm_mday = day;
  time.tm_hour = hour;
  time.tm_min = minute;
  time.tm_sec = second;
#if defined(_WIN32)
  const auto converted = _mkgmtime64(&time);
#else
  const auto converted = timegm(&time);
#endif
  if (converted < 0) return false;
  std::tm roundtrip{};
#if defined(_WIN32)
  if (gmtime_s(&roundtrip, &converted) != 0) return false;
#else
  if (gmtime_r(&converted, &roundtrip) == nullptr) return false;
#endif
  if (roundtrip.tm_year != year - 1900 || roundtrip.tm_mon != month - 1 ||
      roundtrip.tm_mday != day || roundtrip.tm_hour != hour ||
      roundtrip.tm_min != minute || roundtrip.tm_sec != second) {
    return false;
  }
  seconds = static_cast<std::int64_t>(converted);
  return true;
}

bool validExpiry(std::string_view value) {
  std::int64_t ignored = 0;
  return parseRfc3339(value, ignored);
}

std::string rootPayload(const RegistryRootMetadata &root) {
  std::ostringstream out;
  out << (root.format == RegistryRootFormatV2
              ? "chtholly-registry-root-signature-v2\n"
              : "chtholly-registry-root-signature-v1\n");
  appendField(out, "registry", root.registry_name);
  appendField(out, "version", std::to_string(root.version));
  appendField(out, "expires", root.expires);
  const auto append_role = [&](std::string_view name,
                               const RegistryTrustRole &role) {
    appendField(out, std::string(name) + "-threshold",
                std::to_string(role.threshold));
    auto keys = role.keys;
    std::sort(keys.begin(), keys.end());
    out << name << "-key-count\t" << keys.size() << '\n';
    for (const auto &key : keys) appendField(out, name, key);
  };
  append_role("root", root.root);
  append_role("targets", root.targets);
  append_role("snapshot", root.snapshot);
  append_role("timestamp", root.timestamp);
  if (root.format == RegistryRootFormatV2) append_role("audit", root.audit);
  auto revoked = root.revoked_key_ids;
  std::sort(revoked.begin(), revoked.end());
  out << "revoked-count\t" << revoked.size() << '\n';
  for (const auto &id : revoked) appendField(out, "revoked", id);
  return out.str();
}

std::string snapshotSignatureInput(const RegistrySnapshotMetadata &snapshot) {
  std::ostringstream out;
  out << (snapshot.format == RegistrySnapshotFormatV2
              ? "chtholly-registry-snapshot-signature-v2\n"
              : "chtholly-registry-snapshot-signature-v1\n");
  appendField(out, "registry", snapshot.registry_name);
  appendField(out, "version", std::to_string(snapshot.version));
  appendField(out, "root-version", std::to_string(snapshot.root_version));
  appendField(out, "expires", snapshot.expires);
  appendField(out, "packages-sha256", snapshot.packages_sha256);
  if (snapshot.format == RegistrySnapshotFormatV2)
    appendField(out, "audit-checkpoint-sha256",
                snapshot.audit_checkpoint_sha256);
  return out.str();
}

std::string timestampSignatureInput(const RegistryTimestampMetadata &timestamp) {
  std::ostringstream out;
  out << "chtholly-registry-timestamp-signature-v1\n";
  appendField(out, "registry", timestamp.registry_name);
  appendField(out, "version", std::to_string(timestamp.version));
  appendField(out, "root-version", std::to_string(timestamp.root_version));
  appendField(out, "expires", timestamp.expires);
  appendField(out, "snapshot-version",
              std::to_string(timestamp.snapshot_version));
  appendField(out, "snapshot-sha256", timestamp.snapshot_sha256);
  return out.str();
}

bool validRole(RegistryTrustRole &role) {
  if (!sortedUniqueNonempty(role.keys) || role.threshold == 0 ||
      role.threshold > role.keys.size()) {
    return false;
  }
  return std::all_of(role.keys.begin(), role.keys.end(), [](const auto &key) {
    return registry_crypto::parsePublicKey(key).has_value();
  });
}

bool validSignatures(std::vector<RegistryMetadataSignature> &signatures) {
  std::sort(signatures.begin(), signatures.end(), [](const auto &lhs,
                                                     const auto &rhs) {
    return lhs.key_id < rhs.key_id;
  });
  return std::adjacent_find(signatures.begin(), signatures.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return lhs.key_id == rhs.key_id;
                            }) == signatures.end();
}

bool verifyThreshold(std::string_view payload,
                     const std::vector<RegistryMetadataSignature> &signatures,
                     const RegistryTrustRole &role,
                     const std::vector<std::string> &revoked,
                     std::string &error) {
  if (!registry_crypto::initialize(error)) return false;
  std::set<std::string> revoked_set(revoked.begin(), revoked.end());
  std::uint32_t accepted = 0;
  for (const auto &public_text : role.keys) {
    auto public_key = registry_crypto::parsePublicKey(public_text);
    if (!public_key) continue;
    const auto id = registry_crypto::publicKeyId(*public_key);
    if (revoked_set.contains(id)) continue;
    const auto found = std::find_if(signatures.begin(), signatures.end(),
                                    [&](const auto &signature) {
                                      return signature.key_id == id;
                                    });
    if (found == signatures.end()) continue;
    registry_crypto::Signature signature{};
    if (!registry_crypto::decodeSignature(found->signature, signature))
      continue;
    if (crypto_sign_verify_detached(
            signature.data(),
            reinterpret_cast<const unsigned char *>(payload.data()),
            static_cast<unsigned long long>(payload.size()),
            public_key->data()) == 0) {
      ++accepted;
    }
  }
  if (accepted < role.threshold) {
    error = "registry metadata signature threshold was not satisfied";
    return false;
  }
  return true;
}

bool signWithProvider(const RegistrySigningProvider &signer,
                      RegistrySigningRole signing_role,
                      const RegistryTrustRole &role,
                      const std::vector<std::string> &revoked_key_ids,
                      std::string_view registry_name,
                      std::uint64_t root_version, std::string payload,
                      std::vector<RegistryMetadataSignature> &signatures,
                      std::string &error) {
  RegistrySignerRequest request;
  request.operation = RegistrySignerOperation::Sign;
  request.registry_name = std::string(registry_name);
  request.role = signing_role;
  request.root_version = root_version;
  request.threshold = role.threshold;
  request.payload = std::move(payload);
  auto authorized = registryActiveRoleKeyIds(role, revoked_key_ids, error);
  if (!authorized)
    return false;
  request.authorized_key_ids = std::move(*authorized);
  if (request.threshold == 0 ||
      request.authorized_key_ids.size() < request.threshold) {
    error = "registry signing role has insufficient active keys";
    return false;
  }
  auto response = signer.execute(request, error);
  if (!response)
    return false;
  if (response->payload_sha256 != sha256Hex(request.payload) ||
      response->key_ids.size() < request.threshold ||
      !std::is_sorted(response->key_ids.begin(), response->key_ids.end()) ||
      std::adjacent_find(response->key_ids.begin(), response->key_ids.end()) !=
          response->key_ids.end() ||
      !std::is_sorted(response->signatures.begin(), response->signatures.end(),
                      [](const auto &lhs, const auto &rhs) {
                        return lhs.key_id < rhs.key_id;
                      }) ||
      std::adjacent_find(response->signatures.begin(),
                         response->signatures.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.key_id == rhs.key_id;
                         }) != response->signatures.end() ||
      std::any_of(response->key_ids.begin(), response->key_ids.end(),
                  [&](const auto &id) {
                    return !std::binary_search(request.authorized_key_ids.begin(),
                                               request.authorized_key_ids.end(),
                                               id);
                  }) ||
      std::any_of(response->signatures.begin(), response->signatures.end(),
                  [&](const auto &signature) {
                    return !std::binary_search(request.authorized_key_ids.begin(),
                                               request.authorized_key_ids.end(),
                                               signature.key_id);
                  })) {
    error = "registry signer returned a key outside the current role";
    return false;
  }
  signatures = std::move(response->signatures);
  return verifyThreshold(request.payload, signatures, role, revoked_key_ids,
                         error);
}

std::string renderSignatures(std::vector<RegistryMetadataSignature> values) {
  std::sort(values.begin(), values.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.key_id < rhs.key_id;
  });
  std::ostringstream out;
  out << "\n[signatures]\n";
  for (const auto &signature : values) {
    out << signature.key_id.substr(7) << " = " << quote(signature.signature)
        << '\n';
  }
  return out.str();
}

bool parseSignatureAssignment(const manifest_toml::Assignment &assignment,
                              std::vector<RegistryMetadataSignature> &out) {
  if (assignment.table != "signatures" ||
      !isHexDigest(assignment.key)) {
    return false;
  }
  RegistryMetadataSignature signature;
  signature.key_id = "sha256:" + assignment.key;
  return manifest_toml::parseString(assignment.value, signature.signature) &&
         signature.signature.starts_with("ed25519:") &&
         (out.push_back(std::move(signature)), true);
}

template <typename Metadata>
bool parseCommon(const std::vector<manifest_toml::Assignment> &assignments,
                 std::string_view expected_format, Metadata &metadata,
                 std::set<std::string> &seen, std::string &error) {
  for (const auto &assignment : assignments) {
    if (assignment.table == "signatures") {
      if (!parseSignatureAssignment(assignment, metadata.signatures)) {
        error = "invalid registry metadata signature";
        return false;
      }
      continue;
    }
    if (!assignment.table.empty()) continue;
    if (!seen.insert(assignment.key).second) {
      error = "duplicate registry metadata field '" + assignment.key + "'";
      return false;
    }
    if (assignment.key == "format") {
      std::string format;
      if (!manifest_toml::parseString(assignment.value, format) ||
          format != expected_format) {
        error = "invalid registry metadata format";
        return false;
      }
    } else if (assignment.key == "registry") {
      if (!manifest_toml::parseString(assignment.value,
                                      metadata.registry_name) ||
          metadata.registry_name.empty()) {
        error = "invalid registry metadata registry identity";
        return false;
      }
    } else if (assignment.key == "version") {
      if (!manifest_toml::parseUnsigned(assignment.value, metadata.version) ||
          metadata.version == 0) {
        error = "invalid registry metadata version";
        return false;
      }
    } else if (assignment.key == "expires") {
      if (!manifest_toml::parseString(assignment.value, metadata.expires) ||
          !validExpiry(metadata.expires)) {
        error = "invalid registry metadata expiry";
        return false;
      }
    }
  }
  return true;
}

struct TrustState {
  std::uint64_t root_version = 0;
  std::string root_sha256;
  std::uint64_t snapshot_version = 0;
  std::string snapshot_sha256;
  std::uint64_t timestamp_version = 0;
  std::string timestamp_sha256;
  std::int64_t last_verified_time = 0;
};

class TrustStateLock {
public:
  TrustStateLock(const std::filesystem::path &path, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      error = "failed to create registry trust state directory: " + ec.message();
      return;
    }
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    OVERLAPPED overlapped{};
    if (handle_ == INVALID_HANDLE_VALUE ||
        !LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlapped)) {
      error = "failed to lock registry trust state";
      if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
#else
    descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
      error = "failed to lock registry trust state";
      if (descriptor_ >= 0) ::close(descriptor_);
      descriptor_ = -1;
      return;
    }
#endif
    locked_ = true;
  }
  ~TrustStateLock() {
    if (!locked_) return;
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle_);
#else
    flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
#endif
  }
  bool locked() const { return locked_; }
private:
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

std::optional<TrustState> loadState(const std::filesystem::path &path,
                                    std::string &error) {
  TrustState state;
  if (!std::filesystem::exists(path)) return state;
  auto text = readTextFile(path.string(), error);
  if (!text) return std::nullopt;
  std::istringstream input(*text);
  std::string header;
  if (!std::getline(input, header) || header != "chtholly-registry-trust-state-v1") {
    error = "invalid registry trust state: " + path.string();
    return std::nullopt;
  }
  std::map<std::string, std::string> fields;
  std::string line;
  bool saw_end = false;
  while (std::getline(input, line)) {
    if (line == "end") {
      saw_end = true;
      break;
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos ||
        !fields.emplace(line.substr(0, tab), line.substr(tab + 1)).second) {
      error = "invalid registry trust state: " + path.string();
      return std::nullopt;
    }
  }
  std::string trailing;
  if (!saw_end || fields.size() != 7 || std::getline(input, trailing)) {
    error = "invalid registry trust state: " + path.string();
    return std::nullopt;
  }
  auto unsigned_field = [&](std::string_view name, std::uint64_t &value) {
    const auto found = fields.find(std::string(name));
    return found != fields.end() &&
           manifest_toml::parseUnsigned(found->second, value);
  };
  std::uint64_t last_time = 0;
  if (!unsigned_field("root-version", state.root_version) ||
      !unsigned_field("snapshot-version", state.snapshot_version) ||
      !unsigned_field("timestamp-version", state.timestamp_version) ||
      !unsigned_field("last-verified-time", last_time) ||
      last_time > static_cast<std::uint64_t>(
                      (std::numeric_limits<std::int64_t>::max)()) ||
      !isHexDigest(fields["root-sha256"]) ||
      !isHexDigest(fields["snapshot-sha256"]) ||
      !isHexDigest(fields["timestamp-sha256"])) {
    error = "invalid registry trust state: " + path.string();
    return std::nullopt;
  }
  state.root_sha256 = fields["root-sha256"];
  state.snapshot_sha256 = fields["snapshot-sha256"];
  state.timestamp_sha256 = fields["timestamp-sha256"];
  state.last_verified_time = static_cast<std::int64_t>(last_time);
  return state;
}

bool writeState(const std::filesystem::path &path, const TrustState &state,
                std::string &error) {
  std::ostringstream out;
  out << "chtholly-registry-trust-state-v1\n"
      << "root-version\t" << state.root_version << '\n'
      << "root-sha256\t" << state.root_sha256 << '\n'
      << "snapshot-version\t" << state.snapshot_version << '\n'
      << "snapshot-sha256\t" << state.snapshot_sha256 << '\n'
      << "timestamp-version\t" << state.timestamp_version << '\n'
      << "timestamp-sha256\t" << state.timestamp_sha256 << '\n'
      << "last-verified-time\t" << state.last_verified_time << "\nend\n";
  const auto temporary = path.string() + ".tmp";
  if (!writeTextFile(temporary, out.str(), error)) return false;
  std::error_code ec;
  if (!replaceFile(temporary, path.string(), ec)) {
    error = "failed to publish registry trust state: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}

bool checkRollback(std::string_view name, std::uint64_t version,
                   std::string_view digest, std::uint64_t previous_version,
                   std::string_view previous_digest, std::string &error) {
  if (version < previous_version ||
      (version == previous_version && previous_version != 0 &&
       digest != previous_digest)) {
    error = "registry " + std::string(name) +
            " rollback or same-version equivocation detected";
    return false;
  }
  return true;
}

template <typename Metadata>
bool addSignature(Metadata &metadata, std::string_view payload,
                  const std::string &secret_path, std::string &error) {
  if (!registry_crypto::initialize(error)) return false;
  registry_crypto::SecretKey secret{};
  registry_crypto::PublicKey public_key{};
  if (!registry_crypto::loadSecretKeyFile(secret_path, secret, public_key,
                                          error)) {
    return false;
  }
  const auto id = registry_crypto::publicKeyId(public_key);
  registry_crypto::Signature signature{};
  crypto_sign_detached(
      signature.data(), nullptr,
      reinterpret_cast<const unsigned char *>(payload.data()),
      static_cast<unsigned long long>(payload.size()), secret.data());
  sodium_memzero(secret.data(), secret.size());
  auto found = std::find_if(metadata.signatures.begin(),
                            metadata.signatures.end(), [&](const auto &item) {
                              return item.key_id == id;
                            });
  RegistryMetadataSignature value{
      id, registry_crypto::encodeSignature(signature)};
  if (found == metadata.signatures.end()) {
    metadata.signatures.push_back(std::move(value));
  } else {
    *found = std::move(value);
  }
  return true;
}
