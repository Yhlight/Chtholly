#include "chtholly/Driver/CompilerStandardLibrary.h"

#include "ManifestToml.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>

namespace chtholly {
namespace {

void appendField(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

bool isContainedRelativeSource(std::string_view text) {
  const std::filesystem::path path(text);
  const auto extension = path.extension();
  if (text.empty() || path.is_absolute() ||
      (extension != ".cns" && extension != ".cfdl"))
    return false;
  const auto normalized = path.lexically_normal();
  return normalized == path &&
         std::ranges::none_of(normalized, [](const auto &component) {
           return component == ".." || component == ".";
         });
}

} // namespace

std::optional<CompilerStandardLibraryManifest>
CompilerStandardLibraryManifest::load(std::string_view resource_dir,
                                  const CompilerInputFileSystem &file_system,
                                  std::string &error) {
  error.clear();
  const auto root = normalizeCompilerInputPath(
      (std::filesystem::path(resource_dir) / "stdlib").string());
  const auto manifest_path = normalizeCompilerInputPath(
      (std::filesystem::path(root) / "manifest.toml").string());
  if (root.empty() || manifest_path.empty()) {
    error = "invalid compiler standard-library resource path";
    return std::nullopt;
  }
  CompilerInputFile manifest_file;
  if (!file_system.readText(manifest_path, manifest_file, error))
    return std::nullopt;
  if (!manifest_file.exists || !manifest_file.text) {
    error = "compiler standard-library manifest is missing: " + manifest_path;
    return std::nullopt;
  }
  auto assignments = manifest_toml::parseAssignments(
      *manifest_file.text, {"module"}, "compiler standard-library manifest", error);
  if (!assignments)
    return std::nullopt;
  std::map<std::string, manifest_toml::Assignment> values;
  std::vector<manifest_toml::Assignment> module_values;
  for (const auto &assignment : *assignments) {
    if (assignment.table == "module") {
      module_values.push_back(assignment);
      continue;
    }
    if (!assignment.table.empty() || assignment.key.empty() ||
        !values.emplace(assignment.key, assignment).second) {
      error = "duplicate or invalid compiler standard-library key at line " +
              std::to_string(assignment.line);
      return std::nullopt;
    }
  }
  static constexpr std::string_view RequiredKeys[] = {
      "format", "package", "compiler_contract", "library_api"};
  for (const auto &[key, assignment] : values) {
    (void)assignment;
    if (std::ranges::find(RequiredKeys, key) == std::end(RequiredKeys)) {
      error = "unknown compiler standard-library key '" + key + "'";
      return std::nullopt;
    }
  }
  for (const auto key : RequiredKeys) {
    if (!values.contains(std::string(key))) {
      error = "compiler standard-library manifest is missing '" + std::string(key) +
              "'";
      return std::nullopt;
    }
  }

  std::uint64_t format = 0;
  std::uint64_t compiler_contract = 0;
  std::uint64_t library_api = 0;
  std::string package;
  if (!manifest_toml::parseUnsigned(values.at("format").value, format) ||
      !manifest_toml::parseUnsigned(values.at("compiler_contract").value,
                                    compiler_contract) ||
      !manifest_toml::parseUnsigned(values.at("library_api").value,
                                    library_api) ||
      !manifest_toml::parseString(values.at("package").value, package)) {
    error = "compiler standard-library manifest has an invalid value";
    return std::nullopt;
  }
  if (format != CompilerStandardLibraryFormatVersion) {
    error = "unsupported compiler standard-library format " +
            std::to_string(format) + "; expected " +
            std::to_string(CompilerStandardLibraryFormatVersion);
    return std::nullopt;
  }
  if (compiler_contract != CompilerCompilerContractEpoch) {
    error = "compiler standard-library compiler contract mismatch: found " +
            std::to_string(compiler_contract) + ", expected " +
            std::to_string(CompilerCompilerContractEpoch);
    return std::nullopt;
  }
  if (library_api != CompilerStandardLibraryApiEpoch) {
    error = "compiler standard-library API epoch mismatch: found " +
            std::to_string(library_api) + ", expected " +
            std::to_string(CompilerStandardLibraryApiEpoch);
    return std::nullopt;
  }
  if (package != "std" || module_values.empty()) {
    error =
        "compiler standard-library manifest must describe nonempty package 'std'";
    return std::nullopt;
  }

  CompilerStandardLibraryManifest result;
  result.format_version_ = static_cast<std::uint32_t>(format);
  result.compiler_contract_epoch_ =
      static_cast<std::uint32_t>(compiler_contract);
  result.library_api_epoch_ = static_cast<std::uint32_t>(library_api);
  result.package_name_ = std::move(package);
  result.manifest_path_ = manifest_path;
  result.root_path_ = root;
  for (const auto &assignment : module_values) {
    CompilerStandardLibraryModule module;
    std::vector<std::pair<std::string, std::string>> fields;
    if (assignment.key.empty() ||
        !manifest_toml::parseInlineTable(assignment.value, fields)) {
      error = "compiler standard-library manifest has invalid module record at "
              "line " +
              std::to_string(assignment.line);
      return std::nullopt;
    }
    std::map<std::string, std::string> unique_fields;
    for (auto &[key, value] : fields)
      if (!unique_fields.emplace(std::move(key), std::move(value)).second) {
        error = "compiler standard-library module record has duplicate fields";
        return std::nullopt;
      }
    if (unique_fields.size() != 6 || !unique_fields.contains("name") ||
        !unique_fields.contains("path") || !unique_fields.contains("since") ||
        !unique_fields.contains("imports") ||
        !unique_fields.contains("runtime_symbols") ||
        !unique_fields.contains("intrinsics")) {
      error = "compiler standard-library module record requires name, path, since, "
              "imports, runtime_symbols, and intrinsics";
      return std::nullopt;
    }
    std::string since;
    std::vector<std::string> imports;
    std::vector<std::string> runtime_symbols;
    std::vector<std::string> intrinsic_entries;
    if (!manifest_toml::parseString(unique_fields.at("name"),
                                    module.module_name) ||
        !manifest_toml::parseString(unique_fields.at("path"),
                                    module.relative_path) ||
        !manifest_toml::parseString(unique_fields.at("since"), since) ||
        !manifest_toml::parseStringArray(unique_fields.at("imports"),
                                         imports) ||
        !manifest_toml::parseStringArray(unique_fields.at("runtime_symbols"),
                                         runtime_symbols) ||
        !manifest_toml::parseStringArray(unique_fields.at("intrinsics"),
                                         intrinsic_entries) ||
        (module.module_name != "std" &&
         !module.module_name.starts_with("std::")) ||
        !isContainedRelativeSource(module.relative_path)) {
      error = "compiler standard-library manifest has invalid module record '" +
              assignment.key + "'";
      return std::nullopt;
    }
    const auto minimum = LanguageVersion::parse(since);
    if (!minimum || !isSupportedLanguageVersion(*minimum)) {
      error = "compiler standard-library module has unsupported since version '" +
              since + "'";
      return std::nullopt;
    }
    module.minimum_language_version = *minimum;
    module.imports = std::move(imports);
    module.runtime_symbols = std::move(runtime_symbols);
    if (!std::ranges::is_sorted(module.imports) ||
        std::adjacent_find(module.imports.begin(), module.imports.end()) !=
            module.imports.end() ||
        !std::ranges::is_sorted(module.runtime_symbols) ||
        std::adjacent_find(module.runtime_symbols.begin(),
                           module.runtime_symbols.end()) !=
            module.runtime_symbols.end() ||
        std::ranges::any_of(module.imports,
                            [](const auto &name) {
                              return name.empty() ||
                                     !(name == "std" ||
                                       name.starts_with("std::"));
                            }) ||
        std::ranges::any_of(module.runtime_symbols, [](const auto &symbol) {
          return symbol.empty();
        })) {
      error = "compiler standard-library module has non-canonical imports or "
              "runtime symbols";
      return std::nullopt;
    }
    for (const auto &entry : intrinsic_entries) {
      const auto equal = entry.find('=');
      const auto entity = entry.substr(0, equal);
      const auto role_name = equal == std::string::npos
                                 ? std::string_view{}
                                 : std::string_view(entry).substr(equal + 1);
      const auto role = compiler::parseCompilerIntrinsicRole(role_name);
      if (entity.empty() || !role ||
          std::ranges::any_of(
              module.compiler_intrinsics, [&](const auto &item) {
                return item.entity_name == entity || item.role == *role;
              })) {
        error = "compiler standard-library module has invalid intrinsic binding '" +
                entry + "'";
        return std::nullopt;
      }
      module.compiler_intrinsics.push_back({.module_name = module.module_name,
                                            .entity_name = entity,
                                            .role = *role});
    }
    module.source_path = normalizeCompilerInputPath(
        (std::filesystem::path(root) / module.relative_path).string());
    const auto relative = std::filesystem::path(module.source_path)
                              .lexically_relative(std::filesystem::path(root));
    if (module.source_path.empty() || relative.empty() ||
        relative.is_absolute() || *relative.begin() == "..") {
      error = "compiler standard-library module escapes its resource root";
      return std::nullopt;
    }
    CompilerInputFile source;
    if (!file_system.readText(module.source_path, source, error))
      return std::nullopt;
    if (!source.exists || !source.text) {
      error = "compiler standard-library source is missing: " + module.source_path;
      return std::nullopt;
    }
    module.source_fingerprint =
        compiler::StableFingerprint::fromCanonicalBytes(*source.text);
    result.modules_.push_back(std::move(module));
  }
  if (!std::ranges::is_sorted(result.modules_, {},
                              &CompilerStandardLibraryModule::module_name)) {
    error = "compiler standard-library manifest has a non-canonical module order";
    return std::nullopt;
  }
  for (std::size_t index = 0; index < result.modules_.size(); ++index) {
    if ((index != 0 && result.modules_[index - 1].module_name ==
                           result.modules_[index].module_name) ||
        std::ranges::count(result.modules_,
                           result.modules_[index].relative_path,
                           &CompilerStandardLibraryModule::relative_path) != 1) {
      error = "compiler standard-library manifest has duplicate modules or paths";
      return std::nullopt;
    }
    for (const auto &dependency : result.modules_[index].imports) {
      if (dependency == result.modules_[index].module_name ||
          std::ranges::find(result.modules_, dependency,
                            &CompilerStandardLibraryModule::module_name) ==
              result.modules_.end()) {
        error = "compiler standard-library module '" +
                result.modules_[index].module_name +
                "' has an invalid module dependency '" + dependency + "'";
        return std::nullopt;
      }
    }
  }
  std::ostringstream canonical;
  canonical << "chtholly.next.standard-library-distribution.v2\n";
  canonical << result.format_version_ << '\n'
            << result.compiler_contract_epoch_ << '\n'
            << result.library_api_epoch_ << '\n';
  appendField(canonical, result.package_name_);
  canonical << result.modules_.size() << '\n';
  for (const auto &module : result.modules_) {
    appendField(canonical, module.module_name);
    appendField(canonical, module.relative_path);
    appendField(canonical, module.minimum_language_version.str());
    canonical << module.imports.size() << '\n';
    for (const auto &import : module.imports)
      appendField(canonical, import);
    canonical << module.runtime_symbols.size() << '\n';
    for (const auto &symbol : module.runtime_symbols)
      appendField(canonical, symbol);
    canonical << module.compiler_intrinsics.size() << '\n';
    for (const auto &binding : module.compiler_intrinsics) {
      appendField(canonical, binding.entity_name);
      appendField(canonical, compiler::compilerIntrinsicRoleName(binding.role));
    }
    appendField(canonical, module.source_fingerprint.hex());
  }
  result.distribution_fingerprint_ =
      compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
  return result;
}

} // namespace chtholly
