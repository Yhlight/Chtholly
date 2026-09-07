#include "chtholly/Compiler/CFFIIdentity.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace chtholly::compiler {
namespace {

constexpr std::array<std::string_view, 13> FieldNames = {
    "target",   "compiler-family",  "clang-version", "libclang",
    "compiler", "compiler-version", "toolchain",     "sdk",
    "config",   "headers",          "cfdl",          "probe",
    "facts"};
constexpr std::size_t MaxReceiptBytes = 16U * 1024U;
constexpr std::size_t MaxTargetBytes = 256U;

bool isLowerHexDigest(std::string_view value) {
  if (value.size() != StableFingerprint::ByteCount * 2)
    return false;
  for (const unsigned char ch : value)
    if (!(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f'))
      return false;
  return true;
}

bool isTargetCharacter(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
}

std::array<const std::string *, 13>
fields(const CFFIReceiptIdentity &identity) {
  return {&identity.target,        &identity.compiler_family,
          &identity.clang_version, &identity.libclang,
          &identity.compiler,      &identity.compiler_version,
          &identity.toolchain,     &identity.sdk,
          &identity.config,        &identity.headers,
          &identity.cfdl,          &identity.probe,
          &identity.facts};
}

std::array<std::string *, 13> fields(CFFIReceiptIdentity &identity) {
  return {&identity.target,        &identity.compiler_family,
          &identity.clang_version, &identity.libclang,
          &identity.compiler,      &identity.compiler_version,
          &identity.toolchain,     &identity.sdk,
          &identity.config,        &identity.headers,
          &identity.cfdl,          &identity.probe,
          &identity.facts};
}

} // namespace

bool CFFIReceiptIdentity::verify(std::string &error) const {
  error.clear();
  if (target.empty() || target.size() > MaxTargetBytes ||
      !std::ranges::all_of(target, isTargetCharacter)) {
    error = "CFFI receipt target is missing or invalid";
    return false;
  }
  if (compiler_family != "msvc" && compiler_family != "clang" &&
      compiler_family != "gcc") {
    error = "CFFI receipt compiler family is invalid";
    return false;
  }
  const auto values = fields(*this);
  for (std::size_t index = 2; index < values.size(); ++index) {
    if (!isLowerHexDigest(*values[index])) {
      error = "CFFI receipt field '" + std::string(FieldNames[index]) +
              "' is not a lowercase SHA-256 digest";
      return false;
    }
  }
  return true;
}

StableFingerprint CFFIReceiptIdentity::fingerprint() const {
  std::string error;
  const auto receipt = renderCFFIReceipt(*this, error);
  return error.empty() ? StableFingerprint::fromCanonicalBytes(receipt)
                       : StableFingerprint{};
}

std::string renderCFFIReceipt(const CFFIReceiptIdentity &identity,
                              std::string &error) {
  if (!identity.verify(error))
    return {};
  std::ostringstream out;
  out << "CHCFFI3\n";
  const auto values = fields(identity);
  for (std::size_t index = 0; index < values.size(); ++index)
    out << FieldNames[index] << '\t' << *values[index] << '\n';
  return out.str();
}

std::optional<CFFIReceiptIdentity> parseCFFIReceipt(std::string_view text,
                                                    std::string &error) {
  error.clear();
  if (text.size() > MaxReceiptBytes) {
    error = "CFFI receipt exceeds its input budget";
    return std::nullopt;
  }
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != "CHCFFI3") {
    error = "CFFI receipt has an invalid or unsupported header";
    return std::nullopt;
  }
  CFFIReceiptIdentity identity;
  const auto values = fields(identity);
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (!std::getline(input, line)) {
      error = "CFFI receipt is truncated before field '" +
              std::string(FieldNames[index]) + "'";
      return std::nullopt;
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos ||
        std::string_view(line).substr(0, tab) != FieldNames[index]) {
      error = "CFFI receipt field order is invalid at record " +
              std::to_string(index + 1);
      return std::nullopt;
    }
    *values[index] = line.substr(tab + 1);
  }
  if (std::getline(input, line)) {
    error = "CFFI receipt contains trailing records";
    return std::nullopt;
  }
  if (!text.ends_with('\n')) {
    error = "CFFI receipt is not canonically terminated";
    return std::nullopt;
  }
  if (!identity.verify(error))
    return std::nullopt;
  std::string render_error;
  if (renderCFFIReceipt(identity, render_error) != text) {
    error = "CFFI receipt is not canonically encoded";
    return std::nullopt;
  }
  return identity;
}

} // namespace chtholly::compiler
