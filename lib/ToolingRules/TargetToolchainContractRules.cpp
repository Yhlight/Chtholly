#include "chtholly/ToolingRules/TargetToolchainContractRules.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace chtholly {

namespace {

std::string lowerCopy(std::string value) {
  for (auto &ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

TargetToolchainDecision validDecision() {
  TargetToolchainDecision decision;
  decision.valid = true;
  decision.failure = TargetToolchainFailure::None;
  decision.stable_reason = std::string(targetToolchainFailureSpelling(
      TargetToolchainFailure::None));
  return decision;
}

TargetToolchainDecision failureDecision(TargetToolchainFailure failure,
                                        std::string message) {
  TargetToolchainDecision decision;
  decision.valid = false;
  decision.failure = failure;
  decision.stable_reason =
      std::string(targetToolchainFailureSpelling(failure));
  decision.diagnostic_message = std::move(message);
  return decision;
}

} // namespace

std::string_view targetToolchainSourceSpelling(TargetToolchainSource source) {
  switch (source) {
  case TargetToolchainSource::Cli:
    return "cli";
  case TargetToolchainSource::Manifest:
    return "manifest";
  case TargetToolchainSource::Host:
    return "host";
  case TargetToolchainSource::Unknown:
    break;
  }
  return "unknown";
}

std::string_view targetToolchainFailureSpelling(
    TargetToolchainFailure failure) {
  switch (failure) {
  case TargetToolchainFailure::None:
    return "none";
  case TargetToolchainFailure::MissingTriple:
    return "target-missing-triple";
  case TargetToolchainFailure::UnknownArchitecture:
    return "target-unknown-architecture";
  case TargetToolchainFailure::UnsupportedArchitecture:
    return "target-unsupported-architecture";
  }
  return "target-unknown-architecture";
}

std::string_view targetArchitectureFamilySpelling(
    TargetArchitectureFamily family) {
  switch (family) {
  case TargetArchitectureFamily::Unknown:
    return "unknown";
  case TargetArchitectureFamily::X86:
    return "x86";
  case TargetArchitectureFamily::AArch64:
    return "aarch64";
  case TargetArchitectureFamily::Unsupported:
    return "unsupported";
  }
  return "unknown";
}

std::string_view targetLinkerStyleSpelling(TargetLinkerStyle style) {
  switch (style) {
  case TargetLinkerStyle::Msvc:
    return "msvc";
  case TargetLinkerStyle::Dash:
    return "dash";
  case TargetLinkerStyle::Clang:
    return "clang";
  case TargetLinkerStyle::Unknown:
    break;
  }
  return "unknown";
}

std::string targetToolchainDiagnostic(
    const TargetToolchainDecision &decision) {
  if (decision.stable_reason.empty()) {
    return decision.diagnostic_message;
  }
  if (decision.diagnostic_message.empty()) {
    return "[" + decision.stable_reason + "]";
  }
  return decision.diagnostic_message + " [" + decision.stable_reason + "]";
}

std::string TargetToolchainContractResolver::normalizeTriple(
    std::string triple) {
  return lowerCopy(std::move(triple));
}

std::string_view TargetToolchainContractResolver::tripleArch(
    std::string_view triple) {
  const auto dash = triple.find('-');
  return dash == std::string_view::npos ? triple : triple.substr(0, dash);
}

std::uint32_t TargetToolchainContractResolver::pointerWidthForArch(
    std::string_view arch) {
  if (arch == "x86_64" || arch == "amd64" || arch == "aarch64" ||
      arch == "arm64") {
    return 64;
  }
  if (arch == "i386" || arch == "i486" || arch == "i586" ||
      arch == "i686" || arch == "x86") {
    return 32;
  }
  return 0;
}

TargetArchitectureFamily
TargetToolchainContractResolver::architectureFamilyForArch(
    std::string_view arch) {
  if (arch == "x86_64" || arch == "amd64" || arch == "i386" ||
      arch == "i486" || arch == "i586" || arch == "i686" || arch == "x86") {
    return TargetArchitectureFamily::X86;
  }
  if (arch == "aarch64" || arch == "arm64") {
    return TargetArchitectureFamily::AArch64;
  }
  if (arch.empty()) {
    return TargetArchitectureFamily::Unknown;
  }
  return TargetArchitectureFamily::Unsupported;
}

std::string TargetToolchainContractResolver::objectExtensionForTriple(
    std::string_view triple) {
  return triple.find("windows") != std::string_view::npos ? "obj" : "o";
}

TargetLinkerStyle TargetToolchainContractResolver::classifyLinker(
    std::string_view linker) {
  if (linker.empty()) {
    return TargetLinkerStyle::Unknown;
  }
  auto name =
      lowerCopy(std::filesystem::path(std::string(linker)).filename().string());
  if (name == "link" || name == "link.exe" || name == "lld-link" ||
      name == "lld-link.exe") {
    return TargetLinkerStyle::Msvc;
  }
  if (name == "clang" || name == "clang.exe" || name == "clang++" ||
      name == "clang++.exe") {
    return TargetLinkerStyle::Clang;
  }
  return TargetLinkerStyle::Dash;
}

bool TargetToolchainContractResolver::linkerUsesDashOptions(
    TargetLinkerStyle style) {
  return style != TargetLinkerStyle::Msvc;
}

bool TargetToolchainContractResolver::linkerSupportsTargetFlag(
    std::string_view linker, TargetLinkerStyle style) {
  if (style == TargetLinkerStyle::Clang) {
    return true;
  }
  if (style != TargetLinkerStyle::Unknown) {
    return false;
  }
  return classifyLinker(linker) == TargetLinkerStyle::Clang;
}

TargetToolchainDecision TargetToolchainContractResolver::resolve(
    const TargetToolchainFacts &facts) {
  std::string requested;
  TargetToolchainSource source = TargetToolchainSource::Unknown;
  if (!facts.cli_triple.empty()) {
    requested = facts.cli_triple;
    source = TargetToolchainSource::Cli;
  } else if (!facts.manifest_triple.empty()) {
    requested = facts.manifest_triple;
    source = TargetToolchainSource::Manifest;
  } else if (!facts.host_triple.empty()) {
    requested = facts.host_triple;
    source = TargetToolchainSource::Host;
  } else {
    return failureDecision(TargetToolchainFailure::MissingTriple,
                           "unable to resolve target triple: missing target");
  }

  auto normalized = normalizeTriple(requested);
  const auto arch_view = tripleArch(normalized);
  const std::string arch(arch_view);
  const auto architecture_family = architectureFamilyForArch(arch_view);
  if (architecture_family == TargetArchitectureFamily::Unsupported) {
    return failureDecision(
        TargetToolchainFailure::UnsupportedArchitecture,
        "unable to resolve target triple '" + requested +
            "': unsupported architecture '" + arch +
            "'; supported target families are X86 and AArch64");
  }
  const auto pointer_width = pointerWidthForArch(arch_view);
  if (pointer_width == 0) {
    return failureDecision(TargetToolchainFailure::UnknownArchitecture,
                           "unable to resolve target triple '" + requested +
                               "': unknown architecture");
  }

  auto decision = validDecision();
  decision.source = source;
  decision.requested_triple = requested;
  decision.normalized_triple = std::move(normalized);
  decision.arch = arch;
  decision.architecture_family = architecture_family;
  decision.pointer_width_bits = pointer_width;
  decision.object_extension =
      objectExtensionForTriple(decision.normalized_triple);
  decision.sysroot_path = !facts.cli_sysroot.empty() ? facts.cli_sysroot
                                                     : facts.manifest_sysroot;
  decision.linker_path = !facts.cli_linker.empty() ? facts.cli_linker
                                                   : facts.manifest_linker;
  decision.host_compatible =
      !facts.host_triple.empty() &&
      decision.normalized_triple == normalizeTriple(facts.host_triple);
  decision.linker_style = classifyLinker(decision.linker_path);
  decision.linker_uses_dash_options =
      linkerUsesDashOptions(decision.linker_style);
  decision.linker_supports_target_flag =
      linkerSupportsTargetFlag(decision.linker_path, decision.linker_style);
  return decision;
}

} // namespace chtholly
