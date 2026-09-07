#include "chtholly/Driver/PackageArtifactClosure.h"

#include "chtholly/Compiler/ComponentABI.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace chtholly {
namespace {

std::filesystem::path normalizedAbsolute(const std::filesystem::path &path) {
  std::error_code ec;
  auto result = std::filesystem::absolute(path, ec);
  if (ec) {
    result = path;
  }
  ec.clear();
  const auto canonical = std::filesystem::weakly_canonical(result, ec);
  return (ec ? result : canonical).lexically_normal();
}

std::filesystem::path lexicalAbsolute(const std::filesystem::path &path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal();
}

std::filesystem::path
referencedSourcePath(const PackageArtifactManifest &manifest,
                     std::string_view relative_path) {
  return lexicalAbsolute(
      std::filesystem::path(manifest.manifest_path).parent_path() /
      std::filesystem::path(std::string(relative_path)));
}

bool pathWithin(const std::filesystem::path &path,
                const std::filesystem::path &root) {
  const auto candidate = normalizedAbsolute(path);
  const auto boundary = normalizedAbsolute(root);
  auto candidate_it = candidate.begin();
  for (auto root_it = boundary.begin(); root_it != boundary.end();
       ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::string genericUtf8(const std::filesystem::path &path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}

bool addFile(PackageArtifactClosure &closure,
             std::map<std::string, std::string> &known_files,
             const std::filesystem::path &absolute_path, std::string &error) {
  if (!pathWithin(absolute_path, closure.root_directory)) {
    error = "package artifact closure file escapes root: '" +
            absolute_path.string() + "'";
    return false;
  }
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(absolute_path, ec);
  if (ec || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    error = "package artifact closure requires a regular non-symlink file: '" +
            absolute_path.string() + "'";
    return false;
  }
  const auto relative =
      std::filesystem::relative(normalizedAbsolute(absolute_path),
                                normalizedAbsolute(closure.root_directory), ec);
  if (ec || relative.empty() || relative.is_absolute()) {
    error = "failed to describe package artifact closure file: '" +
            absolute_path.string() + "'";
    return false;
  }
  const auto relative_text = genericUtf8(relative.lexically_normal());
  if (relative_text.starts_with("../") || relative_text == ".." ||
      relative_text.find('\\') != std::string::npos) {
    error = "package artifact closure contains an unsafe path: '" +
            relative_text + "'";
    return false;
  }
  const auto digest = sha256File(absolute_path.string());
  if (!digest) {
    error = "failed to hash package artifact closure file: '" +
            absolute_path.string() + "'";
    return false;
  }
  const auto existing = known_files.find(relative_text);
  if (existing != known_files.end()) {
    if (existing->second != *digest) {
      error = "package artifact closure path has conflicting contents: '" +
              relative_text + "'";
      return false;
    }
    return true;
  }
  const auto size = std::filesystem::file_size(absolute_path, ec);
  if (ec) {
    error = "failed to measure package artifact closure file: '" +
            absolute_path.string() + "'";
    return false;
  }
  known_files.emplace(relative_text, *digest);
  closure.files.push_back({relative_text,
                           normalizedAbsolute(absolute_path).string(), *digest,
                           size});
  return true;
}

bool verifyManifestPayload(const PackageArtifactManifest &manifest,
                           const TargetInfo &target, AbiVersion abi_version,
                           std::string &error) {
  const auto verify = [&](const PackageArtifactFile &file,
                          std::string_view subject) {
    const auto path = packageArtifactResolvedPath(manifest, file.relative_path);
    const auto digest = sha256File(path);
    if (!digest || *digest != file.sha256) {
      error = "package artifact " + std::string(subject) +
              " is missing or has a SHA-256 mismatch: '" + path + "'";
      return false;
    }
    return true;
  };
  if (!validatePackageArtifactCompatibility(manifest, target, abi_version,
                                            error)) {
    return false;
  }
  if (manifest.modules.empty()) {
    if (!verify(manifest.object, "object"))
      return false;
  } else {
    for (const auto &module : manifest.modules)
      if (!verify(module.object,
                  "object for module '" + module.module_name + "'"))
        return false;
  }
  for (const auto &interface : manifest.interfaces) {
    if (!verify(interface.file, "interface '" + interface.module_name + "'")) {
      return false;
    }
    const auto path =
        packageArtifactResolvedPath(manifest, interface.file.relative_path);
    const auto text = readTextFile(path, error);
    if (text && !text->starts_with("CHTCSI17")) {
      error = "package artifact interface requires CSI v17";
      return false;
    }
    if (!text) {
      return false;
    }
  }
  if (manifest.interop_bundle &&
      !verify(*manifest.interop_bundle, "Interop bundle")) {
    return false;
  }
  if (manifest.cffi_receipt &&
      !verify(manifest.cffi_receipt->file, "CFFI receipt")) {
    return false;
  }
  if (manifest.component_contract &&
      !verify(*manifest.component_contract, "component contract"))
    return false;
  if (manifest.component_contract) {
    const auto path = packageArtifactResolvedPath(
        manifest, manifest.component_contract->relative_path);
    const auto text = readTextFile(path, error);
    const auto contract =
        text ? compiler::ComponentContractArtifact::decode(*text, error)
             : std::optional<compiler::ComponentContractArtifact>{};
    if (!contract || contract->abi_epoch != manifest.component_abi_epoch ||
        contract->identity != manifest.component_identity ||
        contract->contract_digest.hex() != manifest.component_contract_digest) {
      if (error.empty())
        error = "package artifact component metadata disagrees with CHNXCMP1";
      return false;
    }
  }
  return true;
}

} // namespace

std::optional<PackageArtifactClosure>
loadPackageArtifactClosure(const std::string &root_manifest_path,
                           std::string &error) {
  const auto root_path = normalizedAbsolute(root_manifest_path);
  const auto root_text = readTextFile(root_path.string(), error);
  if (!root_text) {
    return std::nullopt;
  }
  auto root_manifest =
      parsePackageArtifactManifest(*root_text, root_path.string(), error);
  if (!root_manifest) {
    return std::nullopt;
  }

  PackageArtifactClosure closure;
  closure.root_directory =
      normalizedAbsolute(root_path.parent_path().parent_path()).string();
  if (!pathWithin(root_path, closure.root_directory)) {
    error = "package artifact root manifest is outside its closure root";
    return std::nullopt;
  }
  closure.root_artifact_identity = root_manifest->artifact_identity;

  std::map<std::string, std::string> known_files;
  std::map<std::string, std::string> identities;
  std::set<std::string> visiting;
  std::set<std::string> visited;

  std::function<bool(const std::filesystem::path &, std::string_view,
                     std::string_view)>
      visit;
  visit = [&](const std::filesystem::path &manifest_path,
              std::string_view expected_identity,
              std::string_view expected_digest) -> bool {
    const auto source_path = lexicalAbsolute(manifest_path);
    const auto normalized = normalizedAbsolute(source_path);
    const auto key = normalized.string();
    if (!pathWithin(normalized, closure.root_directory)) {
      error = "package artifact dependency manifest escapes closure root: '" +
              key + "'";
      return false;
    }
    if (visiting.contains(key)) {
      error = "package artifact dependency closure contains a cycle at '" +
              key + "'";
      return false;
    }
    if (visited.contains(key)) {
      return true;
    }
    if (!addFile(closure, known_files, source_path, error)) {
      return false;
    }
    if (!expected_digest.empty()) {
      const auto digest = sha256File(key);
      if (!digest || *digest != expected_digest) {
        error = "package artifact dependency manifest SHA-256 mismatch: '" +
                key + "'";
        return false;
      }
    }

    const auto text = readTextFile(key, error);
    auto manifest = text ? parsePackageArtifactManifest(*text, key, error)
                         : std::optional<PackageArtifactManifest>{};
    if (!manifest ||
        !verifyManifestPayload(*manifest, root_manifest->target,
                               root_manifest->abi_version, error)) {
      return false;
    }
    if (!expected_identity.empty() &&
        manifest->artifact_identity != expected_identity) {
      error = "package artifact dependency identity mismatch for '" +
              manifest->package_name + "'";
      return false;
    }
    const auto identity_owner = identities.find(manifest->artifact_identity);
    if (identity_owner != identities.end() && identity_owner->second != key) {
      error =
          "package artifact identity is represented by multiple manifests: " +
          manifest->artifact_identity;
      return false;
    }
    identities[manifest->artifact_identity] = key;
    visiting.insert(key);

    if (manifest->modules.empty()) {
      if (!addFile(
              closure, known_files,
              referencedSourcePath(*manifest, manifest->object.relative_path),
              error))
        return false;
    } else {
      for (const auto &module : manifest->modules) {
        if (!addFile(
                closure, known_files,
                referencedSourcePath(*manifest, module.object.relative_path),
                error))
          return false;
      }
    }
    for (const auto &interface : manifest->interfaces) {
      if (!addFile(
              closure, known_files,
              referencedSourcePath(*manifest, interface.file.relative_path),
              error)) {
        return false;
      }
    }
    if (manifest->interop_bundle &&
        !addFile(closure, known_files,
                 referencedSourcePath(*manifest,
                                      manifest->interop_bundle->relative_path),
                 error)) {
      return false;
    }
    if (manifest->cffi_receipt &&
        !addFile(closure, known_files,
                 referencedSourcePath(
                     *manifest, manifest->cffi_receipt->file.relative_path),
                 error)) {
      return false;
    }
    if (manifest->component_contract &&
        !addFile(closure, known_files,
                 referencedSourcePath(
                     *manifest, manifest->component_contract->relative_path),
                 error))
      return false;
    const auto manifest_copy = *manifest;
    closure.manifests.push_back(manifest_copy);
    for (const auto &dependency : manifest_copy.dependencies) {
      const auto dependency_path = referencedSourcePath(
          manifest_copy, dependency.manifest.relative_path);
      if (!visit(dependency_path, dependency.artifact_identity,
                 dependency.manifest.sha256)) {
        return false;
      }
    }
    visiting.erase(key);
    visited.insert(key);
    return true;
  };

  if (!visit(root_path, root_manifest->artifact_identity, {})) {
    return std::nullopt;
  }
  std::error_code ec;
  const auto root_relative = std::filesystem::relative(
      root_path, normalizedAbsolute(closure.root_directory), ec);
  if (ec) {
    error = "failed to describe package artifact root manifest";
    return std::nullopt;
  }
  closure.root_manifest_relative_path = genericUtf8(root_relative);
  std::sort(closure.files.begin(), closure.files.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.relative_path < rhs.relative_path;
            });
  std::sort(closure.manifests.begin(), closure.manifests.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.artifact_identity < rhs.artifact_identity;
            });
  return closure;
}

std::string
canonicalPackageArtifactClosureIndex(const PackageArtifactClosure &closure) {
  std::ostringstream out;
  out << "chtholly-archive-index-v1\n"
      << "root\t" << closure.root_manifest_relative_path << '\t'
      << closure.root_artifact_identity << '\n';
  for (const auto &file : closure.files) {
    out << "file\t" << file.relative_path << '\t' << file.sha256 << '\t'
        << file.size << '\n';
  }
  out << "end\n";
  return out.str();
}

std::string
packageArtifactClosureDigest(const PackageArtifactClosure &closure) {
  return sha256Hex(canonicalPackageArtifactClosureIndex(closure));
}

} // namespace chtholly
