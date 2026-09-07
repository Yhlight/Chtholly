#pragma once

#include <string_view>

namespace chtholly {

enum class AbiVersion {
  V0,
  V1,
  V2,
};

inline constexpr AbiVersion DefaultChthollyAbiVersion = AbiVersion::V2;

inline constexpr std::string_view abiVersionSpelling(AbiVersion version) {
  switch (version) {
  case AbiVersion::V0:
    return "v0";
  case AbiVersion::V1:
    return "v1";
  case AbiVersion::V2:
    return "v2";
  }
  return "v2";
}

inline constexpr std::string_view abiSymbolPrefix(AbiVersion version) {
  switch (version) {
  case AbiVersion::V0:
    return "chtholly.v0.";
  case AbiVersion::V1:
    return "chtholly.v1.";
  case AbiVersion::V2:
    return "chtholly.v2.";
  }
  return "chtholly.v2.";
}

inline constexpr bool abiSupportsConcreteDynRtti(AbiVersion version) {
  return version == AbiVersion::V1 || version == AbiVersion::V2;
}

inline constexpr bool abiSupportsDynFamilyQueries(AbiVersion version) {
  return version == AbiVersion::V2;
}

} // namespace chtholly
