#include "chtholly/Driver/RegistryBackup.h"

#include "ManifestToml.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Driver/RegistryArtifact.h"
#include "chtholly/Driver/RegistryServer.h"
#include "chtholly/Driver/RegistryTrust.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include "miniz.h"
#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace chtholly {
namespace {

constexpr std::uint64_t kMaximumManifestSize = 16u * 1024u * 1024u;

struct Database {
  sqlite3 *handle = nullptr;
  Database() = default;
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Database(Database &&other) noexcept : handle(other.handle) {
    other.handle = nullptr;
  }
  Database &operator=(Database &&other) noexcept {
    if (this != &other) {
      sqlite3_close(handle);
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }
  ~Database() { sqlite3_close(handle); }
};

struct ZipReader {
  mz_zip_archive archive{};
  bool open = false;
  ~ZipReader() {
    if (open)
      mz_zip_reader_end(&archive);
  }
};

struct ZipWriter {
  mz_zip_archive archive{};
  bool open = false;
  ~ZipWriter() {
    if (open)
      mz_zip_writer_end(&archive);
  }
};

struct StagedBackup {
  RegistryBackupInfo info;
  std::filesystem::path root;
  std::filesystem::path database;
  std::filesystem::path index_bundle;
};

struct RemoveTree {
  std::filesystem::path path;
  ~RemoveTree() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string sqliteError(sqlite3 *database, std::string_view context) {
  return std::string(context) + ": " +
         (database == nullptr ? "database is unavailable"
                              : sqlite3_errmsg(database));
}

bool execute(sqlite3 *database, const char *sql, std::string &error) {
  char *message = nullptr;
  const auto status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (status == SQLITE_OK)
    return true;
  error = "registry backup database operation failed: " +
          std::string(message == nullptr ? sqlite3_errmsg(database) : message);
  sqlite3_free(message);
  return false;
}

std::optional<Database> openDatabase(const std::filesystem::path &path,
                                     int flags, std::string &error) {
  Database database;
  if (sqlite3_open_v2(path.string().c_str(), &database.handle, flags,
                      nullptr) != SQLITE_OK) {
    error =
        sqliteError(database.handle, "failed to open registry backup database");
    return std::nullopt;
  }
  return database;
}

bool queryUnsigned(sqlite3 *database, const char *sql, std::uint64_t &value,
                   std::string &error) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    error = sqliteError(database, "failed to prepare registry backup query");
    return false;
  }
  const auto status = sqlite3_step(statement);
  if (status != SQLITE_ROW ||
      sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
      sqlite3_column_int64(statement, 0) < 0) {
    error = "registry backup query did not return a non-negative integer";
    sqlite3_finalize(statement);
    return false;
  }
  value = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
  const bool done = sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  if (!done)
    error = "registry backup query returned more than one row";
  return done;
}

std::optional<std::string> queryText(sqlite3 *database, const char *sql,
                                     std::string &error) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    error = sqliteError(database, "failed to prepare registry backup query");
    return std::nullopt;
  }
  if (sqlite3_step(statement) != SQLITE_ROW ||
      sqlite3_column_type(statement, 0) != SQLITE_TEXT) {
    error = "registry backup query did not return text";
    sqlite3_finalize(statement);
    return std::nullopt;
  }
  const auto *text = sqlite3_column_text(statement, 0);
  const auto size = sqlite3_column_bytes(statement, 0);
  std::string result(reinterpret_cast<const char *>(text),
                     static_cast<std::size_t>(size));
  const bool done = sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  if (!done) {
    error = "registry backup query returned more than one row";
    return std::nullopt;
  }
  return result;
}

bool snapshotDatabase(const std::filesystem::path &source,
                      const std::filesystem::path &destination,
                      std::string &error) {
  auto input = openDatabase(source, SQLITE_OPEN_READONLY, error);
  auto output =
      input ? openDatabase(destination,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, error)
            : std::optional<Database>{};
  if (!input || !output)
    return false;
  sqlite3_backup *backup =
      sqlite3_backup_init(output->handle, "main", input->handle, "main");
  if (backup == nullptr) {
    error = sqliteError(output->handle, "failed to initialize SQLite backup");
    return false;
  }
  const auto status = sqlite3_backup_step(backup, -1);
  const auto finish_status = sqlite3_backup_finish(backup);
  if (status != SQLITE_DONE || finish_status != SQLITE_OK) {
    error = sqliteError(output->handle, "failed to create SQLite backup");
    return false;
  }
  return execute(output->handle, "PRAGMA wal_checkpoint(TRUNCATE)", error);
}

bool safePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos)
    return false;
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto slash = path.find('/', begin);
    const auto part =
        path.substr(begin, slash == std::string_view::npos ? path.size() - begin
                                                           : slash - begin);
    if (part.empty() || part == "." || part == "..")
      return false;
    if (slash == std::string_view::npos)
      break;
    begin = slash + 1;
  }
  return true;
}

bool isHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool validBlobMemberPath(std::string_view path) {
  constexpr std::string_view prefix = "blobs/sha256/";
  constexpr std::string_view suffix = ".cpa";
  if (!path.starts_with(prefix) || !path.ends_with(suffix))
    return false;
  path.remove_prefix(prefix.size());
  path.remove_suffix(suffix.size());
  const auto slash = path.find('/');
  return slash == 2 && path.find('/', slash + 1) == std::string_view::npos &&
         isHex(path.substr(0, slash), 2) && isHex(path.substr(slash + 1), 64) &&
         path.substr(0, slash) == path.substr(slash + 1, 2);
}

bool validGitObjectId(std::string_view value) {
  return isHex(value, 40) || isHex(value, 64);
}

std::string quote(std::string_view value) {
  std::string result = "\"";
  for (const char ch : value) {
    if (ch == '\\' || ch == '"')
      result.push_back('\\');
    result.push_back(ch);
  }
  result.push_back('"');
  return result;
}

std::string renderManifest(const RegistryBackupInfo &info) {
  std::ostringstream out;
  const bool recovery_point = !info.recovery_point_id.empty();
  out << "format = "
      << quote(recovery_point ? RegistryBackupFormat : RegistryBackupFormatV1)
      << '\n'
      << "registry = " << quote(info.registry_name) << '\n'
      << "created_at = " << quote(info.created_at) << '\n'
      << "database_schema = " << info.database_schema << '\n'
      << "git_head = " << quote(info.git_head) << '\n'
      << "tree_size = " << info.tree_size << '\n'
      << "tree_root = " << quote(info.tree_root) << '\n'
      << "root_version = " << info.root_version << '\n'
      << "root_sha256 = " << quote(info.root_sha256) << '\n';
  if (recovery_point)
    out << "recovery_point_id = " << quote(info.recovery_point_id) << '\n'
        << "retain_until = " << quote(info.retain_until) << '\n';
  out << "file_count = " << info.files.size() << "\n\n[files]\n";
  for (std::size_t index = 0; index < info.files.size(); ++index) {
    const auto &file = info.files[index];
    out << "file" << index << " = { path = " << quote(file.relative_path)
        << ", size = " << file.size << ", sha256 = " << quote(file.sha256)
        << " }\n";
  }
  return out.str();
}

std::optional<RegistryBackupInfo> parseManifest(std::string_view text,
                                                std::string &error) {
  auto assignments = manifest_toml::parseAssignments(
      text, {"", "files"}, "registry backup manifest", error);
  if (!assignments)
    return std::nullopt;
  RegistryBackupInfo info;
  std::map<std::string, std::string> root;
  std::map<std::size_t, RegistryBackupFile> files;
  for (const auto &assignment : *assignments) {
    if (assignment.table.empty()) {
      if (!root.emplace(assignment.key, assignment.value).second) {
        error =
            "duplicate registry backup manifest field '" + assignment.key + "'";
        return std::nullopt;
      }
      continue;
    }
    if (!assignment.key.starts_with("file")) {
      error = "unknown registry backup file field '" + assignment.key + "'";
      return std::nullopt;
    }
    std::size_t index = 0;
    const auto suffix = std::string_view(assignment.key).substr(4);
    const auto [end, status] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    std::vector<std::pair<std::string, std::string>> values;
    if (status != std::errc{} || end != suffix.data() + suffix.size() ||
        !manifest_toml::parseInlineTable(assignment.value, values) ||
        files.contains(index)) {
      error = "invalid registry backup file record '" + assignment.key + "'";
      return std::nullopt;
    }
    RegistryBackupFile file;
    std::set<std::string> seen;
    for (const auto &[key, value] : values) {
      if (!seen.insert(key).second) {
        error = "duplicate registry backup file property '" + key + "'";
        return std::nullopt;
      }
      if (key == "path") {
        if (!manifest_toml::parseString(value, file.relative_path))
          return error = "invalid registry backup file path", std::nullopt;
      } else if (key == "size") {
        if (!manifest_toml::parseUnsigned(value, file.size))
          return error = "invalid registry backup file size", std::nullopt;
      } else if (key == "sha256") {
        if (!manifest_toml::parseString(value, file.sha256))
          return error = "invalid registry backup file digest", std::nullopt;
      } else {
        error = "unknown registry backup file property '" + key + "'";
        return std::nullopt;
      }
    }
    if (seen.size() != 3 || !safePath(file.relative_path) ||
        !isHex(file.sha256, 64)) {
      error = "incomplete or unsafe registry backup file record";
      return std::nullopt;
    }
    files.emplace(index, std::move(file));
  }
  std::string format;
  if (!root.contains("format") ||
      !manifest_toml::parseString(root["format"], format) ||
      (format != RegistryBackupFormatV1 && format != RegistryBackupFormat)) {
    error = "registry backup manifest format is unsupported";
    return std::nullopt;
  }
  std::set<std::string> expected{
      "format",      "registry",  "created_at", "database_schema",
      "git_head",    "tree_size", "tree_root",  "root_version",
      "root_sha256", "file_count"};
  if (format == RegistryBackupFormat) {
    expected.insert("recovery_point_id");
    expected.insert("retain_until");
  }
  if (root.size() != expected.size() ||
      !std::all_of(expected.begin(), expected.end(),
                   [&](const auto &key) { return root.contains(key); })) {
    error = "registry backup manifest fields are incomplete or unknown";
    return std::nullopt;
  }
  std::uint64_t file_count = 0;
  if (!manifest_toml::parseString(root["registry"], info.registry_name) ||
      !manifest_toml::parseString(root["created_at"], info.created_at) ||
      !manifest_toml::parseUnsigned(root["database_schema"],
                                    info.database_schema) ||
      !manifest_toml::parseString(root["git_head"], info.git_head) ||
      !manifest_toml::parseUnsigned(root["tree_size"], info.tree_size) ||
      !manifest_toml::parseString(root["tree_root"], info.tree_root) ||
      !manifest_toml::parseUnsigned(root["root_version"], info.root_version) ||
      !manifest_toml::parseString(root["root_sha256"], info.root_sha256) ||
      !manifest_toml::parseUnsigned(root["file_count"], file_count) ||
      info.registry_name.empty() || !validGitObjectId(info.git_head) ||
      !isHex(info.tree_root, 64) || !isHex(info.root_sha256, 64) ||
      file_count != files.size()) {
    error = "registry backup manifest values are invalid";
    return std::nullopt;
  }
  if (format == RegistryBackupFormat) {
    std::int64_t retain_until = 0;
    if (!manifest_toml::parseString(root["recovery_point_id"],
                                    info.recovery_point_id) ||
        !isHex(info.recovery_point_id, 64) ||
        !manifest_toml::parseString(root["retain_until"], info.retain_until) ||
        !parseRegistryUtcTimestamp(info.retain_until, retain_until)) {
      error = "registry recovery point manifest values are invalid";
      return std::nullopt;
    }
  }
  for (std::size_t index = 0; index < files.size(); ++index) {
    auto found = files.find(index);
    if (found == files.end()) {
      error = "registry backup file records are not contiguous";
      return std::nullopt;
    }
    info.files.push_back(std::move(found->second));
  }
  if (info.files.size() < 2 ||
      info.files[0].relative_path != "state/registry.sqlite3" ||
      info.files[1].relative_path != "index.bundle") {
    error = "registry backup mandatory members are missing or reordered";
    return std::nullopt;
  }
  for (std::size_t index = 2; index < info.files.size(); ++index) {
    if (!validBlobMemberPath(info.files[index].relative_path) ||
        (index > 2 && info.files[index - 1].relative_path >=
                          info.files[index].relative_path)) {
      error = "registry backup blob members are invalid or unordered";
      return std::nullopt;
    }
  }
  return info;
}

std::string timestamp(std::int64_t requested) {
  const auto seconds =
      requested >= 0
          ? requested
          : static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()));
  const auto time = static_cast<std::time_t>(seconds);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &time) != 0)
    return {};
#else
  if (gmtime_r(&time, &utc) == nullptr)
    return {};
#endif
  char value[21]{};
  return std::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &utc) == 20
             ? std::string(value)
             : std::string{};
}

std::filesystem::path temporaryPath(const std::filesystem::path &parent,
                                    std::string_view prefix) {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return parent / (std::string(prefix) + std::to_string(nonce));
}

bool runGit(const std::vector<std::string> &arguments, std::string &output,
            std::string &error) {
  auto result = runProcess("git", arguments, error);
  if (!result || result->exit_code != 0) {
    if (result)
      error = summarizeCommandFailure("git", *result);
    return false;
  }
  output = std::move(result->stdout_text);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  return true;
}

bool copyFileChecked(const std::filesystem::path &source,
                     const std::filesystem::path &destination,
                     std::uint64_t expected_size, std::string_view digest,
                     std::string &error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(source, ec);
  const auto actual =
      ec ? std::optional<std::string>{} : sha256File(source.string());
  if (ec || size != expected_size || !actual || *actual != digest) {
    error = "registry backup source file failed integrity verification: " +
            source.string();
    return false;
  }
  std::filesystem::create_directories(destination.parent_path(), ec);
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec || sha256File(destination.string()) != actual) {
    error = "failed to copy registry backup member: " + destination.string();
    return false;
  }
  return true;
}

bool addBackupFile(RegistryBackupInfo &info, std::string relative_path,
                   const std::filesystem::path &absolute, std::string &error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(absolute, ec);
  auto digest =
      ec ? std::optional<std::string>{} : sha256File(absolute.string());
  if (ec || !digest) {
    error = "failed to inspect registry backup member: " + absolute.string();
    return false;
  }
  info.files.push_back(
      {std::move(relative_path), static_cast<std::uint64_t>(size), *digest});
  return true;
}

bool writeArchive(const std::filesystem::path &stage,
                  const RegistryBackupInfo &info,
                  const std::filesystem::path &output, std::string &error) {
  const auto temporary = output.string() + ".tmp";
  std::error_code ec;
  removeFile(temporary, ec);
  ZipWriter writer;
  if (!mz_zip_writer_init_file_v2(&writer.archive, temporary.c_str(), 0, 0)) {
    error = "failed to initialize registry backup archive";
    return false;
  }
  writer.open = true;
  const auto manifest = renderManifest(info);
  if (!mz_zip_writer_add_mem_ex_v2(&writer.archive, "manifest.toml",
                                   manifest.data(), manifest.size(), nullptr, 0,
                                   MZ_NO_COMPRESSION, 0, 0, nullptr, nullptr, 0,
                                   nullptr, 0)) {
    error = "failed to add registry backup manifest";
    return false;
  }
  for (const auto &file : info.files) {
    const auto source = (stage / file.relative_path).string();
    if (!mz_zip_writer_add_file(&writer.archive, file.relative_path.c_str(),
                                source.c_str(), nullptr, 0,
                                MZ_NO_COMPRESSION)) {
      error =
          "failed to add registry backup member '" + file.relative_path + "'";
      return false;
    }
  }
  if (!mz_zip_writer_finalize_archive(&writer.archive) ||
      !mz_zip_writer_end(&writer.archive)) {
    writer.open = false;
    error = "failed to finalize registry backup archive";
    return false;
  }
  writer.open = false;
  if (!replaceFile(temporary, output.string(), ec)) {
    error = "failed to publish registry backup archive: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}

bool isSymlink(const mz_zip_archive_file_stat &stat) {
  constexpr mz_uint32 symlink = 0120000u;
  constexpr mz_uint32 mask = 0170000u;
  return (((stat.m_external_attr >> 16) & 0xffffu) & mask) == symlink;
}

std::optional<std::string> zipName(mz_zip_archive &archive, mz_uint index,
                                   std::string &error) {
  const auto size = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
  if (size == 0)
    return error = "registry backup contains an unreadable path", std::nullopt;
  std::string name(size, '\0');
  if (mz_zip_reader_get_filename(&archive, index, name.data(), size) != size)
    return error = "registry backup contains an unreadable path", std::nullopt;
  name.resize(size - 1);
  return name.find('\0') == std::string::npos ? std::optional{name}
                                              : std::nullopt;
}

bool cloneBundle(const std::filesystem::path &bundle,
                 const std::filesystem::path &destination,
                 std::string_view head, std::string &error) {
  std::string output;
  if (!runGit({"init", destination.string()}, output, error) ||
      !runGit({"-C", destination.string(), "config", "core.autocrlf", "false"},
              output, error) ||
      !runGit({"-C", destination.string(), "config", "core.eol", "lf"}, output,
              error) ||
      !runGit({"-C", destination.string(), "bundle", "verify",
               bundle.string()},
              output, error) ||
      !runGit({"-C", destination.string(), "fetch", bundle.string(),
               std::string(head)},
              output, error) ||
      !runGit(
          {"-C", destination.string(), "checkout", "--detach", "FETCH_HEAD"},
          output, error))
    return false;
  return runGit({"-C", destination.string(), "rev-parse", "HEAD"}, output,
                error) &&
         output == head;
}

bool verifyDatabaseAndIndex(const RegistryBackupRestoreRequest &request,
                            const StagedBackup &staged,
                            const std::filesystem::path &index,
                            std::string &error) {
  auto database = openDatabase(staged.database, SQLITE_OPEN_READONLY, error);
  if (!database)
    return false;
  auto integrity = queryText(database->handle, "PRAGMA integrity_check", error);
  std::uint64_t schema = 0;
  std::uint64_t pending = 0;
  std::uint64_t leaf_count = 0;
  if (!integrity || *integrity != "ok" ||
      !queryUnsigned(database->handle, "SELECT version FROM schema_info",
                     schema, error) ||
      !queryUnsigned(database->handle,
                     "SELECT COUNT(*) FROM mutations WHERE distributed = 0",
                     pending, error) ||
      !queryUnsigned(database->handle, "SELECT COUNT(*) FROM audit_leaves",
                     leaf_count, error) ||
      (schema != 2 && schema != 3) || schema != staged.info.database_schema ||
      pending != 0 ||
      leaf_count != staged.info.tree_size) {
    if (error.empty())
      error = "registry backup database invariants are not satisfied";
    return false;
  }
  std::vector<std::string> leaves;
  sqlite3_stmt *leaf_statement = nullptr;
  if (sqlite3_prepare_v2(database->handle,
                         "SELECT leaf_index, canonical_leaf, leaf_hash FROM "
                         "audit_leaves ORDER BY leaf_index",
                         -1, &leaf_statement, nullptr) != SQLITE_OK) {
    error =
        sqliteError(database->handle, "failed to read registry audit leaves");
    return false;
  }
  while (sqlite3_step(leaf_statement) == SQLITE_ROW) {
    const auto index_value = sqlite3_column_int64(leaf_statement, 0);
    const auto *canonical = sqlite3_column_text(leaf_statement, 1);
    const auto canonical_size = sqlite3_column_bytes(leaf_statement, 1);
    const auto *hash = sqlite3_column_text(leaf_statement, 2);
    const std::string hash_text(reinterpret_cast<const char *>(hash),
                                sqlite3_column_bytes(leaf_statement, 2));
    const std::string_view canonical_text(
        reinterpret_cast<const char *>(canonical), canonical_size);
    if (index_value != static_cast<sqlite3_int64>(leaves.size()) ||
        registryAuditLeafHash(canonical_text) != hash_text) {
      sqlite3_finalize(leaf_statement);
      error = "registry backup audit log is non-contiguous or corrupted";
      return false;
    }
    leaves.push_back(hash_text);
  }
  sqlite3_finalize(leaf_statement);
  if (registryMerkleRoot(leaves) != staged.info.tree_root) {
    error = "registry backup audit Merkle root does not match its manifest";
    return false;
  }
  RegistryRootChainVerificationRequest root_request;
  root_request.registry_name = request.registry_name;
  root_request.checkout_root = index.string();
  root_request.bootstrap_root_keys = request.bootstrap_root_keys;
  root_request.bootstrap_root_threshold = request.bootstrap_root_threshold;
  root_request.now_unix_seconds = request.now_unix_seconds;
  auto root = verifyRegistryRootChain(root_request, error);
  if (!root || root->root.version != staged.info.root_version ||
      root->root_sha256 != staged.info.root_sha256)
    return false;
  if (staged.info.tree_size != 0) {
    auto receipt_text = queryText(
        database->handle,
        "SELECT receipt FROM mutations ORDER BY rowid DESC LIMIT 1", error);
    auto receipt = receipt_text
                       ? parseRegistryAuditReceipt(*receipt_text, error)
                       : std::optional<RegistryAuditReceipt>{};
    if (receipt) {
      if (receipt->checkpoint.tree_size != staged.info.tree_size ||
          receipt->checkpoint.root_hash != staged.info.tree_root ||
          !verifyRegistryAuditReceipt(*receipt, root->root, error))
        return false;
    } else {
      error.clear();
      auto lifecycle = receipt_text
                           ? parseRegistryLifecycleReceipt(*receipt_text, error)
                           : std::optional<RegistryLifecycleReceipt>{};
      if (!lifecycle ||
          lifecycle->checkpoint.tree_size != staged.info.tree_size ||
          lifecycle->checkpoint.root_hash != staged.info.tree_root ||
          !verifyRegistryLifecycleReceipt(*lifecycle, root->root, error))
        return false;
    }
  }
  const auto snapshot_path = index / "trust" / "snapshot.toml";
  const auto timestamp_path = index / "trust" / "timestamp.toml";
  std::error_code trust_ec;
  const bool has_snapshot = std::filesystem::exists(snapshot_path, trust_ec);
  const bool has_timestamp =
      !trust_ec && std::filesystem::exists(timestamp_path, trust_ec);
  if (trust_ec || has_snapshot != has_timestamp ||
      (staged.info.tree_size != 0 && !has_snapshot)) {
    error = "registry backup trust metadata is incomplete";
    return false;
  }
  if (has_snapshot) {
    const auto identity = staged.root / "trust-state";
    RegistryTrustVerificationRequest trust_request;
    trust_request.registry_name = request.registry_name;
    trust_request.registry_index = "backup://" + request.registry_name;
    trust_request.checkout_root = index.string();
    trust_request.identity_store_root = identity.string();
    trust_request.bootstrap_root_keys = request.bootstrap_root_keys;
    trust_request.bootstrap_root_threshold = request.bootstrap_root_threshold;
    trust_request.now_unix_seconds = request.now_unix_seconds;
    if (!verifyRegistryTrust(trust_request, error))
      return false;
  }

  sqlite3_stmt *blob_statement = nullptr;
  if (sqlite3_prepare_v2(database->handle,
                         "SELECT sha256, size FROM blobs ORDER BY sha256", -1,
                         &blob_statement, nullptr) != SQLITE_OK) {
    error =
        sqliteError(database->handle, "failed to read registry backup blobs");
    return false;
  }
  bool blobs_ok = true;
  while (sqlite3_step(blob_statement) == SQLITE_ROW) {
    const std::string digest(
        reinterpret_cast<const char *>(sqlite3_column_text(blob_statement, 0)),
        sqlite3_column_bytes(blob_statement, 0));
    const auto size = sqlite3_column_int64(blob_statement, 1);
    const auto path = staged.root / "blobs" / "sha256" / digest.substr(0, 2) /
                      (digest + ".cpa");
    std::error_code ec;
    blobs_ok = size >= 0 &&
               std::filesystem::file_size(path, ec) ==
                   static_cast<std::uint64_t>(size) &&
               !ec && sha256File(path.string()) == digest;
    if (!blobs_ok)
      break;
  }
  sqlite3_finalize(blob_statement);
  if (!blobs_ok) {
    error = "registry backup CAS does not match its database";
    return false;
  }
  std::uint64_t dangling_variants = 0;
  if (!queryUnsigned(database->handle,
                     "SELECT COUNT(*) FROM variants v LEFT JOIN blobs b ON "
                     "b.sha256 = v.archive_sha256 WHERE b.sha256 IS NULL",
                     dangling_variants, error) ||
      dangling_variants != 0) {
    if (error.empty())
      error = "registry backup contains variants without CAS objects";
    return false;
  }
  sqlite3_stmt *packages = nullptr;
  if (sqlite3_prepare_v2(database->handle,
                         "SELECT DISTINCT package_name, version FROM variants "
                         "ORDER BY package_name, version",
                         -1, &packages, nullptr) != SQLITE_OK) {
    error = sqliteError(database->handle,
                        "failed to enumerate registry backup packages");
    return false;
  }
  bool packages_ok = true;
  while (sqlite3_step(packages) == SQLITE_ROW && packages_ok) {
    const std::string package(
        reinterpret_cast<const char *>(sqlite3_column_text(packages, 0)),
        sqlite3_column_bytes(packages, 0));
    const std::string version(
        reinterpret_cast<const char *>(sqlite3_column_text(packages, 1)),
        sqlite3_column_bytes(packages, 1));
    if (package.empty() || package == "." || package == ".." ||
        package.find_first_of("/\\:") != std::string::npos || version.empty() ||
        version.find_first_of("/\\:") != std::string::npos) {
      packages_ok = false;
      break;
    }
    sqlite3_stmt *variants = nullptr;
    if (sqlite3_prepare_v2(
            database->handle,
            "SELECT entry_text FROM variants WHERE package_name = ? AND "
            "version = ? ORDER BY variant_name",
            -1, &variants, nullptr) != SQLITE_OK ||
        sqlite3_bind_text(variants, 1, package.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_text(variants, 2, version.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
      sqlite3_finalize(variants);
      packages_ok = false;
      break;
    }
    SignedRegistryEntry combined;
    combined.package_name = package;
    combined.version = version;
    while (sqlite3_step(variants) == SQLITE_ROW) {
      const std::string text(
          reinterpret_cast<const char *>(sqlite3_column_text(variants, 0)),
          sqlite3_column_bytes(variants, 0));
      auto entry = parseSignedRegistryEntryText(text, "backup database", error);
      if (!entry || entry->package_name != package ||
          entry->version != version || entry->variants.size() != 1) {
        packages_ok = false;
        break;
      }
      combined.variants.push_back(std::move(entry->variants.front()));
    }
    sqlite3_finalize(variants);
    auto index_text =
        packages_ok
            ? readTextFile(
                  (index / "packages" / package / (version + ".toml")).string(),
                  error)
            : std::optional<std::string>{};
    if (schema == 2) {
      packages_ok =
          index_text && *index_text == renderSignedRegistryEntry(combined);
      continue;
    }
    sqlite3_stmt *state = nullptr;
    if (sqlite3_prepare_v2(
            database->handle,
            "SELECT yanked, state_leaf_index, state_leaf_hash, changed_at, "
            "reason_code FROM release_states WHERE package_name = ? AND "
            "version = ?",
            -1, &state, nullptr) != SQLITE_OK ||
        sqlite3_bind_text(state, 1, package.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_text(state, 2, version.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_step(state) != SQLITE_ROW) {
      sqlite3_finalize(state);
      packages_ok = false;
      break;
    }
    RegistryIndexEntry expected;
    expected.artifact = std::move(combined);
    expected.state = sqlite3_column_int(state, 0) != 0
                         ? RegistryReleaseState::Yanked
                         : RegistryReleaseState::Active;
    expected.state_leaf_index =
        static_cast<std::uint64_t>(sqlite3_column_int64(state, 1));
    expected.state_leaf_hash = reinterpret_cast<const char *>(
        sqlite3_column_text(state, 2));
    expected.changed_at =
        reinterpret_cast<const char *>(sqlite3_column_text(state, 3));
    expected.reason_code =
        reinterpret_cast<const char *>(sqlite3_column_text(state, 4));
    sqlite3_finalize(state);
    packages_ok = index_text && *index_text == renderRegistryIndexEntry(expected);
  }
  sqlite3_finalize(packages);
  if (!packages_ok) {
    if (error.empty())
      error = "registry backup database variants do not match its Git index";
    return false;
  }
  return true;
}

std::optional<StagedBackup>
extractAndVerify(const RegistryBackupRestoreRequest &request,
                 const std::filesystem::path &stage, std::string &error) {
  ZipReader reader;
  if (!mz_zip_reader_init_file(&reader.archive, request.archive_path.c_str(),
                               0)) {
    error = "invalid registry backup archive";
    return std::nullopt;
  }
  reader.open = true;
  const auto count = mz_zip_reader_get_num_files(&reader.archive);
  if (count < 3)
    return error = "registry backup archive is incomplete", std::nullopt;
  mz_zip_archive_file_stat manifest_stat{};
  if (!mz_zip_reader_file_stat(&reader.archive, 0, &manifest_stat) ||
      manifest_stat.m_uncomp_size > kMaximumManifestSize)
    return error = "registry backup manifest is missing or too large",
           std::nullopt;
  auto manifest_name = zipName(reader.archive, 0, error);
  if (!manifest_name || *manifest_name != "manifest.toml")
    return error = "registry backup manifest must be the first member",
           std::nullopt;
  std::string manifest(static_cast<std::size_t>(manifest_stat.m_uncomp_size),
                       '\0');
  if (!mz_zip_reader_extract_to_mem(&reader.archive, 0, manifest.data(),
                                    manifest.size(), 0))
    return error = "failed to extract registry backup manifest", std::nullopt;
  auto info = parseManifest(manifest, error);
  if (!info || info->registry_name != request.registry_name ||
      count != info->files.size() + 1)
    return error.empty()
               ? (error = "registry backup identity or member count mismatch",
                  std::nullopt)
               : std::nullopt;
  std::error_code ec;
  std::filesystem::create_directories(stage, ec);
  std::set<std::string> names{"manifest.toml"};
  for (std::size_t position = 0; position < info->files.size(); ++position) {
    const auto archive_index = static_cast<mz_uint>(position + 1);
    mz_zip_archive_file_stat stat{};
    auto name = zipName(reader.archive, archive_index, error);
    if (!name || *name != info->files[position].relative_path ||
        !safePath(*name) || !names.insert(*name).second ||
        !mz_zip_reader_file_stat(&reader.archive, archive_index, &stat) ||
        stat.m_is_directory || stat.m_is_encrypted || stat.m_method != 0 ||
        isSymlink(stat) || stat.m_uncomp_size != info->files[position].size) {
      error =
          "registry backup contains an unsafe, reordered, or unexpected member";
      return std::nullopt;
    }
    const auto output = stage / *name;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec ||
        !mz_zip_reader_extract_to_file(&reader.archive, archive_index,
                                       output.string().c_str(), 0) ||
        sha256File(output.string()) != info->files[position].sha256) {
      error =
          "registry backup member failed extraction or digest verification: " +
          *name;
      return std::nullopt;
    }
  }
  StagedBackup staged{*info, stage, stage / "state" / "registry.sqlite3",
                      stage / "index.bundle"};
  const auto index = stage / "index";
  if (!cloneBundle(staged.index_bundle, index, staged.info.git_head, error) ||
      !verifyDatabaseAndIndex(request, staged, index, error))
    return std::nullopt;
  return staged;
}

bool emptyOrAbsent(const std::filesystem::path &path, std::string &error) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return !ec;
  if (!std::filesystem::is_directory(path, ec) || ec ||
      !std::filesystem::is_empty(path, ec) || ec) {
    error = "registry backup restore target is not an empty directory: " +
            path.string();
    return false;
  }
  return true;
}

bool relocateBlobPaths(const std::filesystem::path &database_path,
                       const std::filesystem::path &state, std::string &error) {
  auto database = openDatabase(database_path, SQLITE_OPEN_READWRITE, error);
  if (!database)
    return false;
  sqlite3_stmt *read = nullptr;
  if (sqlite3_prepare_v2(database->handle, "SELECT sha256 FROM blobs", -1,
                         &read, nullptr) != SQLITE_OK)
    return error = sqliteError(database->handle, "failed to read blob paths"),
           false;
  std::vector<std::string> digests;
  while (sqlite3_step(read) == SQLITE_ROW)
    digests.emplace_back(
        reinterpret_cast<const char *>(sqlite3_column_text(read, 0)),
        sqlite3_column_bytes(read, 0));
  sqlite3_finalize(read);
  sqlite3_stmt *update = nullptr;
  if (sqlite3_prepare_v2(database->handle,
                         "UPDATE blobs SET path = ? WHERE sha256 = ?", -1,
                         &update, nullptr) != SQLITE_OK)
    return error = sqliteError(database->handle,
                               "failed to prepare blob relocation"),
           false;
  for (const auto &digest : digests) {
    const auto path =
        state / "blobs" / "sha256" / digest.substr(0, 2) / (digest + ".cpa");
    sqlite3_reset(update);
    sqlite3_clear_bindings(update);
    if (sqlite3_bind_text(update, 1, path.string().c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(update, 2, digest.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_step(update) != SQLITE_DONE) {
      error = sqliteError(database->handle,
                          "failed to relocate registry blob path");
      sqlite3_finalize(update);
      return false;
    }
  }
  sqlite3_finalize(update);
  return true;
}

} // namespace

std::optional<RegistryBackupInfo>
createRegistryBackupArchive(const RegistryBackupSource &source,
                            const std::string &archive_path,
                            std::string &error) {
  std::int64_t retain_until = 0;
  const bool recovery_point = !source.recovery_point_id.empty() ||
                              !source.retain_until.empty();
  if (source.registry_name.empty() || source.state_directory.empty() ||
      source.index_worktree.empty() || archive_path.empty() ||
      source.bootstrap_root_threshold == 0 ||
      source.bootstrap_root_threshold > source.bootstrap_root_keys.size() ||
      (recovery_point &&
       (!isHex(source.recovery_point_id, 64) ||
        !parseRegistryUtcTimestamp(source.retain_until, retain_until)))) {
    error = "registry backup request is incomplete";
    return std::nullopt;
  }
  const auto output = std::filesystem::absolute(archive_path);
  const auto stage = temporaryPath(output.parent_path(), ".registry-backup-");
  RemoveTree stage_cleanup{stage};
  std::error_code ec;
  std::filesystem::create_directories(stage / "state", ec);
  if (ec || !snapshotDatabase(std::filesystem::path(source.state_directory) /
                                  "registry.sqlite3",
                              stage / "state" / "registry.sqlite3", error)) {
    return std::nullopt;
  }
  auto database = openDatabase(stage / "state" / "registry.sqlite3",
                               SQLITE_OPEN_READONLY, error);
  RegistryBackupInfo info;
  info.registry_name = source.registry_name;
  info.created_at = timestamp(source.now_unix_seconds);
  info.recovery_point_id = source.recovery_point_id;
  info.retain_until = source.retain_until;
  std::uint64_t pending = 0;
  std::uint64_t leaf_count = 0;
  if (!database || info.created_at.empty() ||
      !queryUnsigned(database->handle, "SELECT version FROM schema_info",
                     info.database_schema, error) ||
      !queryUnsigned(database->handle,
                     "SELECT COUNT(*) FROM mutations WHERE distributed = 0",
                     pending, error) ||
      !queryUnsigned(database->handle, "SELECT COUNT(*) FROM audit_leaves",
                     leaf_count, error) ||
      pending != 0) {
    if (error.empty())
      error = "registry backup cannot capture pending publications";
    return std::nullopt;
  }
  RegistryRootChainVerificationRequest root_request;
  root_request.registry_name = source.registry_name;
  root_request.checkout_root = source.index_worktree;
  root_request.bootstrap_root_keys = source.bootstrap_root_keys;
  root_request.bootstrap_root_threshold = source.bootstrap_root_threshold;
  root_request.now_unix_seconds = source.now_unix_seconds;
  auto root = verifyRegistryRootChain(root_request, error);
  if (!root) {
    return std::nullopt;
  }
  info.tree_size = leaf_count;
  info.tree_root = registryMerkleRoot({});
  info.root_version = root->root.version;
  info.root_sha256 = root->root_sha256;
  if (leaf_count != 0) {
    auto receipt_text = queryText(
        database->handle,
        "SELECT receipt FROM mutations ORDER BY rowid DESC LIMIT 1", error);
    auto receipt = receipt_text
                       ? parseRegistryAuditReceipt(*receipt_text, error)
                       : std::optional<RegistryAuditReceipt>{};
    RegistryAuditCheckpoint checkpoint;
    if (receipt) {
      checkpoint = receipt->checkpoint;
    } else {
      error.clear();
      auto lifecycle = receipt_text
                           ? parseRegistryLifecycleReceipt(*receipt_text, error)
                           : std::optional<RegistryLifecycleReceipt>{};
      if (!lifecycle)
        return std::nullopt;
      checkpoint = lifecycle->checkpoint;
    }
    if (checkpoint.tree_size != leaf_count ||
        checkpoint.root_version != info.root_version ||
        checkpoint.root_sha256 != info.root_sha256) {
      if (error.empty())
        error = "registry backup latest checkpoint does not match current root";
      return std::nullopt;
    }
    info.tree_root = checkpoint.root_hash;
  }
  std::string output_text;
  if (!runGit({"-C", source.index_worktree, "status", "--porcelain=v1",
               "--untracked-files=all"},
              output_text, error) ||
      !output_text.empty() ||
      !runGit({"-C", source.index_worktree, "rev-parse", "HEAD"}, info.git_head,
              error) ||
      !validGitObjectId(info.git_head) ||
      !runGit({"-C", source.index_worktree, "bundle", "create",
               (stage / "index.bundle").string(), "--all"},
              output_text, error) ||
      !runGit({"-C", source.index_worktree, "bundle", "verify",
               (stage / "index.bundle").string()},
              output_text, error)) {
    if (error.empty())
      error = "registry index worktree is dirty";
    return std::nullopt;
  }
  if (!addBackupFile(info, "state/registry.sqlite3",
                     stage / "state" / "registry.sqlite3", error) ||
      !addBackupFile(info, "index.bundle", stage / "index.bundle", error)) {
    return std::nullopt;
  }
  sqlite3_stmt *blobs = nullptr;
  if (sqlite3_prepare_v2(database->handle,
                         "SELECT sha256, size, path FROM blobs ORDER BY sha256",
                         -1, &blobs, nullptr) != SQLITE_OK) {
    error = sqliteError(database->handle, "failed to enumerate registry blobs");
    return std::nullopt;
  }
  bool copied = true;
  while (sqlite3_step(blobs) == SQLITE_ROW && copied) {
    const std::string digest(
        reinterpret_cast<const char *>(sqlite3_column_text(blobs, 0)),
        sqlite3_column_bytes(blobs, 0));
    const auto size = sqlite3_column_int64(blobs, 1);
    const std::string path(
        reinterpret_cast<const char *>(sqlite3_column_text(blobs, 2)),
        sqlite3_column_bytes(blobs, 2));
    const auto relative =
        "blobs/sha256/" + digest.substr(0, 2) + "/" + digest + ".cpa";
    copied = size >= 0 && isHex(digest, 64) &&
             copyFileChecked(path, stage / relative,
                             static_cast<std::uint64_t>(size), digest, error) &&
             addBackupFile(info, relative, stage / relative, error);
  }
  sqlite3_finalize(blobs);
  if (!copied || !writeArchive(stage, info, output, error)) {
    return std::nullopt;
  }
  return info;
}

std::optional<RegistryBackupInfo>
verifyRegistryBackupArchive(const RegistryBackupRestoreRequest &request,
                            std::string &error) {
  const auto stage = temporaryPath(std::filesystem::temp_directory_path(),
                                   "chtholly-registry-verify-");
  auto staged = extractAndVerify(request, stage, error);
  std::error_code ec;
  std::filesystem::remove_all(stage, ec);
  return staged ? std::optional{staged->info} : std::nullopt;
}

std::optional<RegistryBackupInfo>
restoreRegistryBackupArchive(const RegistryBackupRestoreRequest &request,
                             std::string &error) {
  const auto state = std::filesystem::absolute(request.state_directory);
  const auto index = std::filesystem::absolute(request.index_worktree);
  if (state == index || !emptyOrAbsent(state, error) ||
      !emptyOrAbsent(index, error))
    return std::nullopt;
  const auto verify_stage =
      temporaryPath(std::filesystem::temp_directory_path(),
                    "chtholly-registry-restore-verify-");
  auto staged = extractAndVerify(request, verify_stage, error);
  if (!staged) {
    std::error_code ignored;
    std::filesystem::remove_all(verify_stage, ignored);
    return std::nullopt;
  }
  const auto state_stage =
      temporaryPath(state.parent_path(), ".registry-state-");
  const auto index_stage =
      temporaryPath(index.parent_path(), ".registry-index-");
  std::error_code ec;
  std::filesystem::create_directories(state_stage / "blobs", ec);
  std::filesystem::copy_file(staged->database, state_stage / "registry.sqlite3",
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (!ec && std::filesystem::exists(staged->root / "blobs"))
    std::filesystem::copy(staged->root / "blobs", state_stage / "blobs",
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
  const auto cleanup_stages = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(state_stage, ignored);
    std::filesystem::remove_all(index_stage, ignored);
    std::filesystem::remove_all(verify_stage, ignored);
  };
  if (ec ||
      !cloneBundle(staged->index_bundle, index_stage, staged->info.git_head,
                   error) ||
      !relocateBlobPaths(state_stage / "registry.sqlite3", state, error)) {
    if (error.empty())
      error = "failed to stage registry backup restore: " + ec.message();
    cleanup_stages();
    return std::nullopt;
  }
  if (std::filesystem::exists(state))
    std::filesystem::remove(state, ec);
  if (!ec && std::filesystem::exists(index))
    std::filesystem::remove(index, ec);
  if (ec) {
    const auto message = ec.message();
    cleanup_stages();
    error = "failed to prepare empty registry restore targets: " + message;
    return std::nullopt;
  }
  bool index_published = false;
  bool state_published = false;
  std::filesystem::rename(index_stage, index, ec);
  index_published = !ec;
  if (!ec) {
    std::filesystem::rename(state_stage, state, ec);
    state_published = !ec;
  }
  if (ec) {
    const auto message = ec.message();
    std::error_code ignored;
    if (index_published)
      std::filesystem::remove_all(index, ignored);
    if (state_published)
      std::filesystem::remove_all(state, ignored);
    cleanup_stages();
    error = "failed to publish registry restore targets: " + message;
    return std::nullopt;
  }
  auto info = staged->info;
  cleanup_stages();
  return info;
}

} // namespace chtholly
