#include "chtholly/Driver/CFFITool.h"

#include "../ManifestToml.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include "clang-c/Index.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace chtholly {
namespace {

CFFIToolchainCacheOptions cacheOptions(const CFFIConfig &config) {
  return CFFIToolchainCacheOptions{.directory = config.cache_directory};
}

#include "CFFIToolModel.inc"
#include "CFFIToolGenerator.inc"
#include "CFFIToolProbe.inc"
#include "CFFIToolConfig.inc"
bool generateCFFI(const CFFIConfig &config, std::string &source,
                  std::string &error) {
  CModel model;
  if (!buildModel(config, model, error))
    return false;
  Generator generator(config, model);
  return generator.run(source, error);
}

bool doctorCFFI(const CFFIConfig *config, std::string_view target,
                CFFIDoctorReport &report, std::string &error,
                const CFFIToolchainCacheOptions &cache_options) {
  report = {};
  if (config && !target.empty() && config->target != target) {
    error = "CFFI doctor target disagrees with the supplied config";
    return false;
  }
  CFFIConfig smoke;
  if (config) {
    smoke = *config;
  } else {
    smoke.version = 3;
    smoke.target =
        target.empty() ? nativeTier1CFFITarget() : std::string(target);
    smoke.headers = {"stddef.h", "stdint.h"};
    smoke.toolchain_request.target = smoke.target;
    auto options = cache_options;
    if (!smoke.cache_directory.empty()) options.directory = smoke.cache_directory;
    if (!resolveCFFIToolchain(smoke.toolchain_request, smoke.toolchain, error,
                              options))
      return false;
  }
  CModel model;
  if (!buildModel(smoke, model, error))
    return false;
  if (config) {
    Generator generator(smoke, model);
    std::string draft;
    if (!generator.run(draft, error))
      return false;
  }
  const auto libclang = loadedLibclangPath();
  const auto digest = libclang ? sha256File(*libclang) : std::nullopt;
  if (!libclang || !digest) {
    error = "unable to fingerprint the loaded libclang runtime";
    return false;
  }
  report.toolchain = smoke.toolchain;
  report.libclang_path = *libclang;
  report.libclang_digest = *digest;
  report.declaration_count = model.declarations.size();
  return probeCFFIToolchain(report.toolchain, report.probe_description, error);
}

bool verifyCFFI(const CFFIConfig &config, const std::string &cfdl_path,
                bool keep_temporary, CFFIVerification &verification,
                std::string &error) {
  if (config.target != hostTriple()) {
    error = "CFFI verification requires a native Tier-1 target";
    return false;
  }
  CModel model;
  if (!buildModel(config, model, error))
    return false;
  Generator generator(config, model);
  std::string expected, actual;
  if (!generator.run(expected, error) ||
      !compareCFDL(expected, cfdl_path, actual, error))
    return false;
  verification.probe_source = makeProbe(config, generator);
  LinkClosure link_closure;
  if (!computeLinkClosure(config, link_closure, error))
    return false;
  if (!compileAndRunProbe(config, verification.probe_source, keep_temporary,
                          verification.compiler_version, error))
    return false;
  auto config_text = readTextFile(config.path, error);
  if (!config_text)
    return false;
  std::string header_closure;
  std::size_t total = 0;
  for (const auto &header : model.included_files) {
    auto text = readTextFile(header, error);
    if (!text) {
      error = "unable to fingerprint Clang inclusion: " + header;
      return false;
    }
    total += text->size();
    if (total > MaxHeaderBytes) {
      error = "CFFI header closure exceeds its input budget";
      return false;
    }
    header_closure += header + "\n" + sha256Hex(*text) + "\n";
  }
  const auto libclang_path = loadedLibclangPath();
  const auto libclang_digest =
      libclang_path ? sha256File(*libclang_path) : std::nullopt;
  if (!libclang_path || !libclang_digest) {
    error = "unable to fingerprint the loaded libclang runtime";
    return false;
  }
  verification.identity = {
      .target = config.target,
      .compiler_family =
          std::string(cffiCompilerFamilyName(config.toolchain.family)),
      .clang_version = sha256Hex(model.clang_version),
      .libclang = *libclang_digest,
      .compiler = sha256Hex(config.toolchain.compiler),
      .compiler_version = sha256Hex(verification.compiler_version),
      .toolchain = config.toolchain.fingerprint,
      .sdk = config.toolchain.sdk_fingerprint,
      .config = sha256Hex(*config_text),
      .headers = sha256Hex(header_closure),
      .cfdl = sha256Hex(actual),
      .probe = sha256Hex(verification.probe_source +
                         "\nlink-closure\t" + link_closure.fingerprint),
      .facts = sha256Hex(expected),
  };
  verification.receipt = compiler::renderCFFIReceipt(verification.identity, error);
  return !verification.receipt.empty();
}

} // namespace chtholly
