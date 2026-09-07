#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

inline constexpr std::uint32_t ComponentAbiEpoch = 1;

enum class ComponentValueKind : std::uint8_t {
  Void,
  Bool,
  I8,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  F32,
  F64,
  Bytes,
  Count,
};

struct ComponentExportArtifact {
  std::string canonical_name;
  StableFingerprint export_id;
  StableFingerprint signature_digest;
  std::vector<ComponentValueKind> parameters;
  ComponentValueKind result = ComponentValueKind::Count;

  friend bool operator==(const ComponentExportArtifact &,
                         const ComponentExportArtifact &) = default;
};

struct ComponentExportLoweringPlan {
  ComponentExportArtifact artifact;
  std::uint32_t target_index = core::AnyId::InvalidIndex;
};

struct ComponentContractArtifact {
  std::uint32_t abi_epoch = ComponentAbiEpoch;
  std::string identity;
  StableFingerprint identity_digest;
  StableFingerprint contract_digest;
  std::vector<ComponentExportArtifact> exports;

  void canonicalize();
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode(std::string &error) const;
  [[nodiscard]] static std::optional<ComponentContractArtifact>
  decode(std::string_view bytes, std::string &error);
};

[[nodiscard]] std::optional<ComponentValueKind>
componentValueKind(const PublicType &type, bool result);
[[nodiscard]] StableFingerprint
componentExportId(std::string_view identity, std::string_view canonical_name);
[[nodiscard]] StableFingerprint
componentSignatureDigest(std::span<const ComponentValueKind> parameters,
                         ComponentValueKind result);
[[nodiscard]] StableFingerprint
componentContractDigest(std::string_view identity,
                        std::span<const ComponentExportArtifact> exports);
[[nodiscard]] std::string componentWrapperSymbol(const StableFingerprint &id);

} // namespace chtholly::compiler
