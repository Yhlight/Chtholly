#include "chtholly/Driver/TargetConfig.h"

#include "chtholly/ToolingRules/TargetToolchainContractRules.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

namespace chtholly {

namespace {

std::filesystem::path resolveAgainst(const std::filesystem::path &base,
                                     const std::string &value) {
  std::filesystem::path path(value);
  if (path.is_absolute()) {
    return path;
  }
  return base / path;
}

std::string manifestSysroot(const TargetConfigInput &manifest) {
  if (!manifest.sysroot.empty()) {
    return resolveAgainst(manifest.root_directory, manifest.sysroot).string();
  }
  return {};
}

std::string manifestLinker(const TargetConfigInput &manifest) {
  if (!manifest.linker.empty()) {
    const std::filesystem::path linker(manifest.linker);
    if (linker.has_parent_path()) {
      return resolveAgainst(manifest.root_directory, manifest.linker).string();
    }
    return manifest.linker;
  }
  return {};
}

} // namespace

std::string hostTargetTriple() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
  return "x86_64-pc-windows-msvc";
#elif defined(_M_IX86) || defined(__i386__)
  return "i686-pc-windows-msvc";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "aarch64-pc-windows-msvc";
#else
  return "unknown-pc-windows-msvc";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
  return "aarch64-apple-darwin";
#else
  return "x86_64-apple-darwin";
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
  return "x86_64-unknown-linux-gnu";
#elif defined(__aarch64__)
  return "aarch64-unknown-linux-gnu";
#else
  return "unknown-unknown-linux-gnu";
#endif
#else
  return "unknown-unknown-unknown";
#endif
}

bool isMsvcStyleLinker(const std::string &linker) {
  return TargetToolchainContractResolver::classifyLinker(linker) ==
         TargetLinkerStyle::Msvc;
}

std::optional<TargetConfig>
resolveTargetConfig(const CompilerInvocation &invocation,
                    const TargetConfigInput &manifest, std::string &error) {
  TargetToolchainFacts facts;
  facts.cli_triple = invocation.target_triple;
  facts.manifest_triple = manifest.triple;
  facts.host_triple = hostTargetTriple();
  facts.cli_sysroot = invocation.sysroot_path;
  facts.manifest_sysroot = manifestSysroot(manifest);
  facts.cli_linker = invocation.linker_path;
  facts.manifest_linker = manifestLinker(manifest);

  const auto decision = TargetToolchainContractResolver::resolve(facts);
  if (!decision.valid) {
    error = targetToolchainDiagnostic(decision);
    return std::nullopt;
  }

  TargetConfig config;
  config.info.triple = decision.normalized_triple;
  config.info.pointer_width_bits = decision.pointer_width_bits;
  config.sysroot_path = decision.sysroot_path;
  config.linker_path = decision.linker_path;
  config.object_extension = decision.object_extension;
  config.is_host_compatible = decision.host_compatible;
  config.debug_info = invocation.debug_info != DebugInfoKind::None;
  return config;
}

} // namespace chtholly
