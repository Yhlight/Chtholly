#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace chtholly {

enum class TargetToolchainSource {
  Unknown,
  Cli,
  Manifest,
  Host,
};

enum class TargetLinkerStyle {
  Unknown,
  Msvc,
  Dash,
  Clang,
};

enum class TargetArchitectureFamily {
  Unknown,
  X86,
  AArch64,
  Unsupported,
};

enum class TargetToolchainFailure {
  None,
  MissingTriple,
  UnknownArchitecture,
  UnsupportedArchitecture,
};

struct TargetToolchainFacts {
  std::string cli_triple;
  std::string manifest_triple;
  std::string host_triple;
  std::string cli_sysroot;
  std::string manifest_sysroot;
  std::string cli_linker;
  std::string manifest_linker;
};

struct TargetToolchainDecision {
  bool valid = false;
  TargetToolchainSource source = TargetToolchainSource::Unknown;
  TargetToolchainFailure failure = TargetToolchainFailure::None;
  std::string stable_reason;
  std::string diagnostic_message;
  std::string requested_triple;
  std::string normalized_triple;
  std::string arch;
  TargetArchitectureFamily architecture_family =
      TargetArchitectureFamily::Unknown;
  std::uint32_t pointer_width_bits = 0;
  std::string object_extension;
  std::string sysroot_path;
  std::string linker_path;
  bool host_compatible = false;
  TargetLinkerStyle linker_style = TargetLinkerStyle::Unknown;
  bool linker_uses_dash_options = true;
  bool linker_supports_target_flag = false;
};

std::string_view targetToolchainSourceSpelling(TargetToolchainSource source);
std::string_view
targetArchitectureFamilySpelling(TargetArchitectureFamily family);
std::string_view targetToolchainFailureSpelling(TargetToolchainFailure failure);
std::string_view targetLinkerStyleSpelling(TargetLinkerStyle style);
std::string targetToolchainDiagnostic(
    const TargetToolchainDecision &decision);

class TargetToolchainContractResolver {
public:
  static TargetToolchainDecision resolve(const TargetToolchainFacts &facts);

  static std::string normalizeTriple(std::string triple);
  static std::string_view tripleArch(std::string_view triple);
  static TargetArchitectureFamily architectureFamilyForArch(
      std::string_view arch);
  static std::uint32_t pointerWidthForArch(std::string_view arch);
  static std::string objectExtensionForTriple(std::string_view triple);
  static TargetLinkerStyle classifyLinker(std::string_view linker);
  static bool linkerUsesDashOptions(TargetLinkerStyle style);
  static bool linkerSupportsTargetFlag(std::string_view linker,
                                       TargetLinkerStyle style);
};

} // namespace chtholly
