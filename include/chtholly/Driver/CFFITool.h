#pragma once

#include "chtholly/Driver/CFFIToolchain.h"
#include "chtholly/Compiler/CFFIIdentity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

enum class CFFIRootKind : std::uint8_t { Type, Function, Constant, Count };

struct CFFIRoot {
  CFFIRootKind kind = CFFIRootKind::Count;
  std::string name;
};

struct CFFITypeMapping {
  std::string c_type;
  std::string cfdl_name;
  std::string carrier;
};

struct CFFIConfig {
  std::uint32_t version = 0;
  std::string path;
  std::string root_directory;
  std::string module;
  std::string target;
  std::string cache_directory;
  std::vector<std::string> headers;
  std::string language = "c";
  std::string standard = "c17";
  std::vector<std::string> include_paths;
  std::vector<std::string> system_include_paths;
  std::vector<std::string> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> clang_arguments;
  CFFIToolchainRequest toolchain_request;
  CFFIToolchainContract toolchain;
  std::vector<std::string> compile_arguments;
  std::vector<std::string> link_arguments;
  std::vector<std::string> library_paths;
  std::vector<std::string> libraries;
  std::uint64_t timeout_ms = 30000;
  std::vector<CFFIRoot> roots;
  std::vector<CFFITypeMapping> type_mappings;
};

struct CFFIVerification {
  compiler::CFFIReceiptIdentity identity;
  std::string receipt;
  std::string probe_source;
  std::string compiler_version;
};

struct CFFIDoctorReport {
  CFFIToolchainContract toolchain;
  std::string libclang_path;
  std::string libclang_digest;
  std::string probe_description;
  std::size_t declaration_count = 0;
};

enum class CFFIRegenerationChangeKind : std::uint8_t {
  Add,
  Remove,
  MechanicalUpdate,
  ParameterRename,
  SemanticPreserved,
  ManualRetained,
  StateBootstrap,
  StateUpdate,
  Count,
};

struct CFFIRegenerationChange {
  CFFIRegenerationChangeKind kind = CFFIRegenerationChangeKind::Count;
  std::string declaration_kind;
  std::string key;
  std::string detail;
};

struct CFFIGeneration {
  std::string source;
  std::string state;
};

struct CFFIRegeneration {
  std::string source;
  std::string state;
  std::string input_digest;
  std::optional<std::string> state_input_digest;
  std::vector<CFFIRegenerationChange> changes;
  bool changed = false;
};

[[nodiscard]] std::string_view
cffiRegenerationChangeName(CFFIRegenerationChangeKind kind);
[[nodiscard]] std::string defaultCFFIStatePath(std::string_view cfdl_path);

[[nodiscard]] bool loadCFFIConfig(const std::string &path, CFFIConfig &config,
                                  std::string &error,
                                  std::string_view cache_directory = {});
[[nodiscard]] bool doctorCFFI(const CFFIConfig *config, std::string_view target,
                              CFFIDoctorReport &report, std::string &error,
                              const CFFIToolchainCacheOptions &cache_options = {});
[[nodiscard]] bool generateCFFI(const CFFIConfig &config, std::string &source,
                                std::string &error);
[[nodiscard]] bool generateCFFIWithState(const CFFIConfig &config,
                                         CFFIGeneration &generation,
                                         std::string &error);
[[nodiscard]] bool regenerateCFFI(const CFFIConfig &config,
                                  const std::string &cfdl_path,
                                  const std::string &state_path,
                                  CFFIRegeneration &regeneration,
                                  std::string &error);
[[nodiscard]] bool writeCFFIGeneration(const std::string &cfdl_path,
                                       const std::string &state_path,
                                       const CFFIGeneration &generation,
                                       std::string &error);
[[nodiscard]] bool applyCFFIRegeneration(const std::string &cfdl_path,
                                         const std::string &state_path,
                                         const CFFIRegeneration &regeneration,
                                         std::string &error);
[[nodiscard]] bool verifyCFFI(const CFFIConfig &config,
                              const std::string &cfdl_path, bool keep_temporary,
                              CFFIVerification &verification,
                              std::string &error);

} // namespace chtholly
