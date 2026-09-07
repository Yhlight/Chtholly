#include "chtholly/Driver/PackageArtifact.h"

#include "chtholly/Driver/ArtifactCompatibility.h"
#include "chtholly/Compiler/ComponentABI.h"
#include "chtholly/Compiler/InteropArtifact.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace chtholly {

namespace {

constexpr std::string_view kHeaderV15 = "chtholly-package-artifact-v15";

void writeCFFIReceiptRecord(std::ostringstream &out,
                            const PackageArtifactCFFIReceipt &receipt) {
  const auto &identity = receipt.identity;
  out << "cffi-receipt\t" << identity.fingerprint().hex() << '\t'
      << identity.target << '\t' << identity.compiler_family << '\t'
      << identity.clang_version << '\t' << identity.libclang << '\t'
      << identity.compiler << '\t' << identity.compiler_version << '\t'
      << identity.toolchain << '\t' << identity.sdk << '\t' << identity.config
      << '\t' << identity.headers << '\t' << identity.cfdl << '\t'
      << identity.probe << '\t' << identity.facts << '\t' << receipt.file.sha256
      << '\t' << receipt.file.relative_path << '\n';
}

std::vector<std::string> splitTabs(std::string_view line) {
  std::vector<std::string> fields;
  while (true) {
    const auto separator = line.find('\t');
    fields.emplace_back(line.substr(0, separator));
    if (separator == std::string_view::npos) {
      break;
    }
    line.remove_prefix(separator + 1);
  }
  return fields;
}

bool hasRecordSeparator(std::string_view value) {
  return value.find_first_of("\t\r\n") != std::string_view::npos;
}

bool isHexDigest(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool isLowerHexDigest(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool parseAbiVersion(std::string_view value, AbiVersion &version) {
  if (value == "v0") {
    version = AbiVersion::V0;
    return true;
  }
  if (value == "v1") {
    version = AbiVersion::V1;
    return true;
  }
  if (value == "v2") {
    version = AbiVersion::V2;
    return true;
  }
  return false;
}

bool pathWithin(const std::filesystem::path &path,
                const std::filesystem::path &root) {
  const auto normalized_path = path.lexically_normal();
  const auto normalized_root = root.lexically_normal();
  auto path_it = normalized_path.begin();
  for (auto root_it = normalized_root.begin(); root_it != normalized_root.end();
       ++root_it, ++path_it) {
    if (path_it == normalized_path.end() || *path_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::filesystem::path absoluteNormalized(const std::filesystem::path &path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  const auto candidate = ec ? path : absolute;
  ec.clear();
  const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
  return (ec ? candidate : canonical).lexically_normal();
}

bool validateRelativePath(std::string_view value, bool dependency,
                          const std::string &manifest_path,
                          std::string &error) {
  if (value.empty() || hasRecordSeparator(value)) {
    error = "package artifact contains an invalid empty path";
    return false;
  }
  const std::filesystem::path relative{std::string(value)};
  if (relative.is_absolute() || relative.has_root_name() ||
      relative.has_root_directory()) {
    error =
        "package artifact path must be relative: '" + std::string(value) + "'";
    return false;
  }
  const auto manifest_dir =
      absoluteNormalized(std::filesystem::path(manifest_path).parent_path());
  const auto root = dependency ? manifest_dir.parent_path() : manifest_dir;
  const auto resolved = absoluteNormalized(manifest_dir / relative);
  if (!pathWithin(resolved, root)) {
    error = "package artifact path escapes its closure: '" +
            std::string(value) + "'";
    return false;
  }
  return true;
}

bool validateUniqueSorted(const std::vector<std::string> &values,
                          std::string_view subject, std::string &error) {
  for (std::size_t index = 1; index < values.size(); ++index) {
    if (values[index - 1] >= values[index]) {
      error = "package artifact " + std::string(subject) +
              " must be unique and sorted";
      return false;
    }
  }
  return true;
}

std::string canonicalIdentityInput(const PackageArtifactManifest &manifest) {
  std::ostringstream out;
  out << kHeaderV15 << '\n'
      << "package\t" << manifest.package_name << '\n'
      << "semantic-interface-format\t" << manifest.semantic_interface_format
      << '\n'
      << "target\t" << manifest.target.triple << '\n'
      << "pointer-width\t" << manifest.target.pointer_width_bits << '\n'
      << "abi-version\t" << abiVersionSpelling(manifest.abi_version) << '\n'
      << "runtime-abi\t" << manifest.runtime_abi << '\n'
      << "default-features\t" << (manifest.default_features ? "true" : "false")
      << '\n';
  for (const auto &feature : manifest.resolved_features) {
    out << "feature\t" << feature << '\n';
  }
  if (!manifest.modules.empty()) {
    for (const auto &module : manifest.modules) {
      out << "module\t" << module.module_name << '\t' << module.object.sha256
          << '\t' << module.object.relative_path << '\t'
          << module.interface.contract_digest << '\t'
          << module.interface.implementation_digest << '\t'
          << module.interface.file.sha256 << '\t'
          << module.interface.file.relative_path << '\t'
          << module.template_ir_digest << '\n';
      for (const auto &dependency : module.dependencies) {
        out << "module-dependency\t" << module.module_name << '\t'
            << dependency.module_name << '\t' << dependency.contract_digest
            << '\n';
        for (const auto &key : dependency.template_use_keys)
          out << "module-template-use\t" << module.module_name << '\t'
              << dependency.module_name << '\t' << key << '\n';
        for (const auto &edge : dependency.semantic_edges) {
          out << "module-semantic-edge\t" << module.module_name << '\t'
              << dependency.module_name << '\t'
              << static_cast<unsigned>(edge.kind) << '\t' << edge.consumer
              << '\t' << edge.query_key << '\t' << edge.record_id << '\t'
              << edge.result_digest << '\n';
        }
      }
    }
  } else {
    out << "object\t" << manifest.object.sha256 << '\n';
    for (const auto &interface : manifest.interfaces) {
      out << "interface\t" << interface.module_name << '\t'
          << interface.contract_digest << '\t'
          << interface.implementation_digest << '\t' << interface.file.sha256
          << '\n';
    }
  }
  if (manifest.interop_bundle) {
    out << "interop-bundle\t" << manifest.interop_bundle->sha256 << '\t'
        << manifest.interop_bundle->relative_path << '\n';
  }
  if (manifest.cffi_receipt)
    writeCFFIReceiptRecord(out, *manifest.cffi_receipt);
  if (manifest.component_contract) {
    out << "component-contract\t" << manifest.component_abi_epoch << '\t'
        << manifest.component_identity << '\t'
        << manifest.component_contract_digest << '\t'
        << manifest.component_contract->sha256 << '\t'
        << manifest.component_contract->relative_path << '\n';
  }
  for (const auto &dependency : manifest.dependencies) {
    out << "dependency\t" << dependency.package_name << '\t'
        << dependency.artifact_identity << '\t' << dependency.manifest.sha256
        << '\t' << (dependency.default_features ? "true" : "false") << '\n';
    for (const auto &feature : dependency.requested_features) {
      out << "dependency-feature\t" << dependency.package_name << '\t'
          << feature << '\n';
    }
  }
  for (const auto &library : manifest.native_link_libraries) {
    out << "link-library\t" << library << '\n';
  }
  return out.str();
}

bool validateStructure(PackageArtifactManifest &manifest, std::string &error) {
  if (manifest.package_name.empty() ||
      hasRecordSeparator(manifest.package_name)) {
    error = "package artifact package name is missing or invalid";
    return false;
  }
  if (manifest.producer_compiler.empty() ||
      hasRecordSeparator(manifest.producer_compiler)) {
    error = "package artifact producer compiler is missing or invalid";
    return false;
  }
  if (manifest.semantic_interface_format.empty() ||
      manifest.target.triple.empty() ||
      manifest.target.pointer_width_bits == 0 || manifest.runtime_abi.empty()) {
    error = "package artifact compatibility metadata is incomplete";
    return false;
  }
  if (manifest.modules.empty()) {
    if (!isHexDigest(manifest.object.sha256, 64) ||
        !validateRelativePath(manifest.object.relative_path, false,
                              manifest.manifest_path, error)) {
      if (error.empty())
        error = "package artifact object digest is invalid";
      return false;
    }
  } else if (!manifest.object.sha256.empty() || !manifest.interfaces.empty()) {
    error = "package artifact cannot mix module and single-module records";
    return false;
  }
  if (manifest.interop_bundle &&
      (!isLowerHexDigest(manifest.interop_bundle->sha256, 64) ||
       !validateRelativePath(manifest.interop_bundle->relative_path, false,
                             manifest.manifest_path, error))) {
    if (error.empty())
      error = "package artifact Interop bundle record is invalid";
    return false;
  }
  if (manifest.cffi_receipt &&
      (manifest.cffi_receipt->identity.target != manifest.target.triple ||
       !manifest.cffi_receipt->identity.fingerprint().hasValue() ||
       !isLowerHexDigest(manifest.cffi_receipt->file.sha256, 64) ||
       !validateRelativePath(manifest.cffi_receipt->file.relative_path, false,
                             manifest.manifest_path, error))) {
    if (error.empty())
      error = "package artifact CFFI receipt record is invalid";
    return false;
  }
  if (manifest.component_contract) {
    if (manifest.component_abi_epoch != 1 ||
        manifest.component_identity.empty() ||
        hasRecordSeparator(manifest.component_identity) ||
        !isLowerHexDigest(manifest.component_contract_digest, 64) ||
        !isLowerHexDigest(manifest.component_contract->sha256, 64) ||
        !validateRelativePath(manifest.component_contract->relative_path, false,
                              manifest.manifest_path, error)) {
      if (error.empty())
        error = "package artifact component contract record is invalid";
      return false;
    }
  } else if (manifest.component_abi_epoch != 0 ||
             !manifest.component_identity.empty() ||
             !manifest.component_contract_digest.empty()) {
    error = "package artifact component metadata has no contract file";
    return false;
  }
  if (!validateUniqueSorted(manifest.resolved_features, "features", error)) {
    return false;
  }
  std::string previous_module;
  for (auto &interface : manifest.interfaces) {
    if (interface.module_name.empty() ||
        (!previous_module.empty() &&
         previous_module >= interface.module_name)) {
      error = "package artifact interfaces must be named, unique and sorted";
      return false;
    }
    previous_module = interface.module_name;
    if (!isHexDigest(interface.contract_digest, 64) ||
        !isHexDigest(interface.implementation_digest, 64) ||
        !isHexDigest(interface.file.sha256, 64) ||
        !validateRelativePath(interface.file.relative_path, false,
                              manifest.manifest_path, error)) {
      if (error.empty()) {
        error = "package artifact interface record is invalid for module '" +
                interface.module_name + "'";
      }
      return false;
    }
  }
  std::string previous_module_record;
  for (auto &module : manifest.modules) {
    if (module.module_name.empty() ||
        (!previous_module_record.empty() &&
         previous_module_record >= module.module_name)) {
      error = "package artifact modules must be named, unique and sorted";
      return false;
    }
    previous_module_record = module.module_name;
    if (module.interface.module_name != module.module_name ||
        !isHexDigest(module.object.sha256, 64) ||
        !validateRelativePath(module.object.relative_path, false,
                              manifest.manifest_path, error) ||
        !isHexDigest(module.interface.contract_digest, 64) ||
        !isHexDigest(module.interface.implementation_digest, 64) ||
        !isHexDigest(module.interface.file.sha256, 64) ||
        !isHexDigest(module.template_ir_digest, 64) ||
        !validateRelativePath(module.interface.file.relative_path, false,
                              manifest.manifest_path, error)) {
      if (error.empty())
        error = "package artifact module record is invalid";
      return false;
    }
    std::string previous_dependency;
    for (auto &dependency : module.dependencies) {
      if (dependency.module_name.empty() ||
          (!previous_dependency.empty() &&
           previous_dependency >= dependency.module_name) ||
          !isHexDigest(dependency.contract_digest, 64) ||
          !validateUniqueSorted(dependency.template_use_keys,
                                "module template uses", error)) {
        if (error.empty())
          error = "package artifact module dependency is invalid";
        return false;
      }
      previous_dependency = dependency.module_name;
      for (const auto &edge : dependency.semantic_edges) {
        if (!isHexDigest(edge.consumer, 64) ||
            edge.provider_module != dependency.module_name ||
            edge.query_key.empty() || !isHexDigest(edge.record_id, 64) ||
            !isHexDigest(edge.result_digest, 64)) {
          error = "package artifact semantic dependency edge is invalid";
          return false;
        }
      }
      if (!std::is_sorted(dependency.semantic_edges.begin(),
                          dependency.semantic_edges.end()) ||
          std::adjacent_find(dependency.semantic_edges.begin(),
                             dependency.semantic_edges.end()) !=
              dependency.semantic_edges.end()) {
        error = "package artifact semantic dependency edges must be unique and "
                "sorted";
        return false;
      }
    }
  }
  std::string previous_package;
  for (auto &dependency : manifest.dependencies) {
    if (dependency.package_name.empty() ||
        (!previous_package.empty() &&
         previous_package >= dependency.package_name)) {
      error = "package artifact dependencies must be named, unique and sorted";
      return false;
    }
    previous_package = dependency.package_name;
    if (!isHexDigest(dependency.artifact_identity, 64) ||
        !isHexDigest(dependency.manifest.sha256, 64) ||
        !validateRelativePath(dependency.manifest.relative_path, true,
                              manifest.manifest_path, error) ||
        !validateUniqueSorted(dependency.requested_features,
                              "dependency features", error)) {
      if (error.empty()) {
        error = "package artifact dependency record is invalid for package '" +
                dependency.package_name + "'";
      }
      return false;
    }
  }
  if (!validateUniqueSorted(manifest.native_link_libraries, "link libraries",
                            error)) {
    return false;
  }
  const auto expected_identity = packageArtifactIdentity(manifest);
  if (!isHexDigest(manifest.artifact_identity, 64) ||
      manifest.artifact_identity != expected_identity) {
    error = "package artifact identity mismatch";
    return false;
  }
  return true;
}

bool verifyFile(const PackageArtifactManifest &manifest,
                const PackageArtifactFile &file, std::string_view subject,
                std::string &error) {
  const auto path = packageArtifactResolvedPath(manifest, file.relative_path);
  const auto digest = sha256File(path);
  if (!digest) {
    error = "package artifact " + std::string(subject) + " is missing: '" +
            path + "'";
    return false;
  }
  if (*digest != file.sha256) {
    error = "package artifact " + std::string(subject) +
            " SHA-256 mismatch: '" + path + "'";
    return false;
  }
  return true;
}

bool verifyCFFIReceipt(const PackageArtifactManifest &manifest,
                       const PackageArtifactCFFIReceipt &receipt,
                       std::string &error) {
  if (!verifyFile(manifest, receipt.file, "CFFI receipt", error))
    return false;
  const auto path =
      packageArtifactResolvedPath(manifest, receipt.file.relative_path);
  const auto text = readTextFile(path, error);
  if (!text)
    return false;
  auto parsed = compiler::parseCFFIReceipt(*text, error);
  if (!parsed) {
    error =
        "package artifact CFFI receipt is invalid: '" + path + "': " + error;
    return false;
  }
  if (*parsed != receipt.identity || parsed->target != manifest.target.triple ||
      parsed->fingerprint().hex() != receipt.file.sha256) {
    error = "package artifact CFFI receipt identity mismatch: '" + path + "'";
    return false;
  }
  return true;
}

} // namespace

std::string packageArtifactIdentity(const PackageArtifactManifest &manifest) {
  return sha256Hex(canonicalIdentityInput(manifest));
}

std::string packageArtifactResolvedPath(const PackageArtifactManifest &manifest,
                                        std::string_view relative_path) {
  return absoluteNormalized(
             std::filesystem::path(manifest.manifest_path).parent_path() /
             std::filesystem::path(std::string(relative_path)))
      .string();
}

bool loadPackageArtifactInterop(const PackageArtifactManifest &manifest,
                                compiler::interop::ArtifactRegistry &registry,
                                std::string &error) {
  error.clear();
  if (!manifest.interop_bundle)
    return true;
  const auto path = packageArtifactResolvedPath(
      manifest, manifest.interop_bundle->relative_path);
  const auto digest = sha256File(path);
  if (!digest || *digest != manifest.interop_bundle->sha256) {
    error = "package artifact Interop bundle is missing or has a SHA-256 "
            "mismatch: '" +
            path + "'";
    return false;
  }
  compiler::interop::ArtifactBundle bundle;
  if (!compiler::interop::readArtifactBundle(path, bundle, error))
    return false;
  for (const auto &record : bundle.records) {
    if (record.reference.canonical_package != manifest.package_name) {
      error = "package artifact Interop bundle contains a reference for a "
              "different package";
      return false;
    }
  }
  if (!registry.registerBundle(bundle, error))
    return false;
  return true;
}

std::optional<PackageArtifactManifest>
parsePackageArtifactManifest(std::string_view text, std::string manifest_path,
                             std::string &error) {
  PackageArtifactManifest manifest;
  manifest.manifest_path = std::move(manifest_path);
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line)) {
    error = "invalid Chtholly package artifact header";
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != kHeaderV15) {
    error = "unsupported Chtholly package artifact format; rebuild the "
            "artifact with the current compiler";
    return std::nullopt;
  }
  manifest.format_version = 21;

  std::set<std::string> scalar_fields;
  std::map<std::string, std::vector<std::string>> dependency_features;
  bool found_end = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line == "end") {
      found_end = true;
      break;
    }
    const auto fields = splitTabs(line);
    if (fields.empty() || fields[0].empty()) {
      error = "invalid package artifact record";
      return std::nullopt;
    }
    const auto scalar = [&](std::string &target) -> bool {
      if (fields.size() != 2 || !scalar_fields.insert(fields[0]).second) {
        error =
            "invalid or duplicate package artifact field '" + fields[0] + "'";
        return false;
      }
      target = fields[1];
      return true;
    };
    if (fields[0] == "package") {
      if (!scalar(manifest.package_name))
        return std::nullopt;
    } else if (fields[0] == "identity") {
      if (!scalar(manifest.artifact_identity))
        return std::nullopt;
    } else if (fields[0] == "producer-compiler") {
      if (!scalar(manifest.producer_compiler))
        return std::nullopt;
    } else if (fields[0] == "semantic-interface-format") {
      if (!scalar(manifest.semantic_interface_format))
        return std::nullopt;
    } else if (fields[0] == "target") {
      if (!scalar(manifest.target.triple))
        return std::nullopt;
    } else if (fields[0] == "pointer-width") {
      std::string value;
      if (!scalar(value))
        return std::nullopt;
      const auto [end, conversion_error] =
          std::from_chars(value.data(), value.data() + value.size(),
                          manifest.target.pointer_width_bits);
      if (conversion_error != std::errc{} ||
          end != value.data() + value.size()) {
        error = "invalid package artifact pointer width";
        return std::nullopt;
      }
    } else if (fields[0] == "abi-version") {
      std::string value;
      if (!scalar(value))
        return std::nullopt;
      if (!parseAbiVersion(value, manifest.abi_version)) {
        error = "invalid package artifact ABI version '" + value + "'";
        return std::nullopt;
      }
    } else if (fields[0] == "runtime-abi") {
      if (!scalar(manifest.runtime_abi))
        return std::nullopt;
    } else if (fields[0] == "default-features") {
      std::string value;
      if (!scalar(value))
        return std::nullopt;
      if (value != "true" && value != "false") {
        error = "invalid package artifact default-features value";
        return std::nullopt;
      }
      manifest.default_features = value == "true";
    } else if (fields[0] == "feature" && fields.size() == 2) {
      manifest.resolved_features.push_back(fields[1]);
    } else if (fields[0] == "object" && fields.size() == 3) {
      if (!manifest.object.sha256.empty()) {
        error = "duplicate package artifact object record";
        return std::nullopt;
      }
      manifest.object = {fields[1], fields[2]};
    } else if (fields[0] == "interop-bundle" && fields.size() == 3) {
      if (manifest.interop_bundle) {
        error = "duplicate package artifact Interop bundle record";
        return std::nullopt;
      }
      manifest.interop_bundle = PackageArtifactFile{fields[1], fields[2]};
    } else if (fields[0] == "cffi-receipt" && fields.size() == 17) {
      if (manifest.cffi_receipt) {
        error = "duplicate package artifact CFFI receipt record";
        return std::nullopt;
      }
      compiler::CFFIReceiptIdentity identity{
          .target = fields[2],
          .compiler_family = fields[3],
          .clang_version = fields[4],
          .libclang = fields[5],
          .compiler = fields[6],
          .compiler_version = fields[7],
          .toolchain = fields[8],
          .sdk = fields[9],
          .config = fields[10],
          .headers = fields[11],
          .cfdl = fields[12],
          .probe = fields[13],
          .facts = fields[14],
      };
      if (identity.fingerprint().hex() != fields[1]) {
        error = "package artifact CFFI receipt identity field mismatch";
        return std::nullopt;
      }
      manifest.cffi_receipt = PackageArtifactCFFIReceipt{
          std::move(identity), PackageArtifactFile{fields[15], fields[16]}};
    } else if (fields[0] == "component-contract" && fields.size() == 6) {
      if (manifest.component_contract) {
        error = "duplicate package artifact component contract record";
        return std::nullopt;
      }
      const auto parsed =
          std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(),
                          manifest.component_abi_epoch);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != fields[1].data() + fields[1].size()) {
        error = "invalid package artifact component ABI epoch";
        return std::nullopt;
      }
      manifest.component_identity = fields[2];
      manifest.component_contract_digest = fields[3];
      manifest.component_contract = PackageArtifactFile{fields[4], fields[5]};
    } else if (fields[0] == "interface" && fields.size() == 6) {
      manifest.interfaces.push_back(
          {fields[1], fields[2], fields[3], {fields[4], fields[5]}});
    } else if (fields[0] == "module" && fields.size() == 9) {
      PackageArtifactModule module;
      module.module_name = fields[1];
      module.object = {fields[2], fields[3]};
      module.interface = {
          fields[1], fields[4], fields[5], {fields[6], fields[7]}};
      module.template_ir_digest = fields[8];
      manifest.modules.push_back(std::move(module));
    } else if (fields[0] == "module-dependency" && fields.size() == 4) {
      const auto found = std::find_if(
          manifest.modules.begin(), manifest.modules.end(),
          [&](const auto &module) { return module.module_name == fields[1]; });
      if (found == manifest.modules.end()) {
        error = "package artifact module dependency references unknown module";
        return std::nullopt;
      }
      found->dependencies.push_back({fields[2], fields[3], {}});
    } else if (fields[0] == "module-template-use" && fields.size() == 4) {
      const auto found = std::find_if(
          manifest.modules.begin(), manifest.modules.end(),
          [&](const auto &module) { return module.module_name == fields[1]; });
      if (found == manifest.modules.end() || found->dependencies.empty() ||
          found->dependencies.back().module_name != fields[2]) {
        error = "package artifact module template use is out of order";
        return std::nullopt;
      }
      found->dependencies.back().template_use_keys.push_back(fields[3]);
    } else if (fields[0] == "module-semantic-edge" && fields.size() == 8) {
      const auto found = std::find_if(
          manifest.modules.begin(), manifest.modules.end(),
          [&](const auto &module) { return module.module_name == fields[1]; });
      if (found == manifest.modules.end() || found->dependencies.empty() ||
          found->dependencies.back().module_name != fields[2]) {
        error = "package artifact semantic edge is out of order";
        return std::nullopt;
      }
      unsigned kind = 0;
      const auto parsed = std::from_chars(
          fields[3].data(), fields[3].data() + fields[3].size(), kind);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != fields[3].data() + fields[3].size() || kind < 1 ||
          kind > 12u) {
        error = "package artifact semantic edge kind is invalid";
        return std::nullopt;
      }
      ArtifactDependencyEdge edge{
          fields[4], static_cast<ArtifactDependencyKind>(kind),
          fields[2], fields[5],
          fields[6], fields[7]};
      found->dependencies.back().semantic_edges.push_back(std::move(edge));
    } else if (fields[0] == "dependency" && fields.size() == 6) {
      if (fields[5] != "true" && fields[5] != "false") {
        error = "invalid package artifact dependency default-features value";
        return std::nullopt;
      }
      manifest.dependencies.push_back({fields[1],
                                       fields[2],
                                       {fields[3], fields[4]},
                                       {},
                                       fields[5] == "true"});
    } else if (fields[0] == "dependency-feature" && fields.size() == 3) {
      dependency_features[fields[1]].push_back(fields[2]);
    } else if (fields[0] == "link-library" && fields.size() == 2) {
      manifest.native_link_libraries.push_back(fields[1]);
    } else {
      error =
          "unknown or malformed package artifact record '" + fields[0] + "'";
      return std::nullopt;
    }
  }
  if (!found_end) {
    error = "package artifact end marker is missing";
    return std::nullopt;
  }
  while (std::getline(input, line)) {
    if (!line.empty() && line != "\r") {
      error = "package artifact contains data after its end marker";
      return std::nullopt;
    }
  }
  constexpr std::array<std::string_view, 9> required_scalars = {
      "package",
      "identity",
      "producer-compiler",
      "semantic-interface-format",
      "target",
      "pointer-width",
      "abi-version",
      "runtime-abi",
      "default-features"};
  for (const auto field : required_scalars) {
    if (!scalar_fields.contains(std::string(field))) {
      error = "package artifact required field is missing: '" +
              std::string(field) + "'";
      return std::nullopt;
    }
  }
  for (auto &dependency_feature : dependency_features) {
    const auto &package = dependency_feature.first;
    auto &features = dependency_feature.second;
    const auto dependency =
        std::find_if(manifest.dependencies.begin(), manifest.dependencies.end(),
                     [&](const auto &candidate) {
                       return candidate.package_name == package;
                     });
    if (dependency == manifest.dependencies.end()) {
      error =
          "package artifact dependency feature references unknown package '" +
          package + "'";
      return std::nullopt;
    }
    dependency->requested_features = std::move(features);
  }
  if (!validateStructure(manifest, error)) {
    return std::nullopt;
  }
  if (!manifest.modules.empty()) {
    manifest.object = {};
    manifest.interfaces.clear();
    for (const auto &module : manifest.modules) {
      manifest.interfaces.push_back(module.interface);
    }
  }
  return manifest;
}

bool validatePackageArtifactCompatibility(
    const PackageArtifactManifest &manifest, const TargetInfo &target,
    AbiVersion abi_version, std::string &error) {
  if (manifest.semantic_interface_format != "17") {
    error = "package artifact semantic interface format mismatch: artifact=" +
            manifest.semantic_interface_format + " consumer=17";
    return false;
  }
  const ArtifactCompatibilityKey artifact_key{
      manifest.target, manifest.abi_version, manifest.semantic_interface_format,
      manifest.semantic_interface_format, manifest.runtime_abi};
  const ArtifactCompatibilityKey consumer_key{
      target, abi_version, manifest.semantic_interface_format,
      manifest.semantic_interface_format, std::string(HostedRuntimeAbiVersion)};
  if (manifest.target.triple != target.triple) {
    error =
        "package artifact target mismatch: artifact=" + manifest.target.triple +
        " consumer=" + target.triple;
    return false;
  }
  if (manifest.target.pointer_width_bits != target.pointer_width_bits) {
    error = "package artifact pointer width mismatch: artifact=" +
            std::to_string(manifest.target.pointer_width_bits) +
            " consumer=" + std::to_string(target.pointer_width_bits);
    return false;
  }
  if (manifest.abi_version != abi_version) {
    error = "package artifact ABI mismatch: artifact=" +
            std::string(abiVersionSpelling(manifest.abi_version)) +
            " consumer=" + std::string(abiVersionSpelling(abi_version));
    return false;
  }
  if (manifest.runtime_abi != HostedRuntimeAbiVersion) {
    error = "package artifact hosted runtime ABI mismatch: artifact=" +
            manifest.runtime_abi +
            " consumer=" + std::string(HostedRuntimeAbiVersion);
    return false;
  }
  if (!(artifact_key == consumer_key)) {
    error = "package artifact compatibility key mismatch";
    return false;
  }
  return true;
}

std::optional<PackageArtifactManifest>
loadPackageArtifactManifest(const std::string &manifest_path,
                            const TargetInfo &target, AbiVersion abi_version,
                            std::string &error) {
  const auto text = readTextFile(manifest_path, error);
  if (!text) {
    return std::nullopt;
  }
  auto manifest = parsePackageArtifactManifest(*text, manifest_path, error);
  if (!manifest || !validatePackageArtifactCompatibility(*manifest, target,
                                                         abi_version, error)) {
    return std::nullopt;
  }
  if (!manifest->modules.empty()) {
    for (const auto &module : manifest->modules) {
      if (!verifyFile(*manifest, module.object,
                      "object for module '" + module.module_name + "'",
                      error) ||
          !verifyFile(*manifest, module.interface.file,
                      "interface '" + module.module_name + "'", error)) {
        return std::nullopt;
      }
      const auto interface_path = packageArtifactResolvedPath(
          *manifest, module.interface.file.relative_path);
      const auto interface_text = readTextFile(interface_path, error);
      if (interface_text && !interface_text->starts_with("CHTCSI17")) {
        error = "package artifact module interface requires CSI v17";
        return std::nullopt;
      }
      if (!interface_text) {
        return std::nullopt;
      }
    }
  } else {
    if (!verifyFile(*manifest, manifest->object, "object", error))
      return std::nullopt;
  }
  for (const auto &interface : manifest->interfaces) {
    if (!verifyFile(*manifest, interface.file,
                    "interface '" + interface.module_name + "'", error)) {
      return std::nullopt;
    }
    const auto interface_path =
        packageArtifactResolvedPath(*manifest, interface.file.relative_path);
    const auto interface_text = readTextFile(interface_path, error);
    if (interface_text && !interface_text->starts_with("CHTCSI17")) {
      error = "package artifact interface requires CSI v17";
      return std::nullopt;
    }
    if (!interface_text) {
      return std::nullopt;
    }
  }
  for (const auto &dependency : manifest->dependencies) {
    if (!verifyFile(*manifest, dependency.manifest,
                    "dependency manifest '" + dependency.package_name + "'",
                    error)) {
      return std::nullopt;
    }
  }
  if (manifest->interop_bundle &&
      !verifyFile(*manifest, *manifest->interop_bundle, "Interop bundle",
                  error)) {
    return std::nullopt;
  }
  if (manifest->cffi_receipt &&
      !verifyCFFIReceipt(*manifest, *manifest->cffi_receipt, error)) {
    return std::nullopt;
  }
  if (manifest->component_contract) {
    if (!verifyFile(*manifest, *manifest->component_contract,
                    "component contract", error))
      return std::nullopt;
    const auto path = packageArtifactResolvedPath(
        *manifest, manifest->component_contract->relative_path);
    const auto component_text = readTextFile(path, error);
    const auto component =
        component_text
            ? compiler::ComponentContractArtifact::decode(*component_text, error)
            : std::optional<compiler::ComponentContractArtifact>{};
    if (!component || component->abi_epoch != manifest->component_abi_epoch ||
        component->identity != manifest->component_identity ||
        component->contract_digest.hex() !=
            manifest->component_contract_digest) {
      if (error.empty())
        error = "package artifact component metadata disagrees with CHNXCMP1";
      return std::nullopt;
    }
  }
  return manifest;
}

bool writePackageArtifactManifest(PackageArtifactManifest &manifest,
                                  const std::string &manifest_path,
                                  std::string &error) {
  manifest.manifest_path = manifest_path;
  manifest.format_version = 21;
  if (!manifest.modules.empty()) {
    manifest.object = {};
    manifest.interfaces.clear();
  }
  std::sort(manifest.resolved_features.begin(),
            manifest.resolved_features.end());
  manifest.resolved_features.erase(
      std::unique(manifest.resolved_features.begin(),
                  manifest.resolved_features.end()),
      manifest.resolved_features.end());
  std::sort(manifest.interfaces.begin(), manifest.interfaces.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.module_name < rhs.module_name;
            });
  std::sort(manifest.modules.begin(), manifest.modules.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.module_name < rhs.module_name;
            });
  for (auto &module : manifest.modules) {
    std::sort(module.dependencies.begin(), module.dependencies.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.module_name < rhs.module_name;
              });
    for (auto &dependency : module.dependencies) {
      std::sort(dependency.template_use_keys.begin(),
                dependency.template_use_keys.end());
      dependency.template_use_keys.erase(
          std::unique(dependency.template_use_keys.begin(),
                      dependency.template_use_keys.end()),
          dependency.template_use_keys.end());
      std::sort(dependency.semantic_edges.begin(),
                dependency.semantic_edges.end());
      dependency.semantic_edges.erase(
          std::unique(dependency.semantic_edges.begin(),
                      dependency.semantic_edges.end()),
          dependency.semantic_edges.end());
    }
  }
  std::sort(manifest.dependencies.begin(), manifest.dependencies.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.package_name < rhs.package_name;
            });
  for (auto &dependency : manifest.dependencies) {
    std::sort(dependency.requested_features.begin(),
              dependency.requested_features.end());
    dependency.requested_features.erase(
        std::unique(dependency.requested_features.begin(),
                    dependency.requested_features.end()),
        dependency.requested_features.end());
  }
  std::sort(manifest.native_link_libraries.begin(),
            manifest.native_link_libraries.end());
  manifest.native_link_libraries.erase(
      std::unique(manifest.native_link_libraries.begin(),
                  manifest.native_link_libraries.end()),
      manifest.native_link_libraries.end());
  manifest.artifact_identity = packageArtifactIdentity(manifest);
  if (!validateStructure(manifest, error)) {
    return false;
  }

  std::ostringstream out;
  out << kHeaderV15 << '\n'
      << "package\t" << manifest.package_name << '\n'
      << "identity\t" << manifest.artifact_identity << '\n'
      << "producer-compiler\t" << manifest.producer_compiler << '\n'
      << "semantic-interface-format\t" << manifest.semantic_interface_format
      << '\n'
      << "target\t" << manifest.target.triple << '\n'
      << "pointer-width\t" << manifest.target.pointer_width_bits << '\n'
      << "abi-version\t" << abiVersionSpelling(manifest.abi_version) << '\n'
      << "runtime-abi\t" << manifest.runtime_abi << '\n'
      << "default-features\t" << (manifest.default_features ? "true" : "false")
      << '\n';
  for (const auto &feature : manifest.resolved_features) {
    out << "feature\t" << feature << '\n';
  }
  if (manifest.modules.empty()) {
    out << "object\t" << manifest.object.sha256 << '\t'
        << manifest.object.relative_path << '\n';
    for (const auto &interface : manifest.interfaces) {
      out << "interface\t" << interface.module_name << '\t'
          << interface.contract_digest << '\t'
          << interface.implementation_digest << '\t' << interface.file.sha256
          << '\t' << interface.file.relative_path << '\n';
    }
  }
  for (const auto &module : manifest.modules) {
    out << "module\t" << module.module_name << '\t' << module.object.sha256
        << '\t' << module.object.relative_path << '\t'
        << module.interface.contract_digest << '\t'
        << module.interface.implementation_digest << '\t'
        << module.interface.file.sha256 << '\t'
        << module.interface.file.relative_path << '\t'
        << module.template_ir_digest << '\n';
    for (const auto &dependency : module.dependencies) {
      out << "module-dependency\t" << module.module_name << '\t'
          << dependency.module_name << '\t' << dependency.contract_digest
          << '\n';
      for (const auto &key : dependency.template_use_keys)
        out << "module-template-use\t" << module.module_name << '\t'
            << dependency.module_name << '\t' << key << '\n';
      for (const auto &edge : dependency.semantic_edges)
        out << "module-semantic-edge\t" << module.module_name << '\t'
            << dependency.module_name << '\t'
            << static_cast<unsigned>(edge.kind) << '\t' << edge.consumer << '\t'
            << edge.query_key << '\t' << edge.record_id << '\t'
            << edge.result_digest << '\n';
    }
  }
  if (manifest.interop_bundle) {
    out << "interop-bundle\t" << manifest.interop_bundle->sha256 << '\t'
        << manifest.interop_bundle->relative_path << '\n';
  }
  if (manifest.cffi_receipt)
    writeCFFIReceiptRecord(out, *manifest.cffi_receipt);
  if (manifest.component_contract) {
    out << "component-contract\t" << manifest.component_abi_epoch << '\t'
        << manifest.component_identity << '\t'
        << manifest.component_contract_digest << '\t'
        << manifest.component_contract->sha256 << '\t'
        << manifest.component_contract->relative_path << '\n';
  }
  for (const auto &dependency : manifest.dependencies) {
    out << "dependency\t" << dependency.package_name << '\t'
        << dependency.artifact_identity << '\t' << dependency.manifest.sha256
        << '\t' << dependency.manifest.relative_path << '\t'
        << (dependency.default_features ? "true" : "false") << '\n';
    for (const auto &feature : dependency.requested_features) {
      out << "dependency-feature\t" << dependency.package_name << '\t'
          << feature << '\n';
    }
  }
  for (const auto &library : manifest.native_link_libraries) {
    out << "link-library\t" << library << '\n';
  }
  out << "end\n";
  return writeTextFile(manifest_path, out.str(), error);
}

} // namespace chtholly
