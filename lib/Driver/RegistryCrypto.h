#pragma once

#include <sodium.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace chtholly::registry_crypto {

using PublicKey =
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES>;
using SecretKey =
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES>;
using Signature = std::array<unsigned char, crypto_sign_BYTES>;

bool initialize(std::string &error);
bool generateSigningKeyFiles(const std::string &secret_key_path,
                             const std::string &public_key_path,
                             std::string &error);
std::optional<PublicKey> parsePublicKey(std::string_view text);
std::string publicKeyId(const PublicKey &key);
bool loadSecretKeyFile(const std::string &path, SecretKey &secret,
                       PublicKey &public_key, std::string &error);
std::string encodeSignature(const Signature &signature);
bool decodeSignature(std::string_view text, Signature &signature);

} // namespace chtholly::registry_crypto
