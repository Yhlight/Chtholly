#include "chtholly/Driver/ToolchainManager.h"

#include "RegistryCrypto.h"
#include "ToolchainSpace.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Driver/SemVer.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include "miniz.h"
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace chtholly {
namespace {

constexpr std::uint64_t kMaximumReleaseFileSize = 2ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaximumReleaseSize = 8ull * 1024 * 1024 * 1024;
constexpr std::size_t kMaximumReleaseFiles = 100000;
constexpr std::size_t kMaximumIndexSize = 16u * 1024u * 1024u;

struct SignatureRecord {
  std::string key_id;
  std::string signature;
};
struct TrustRoot {
  std::uint64_t version = 0;
  std::uint32_t threshold = 0;
  std::map<std::string, registry_crypto::PublicKey> keys;
  std::set<std::string> revoked;
  std::vector<SignatureRecord> signatures;
  std::string payload;
};
struct ReleaseFile {
  std::uint64_t size = 0;
  std::string sha256;
  std::string mode;
  std::string path;
  std::filesystem::path source;
};
struct Release {
  ToolchainReleaseInfo info;
  std::vector<ReleaseFile> files;
  std::vector<SignatureRecord> signatures;
  std::string payload;
  std::uint64_t index_bytes = 0;
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

class RootLock {
public:
  RootLock(const std::filesystem::path &root, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
      error = "failed to create toolchain root: " + ec.message();
      return;
    }
#if defined(_WIN32)
    handle_ =
        CreateFileW((root / ".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    OVERLAPPED overlap{};
    if (handle_ == INVALID_HANDLE_VALUE ||
        !LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlap)) {
      error = "failed to lock toolchain root";
      if (handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
#else
    descriptor_ = ::open((root / ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
      error = "failed to lock toolchain root";
      if (descriptor_ >= 0)
        ::close(descriptor_);
      descriptor_ = -1;
      return;
    }
#endif
    locked_ = true;
  }
  ~RootLock() {
    if (!locked_)
      return;
#if defined(_WIN32)
    OVERLAPPED overlap{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlap);
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

class TreeCleanup {
public:
  explicit TreeCleanup(std::filesystem::path path) : path_(std::move(path)) {}
  ~TreeCleanup() {
    if (active_) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }
  void release() { active_ = false; }

private:
  std::filesystem::path path_;
  bool active_ = true;
};

bool lowerHex(std::string_view value, std::size_t size = 64) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool validReleaseId(std::string_view value) {
  return !value.empty() && value.size() <= 200 && value != "." &&
         value != ".." &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' ||
                  ch == '+';
         });
}

bool validHost(std::string_view value) {
  return !value.empty() && value.size() <= 200 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
         });
}

bool safePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos)
    return false;
  std::size_t start = 0;
  while (start < path.size()) {
    const auto slash = path.find('/', start);
    const auto part =
        path.substr(start, slash == std::string_view::npos ? path.size() - start
                                                           : slash - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return true;
}

template <typename Integer>
bool parseUnsigned(std::string_view text, Integer &value) {
  const auto [end, status] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return status == std::errc{} && end == text.data() + text.size();
}

std::vector<std::string_view> fields(std::string_view line) {
  std::vector<std::string_view> result;
  while (true) {
    const auto tab = line.find('\t');
    result.push_back(line.substr(0, tab));
    if (tab == std::string_view::npos)
      break;
    line.remove_prefix(tab + 1);
  }
  return result;
}

std::optional<std::vector<unsigned char>> readBinary(const std::string &path,
                                                     std::string &error) {
  std::ifstream input(pathForFileSystem(path), std::ios::binary);
  if (!input) {
    error = "failed to open file: '" + path + "'";
    return std::nullopt;
  }
  input.seekg(0, std::ios::end);
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) >
                     std::numeric_limits<std::size_t>::max()) {
    error = "file is too large: '" + path + "'";
    return std::nullopt;
  }
  std::vector<unsigned char> data(static_cast<std::size_t>(end));
  input.seekg(0);
  if (!data.empty())
    input.read(reinterpret_cast<char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
  if (!input) {
    error = "failed to read file: '" + path + "'";
    return std::nullopt;
  }
  return data;
}

bool writeBinary(const std::filesystem::path &path, const void *data,
                 std::size_t size, std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "failed to create output directory '" +
            path.parent_path().string() + "': " + ec.message();
    return false;
  }
  std::ofstream output(pathForFileSystem(path.string()),
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to create file: '" + path.string() + "'";
    return false;
  }
  if (size)
    output.write(static_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
  if (!output) {
    error = "failed to write file: '" + path.string() + "'";
    return false;
  }
  return true;
}

bool writeNativeBinary(const std::filesystem::path &path, const void *data,
                       std::size_t size, std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "failed to create output directory '" +
            path.parent_path().string() + "': " + ec.message();
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to write file: '" + path.string() + "'";
    return false;
  }
  if (size)
    output.write(static_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
  if (!output) {
    error = "failed to write file: '" + path.string() + "'";
    return false;
  }
  return true;
}

std::string digest(const std::vector<unsigned char> &data) {
  return sha256Hex(std::string_view(reinterpret_cast<const char *>(data.data()),
                                    data.size()));
}

bool atomicWrite(const std::filesystem::path &path, std::string_view text,
                 std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "failed to create state directory: " + ec.message();
    return false;
  }
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary = path.string() + ".tmp-" + std::to_string(nonce);
  if (!writeTextFile(temporary, std::string(text), error))
    return false;
  if (!replaceFile(temporary, path.string(), ec)) {
    error = "failed to atomically replace state: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}

std::optional<registry_crypto::PublicKey>
loadPublicKeyFile(const std::string &path, std::string &error) {
  auto text = readTextFile(path, error);
  if (!text)
    return std::nullopt;
  std::istringstream input(*text);
  std::string header, id_line, key_line, end, trailing;
  if (!std::getline(input, header) || !std::getline(input, id_line) ||
      !std::getline(input, key_line) || !std::getline(input, end) ||
      std::getline(input, trailing) || header != "chtholly-ed25519-public-v1" ||
      !id_line.starts_with("key-id\t") ||
      !key_line.starts_with("public-key\t") || end != "end") {
    error = "invalid Chtholly Ed25519 public key file: '" + path + "'";
    return std::nullopt;
  }
  auto key = registry_crypto::parsePublicKey(key_line.substr(11));
  if (!key || id_line.substr(7) != registry_crypto::publicKeyId(*key)) {
    error = "public key ID does not match key material: '" + path + "'";
    return std::nullopt;
  }
  return key;
}

std::string signatureInput(std::string_view domain, std::string_view payload,
                           std::string_view key_id) {
  return std::string(domain) + "\n" + std::string(payload) + "key-id\t" +
         std::string(key_id) + "\n";
}

bool signPayload(std::string_view domain, std::string_view payload,
                 const std::vector<std::string> &secret_paths,
                 std::vector<SignatureRecord> &signatures, std::string &error) {
  if (!registry_crypto::initialize(error))
    return false;
  std::map<std::string, SignatureRecord> unique;
  for (const auto &path : secret_paths) {
    registry_crypto::SecretKey secret{};
    registry_crypto::PublicKey key{};
    if (!registry_crypto::loadSecretKeyFile(path, secret, key, error))
      return false;
    const auto key_id = registry_crypto::publicKeyId(key);
    const auto input = signatureInput(domain, payload, key_id);
    registry_crypto::Signature signature{};
    const auto status = crypto_sign_detached(
        signature.data(), nullptr,
        reinterpret_cast<const unsigned char *>(input.data()), input.size(),
        secret.data());
    sodium_memzero(secret.data(), secret.size());
    if (status != 0) {
      error = "failed to sign toolchain contract";
      return false;
    }
    unique.emplace(
        key_id,
        SignatureRecord{key_id, registry_crypto::encodeSignature(signature)});
  }
  signatures.clear();
  for (auto &[id, record] : unique)
    signatures.push_back(std::move(record));
  return !signatures.empty() ||
         (error = "at least one secret key is required", false);
}

bool verifyThreshold(std::string_view domain, std::string_view payload,
                     const std::vector<SignatureRecord> &signatures,
                     const TrustRoot &root, std::string &error) {
  if (!registry_crypto::initialize(error))
    return false;
  std::set<std::string> valid;
  for (const auto &record : signatures) {
    const auto key = root.keys.find(record.key_id);
    if (key == root.keys.end() || root.revoked.contains(record.key_id))
      continue;
    registry_crypto::Signature signature{};
    const auto input = signatureInput(domain, payload, record.key_id);
    if (registry_crypto::decodeSignature(record.signature, signature) &&
        crypto_sign_verify_detached(
            signature.data(),
            reinterpret_cast<const unsigned char *>(input.data()), input.size(),
            key->second.data()) == 0)
      valid.insert(record.key_id);
  }
  if (valid.size() < root.threshold) {
    error = "toolchain signature threshold was not satisfied (" +
            std::to_string(valid.size()) + "/" +
            std::to_string(root.threshold) + ")";
    return false;
  }
  return true;
}

std::optional<TrustRoot> parseTrustRoot(std::string_view text,
                                        std::string &error) {
  TrustRoot root;
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != "chtholly-toolchain-root-v1") {
    error = "invalid toolchain trust root header";
    return std::nullopt;
  }
  root.payload = line + "\n";
  bool signed_end = false, end = false;
  std::string previous_key, previous_revoked, previous_signature;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      error = "trust root is not canonical";
      return std::nullopt;
    }
    const auto item = fields(line);
    if (!signed_end) {
      root.payload += line + "\n";
      if (line == "signed-end") {
        signed_end = true;
        continue;
      }
      if (item.size() == 2 && item[0] == "version" && root.version == 0) {
        if (!parseUnsigned(item[1], root.version) || root.version == 0)
          break;
      } else if (item.size() == 2 && item[0] == "threshold" &&
                 root.threshold == 0) {
        if (!parseUnsigned(item[1], root.threshold) || root.threshold == 0)
          break;
      } else if (item.size() == 3 && item[0] == "key") {
        auto key = registry_crypto::parsePublicKey(item[2]);
        if (!key || item[1] != registry_crypto::publicKeyId(*key) ||
            (!previous_key.empty() && previous_key >= item[1]))
          break;
        previous_key = std::string(item[1]);
        root.keys.emplace(previous_key, *key);
      } else if (item.size() == 2 && item[0] == "revoked") {
        if ((!previous_revoked.empty() && previous_revoked >= item[1]) ||
            !root.keys.contains(std::string(item[1])))
          break;
        previous_revoked = std::string(item[1]);
        root.revoked.insert(previous_revoked);
      } else
        break;
    } else if (line == "end") {
      end = true;
      break;
    } else if (item.size() == 3 && item[0] == "signature" &&
               (previous_signature.empty() || previous_signature < item[1])) {
      previous_signature = std::string(item[1]);
      root.signatures.push_back({previous_signature, std::string(item[2])});
    } else
      break;
  }
  std::string trailing;
  if (!signed_end || !end || std::getline(input, trailing) ||
      root.version == 0 || root.threshold == 0 ||
      root.threshold > root.keys.size() - root.revoked.size() ||
      root.signatures.empty()) {
    error = "invalid or non-canonical toolchain trust root";
    return std::nullopt;
  }
  return root;
}

std::string renderTrustRootPayload(const TrustRoot &root) {
  std::ostringstream out;
  out << "chtholly-toolchain-root-v1\nversion\t" << root.version
      << "\nthreshold\t" << root.threshold << '\n';
  for (const auto &[id, key] : root.keys) {
    std::string encoded(
        sodium_base64_encoded_len(key.size(), sodium_base64_VARIANT_ORIGINAL),
        '\0');
    sodium_bin2base64(encoded.data(), encoded.size(), key.data(), key.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    encoded.resize(encoded.find('\0'));
    out << "key\t" << id << "\ted25519:" << encoded << '\n';
  }
  for (const auto &id : root.revoked)
    out << "revoked\t" << id << '\n';
  out << "signed-end\n";
  return out.str();
}

std::string renderSigned(std::string_view payload,
                         const std::vector<SignatureRecord> &signatures) {
  std::ostringstream out;
  out << payload;
  for (const auto &signature : signatures)
    out << "signature\t" << signature.key_id << '\t' << signature.signature
        << '\n';
  out << "end\n";
  return out.str();
}

std::optional<TrustRoot> loadInstalledRoot(const std::string &manager_root,
                                           std::string &error) {
  auto text = readTextFile(
      (std::filesystem::path(manager_root) / "trust" / "root-v1").string(),
      error);
  return text ? parseTrustRoot(*text, error) : std::nullopt;
}

std::string renderReleasePayload(const Release &release) {
  std::ostringstream out;
  out << "chtholly-toolchain-release-v1\nrelease-id\t"
      << release.info.release_id << "\nversion\t" << release.info.version
      << "\nsource-commit\t" << release.info.source_commit << "\nhost\t"
      << release.info.host << "\nabi\tv2\nmanager-protocol\t1\nfile-count\t"
      << release.files.size() << '\n';
  for (const auto &file : release.files)
    out << "file\t" << file.size << '\t' << file.sha256 << '\t' << file.mode
        << '\t' << file.path << '\n';
  out << "signed-end\n";
  return out.str();
}

std::optional<Release> parseRelease(std::string_view text, std::string &error) {
  Release release;
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != "chtholly-toolchain-release-v1") {
    error = "invalid toolchain release header";
    return std::nullopt;
  }
  release.payload = line + "\n";
  bool signed_end = false, end = false;
  bool abi = false, manager_protocol = false;
  std::size_t declared_files = std::numeric_limits<std::size_t>::max();
  std::uint64_t total = 0;
  std::string previous_path, previous_signature;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      break;
    const auto item = fields(line);
    if (!signed_end) {
      release.payload += line + "\n";
      if (line == "signed-end") {
        signed_end = true;
        continue;
      }
      if (item.size() == 2 && item[0] == "release-id" &&
          release.info.release_id.empty())
        release.info.release_id = item[1];
      else if (item.size() == 2 && item[0] == "version" &&
               release.info.version.empty())
        release.info.version = item[1];
      else if (item.size() == 2 && item[0] == "source-commit" &&
               release.info.source_commit.empty())
        release.info.source_commit = item[1];
      else if (item.size() == 2 && item[0] == "host" &&
               release.info.host.empty())
        release.info.host = item[1];
      else if (item.size() == 2 && item[0] == "abi" && item[1] == "v2" && !abi)
        abi = true;
      else if (item.size() == 2 && item[0] == "manager-protocol" &&
               item[1] == "1" && !manager_protocol)
        manager_protocol = true;
      else if (item.size() == 2 && item[0] == "file-count" &&
               declared_files == std::numeric_limits<std::size_t>::max()) {
        if (!parseUnsigned(item[1], declared_files) ||
            declared_files > kMaximumReleaseFiles)
          break;
      } else if (item.size() == 5 && item[0] == "file") {
        ReleaseFile file;
        if (!parseUnsigned(item[1], file.size) ||
            file.size > kMaximumReleaseFileSize || !lowerHex(item[2]) ||
            (item[3] != "0644" && item[3] != "0755") || !safePath(item[4]) ||
            (!previous_path.empty() && previous_path >= item[4]) ||
            total > kMaximumReleaseSize - file.size)
          break;
        file.sha256 = item[2];
        file.mode = item[3];
        file.path = item[4];
        previous_path = file.path;
        total += file.size;
        release.files.push_back(std::move(file));
      } else
        break;
    } else if (line == "end") {
      end = true;
      break;
    } else if (item.size() == 3 && item[0] == "signature" &&
               (previous_signature.empty() || previous_signature < item[1])) {
      previous_signature = item[1];
      release.signatures.push_back({previous_signature, std::string(item[2])});
    } else
      break;
  }
  std::string trailing, semver_error;
  if (!signed_end || !end || std::getline(input, trailing) || !abi ||
      !manager_protocol || !validReleaseId(release.info.release_id) ||
      !parseSemVer(release.info.version, semver_error) ||
      !lowerHex(release.info.source_commit, 40) ||
      !validHost(release.info.host) || declared_files != release.files.size() ||
      release.signatures.empty() ||
      release.info.release_id !=
          release.info.version + "+" + release.info.source_commit) {
    error = "invalid or non-canonical toolchain release index";
    return std::nullopt;
  }
  release.info.file_count = release.files.size();
  return release;
}

std::string zipError(mz_zip_archive &archive) {
  return mz_zip_get_error_string(mz_zip_get_last_error(&archive));
}

std::optional<std::vector<unsigned char>> extract(mz_zip_archive &archive,
                                                  mz_uint index,
                                                  std::uint64_t size,
                                                  std::string &error) {
  if (size > std::numeric_limits<std::size_t>::max()) {
    error = "release entry is too large";
    return std::nullopt;
  }
  std::vector<unsigned char> data(static_cast<std::size_t>(size));
  if (!mz_zip_reader_extract_to_mem(&archive, index, data.data(), data.size(),
                                    0)) {
    error = "failed to extract release entry: " + zipError(archive);
    return std::nullopt;
  }
  return data;
}

std::optional<Release> inspectArchive(const std::string &archive_path,
                                      const TrustRoot *trust,
                                      std::string_view expected_host,
                                      std::string &error) {
  auto archive_data = readBinary(archive_path, error);
  if (!archive_data)
    return std::nullopt;
  ZipReader reader;
  if (!mz_zip_reader_init_mem(&reader.archive, archive_data->data(),
                              archive_data->size(), 0)) {
    error = "invalid toolchain release archive: " + zipError(reader.archive);
    return std::nullopt;
  }
  reader.open = true;
  const auto count = mz_zip_reader_get_num_files(&reader.archive);
  if (count < 2 || count > kMaximumReleaseFiles + 1) {
    error = "release archive has an invalid entry count";
    return std::nullopt;
  }
  std::set<std::string> names;
  std::vector<mz_zip_archive_file_stat> stats(count);
  for (mz_uint index = 0; index < count; ++index) {
    if (!mz_zip_reader_file_stat(&reader.archive, index, &stats[index]) ||
        !safePath(stats[index].m_filename) ||
        !names.insert(stats[index].m_filename).second ||
        stats[index].m_is_directory || stats[index].m_is_encrypted ||
        stats[index].m_method != 0 ||
        (((stats[index].m_external_attr >> 16) & 0170000u) == 0120000u)) {
      error = "release archive contains an unsafe, duplicate, encrypted, "
              "compressed, or non-file entry";
      return std::nullopt;
    }
  }
  if (std::string_view(stats[0].m_filename) != "release.index" ||
      stats[0].m_uncomp_size > kMaximumIndexSize) {
    error = "release archive canonical index is missing or too large";
    return std::nullopt;
  }
  auto index_data = extract(reader.archive, 0, stats[0].m_uncomp_size, error);
  if (!index_data)
    return std::nullopt;
  auto release = parseRelease(
      std::string_view(reinterpret_cast<const char *>(index_data->data()),
                       index_data->size()),
      error);
  if (!release || count != release->files.size() + 1)
    return std::nullopt;
  release->index_bytes = index_data->size();
  if (!expected_host.empty() && release->info.host != expected_host) {
    error = "toolchain release host '" + release->info.host +
            "' does not match this host '" + std::string(expected_host) + "'";
    return std::nullopt;
  }
  if (trust &&
      !verifyThreshold("chtholly-toolchain-release-signature-v1",
                       release->payload, release->signatures, *trust, error))
    return std::nullopt;
  for (std::size_t position = 0; position < release->files.size(); ++position) {
    const auto index = static_cast<mz_uint>(position + 1);
    const auto expected_name = "payload/" + release->files[position].path;
    if (stats[index].m_filename != expected_name ||
        stats[index].m_uncomp_size != release->files[position].size) {
      error = "release archive entries do not match its signed index";
      return std::nullopt;
    }
    auto data =
        extract(reader.archive, index, stats[index].m_uncomp_size, error);
    if (!data || digest(*data) != release->files[position].sha256) {
      if (error.empty())
        error =
            "release archive payload SHA-256 mismatch: '" + expected_name + "'";
      return std::nullopt;
    }
  }
  release->info.archive_sha256 = digest(*archive_data);
  return release;
}

bool extractArchive(const std::string &archive_path, const Release &release,
                    const std::filesystem::path &destination,
                    std::string &error) {
  auto archive_data = readBinary(archive_path, error);
  if (!archive_data)
    return false;
  if (digest(*archive_data) != release.info.archive_sha256) {
    error = "toolchain release changed between verification and installation";
    return false;
  }
  ZipReader reader;
  if (!mz_zip_reader_init_mem(&reader.archive, archive_data->data(),
                              archive_data->size(), 0)) {
    error = "failed to reopen toolchain release archive";
    return false;
  }
  reader.open = true;
  for (std::size_t position = 0; position < release.files.size(); ++position) {
    const auto &file = release.files[position];
    auto relative = std::filesystem::path(file.path);
    relative.make_preferred();
    const auto output_path = destination / relative;
    auto data = extract(reader.archive, static_cast<mz_uint>(position + 1),
                        file.size, error);
    if (!data || digest(*data) != file.sha256 ||
        !writeNativeBinary(output_path, data->data(), data->size(), error))
      return false;
#if !defined(_WIN32)
    std::error_code ec;
    std::filesystem::permissions(output_path,
                                 file.mode == "0755"
                                     ? std::filesystem::perms::owner_all |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::group_exec |
                                           std::filesystem::perms::others_read |
                                           std::filesystem::perms::others_exec
                                     : std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
      error = "failed to restore release file permissions: " + ec.message();
      return false;
    }
#endif
  }
  return atomicWrite(destination / ".release-sha256",
                     release.info.archive_sha256 + "\n", error);
}

std::filesystem::path normalizedRoot(const std::string &root) {
  return pathForFileSystemTreeRoot(root);
}

std::optional<std::string> readActive(const std::filesystem::path &root,
                                      std::string &error) {
  const auto path = root / "state" / "active-v1";
  if (!std::filesystem::exists(path))
    return std::string{};
  auto text = readTextFile(path.string(), error);
  if (!text)
    return std::nullopt;
  while (!text->empty() && (text->back() == '\n' || text->back() == '\r'))
    text->pop_back();
  if (!text->empty() && !validReleaseId(*text)) {
    error = "invalid active toolchain state";
    return std::nullopt;
  }
  return *text;
}

std::optional<std::vector<std::string>>
readHistory(const std::filesystem::path &root, std::string &error) {
  std::vector<std::string> history;
  const auto path = root / "state" / "history-v1";
  if (!std::filesystem::exists(path))
    return history;
  auto text = readTextFile(path.string(), error);
  if (!text)
    return std::nullopt;
  std::istringstream input(*text);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!validReleaseId(line)) {
      error = "invalid toolchain activation history";
      return std::nullopt;
    }
    history.push_back(std::move(line));
  }
  return history;
}

bool writeHistory(const std::filesystem::path &root,
                  const std::vector<std::string> &history, std::string &error) {
  std::ostringstream out;
  for (const auto &entry : history)
    out << entry << '\n';
  return atomicWrite(root / "state" / "history-v1", out.str(), error);
}

bool activateUnlocked(const std::filesystem::path &root,
                      const std::string &release_id, std::string &error) {
  if (!validReleaseId(release_id) ||
      !std::filesystem::is_directory(root / "generations" / release_id)) {
    error = "toolchain generation is not installed: '" + release_id + "'";
    return false;
  }
  auto active = readActive(root, error);
  auto history = readHistory(root, error);
  if (!active || !history)
    return false;
  if (*active == release_id)
    return true;
  if (!active->empty())
    history->push_back(*active);
  if (!writeHistory(root, *history, error))
    return false;
  return atomicWrite(root / "state" / "active-v1", release_id + "\n", error);
}

} // namespace

bool generateToolchainSigningKeyFiles(const std::string &secret_key_path,
                                      const std::string &public_key_path,
                                      std::string &error) {
  return registry_crypto::generateSigningKeyFiles(secret_key_path,
                                                  public_key_path, error);
}

bool createToolchainTrustRoot(const ToolchainTrustRootRequest &request,
                              std::string &error) {
  error.clear();
  if (request.version == 0 || request.threshold == 0 ||
      request.public_key_paths.empty() || request.secret_key_paths.empty()) {
    error =
        "trust root requires version, threshold, public keys, and secret keys";
    return false;
  }
  TrustRoot root;
  root.version = request.version;
  root.threshold = request.threshold;
  for (const auto &path : request.public_key_paths) {
    auto key = loadPublicKeyFile(path, error);
    if (!key)
      return false;
    root.keys.emplace(registry_crypto::publicKeyId(*key), *key);
  }
  root.revoked.insert(request.revoked_key_ids.begin(),
                      request.revoked_key_ids.end());
  if (root.threshold > root.keys.size() - root.revoked.size() ||
      !std::all_of(root.revoked.begin(), root.revoked.end(),
                   [&](const auto &id) { return root.keys.contains(id); })) {
    error = "trust root threshold or revoked key set is invalid";
    return false;
  }
  root.payload = renderTrustRootPayload(root);
  if (!signPayload("chtholly-toolchain-root-signature-v1", root.payload,
                   request.secret_key_paths, root.signatures, error) ||
      !verifyThreshold("chtholly-toolchain-root-signature-v1", root.payload,
                       root.signatures, root, error))
    return false;
  return atomicWrite(request.output_path,
                     renderSigned(root.payload, root.signatures), error);
}

bool installToolchainTrustRoot(const std::string &manager_root,
                               const std::string &root_file, bool initialize,
                               std::string &error) {
  auto text = readTextFile(root_file, error);
  if (!text)
    return false;
  auto candidate = parseTrustRoot(*text, error);
  if (!candidate)
    return false;
  if (!verifyThreshold("chtholly-toolchain-root-signature-v1",
                       candidate->payload, candidate->signatures, *candidate,
                       error))
    return false;
  const auto root = normalizedRoot(manager_root);
  RootLock lock(root, error);
  if (!lock.locked())
    return false;
  const auto installed_path = root / "trust" / "root-v1";
  if (initialize) {
    if (std::filesystem::exists(installed_path)) {
      error = "toolchain trust root is already initialized";
      return false;
    }
  } else {
    auto installed = loadInstalledRoot(root.string(), error);
    if (!installed)
      return false;
    if (candidate->version <= installed->version) {
      error = "toolchain trust root version must increase";
      return false;
    }
    if (!verifyThreshold("chtholly-toolchain-root-signature-v1",
                         candidate->payload, candidate->signatures, *installed,
                         error))
      return false;
  }
  return atomicWrite(installed_path, *text, error);
}

std::optional<std::string>
inspectToolchainTrustRoot(const std::string &manager_root, std::string &error) {
  auto root = loadInstalledRoot(manager_root, error);
  if (!root)
    return std::nullopt;
  if (!verifyThreshold("chtholly-toolchain-root-signature-v1", root->payload,
                       root->signatures, *root, error))
    return std::nullopt;
  std::ostringstream out;
  out << "root-version\t" << root->version << "\nthreshold\t" << root->threshold
      << "\nkeys\t" << root->keys.size() << "\nrevoked\t"
      << root->revoked.size() << '\n';
  return out.str();
}

std::optional<ToolchainReleaseInfo>
packageToolchainRelease(const ToolchainPackageRequest &request,
                        std::string &error) {
  error.clear();
  Release release;
  std::string version_error;
  if (!parseSemVer(request.version, version_error) ||
      !lowerHex(request.source_commit, 40) || !validHost(request.host) ||
      request.secret_key_paths.empty()) {
    error = "release package requires version, full 40-hex commit, host, and "
            "secret keys";
    return std::nullopt;
  }
  release.info.version = request.version;
  release.info.source_commit = request.source_commit;
  release.info.host = request.host;
  release.info.release_id = request.version + "+" + request.source_commit;
  if (!validReleaseId(release.info.release_id)) {
    error = "release ID contains unsupported characters";
    return std::nullopt;
  }
  const auto tree = pathForFileSystemTreeRoot(request.install_tree);
  std::error_code ec;
  if (!std::filesystem::is_directory(tree, ec)) {
    error = "install tree does not exist";
    return std::nullopt;
  }
  std::uint64_t total = 0;
  for (std::filesystem::recursive_directory_iterator it(tree, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_symlink(ec)) {
      error = "install tree contains a symbolic link";
      return std::nullopt;
    }
    if (!it->is_regular_file(ec))
      continue;
    ReleaseFile file;
    file.source = it->path();
    file.path =
        std::filesystem::relative(it->path(), tree, ec).generic_string();
    file.size = it->file_size(ec);
    if (ec || !safePath(file.path) || file.size > kMaximumReleaseFileSize ||
        total > kMaximumReleaseSize - file.size) {
      error = "install tree contains an unsafe or oversized file";
      return std::nullopt;
    }
    auto digest_value = sha256File(file.source.string());
    if (!digest_value) {
      error = "failed to hash install tree file";
      return std::nullopt;
    }
    file.sha256 = *digest_value;
    const auto permissions = it->status(ec).permissions();
    file.mode = !ec && (permissions & (std::filesystem::perms::owner_exec |
                                       std::filesystem::perms::group_exec |
                                       std::filesystem::perms::others_exec)) !=
                            std::filesystem::perms::none
                    ? "0755"
                    : "0644";
#if defined(_WIN32)
    if (it->path().extension() == ".exe")
      file.mode = "0755";
#endif
    total += file.size;
    release.files.push_back(std::move(file));
  }
  if (ec || release.files.empty() ||
      release.files.size() > kMaximumReleaseFiles) {
    error = ec ? "failed to enumerate install tree: " + ec.message()
               : "install tree is empty or too large";
    return std::nullopt;
  }
  std::sort(release.files.begin(), release.files.end(),
            [](const auto &a, const auto &b) { return a.path < b.path; });
  release.payload = renderReleasePayload(release);
  if (!signPayload("chtholly-toolchain-release-signature-v1", release.payload,
                   request.secret_key_paths, release.signatures, error))
    return std::nullopt;
  const auto signed_index = renderSigned(release.payload, release.signatures);
  ZipWriter writer;
  if (!mz_zip_writer_init_heap(&writer.archive, 0, 0)) {
    error = "failed to initialize release archive";
    return std::nullopt;
  }
  writer.open = true;
  if (!mz_zip_writer_add_mem_ex_v2(&writer.archive, "release.index",
                                   signed_index.data(), signed_index.size(),
                                   nullptr, 0, MZ_NO_COMPRESSION, 0, 0, nullptr,
                                   nullptr, 0, nullptr, 0)) {
    error = "failed to add release index: " + zipError(writer.archive);
    return std::nullopt;
  }
  for (const auto &file : release.files) {
    auto data = readBinary(file.source.string(), error);
    if (!data || digest(*data) != file.sha256) {
      if (error.empty())
        error = "install tree changed while packing";
      return std::nullopt;
    }
    const auto name = "payload/" + file.path;
    if (!mz_zip_writer_add_mem_ex_v2(
            &writer.archive, name.c_str(), data->data(), data->size(), nullptr,
            0, MZ_NO_COMPRESSION, 0, 0, nullptr, nullptr, 0, nullptr, 0)) {
      error = "failed to add release payload: " + zipError(writer.archive);
      return std::nullopt;
    }
  }
  void *archive = nullptr;
  std::size_t archive_size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&writer.archive, &archive,
                                           &archive_size)) {
    error = "failed to finalize release archive: " + zipError(writer.archive);
    return std::nullopt;
  }
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary = request.archive_path + ".tmp-" + std::to_string(nonce);
  const bool wrote = writeBinary(temporary, archive, archive_size, error);
  mz_free(archive);
  mz_zip_writer_end(&writer.archive);
  writer.open = false;
  if (!wrote)
    return std::nullopt;
  if (!replaceFile(temporary, request.archive_path, ec)) {
    error = "failed to replace release archive: " + ec.message();
    removeFile(temporary, ec);
    return std::nullopt;
  }
  release.info.file_count = release.files.size();
  release.info.archive_sha256 =
      sha256File(request.archive_path).value_or(std::string{});
  return release.info;
}

std::optional<ToolchainReleaseInfo>
verifyToolchainRelease(const std::string &archive_path,
                       const std::string &manager_root,
                       const std::string &expected_host, std::string &error) {
  auto trust = loadInstalledRoot(manager_root, error);
  if (!trust)
    return std::nullopt;
  auto release = inspectArchive(archive_path, &*trust, expected_host, error);
  return release ? std::optional(release->info) : std::nullopt;
}

std::optional<ToolchainReleaseInfo> installToolchainRelease(
    const std::string &archive_path, const std::string &manager_root,
    const std::string &expected_host, bool upgrade, std::string &error) {
  const auto root = normalizedRoot(manager_root);
  RootLock lock(root, error);
  if (!lock.locked())
    return std::nullopt;
  auto trust = loadInstalledRoot(root.string(), error);
  if (!trust)
    return std::nullopt;
  auto release = inspectArchive(archive_path, &*trust, expected_host, error);
  if (!release)
    return std::nullopt;
  const auto generation = root / "generations" / release->info.release_id;
  if (std::filesystem::exists(generation)) {
    auto recorded =
        readTextFile((generation / ".release-sha256").string(), error);
    if (!recorded || *recorded != release->info.archive_sha256 + "\n") {
      error =
          "installed generation ID conflicts with different release content";
      return std::nullopt;
    }
  } else {
    const auto staging_path = root.parent_path().empty()
                                  ? std::filesystem::path(".")
                                  : root.parent_path();
    std::error_code space_error;
    const auto space = std::filesystem::space(
        pathForFileSystem(staging_path.string()), space_error);
    const auto available = space_error
                               ? std::uint64_t{0}
                               : static_cast<std::uint64_t>(space.available);
    std::uint64_t payload_bytes = 0;
    for (const auto &file : release->files) {
      if (payload_bytes > std::numeric_limits<std::uint64_t>::max() -
                              file.size) {
        error = "insufficient-space: space-required-bytes=" +
                std::to_string(std::numeric_limits<std::uint64_t>::max()) +
                " space-available-bytes=" + std::to_string(available) +
                " space-path=" + staging_path.lexically_normal().string();
        return std::nullopt;
      }
      payload_bytes += file.size;
    }
    const auto estimate = toolchain_internal::estimateToolchainInstallSpace(
        payload_bytes, release->index_bytes, available);
    const auto normalized_staging = staging_path.lexically_normal().string();
    if (space_error || !estimate.sufficient) {
      error = "insufficient-space: space-required-bytes=" +
              std::to_string(estimate.required_bytes) +
              " space-available-bytes=" +
              std::to_string(estimate.available_bytes) + " space-path=" +
              normalized_staging;
      if (space_error)
        error += " space-error=" + space_error.message();
      return std::nullopt;
    }
    release->info.space_payload_bytes = estimate.payload_bytes;
    release->info.space_index_bytes = estimate.index_bytes;
    release->info.space_required_bytes = estimate.required_bytes;
    release->info.space_available_bytes = estimate.available_bytes;
    release->info.space_path = normalized_staging;
    release->info.space_sufficient = estimate.sufficient;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto staging =
        root / "staging" /
        (release->info.release_id + ".tmp-" + std::to_string(nonce));
    TreeCleanup cleanup(staging);
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec || !extractArchive(archive_path, *release, staging, error)) {
      if (error.empty())
        error = "failed to stage toolchain generation: " + ec.message();
      return std::nullopt;
    }
    std::filesystem::create_directories(generation.parent_path(), ec);
    std::filesystem::rename(staging, generation, ec);
    if (ec) {
      error =
          "failed to publish immutable toolchain generation: " + ec.message();
      return std::nullopt;
    }
    cleanup.release();
  }
  if (upgrade) {
#if defined(_WIN32)
    const auto compiler = generation / "bin" / "chthollyc.exe";
#else
    const auto compiler = generation / "bin" / "chthollyc";
#endif
    ProcessRunOptions options;
    options.timeout_milliseconds = 30000;
    options.max_stdout_bytes = 1024 * 1024;
    options.max_stderr_bytes = 1024 * 1024;
    auto probe =
        std::filesystem::is_regular_file(compiler)
            ? runProcess(compiler.string(), {"--version"}, options, error)
            : std::optional<CommandResult>{};
    if (!probe || probe->exit_code != 0) {
      if (error.empty())
        error = "installed toolchain generation failed its compiler preflight";
      return std::nullopt;
    }
    if (!activateUnlocked(root, release->info.release_id, error))
      return std::nullopt;
  }
  return release->info;
}

bool activateToolchainRelease(const std::string &manager_root,
                              const std::string &release_id,
                              std::string &error) {
  const auto root = normalizedRoot(manager_root);
  RootLock lock(root, error);
  return lock.locked() && activateUnlocked(root, release_id, error);
}

std::optional<std::string>
rollbackToolchainRelease(const std::string &manager_root, std::string &error) {
  const auto root = normalizedRoot(manager_root);
  RootLock lock(root, error);
  if (!lock.locked())
    return std::nullopt;
  auto history = readHistory(root, error);
  if (!history)
    return std::nullopt;
  while (!history->empty()) {
    auto target = history->back();
    history->pop_back();
    if (!std::filesystem::is_directory(root / "generations" / target))
      continue;
    if (!writeHistory(root, *history, error) ||
        !atomicWrite(root / "state" / "active-v1", target + "\n", error))
      return std::nullopt;
    return target;
  }
  error = "toolchain rollback history is empty";
  return std::nullopt;
}

std::optional<std::vector<std::string>>
listToolchainReleases(const std::string &manager_root, std::string &error) {
  const auto root = normalizedRoot(manager_root);
  RootLock lock(root, error);
  if (!lock.locked())
    return std::nullopt;
  auto active = readActive(root, error);
  if (!active)
    return std::nullopt;
  std::vector<std::string> result;
  std::error_code ec;
  const auto generations = root / "generations";
  if (!std::filesystem::exists(generations))
    return result;
  for (const auto &entry : std::filesystem::directory_iterator(generations, ec))
    if (entry.is_directory() &&
        validReleaseId(entry.path().filename().string()))
      result.push_back(entry.path().filename().string() +
                       (entry.path().filename().string() == *active
                            ? "\tactive"
                            : "\tinactive"));
  if (ec) {
    error = "failed to enumerate toolchain generations: " + ec.message();
    return std::nullopt;
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool removeToolchainRelease(const std::string &manager_root,
                            const std::string &release_id, std::string &error) {
  if (!validReleaseId(release_id)) {
    error = "invalid toolchain release ID";
    return false;
  }
  const auto root = normalizedRoot(manager_root);
  RootLock lock(root, error);
  if (!lock.locked())
    return false;
  auto active = readActive(root, error);
  if (!active)
    return false;
  if (*active == release_id) {
    error = "cannot remove the active toolchain generation";
    return false;
  }
  const auto generation = root / "generations" / release_id;
  if (!std::filesystem::is_directory(generation)) {
    error = "toolchain generation is not installed";
    return false;
  }
  std::error_code ec;
  std::filesystem::remove_all(generation, ec);
  if (ec) {
    error = "failed to remove toolchain generation: " + ec.message();
    return false;
  }
  return true;
}

} // namespace chtholly
