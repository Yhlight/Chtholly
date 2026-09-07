#pragma once

#include "chtholly/Basic/LanguageVersion.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

class CompilerInputFileSystem;

inline constexpr std::uint32_t CompilerStandardLibraryFormatVersion = 5;
inline constexpr std::uint32_t CompilerCompilerContractEpoch = 15;
inline constexpr std::uint32_t CompilerStandardLibraryApiEpoch = 22;

struct CompilerStandardLibraryModule {
  std::string module_name;
  std::string relative_path;
  std::string source_path;
  LanguageVersion minimum_language_version = FrozenV1LanguageVersion;
  std::vector<std::string> imports;
  std::vector<std::string> runtime_symbols;
  std::vector<compiler::CompilerIntrinsicBinding> compiler_intrinsics;
  compiler::StableFingerprint source_fingerprint;
};

class CompilerStandardLibraryManifest {
public:
  [[nodiscard]] std::uint32_t formatVersion() const {
    return format_version_;
  }
  [[nodiscard]] std::uint32_t compilerContractEpoch() const {
    return compiler_contract_epoch_;
  }
  [[nodiscard]] std::uint32_t libraryApiEpoch() const {
    return library_api_epoch_;
  }
  [[nodiscard]] std::string_view packageName() const {
    return package_name_;
  }
  [[nodiscard]] std::string_view manifestPath() const {
    return manifest_path_;
  }
  [[nodiscard]] std::string_view rootPath() const {
    return root_path_;
  }
  [[nodiscard]] std::span<const CompilerStandardLibraryModule> modules() const {
    return modules_;
  }
  [[nodiscard]] const compiler::StableFingerprint &distributionFingerprint() const {
    return distribution_fingerprint_;
  }

  [[nodiscard]] static std::optional<CompilerStandardLibraryManifest>
  load(std::string_view resource_dir, const CompilerInputFileSystem &file_system,
       std::string &error);

private:
  std::uint32_t format_version_ = 0;
  std::uint32_t compiler_contract_epoch_ = 0;
  std::uint32_t library_api_epoch_ = 0;
  std::string package_name_;
  std::string manifest_path_;
  std::string root_path_;
  std::vector<CompilerStandardLibraryModule> modules_;
  compiler::StableFingerprint distribution_fingerprint_;
};

} // namespace chtholly
