#pragma once

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {

enum class CFFICompilerFamily : std::uint8_t {
  MSVC,
  Clang,
  GCC,
  Count,
};

struct CFFIToolchainRequest {
  std::string target;
  std::string compiler = "auto";
  std::string msvc_install;
  std::string sysroot;
};

struct CFFIToolchainContract {
  CFFICompilerFamily family = CFFICompilerFamily::Count;
  std::string target;
  std::string compiler;
  std::string compiler_version;
  std::string compiler_target_triple;
  std::string canonical_target_triple;
  bool target_match = false;
  std::string compiler_multiarch;
  std::string compiler_multilibs;
  std::string sysroot_mode;
  std::string sysroot;
  std::string resource_dir;
  std::string sdk_name;
  std::string sdk_version;
  std::vector<std::string> system_include_paths;
  std::vector<std::string> system_library_paths;
  std::vector<std::pair<std::string, std::string>> environment_overrides;
  std::vector<std::string> discovery_trace;
  std::vector<std::string> missing_components;
  std::vector<std::string> validated_components;
  std::vector<std::string> runtime_include_paths;
  std::vector<std::string> runtime_library_paths;
  std::vector<std::string> standard_header_probes;
  std::vector<std::string> runtime_file_probes;
  std::string runtime_architecture;
  std::string runtime_link_probe;
  std::string fingerprint;
  std::string sdk_fingerprint;
};

struct CFFIToolchainCacheMetrics;

struct CFFIToolchainCacheOptions {
  std::string directory;
  bool enable_memory = true;
  bool enable_disk = true;
  std::size_t max_entries = 32;
  std::chrono::seconds max_age = std::chrono::hours(24 * 30);
  CFFIToolchainCacheMetrics *metrics = nullptr;
};

struct CFFIToolchainCacheMetrics {
  std::uint64_t memory_hits = 0;
  std::uint64_t disk_hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t invalid_entries = 0;
  std::uint64_t expired_entries = 0;
  std::uint64_t evictions = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t discovery_avoided = 0;
};

[[nodiscard]] bool pruneCFFIToolchainCache(
    const CFFIToolchainCacheOptions &options, std::string &error);

[[nodiscard]] std::string_view
cffiCompilerFamilyName(CFFICompilerFamily family);
[[nodiscard]] std::string nativeTier1CFFITarget();
[[nodiscard]] bool loadCFFIToolchainRequest(const std::string &config_path,
                                            CFFIToolchainRequest &request,
                                            std::string &error);
[[nodiscard]] bool resolveCFFIToolchain(const CFFIToolchainRequest &request,
                                        CFFIToolchainContract &contract,
                                        std::string &error);
[[nodiscard]] std::string cffiToolchainRequestFingerprint(
    const CFFIToolchainRequest &request);
[[nodiscard]] bool resolveCFFIToolchain(
    const CFFIToolchainRequest &request, CFFIToolchainContract &contract,
    std::string &error, const CFFIToolchainCacheOptions &cache_options);
[[nodiscard]] bool probeCFFIToolchain(const CFFIToolchainContract &contract,
                                      std::string &description,
                                      std::string &error);

} // namespace chtholly
