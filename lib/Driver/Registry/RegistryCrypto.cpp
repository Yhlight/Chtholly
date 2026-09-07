#include "RegistryCrypto.h"

#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <filesystem>

#if defined(_WIN32)
#include <AccCtrl.h>
#include <Aclapi.h>
#include <windows.h>
#endif

namespace chtholly::registry_crypto {

namespace {

std::string encodeBase64(const unsigned char *data, std::size_t size) {
  std::string encoded(
      sodium_base64_encoded_len(size, sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(encoded.data(), encoded.size(), data, size,
                    sodium_base64_VARIANT_ORIGINAL);
  encoded.resize(encoded.find('\0'));
  return encoded;
}

bool restrictSecretKeyPermissions(const std::string &path,
                                  std::string &error) {
#if defined(_WIN32)
  auto native_path = pathForFileSystem(path).wstring();
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PSID owner = nullptr;
  auto result = GetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT,
                                      OWNER_SECURITY_INFORMATION, &owner,
                                      nullptr, nullptr, nullptr, &descriptor);
  if (result != ERROR_SUCCESS || owner == nullptr) {
    if (descriptor != nullptr)
      LocalFree(descriptor);
    error = "failed to read signing key owner: Windows error " +
            std::to_string(result);
    return false;
  }
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(owner);
  PACL acl = nullptr;
  result = SetEntriesInAclW(1, &access, nullptr, &acl);
  if (result == ERROR_SUCCESS) {
    result = SetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION |
                                       PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, acl, nullptr);
  }
  if (acl != nullptr)
    LocalFree(acl);
  LocalFree(descriptor);
  if (result != ERROR_SUCCESS) {
    error = "failed to restrict signing key DACL: Windows error " +
            std::to_string(result);
    return false;
  }
#else
  std::error_code file_error;
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, file_error);
  if (file_error) {
    error = "failed to restrict signing key permissions: " +
            file_error.message();
    return false;
  }
#endif
  return true;
}

} // namespace

bool initialize(std::string &error) {
  static const int result = sodium_init();
  if (result < 0) {
    error = "failed to initialize libsodium";
    return false;
  }
  return true;
}

bool generateSigningKeyFiles(const std::string &secret_key_path,
                             const std::string &public_key_path,
                             std::string &error) {
  error.clear();
  if (!initialize(error))
    return false;
  if (std::filesystem::exists(secret_key_path) ||
      std::filesystem::exists(public_key_path)) {
    error = "refusing to overwrite an existing signing key file";
    return false;
  }
  PublicKey public_key{};
  SecretKey secret_key{};
  if (crypto_sign_keypair(public_key.data(), secret_key.data()) != 0) {
    error = "failed to generate Ed25519 signing key";
    return false;
  }
  const auto encoded_public =
      encodeBase64(public_key.data(), public_key.size());
  auto encoded_secret = encodeBase64(secret_key.data(), secret_key.size());
  auto secret_text =
      "chtholly-ed25519-secret-v1\npublic-key\ted25519:" + encoded_public +
      "\nsecret-key\ted25519:" + encoded_secret + "\nend\n";
  const auto public_text = "chtholly-ed25519-public-v1\nkey-id\t" +
                           publicKeyId(public_key) +
                           "\npublic-key\ted25519:" + encoded_public +
                           "\nend\n";
  const bool written = writeTextFile(secret_key_path, "", error) &&
                       restrictSecretKeyPermissions(secret_key_path, error) &&
                       writeTextFile(secret_key_path, secret_text, error) &&
                       writeTextFile(public_key_path, public_text, error);
  sodium_memzero(secret_key.data(), secret_key.size());
  sodium_memzero(encoded_secret.data(), encoded_secret.size());
  sodium_memzero(secret_text.data(), secret_text.size());
  if (!written) {
    std::error_code remove_error;
    removeFile(secret_key_path, remove_error);
    removeFile(public_key_path, remove_error);
  }
  return written;
}

std::optional<PublicKey> parsePublicKey(std::string_view text) {
  constexpr std::string_view prefix = "ed25519:";
  if (!text.starts_with(prefix)) return std::nullopt;
  text.remove_prefix(prefix.size());
  PublicKey key{};
  std::size_t size = 0;
  if (sodium_base642bin(key.data(), key.size(), text.data(), text.size(),
                       nullptr, &size, nullptr,
                       sodium_base64_VARIANT_ORIGINAL) != 0 ||
      size != key.size()) {
    return std::nullopt;
  }
  return key;
}

std::string publicKeyId(const PublicKey &key) {
  return "sha256:" + sha256Hex(std::string_view(
                         reinterpret_cast<const char *>(key.data()),
                         key.size()));
}

bool loadSecretKeyFile(const std::string &path, SecretKey &secret,
                       PublicKey &public_key, std::string &error) {
  sodium_memzero(secret.data(), secret.size());
  auto text = readTextFile(path, error);
  if (!text) return false;
  const auto wipe_input = [&] {
    sodium_memzero(text->data(), text->size());
  };
  std::size_t offset = 0;
  const auto compiler_line = [&](std::string_view &line) {
    if (offset >= text->size()) return false;
    const auto end = text->find('\n', offset);
    if (end == std::string::npos) {
      line = std::string_view(*text).substr(offset);
      offset = text->size();
    } else {
      line = std::string_view(*text).substr(offset, end - offset);
      offset = end + 1;
    }
    return true;
  };
  std::string_view header, public_line, secret_line, end;
  constexpr std::string_view public_prefix = "public-key\ted25519:";
  constexpr std::string_view secret_prefix = "secret-key\ted25519:";
  if (!compiler_line(header) || !compiler_line(public_line) ||
      !compiler_line(secret_line) || !compiler_line(end) || offset != text->size() ||
      header != "chtholly-ed25519-secret-v1" || end != "end" ||
      !public_line.starts_with(public_prefix) ||
      !secret_line.starts_with(secret_prefix)) {
    wipe_input();
    error = "invalid Chtholly Ed25519 secret key file: '" + path + "'";
    return false;
  }
  auto parsed_public = parsePublicKey(
      std::string("ed25519:") +
      std::string(public_line.substr(public_prefix.size())));
  std::size_t secret_size = 0;
  const auto encoded_secret = secret_line.substr(secret_prefix.size());
  if (!parsed_public ||
      sodium_base642bin(secret.data(), secret.size(), encoded_secret.data(),
                       encoded_secret.size(), nullptr, &secret_size, nullptr,
                       sodium_base64_VARIANT_ORIGINAL) != 0 ||
      secret_size != secret.size()) {
    sodium_memzero(secret.data(), secret.size());
    wipe_input();
    error = "invalid Chtholly Ed25519 secret key file: '" + path + "'";
    return false;
  }
  PublicKey derived{};
  if (crypto_sign_ed25519_sk_to_pk(derived.data(), secret.data()) != 0 ||
      derived != *parsed_public) {
    sodium_memzero(secret.data(), secret.size());
    wipe_input();
    error = "Ed25519 secret key file public key does not match its secret";
    return false;
  }
  public_key = *parsed_public;
  wipe_input();
  return true;
}

std::string encodeSignature(const Signature &signature) {
  std::string encoded(
      sodium_base64_encoded_len(signature.size(),
                                sodium_base64_VARIANT_ORIGINAL),
      '\0');
  sodium_bin2base64(encoded.data(), encoded.size(), signature.data(),
                    signature.size(), sodium_base64_VARIANT_ORIGINAL);
  encoded.resize(encoded.find('\0'));
  return "ed25519:" + encoded;
}

bool decodeSignature(std::string_view text, Signature &signature) {
  constexpr std::string_view prefix = "ed25519:";
  if (!text.starts_with(prefix)) return false;
  text.remove_prefix(prefix.size());
  std::size_t size = 0;
  return sodium_base642bin(signature.data(), signature.size(), text.data(),
                           text.size(), nullptr, &size, nullptr,
                           sodium_base64_VARIANT_ORIGINAL) == 0 &&
         size == signature.size();
}

} // namespace chtholly::registry_crypto
