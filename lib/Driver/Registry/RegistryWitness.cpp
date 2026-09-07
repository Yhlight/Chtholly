#include "chtholly/Driver/RegistryWitness.h"

#include "ManifestToml.h"
#include "RegistryCrypto.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace chtholly {
namespace {

constexpr std::size_t kMaximumObservationBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaximumRootCount = 4096;

bool lowerHex(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

template <typename Integer>
bool parseUnsigned(std::string_view text, Integer &value) {
  const auto [end, status] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return status == std::errc{} && end == text.data() + text.size();
}

std::string base64Url(std::string_view value) {
  std::string result(
      sodium_base64_encoded_len(value.size(),
                                sodium_base64_VARIANT_URLSAFE_NO_PADDING),
      '\0');
  sodium_bin2base64(result.data(), result.size(),
                    reinterpret_cast<const unsigned char *>(value.data()),
                    value.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  result.resize(result.find('\0'));
  return result;
}

std::optional<std::string> decodeBase64Url(std::string_view value) {
  std::string result(value.size(), '\0');
  std::size_t size = 0;
  if (sodium_base642bin(reinterpret_cast<unsigned char *>(result.data()),
                        result.size(), value.data(), value.size(), nullptr,
                        &size, nullptr,
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
    return std::nullopt;
  result.resize(size);
  return result;
}

std::string timestamp(std::int64_t unix_seconds) {
  if (unix_seconds < 0)
    unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto value = static_cast<std::time_t>(unix_seconds);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &value) != 0)
#else
  if (gmtime_r(&value, &utc) == nullptr)
#endif
    return {};
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

void appendCanonical(std::ostringstream &out, std::string_view name,
                     std::string_view value) {
  out << name << '\t' << value.size() << ':' << value << '\n';
}

std::string sqliteMessage(sqlite3 *database, std::string_view context) {
  return std::string(context) + ": " + sqlite3_errmsg(database);
}

bool execute(sqlite3 *database, const char *sql, std::string &error) {
  char *message = nullptr;
  const auto status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (status == SQLITE_OK)
    return true;
  error = std::string(message == nullptr ? sqlite3_errmsg(database) : message);
  sqlite3_free(message);
  return false;
}

class Statement {
public:
  Statement(sqlite3 *database, const char *sql, std::string &error)
      : database_(database) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK)
      error =
          sqliteMessage(database, "failed to prepare witness database query");
  }
  ~Statement() { sqlite3_finalize(statement_); }
  explicit operator bool() const { return statement_ != nullptr; }
  bool bind(int index, std::string_view value, std::string &error) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) == SQLITE_OK)
      return true;
    error = sqliteMessage(database_, "failed to bind witness database text");
    return false;
  }
  bool bind(int index, std::uint64_t value, std::string &error) {
    if (value <= static_cast<std::uint64_t>(
                     (std::numeric_limits<sqlite3_int64>::max)()) &&
        sqlite3_bind_int64(statement_, index,
                           static_cast<sqlite3_int64>(value)) == SQLITE_OK)
      return true;
    error = "witness database integer is out of range";
    return false;
  }
  int step() { return sqlite3_step(statement_); }
  std::string text(int column) const {
    const auto *value = sqlite3_column_text(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    return value == nullptr
               ? std::string{}
               : std::string(reinterpret_cast<const char *>(value), size);
  }
  std::uint64_t integer(int column) const {
    return static_cast<std::uint64_t>(sqlite3_column_int64(statement_, column));
  }

private:
  sqlite3 *database_ = nullptr;
  sqlite3_stmt *statement_ = nullptr;
};

std::optional<std::string> witnessKeyId(std::string_view text) {
  auto key = registry_crypto::parsePublicKey(text);
  return key ? std::optional(registry_crypto::publicKeyId(*key)) : std::nullopt;
}

bool validHttpsOrigin(std::string_view value) {
  if (!value.starts_with("https://") ||
      value.find_first_of("\t\r\n ") != std::string_view::npos ||
      value.find('@') != std::string_view::npos || value.ends_with('/'))
    return false;
  const auto authority = value.substr(8);
  return !authority.empty() && authority.find('/') == std::string_view::npos;
}

} // namespace

std::string renderRegistryWitnessObservation(
    const RegistryWitnessObservation &observation) {
  std::ostringstream out;
  out << RegistryWitnessObservationFormat << '\n'
      << "origin\t" << base64Url(observation.registry_origin) << '\n'
      << "checkpoint\t"
      << base64Url(renderRegistryAuditCheckpoint(observation.checkpoint))
      << '\n'
      << "root-count\t" << observation.root_chain.size() << '\n';
  for (const auto &root : observation.root_chain)
    out << "root\t" << base64Url(root) << '\n';
  return out.str();
}

std::optional<RegistryWitnessObservation>
parseRegistryWitnessObservation(std::string_view text, std::string &error) {
  if (text.size() > kMaximumObservationBytes) {
    error = "registry witness observation exceeds its size limit";
    return std::nullopt;
  }
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != RegistryWitnessObservationFormat) {
    error = "registry witness observation has an invalid format";
    return std::nullopt;
  }
  std::map<std::string, std::string> fields;
  std::vector<std::string> roots;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) {
      error = "registry witness observation contains a malformed field";
      return std::nullopt;
    }
    auto name = line.substr(0, tab);
    auto value = line.substr(tab + 1);
    if (name == "root")
      roots.push_back(std::move(value));
    else if (!fields.emplace(std::move(name), std::move(value)).second) {
      error = "registry witness observation contains a duplicate field";
      return std::nullopt;
    }
  }
  std::size_t root_count = 0;
  if (fields.size() != 3 || !fields.contains("origin") ||
      !fields.contains("checkpoint") || !fields.contains("root-count") ||
      !parseUnsigned(fields["root-count"], root_count) || root_count == 0 ||
      root_count != roots.size() || root_count > kMaximumRootCount) {
    error = "registry witness observation fields are incomplete or invalid";
    return std::nullopt;
  }
  auto origin = decodeBase64Url(fields["origin"]);
  auto checkpoint_text = decodeBase64Url(fields["checkpoint"]);
  auto checkpoint = checkpoint_text
                        ? parseRegistryAuditCheckpoint(*checkpoint_text, error)
                        : std::optional<RegistryAuditCheckpoint>{};
  if (!origin || !validHttpsOrigin(*origin) || !checkpoint)
    return error.empty()
           ? error = "registry witness observation values are invalid",
             std::nullopt : std::nullopt;
  RegistryWitnessObservation observation;
  observation.registry_origin = std::move(*origin);
  observation.checkpoint = std::move(*checkpoint);
  observation.root_chain.reserve(roots.size());
  for (const auto &encoded : roots) {
    auto root = decodeBase64Url(encoded);
    if (!root || root->empty()) {
      error = "registry witness observation contains an invalid root";
      return std::nullopt;
    }
    observation.root_chain.push_back(std::move(*root));
  }
  return observation;
}

std::string registryWitnessStatementSignatureInput(
    const RegistryWitnessStatement &statement) {
  std::ostringstream out;
  out << RegistryWitnessStatementFormat << '\n';
  appendCanonical(out, "witness", statement.witness_name);
  appendCanonical(out, "registry", statement.registry_name);
  appendCanonical(out, "registry-origin", statement.registry_origin);
  appendCanonical(out, "checkpoint-sha256", statement.checkpoint_sha256);
  appendCanonical(out, "tree-size", std::to_string(statement.tree_size));
  appendCanonical(out, "root-hash", statement.root_hash);
  appendCanonical(out, "root-version", std::to_string(statement.root_version));
  appendCanonical(out, "root-sha256", statement.root_sha256);
  appendCanonical(out, "observed-at", statement.observed_at);
  appendCanonical(out, "head-tree-size",
                  std::to_string(statement.head_tree_size));
  appendCanonical(out, "head-root-hash", statement.head_root_hash);
  return out.str();
}

std::string
renderRegistryWitnessStatement(const RegistryWitnessStatement &statement) {
  return registryWitnessStatementSignatureInput(statement) + "signature\t" +
         statement.signature.key_id + '\t' + statement.signature.signature +
         '\n';
}

std::optional<RegistryWitnessStatement>
parseRegistryWitnessStatement(std::string_view text, std::string &error) {
  if (text.size() > 128u * 1024u) {
    error = "registry witness statement exceeds its size limit";
    return std::nullopt;
  }
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != RegistryWitnessStatementFormat) {
    error = "registry witness statement has an invalid format";
    return std::nullopt;
  }
  const std::vector<std::string> names{
      "witness",     "registry",       "registry-origin", "checkpoint-sha256",
      "tree-size",   "root-hash",      "root-version",    "root-sha256",
      "observed-at", "head-tree-size", "head-root-hash"};
  std::map<std::string, std::string> fields;
  RegistryWitnessStatement statement;
  std::size_t index = 0;
  while (std::getline(input, line)) {
    if (line.starts_with("signature\t")) {
      const auto split = line.find('\t', 10);
      if (index != names.size() || split == std::string::npos ||
          !statement.signature.key_id.empty()) {
        error = "registry witness statement signature is malformed";
        return std::nullopt;
      }
      statement.signature = {line.substr(10, split - 10),
                             line.substr(split + 1)};
      continue;
    }
    if (index >= names.size()) {
      error = "registry witness statement contains an unknown field";
      return std::nullopt;
    }
    const auto tab = line.find('\t');
    const auto colon =
        tab == std::string::npos ? std::string::npos : line.find(':', tab + 1);
    std::size_t size = 0;
    if (tab == std::string::npos || colon == std::string::npos ||
        line.substr(0, tab) != names[index] ||
        !parseUnsigned(std::string_view(line).substr(tab + 1, colon - tab - 1),
                       size) ||
        line.size() - colon - 1 != size) {
      error = "registry witness statement field is malformed";
      return std::nullopt;
    }
    fields.emplace(names[index], line.substr(colon + 1));
    ++index;
  }
  if (index != names.size() || statement.signature.key_id.empty() ||
      !parseUnsigned(fields["tree-size"], statement.tree_size) ||
      !parseUnsigned(fields["root-version"], statement.root_version) ||
      !parseUnsigned(fields["head-tree-size"], statement.head_tree_size) ||
      statement.tree_size == 0 || statement.root_version == 0 ||
      statement.head_tree_size == 0 || fields["witness"].empty() ||
      fields["registry"].empty() ||
      !validHttpsOrigin(fields["registry-origin"]) ||
      !lowerHex(fields["checkpoint-sha256"]) ||
      !lowerHex(fields["root-hash"]) || !lowerHex(fields["root-sha256"]) ||
      !lowerHex(fields["head-root-hash"])) {
    error = "registry witness statement values are invalid";
    return std::nullopt;
  }
  std::int64_t observed_at = 0;
  if (!parseRegistryUtcTimestamp(fields["observed-at"], observed_at)) {
    error = "registry witness statement observation time is invalid";
    return std::nullopt;
  }
  statement.witness_name = std::move(fields["witness"]);
  statement.registry_name = std::move(fields["registry"]);
  statement.registry_origin = std::move(fields["registry-origin"]);
  statement.checkpoint_sha256 = std::move(fields["checkpoint-sha256"]);
  statement.root_hash = std::move(fields["root-hash"]);
  statement.root_sha256 = std::move(fields["root-sha256"]);
  statement.observed_at = std::move(fields["observed-at"]);
  statement.head_root_hash = std::move(fields["head-root-hash"]);
  return statement;
}

bool verifyRegistryWitnessStatement(
    const RegistryWitnessStatement &statement,
    const RegistryWitnessObservation &observation,
    const std::vector<std::string> &authorized_keys, std::string &error) {
  const auto checkpoint_text =
      renderRegistryAuditCheckpoint(observation.checkpoint);
  if (statement.registry_name != observation.checkpoint.registry_name ||
      statement.registry_origin != observation.registry_origin ||
      statement.checkpoint_sha256 != sha256Hex(checkpoint_text) ||
      statement.tree_size != observation.checkpoint.tree_size ||
      statement.root_hash != observation.checkpoint.root_hash ||
      statement.root_version != observation.checkpoint.root_version ||
      statement.root_sha256 != observation.checkpoint.root_sha256 ||
      statement.head_tree_size < statement.tree_size) {
    error = "registry witness statement does not bind the observation";
    return false;
  }
  return verifyRegistryWitnessStatementSignature(statement, authorized_keys,
                                                 error);
}

bool verifyRegistryWitnessStatementSignature(
    const RegistryWitnessStatement &statement,
    const std::vector<std::string> &authorized_keys, std::string &error) {
  if (statement.head_tree_size < statement.tree_size) {
    error = "registry witness statement head precedes its observation";
    return false;
  }
  RegistryTrustRole role{authorized_keys, 1};
  return verifyRegistryRoleSignatures(
      registryWitnessStatementSignatureInput(statement), {statement.signature},
      role, {}, error);
}

struct RegistryWitnessStore::Impl {
  RegistryWitnessConfig config;
  sqlite3 *database = nullptr;
  std::unique_ptr<RegistrySigningProvider> signer;
  std::string key_id;
  mutable std::mutex mutex;

  ~Impl() { sqlite3_close(database); }

  bool initialize(std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(config.state_directory, ec);
    if (ec) {
      error = "failed to create registry witness state: " + ec.message();
      return false;
    }
    const auto path =
        std::filesystem::path(config.state_directory) / "witness.sqlite3";
    if (sqlite3_open_v2(path.string().c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      error = sqliteMessage(database, "failed to open registry witness state");
      return false;
    }
    sqlite3_busy_timeout(database, 30000);
    constexpr const char *schema_info = R"SQL(
CREATE TABLE IF NOT EXISTS schema_info(version INTEGER NOT NULL);
INSERT INTO schema_info(version) SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM schema_info);
)SQL";
    if (!execute(database, schema_info, error))
      return false;
    Statement version(database, "SELECT version FROM schema_info", error);
    if (!version)
      return false;
    if (version.step() != SQLITE_ROW || version.integer(0) != 1 ||
        version.step() != SQLITE_DONE) {
      error = "registry witness state has an unsupported schema version";
      return false;
    }
    constexpr const char *schema = R"SQL(
CREATE TABLE IF NOT EXISTS roots(
  version INTEGER PRIMARY KEY, sha256 TEXT NOT NULL UNIQUE, canonical TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS head(
  singleton INTEGER PRIMARY KEY CHECK(singleton = 1), tree_size INTEGER NOT NULL,
  root_hash TEXT NOT NULL, statement TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS statements(
  checkpoint_sha256 TEXT PRIMARY KEY, tree_size INTEGER NOT NULL,
  root_hash TEXT NOT NULL, statement TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS incidents(
  sequence INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,
  known_tree_size INTEGER NOT NULL, known_root_hash TEXT NOT NULL,
  observed_tree_size INTEGER NOT NULL, observed_root_hash TEXT NOT NULL,
  detected_at TEXT NOT NULL);
)SQL";
    return execute(database, schema, error);
  }

  bool incident(std::string_view kind, std::uint64_t known_size,
                std::string_view known_root, std::uint64_t observed_size,
                std::string_view observed_root, std::string_view detected_at,
                std::string &error) {
    Statement statement(
        database,
        "INSERT INTO incidents(kind, known_tree_size, known_root_hash, "
        "observed_tree_size, observed_root_hash, detected_at) VALUES(?, ?, ?, "
        "?, ?, ?)",
        error);
    return statement && statement.bind(1, kind, error) &&
           statement.bind(2, known_size, error) &&
           statement.bind(3, known_root, error) &&
           statement.bind(4, observed_size, error) &&
           statement.bind(5, observed_root, error) &&
           statement.bind(6, detected_at, error) &&
           statement.step() == SQLITE_DONE;
  }
};

RegistryWitnessStore::RegistryWitnessStore() = default;
RegistryWitnessStore::~RegistryWitnessStore() = default;
RegistryWitnessStore::RegistryWitnessStore(RegistryWitnessStore &&) noexcept =
    default;
RegistryWitnessStore &
RegistryWitnessStore::operator=(RegistryWitnessStore &&) noexcept = default;
RegistryWitnessStore::RegistryWitnessStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

std::optional<RegistryWitnessStore>
RegistryWitnessStore::open(RegistryWitnessConfig config, std::string &error) {
  auto id = witnessKeyId(config.witness_public_key);
  if (config.witness_name.empty() || config.state_directory.empty() ||
      config.registry_name.empty() ||
      !validHttpsOrigin(config.registry_origin) ||
      config.bootstrap_root_keys.empty() ||
      config.bootstrap_root_threshold == 0 ||
      config.bootstrap_root_threshold > config.bootstrap_root_keys.size() ||
      !id || config.signer.command.empty() ||
      config.signer.config_path.empty()) {
    error = "registry witness configuration is incomplete";
    return std::nullopt;
  }
  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->key_id = std::move(*id);
  impl->signer = createRegistryCommandSigningProvider(impl->config.signer);
  if (!impl->initialize(error))
    return std::nullopt;
  RegistrySignerRequest keys;
  keys.operation = RegistrySignerOperation::Keys;
  keys.registry_name = impl->config.registry_name;
  keys.role = RegistrySigningRole::Witness;
  keys.root_version = 1;
  keys.threshold = 1;
  keys.authorized_key_ids = {impl->key_id};
  auto capability = impl->signer->execute(keys, error);
  if (!capability || capability->key_ids != keys.authorized_key_ids) {
    if (error.empty())
      error = "registry witness signer capability does not match its identity";
    return std::nullopt;
  }
  return RegistryWitnessStore(std::move(impl));
}

std::optional<RegistryWitnessStatement> RegistryWitnessStore::observe(
    const RegistryWitnessObservation &observation,
    std::int64_t now_unix_seconds,
    const RegistryConsistencyProofFetcher &fetch_proof, std::string &error) {
  if (!impl_ || observation.registry_origin != impl_->config.registry_origin ||
      observation.checkpoint.registry_name != impl_->config.registry_name ||
      observation.root_chain.size() != observation.checkpoint.root_version) {
    error = "registry witness observation does not match this witness";
    return std::nullopt;
  }
  std::lock_guard lock(impl_->mutex);
  const auto observed_at = timestamp(now_unix_seconds);
  if (observed_at.empty()) {
    error = "registry witness observation time is invalid";
    return std::nullopt;
  }

  const auto checkpoint_text =
      renderRegistryAuditCheckpoint(observation.checkpoint);
  const auto checkpoint_sha256 = sha256Hex(checkpoint_text);
  const auto validation = std::filesystem::path(impl_->config.state_directory) /
                          "validation" / checkpoint_sha256;
  const auto root_directory = validation / "trust" / "root";
  std::error_code ec;
  std::filesystem::create_directories(root_directory, ec);
  if (ec) {
    error = "failed to create witness root validation state: " + ec.message();
    return std::nullopt;
  }
  for (std::size_t index = 0; index < observation.root_chain.size(); ++index) {
    auto root = parseRegistryRootMetadata(observation.root_chain[index],
                                          "witness observation", error);
    const auto version = static_cast<std::uint64_t>(index + 1);
    if (!root || root->registry_name != impl_->config.registry_name ||
        root->version != version) {
      if (error.empty())
        error = "registry witness root chain is not contiguous";
      return std::nullopt;
    }
    const auto canonical = renderRegistryRootMetadata(*root);
    if (canonical != observation.root_chain[index] ||
        !writeTextFile(
            (root_directory / (std::to_string(version) + ".toml")).string(),
            canonical, error))
      return std::nullopt;
  }
  RegistryRootChainVerificationRequest verification;
  verification.registry_name = impl_->config.registry_name;
  verification.checkout_root = validation.string();
  verification.bootstrap_root_keys = impl_->config.bootstrap_root_keys;
  verification.bootstrap_root_threshold =
      impl_->config.bootstrap_root_threshold;
  auto verified = verifyRegistryRootChain(verification, error);
  if (!verified || !verifyRegistryAuditCheckpoint(observation.checkpoint,
                                                  verified->root, error))
    return std::nullopt;

  for (std::size_t index = 0; index < observation.root_chain.size(); ++index) {
    Statement known_root(impl_->database,
                         "SELECT sha256 FROM roots WHERE version = ?", error);
    if (!known_root ||
        !known_root.bind(1, static_cast<std::uint64_t>(index + 1), error))
      return std::nullopt;
    if (known_root.step() == SQLITE_ROW &&
        known_root.text(0) != sha256Hex(observation.root_chain[index])) {
      const auto observed_digest = sha256Hex(observation.root_chain[index]);
      impl_->incident("root-equivocation",
                      static_cast<std::uint64_t>(index + 1), known_root.text(0),
                      static_cast<std::uint64_t>(index + 1), observed_digest,
                      observed_at, error);
      if (error.empty())
        error = "registry witness detected root metadata equivocation";
      return std::nullopt;
    }
  }

  Statement existing(
      impl_->database,
      "SELECT statement FROM statements WHERE checkpoint_sha256 = ?", error);
  if (!existing || !existing.bind(1, checkpoint_sha256, error))
    return std::nullopt;
  if (existing.step() == SQLITE_ROW)
    return parseRegistryWitnessStatement(existing.text(0), error);

  std::uint64_t head_size = observation.checkpoint.tree_size;
  std::string head_root = observation.checkpoint.root_hash;
  bool advance = true;
  Statement head(impl_->database,
                 "SELECT tree_size, root_hash FROM head WHERE singleton = 1",
                 error);
  if (!head)
    return std::nullopt;
  if (head.step() == SQLITE_ROW) {
    const auto known_size = head.integer(0);
    const auto known_root = head.text(1);
    head_size = known_size;
    head_root = known_root;
    if (known_size == observation.checkpoint.tree_size) {
      if (known_root != observation.checkpoint.root_hash) {
        impl_->incident("equal-size-fork", known_size, known_root,
                        observation.checkpoint.tree_size,
                        observation.checkpoint.root_hash, observed_at, error);
        if (error.empty())
          error = "registry witness detected an equal-size checkpoint fork";
        return std::nullopt;
      }
      advance = false;
    } else {
      const bool newer = known_size < observation.checkpoint.tree_size;
      const auto old_size =
          newer ? known_size : observation.checkpoint.tree_size;
      const auto new_size =
          newer ? observation.checkpoint.tree_size : known_size;
      const auto &old_root =
          newer ? known_root : observation.checkpoint.root_hash;
      const auto &new_root =
          newer ? observation.checkpoint.root_hash : known_root;
      auto proof = fetch_proof ? fetch_proof(old_size, new_size, error)
                               : std::optional<std::vector<std::string>>{};
      if (!proof || !verifyRegistryMerkleConsistency(
                        old_size, new_size, old_root, new_root, *proof)) {
        impl_->incident("inconsistent-growth", known_size, known_root,
                        observation.checkpoint.tree_size,
                        observation.checkpoint.root_hash, observed_at, error);
        if (error.empty())
          error = "registry witness detected inconsistent checkpoint growth";
        return std::nullopt;
      }
      if (newer) {
        head_size = observation.checkpoint.tree_size;
        head_root = observation.checkpoint.root_hash;
      } else {
        advance = false;
      }
    }
  }

  RegistryWitnessStatement result;
  result.witness_name = impl_->config.witness_name;
  result.registry_name = impl_->config.registry_name;
  result.registry_origin = impl_->config.registry_origin;
  result.checkpoint_sha256 = checkpoint_sha256;
  result.tree_size = observation.checkpoint.tree_size;
  result.root_hash = observation.checkpoint.root_hash;
  result.root_version = observation.checkpoint.root_version;
  result.root_sha256 = observation.checkpoint.root_sha256;
  result.observed_at = observed_at;
  result.head_tree_size = head_size;
  result.head_root_hash = head_root;
  RegistrySignerRequest sign;
  sign.operation = RegistrySignerOperation::Sign;
  sign.registry_name = impl_->config.registry_name;
  sign.role = RegistrySigningRole::Witness;
  sign.root_version = result.root_version;
  sign.threshold = 1;
  sign.authorized_key_ids = {impl_->key_id};
  sign.payload = registryWitnessStatementSignatureInput(result);
  auto signed_result = impl_->signer->execute(sign, error);
  if (!signed_result || signed_result->signatures.size() != 1 ||
      signed_result->signatures.front().key_id != impl_->key_id)
    return std::nullopt;
  result.signature = signed_result->signatures.front();
  const auto rendered = renderRegistryWitnessStatement(result);

  if (!execute(impl_->database, "BEGIN IMMEDIATE", error))
    return std::nullopt;
  bool ok = true;
  for (std::size_t index = 0; index < observation.root_chain.size() && ok;
       ++index) {
    const auto digest = sha256Hex(observation.root_chain[index]);
    Statement root(
        impl_->database,
        "INSERT INTO roots(version, sha256, canonical) VALUES(?, ?, ?) "
        "ON CONFLICT(version) DO NOTHING",
        error);
    ok = root && root.bind(1, static_cast<std::uint64_t>(index + 1), error) &&
         root.bind(2, digest, error) &&
         root.bind(3, observation.root_chain[index], error) &&
         root.step() == SQLITE_DONE;
    Statement check(impl_->database,
                    "SELECT sha256 FROM roots WHERE version = ?", error);
    ok = ok && check &&
         check.bind(1, static_cast<std::uint64_t>(index + 1), error) &&
         check.step() == SQLITE_ROW && check.text(0) == digest;
    if (!ok && error.empty())
      error = "registry witness detected root metadata equivocation";
  }
  Statement stored(impl_->database,
                   "INSERT INTO statements(checkpoint_sha256, tree_size, "
                   "root_hash, statement) "
                   "VALUES(?, ?, ?, ?)",
                   error);
  ok = ok && stored && stored.bind(1, checkpoint_sha256, error) &&
       stored.bind(2, result.tree_size, error) &&
       stored.bind(3, result.root_hash, error) &&
       stored.bind(4, rendered, error) && stored.step() == SQLITE_DONE;
  if (ok && advance) {
    Statement update(
        impl_->database,
        "INSERT INTO head(singleton, tree_size, root_hash, statement) "
        "VALUES(1, ?, ?, ?) "
        "ON CONFLICT(singleton) DO UPDATE SET tree_size=excluded.tree_size, "
        "root_hash=excluded.root_hash, statement=excluded.statement",
        error);
    ok = update && update.bind(1, head_size, error) &&
         update.bind(2, head_root, error) && update.bind(3, rendered, error) &&
         update.step() == SQLITE_DONE;
  }
  if (!execute(impl_->database, ok ? "COMMIT" : "ROLLBACK", error) || !ok)
    return std::nullopt;
  return result;
}

std::optional<RegistryWitnessStatement>
RegistryWitnessStore::latest(std::string &error) const {
  if (!impl_)
    return std::nullopt;
  std::lock_guard lock(impl_->mutex);
  Statement statement(impl_->database,
                      "SELECT statement FROM head WHERE singleton = 1", error);
  if (!statement || statement.step() != SQLITE_ROW) {
    if (error.empty())
      error = "registry witness has not observed a checkpoint";
    return std::nullopt;
  }
  return parseRegistryWitnessStatement(statement.text(0), error);
}

std::optional<std::vector<RegistryWitnessIncident>>
RegistryWitnessStore::incidents(std::string &error) const {
  if (!impl_)
    return std::nullopt;
  std::lock_guard lock(impl_->mutex);
  Statement statement(
      impl_->database,
      "SELECT sequence, kind, known_tree_size, known_root_hash, "
      "observed_tree_size, observed_root_hash, detected_at FROM incidents "
      "ORDER BY sequence",
      error);
  if (!statement)
    return std::nullopt;
  std::vector<RegistryWitnessIncident> result;
  while (statement.step() == SQLITE_ROW)
    result.push_back({statement.integer(0), statement.text(1),
                      statement.integer(2), statement.text(3),
                      statement.integer(4), statement.text(5),
                      statement.text(6)});
  return result;
}

std::optional<RegistryWitnessDaemonConfig>
loadRegistryWitnessDaemonConfig(const std::string &path, std::string &error) {
  auto text = readTextFile(path, error);
  auto assignments = text ? manifest_toml::parseAssignments(
                                *text,
                                {"", "listen", "tls", "storage", "registry",
                                 "trust", "identity", "signer"},
                                "registry witness config", error)
                          : std::nullopt;
  if (!assignments)
    return std::nullopt;
  RegistryWitnessDaemonConfig config;
  std::set<std::string> seen;
  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if (!seen.insert(key).second)
      return error = "duplicate registry witness config field '" + key + "'",
             std::nullopt;
    const auto string = [&](std::string &output) {
      return manifest_toml::parseString(assignment.value, output) &&
             !output.empty();
    };
    std::uint64_t number = 0;
    if (key == "format") {
      std::string value;
      if (!string(value) || value != RegistryWitnessServerConfigFormat)
        return error = "unsupported registry witness config format",
               std::nullopt;
    } else if (key == "witness") {
      if (!string(config.witness.witness_name))
        return error = "registry witness name is invalid", std::nullopt;
    } else if (key == "listen.address") {
      if (!string(config.listen_address))
        return error = "registry witness listen address is invalid",
               std::nullopt;
    } else if (key == "listen.port") {
      if (!manifest_toml::parseUnsigned(assignment.value, number) ||
          number == 0 || number > UINT16_MAX)
        return error = "registry witness listen port is invalid", std::nullopt;
      config.listen_port = static_cast<std::uint16_t>(number);
    } else if (key == "tls.certificate") {
      if (!string(config.tls_certificate_path))
        return error = "registry witness TLS certificate is invalid",
               std::nullopt;
    } else if (key == "tls.private_key") {
      if (!string(config.tls_private_key_path))
        return error = "registry witness TLS private key is invalid",
               std::nullopt;
    } else if (key == "storage.state_directory") {
      if (!string(config.witness.state_directory))
        return error = "registry witness state directory is invalid",
               std::nullopt;
    } else if (key == "registry.name") {
      if (!string(config.witness.registry_name))
        return error = "registry witness registry name is invalid",
               std::nullopt;
    } else if (key == "registry.origin") {
      if (!string(config.witness.registry_origin) ||
          !validHttpsOrigin(config.witness.registry_origin))
        return error = "registry witness registry origin is invalid",
               std::nullopt;
    } else if (key == "registry.ca_bundle") {
      if (!string(config.registry_ca_bundle_path))
        return error = "registry witness CA bundle is invalid", std::nullopt;
    } else if (key == "trust.bootstrap_root_keys") {
      if (!manifest_toml::parseStringArray(
              assignment.value, config.witness.bootstrap_root_keys) ||
          config.witness.bootstrap_root_keys.empty())
        return error = "registry witness bootstrap keys are invalid",
               std::nullopt;
    } else if (key == "trust.bootstrap_root_threshold") {
      if (!manifest_toml::parseUnsigned(assignment.value, number) ||
          number == 0 || number > UINT32_MAX)
        return error = "registry witness bootstrap threshold is invalid",
               std::nullopt;
      config.witness.bootstrap_root_threshold =
          static_cast<std::uint32_t>(number);
    } else if (key == "identity.public_key") {
      if (!string(config.witness.witness_public_key))
        return error = "registry witness public key is invalid", std::nullopt;
    } else if (key == "signer.command") {
      if (!string(config.witness.signer.command))
        return error = "registry witness signer command is invalid",
               std::nullopt;
    } else if (key == "signer.config") {
      if (!string(config.witness.signer.config_path))
        return error = "registry witness signer config is invalid",
               std::nullopt;
    } else if (key == "signer.timeout_milliseconds") {
      if (!manifest_toml::parseUnsigned(assignment.value, number) ||
          number == 0 || number > 300000)
        return error = "registry witness signer timeout is invalid",
               std::nullopt;
      config.witness.signer.timeout_milliseconds = number;
    } else {
      return error = "unknown registry witness config field '" + key + "'",
             std::nullopt;
    }
  }
  if (config.witness.witness_name.empty() ||
      config.witness.state_directory.empty() ||
      config.witness.registry_name.empty() ||
      config.witness.registry_origin.empty() ||
      config.witness.bootstrap_root_keys.empty() ||
      config.witness.bootstrap_root_threshold == 0 ||
      config.witness.bootstrap_root_threshold >
          config.witness.bootstrap_root_keys.size() ||
      !witnessKeyId(config.witness.witness_public_key) ||
      config.witness.signer.command.empty() ||
      config.witness.signer.config_path.empty() ||
      config.tls_certificate_path.empty() ||
      config.tls_private_key_path.empty())
    return error = "registry witness config is incomplete", std::nullopt;
  const auto base = std::filesystem::absolute(path).parent_path();
  const auto resolve = [&](std::string &value) {
    auto candidate = std::filesystem::path(value);
    if (candidate.is_relative())
      candidate = base / candidate;
    value = candidate.lexically_normal().string();
  };
  resolve(config.witness.state_directory);
  resolve(config.witness.signer.config_path);
  if (std::filesystem::path(config.witness.signer.command).has_parent_path())
    resolve(config.witness.signer.command);
  resolve(config.tls_certificate_path);
  resolve(config.tls_private_key_path);
  if (!config.registry_ca_bundle_path.empty())
    resolve(config.registry_ca_bundle_path);
  return config;
}

} // namespace chtholly
