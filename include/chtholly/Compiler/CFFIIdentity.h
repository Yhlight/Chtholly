#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <optional>
#include <string>
#include <string_view>

namespace chtholly::compiler {

struct CFFIReceiptIdentity {
  std::string target;
  std::string compiler_family;
  std::string clang_version;
  std::string libclang;
  std::string compiler;
  std::string compiler_version;
  std::string toolchain;
  std::string sdk;
  std::string config;
  std::string headers;
  std::string cfdl;
  std::string probe;
  std::string facts;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] StableFingerprint fingerprint() const;

  friend bool operator==(const CFFIReceiptIdentity &,
                         const CFFIReceiptIdentity &) = default;
};

[[nodiscard]] std::string renderCFFIReceipt(const CFFIReceiptIdentity &identity,
                                            std::string &error);

[[nodiscard]] std::optional<CFFIReceiptIdentity>
parseCFFIReceipt(std::string_view text, std::string &error);

} // namespace chtholly::compiler
