#include "chtholly/Driver/ProjectManifest.h"

#include "ManifestToml.h"
#include "chtholly/Driver/RegistryArtifact.h"
#include "chtholly/Support/FileSystem.h"
#include "chtholly/ToolingRules/PackageSourceContractRules.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace chtholly {

bool classifyProjectRegistryIndex(std::string_view index,
                                  ProjectRegistryIndexKind &kind,
                                  std::string &error) {
  if (index.starts_with("https://")) {
    const auto authority_end = index.find('/', 8);
    const auto authority = index.substr(
        8, authority_end == std::string_view::npos ? std::string_view::npos
                                                   : authority_end - 8);
    if (authority.empty()) {
      error = "HTTPS registry index requires a host";
      return false;
    }
    if (authority.find('@') != std::string_view::npos) {
      error = "registry index URL cannot contain credentials";
      return false;
    }
    kind = ProjectRegistryIndexKind::RemoteGit;
    return true;
  }
  if (index.starts_with("file://")) {
    const auto authority_end = index.find('/', 7);
    if (authority_end == std::string_view::npos ||
        authority_end + 1 == index.size()) {
      error = "file registry index requires a path";
      return false;
    }
    const auto authority = index.substr(
        7, authority_end == std::string_view::npos ? std::string_view::npos
                                                   : authority_end - 7);
    if (authority.find('@') != std::string_view::npos) {
      error = "registry index URL cannot contain credentials";
      return false;
    }
    kind = ProjectRegistryIndexKind::RemoteGit;
    return true;
  }
  if (index.starts_with("http://")) {
    error = "registry index rejects insecure HTTP; use HTTPS";
    return false;
  }
  const auto colon = index.find(':');
  const bool scp_like =
      colon != std::string_view::npos && colon != 1 &&
      index.substr(0, colon).find_first_of("/\\") == std::string_view::npos;
  if (index.starts_with("ssh://") || index.starts_with("git@") ||
      index.find("://") != std::string_view::npos || scp_like) {
    error =
        "registry index only supports a local path, HTTPS Git URL, or file URL";
    return false;
  }
  kind = ProjectRegistryIndexKind::LocalPath;
  return true;
}

namespace {

bool containsEmpty(const std::vector<std::string> &values) {
  for (const auto &value : values) {
    if (value.empty()) {
      return true;
    }
  }
  return false;
}

bool containsDuplicate(const std::vector<std::string> &values,
                       std::string &duplicate) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t j = i + 1; j < values.size(); ++j) {
      if (values[i] == values[j]) {
        duplicate = values[i];
        return true;
      }
    }
  }
  return false;
}

bool containsLockfileSeparator(std::string_view value) {
  return value.find('\t') != std::string_view::npos ||
         value.find('\n') != std::string_view::npos ||
         value.find('\r') != std::string_view::npos;
}

bool dependencyExists(const std::vector<ProjectDependency> &dependencies,
                      std::string_view name) {
  return std::any_of(dependencies.begin(), dependencies.end(),
                     [&](const ProjectDependency &dependency) {
                       return dependency.name == name;
                     });
}

bool featureExists(const std::vector<ProjectFeature> &features,
                   std::string_view name) {
  return std::any_of(
      features.begin(), features.end(),
      [&](const ProjectFeature &feature) { return feature.name == name; });
}

bool registryExists(const std::vector<ProjectRegistryConfig> &registries,
                    std::string_view name) {
  return std::any_of(registries.begin(), registries.end(),
                     [&](const ProjectRegistryConfig &registry) {
                       return registry.name == name;
                     });
}

std::string manifestRoot(const std::string &manifest_path) {
  const auto parent = std::filesystem::path(manifest_path).parent_path();
  if (parent.empty()) {
    return ".";
  }
  return parent.string();
}

std::string
unsupportedManifestSurfaceDiagnostic(
    UnsupportedManifestSurfaceUseKind use_kind, std::string subject,
    std::string manifest_key) {
  UnsupportedManifestSurfaceFacts facts;
  facts.use_kind = use_kind;
  facts.subject = std::move(subject);
  facts.manifest_key = std::move(manifest_key);
  const auto plan = PackageSourceContractResolver::
      resolveUnsupportedManifestSurfacePlan(facts);
  return chtholly::unsupportedManifestSurfaceDiagnostic(plan);
}

std::optional<std::string> projectUnsupportedTableReason(std::string_view table,
                                                         std::size_t line) {
  const auto line_suffix = " at line " + std::to_string(line);
  if (table == "publish") {
    return unsupportedManifestSurfaceDiagnostic(
        UnsupportedManifestSurfaceUseKind::Publishing, "",
        "[publish]" + line_suffix);
  }
  if (table == "artifact-cache" || table == "remote-artifact-cache") {
    return unsupportedManifestSurfaceDiagnostic(
        UnsupportedManifestSurfaceUseKind::RemoteArtifactCache, "",
        "[" + std::string(table) + "]" + line_suffix);
  }
  if (table == "macro-imports" || table == "macros") {
    return unsupportedManifestSurfaceDiagnostic(
        UnsupportedManifestSurfaceUseKind::MacroImport, "",
        "[" + std::string(table) + "]" + line_suffix);
  }
  return std::nullopt;
}

bool validateFeatureName(std::string_view name, std::string_view subject,
                         std::string &error) {
  if (name.empty()) {
    error = std::string(subject) + " cannot be empty";
    return false;
  }
  if (name.find('/') != std::string_view::npos) {
    error = std::string(subject) + " cannot contain '/'";
    return false;
  }
  if (containsLockfileSeparator(name)) {
    error = std::string(subject) + " cannot contain tabs or newlines";
    return false;
  }
  return true;
}

bool validateFeatureRequirement(std::string_view requirement,
                                std::string_view feature_name,
                                std::string &error) {
  if (requirement.empty()) {
    error = "feature '" + std::string(feature_name) +
            "' requirements cannot contain empty entries";
    return false;
  }
  if (containsLockfileSeparator(requirement)) {
    error = "feature '" + std::string(feature_name) +
            "' requirements cannot contain tabs or newlines";
    return false;
  }
  const auto slash = requirement.find('/');
  if (slash == std::string_view::npos) {
    return true;
  }
  if (slash == 0 || slash + 1 == requirement.size() ||
      requirement.find('/', slash + 1) != std::string_view::npos) {
    error = "feature '" + std::string(feature_name) + "' requirement '" +
            std::string(requirement) +
            "' must use feature or dependency/feature";
    return false;
  }
  return true;
}

bool validateFeatureRequirements(const std::vector<std::string> &requirements,
                                 std::string_view feature_name,
                                 std::string &error) {
  std::string duplicate;
  if (containsDuplicate(requirements, duplicate)) {
    error = "feature '" + std::string(feature_name) +
            "' contains duplicate requirement '" + duplicate + "'";
    return false;
  }
  for (const auto &requirement : requirements) {
    if (!validateFeatureRequirement(requirement, feature_name, error)) {
      return false;
    }
  }
  return true;
}

bool validateDependencyFeatures(const std::vector<std::string> &features,
                                std::string_view dependency_name,
                                std::string &error) {
  std::string duplicate;
  if (containsDuplicate(features, duplicate)) {
    error = "dependency '" + std::string(dependency_name) +
            "' contains duplicate feature '" + duplicate + "'";
    return false;
  }
  for (const auto &feature : features) {
    if (!validateFeatureName(feature,
                             "dependency '" + std::string(dependency_name) +
                                 "' feature",
                             error)) {
      return false;
    }
  }
  return true;
}

bool parseDependencyInlineTable(std::string_view value, std::string_view name,
                                ProjectDependency &dependency,
                                std::string &error) {
  std::vector<std::pair<std::string, std::string>> entries;
  if (!manifest_toml::parseInlineTable(value, entries)) {
    error = "dependency '" + std::string(name) +
            "' only supports { path = \"...\" }, { workspace = true }, { git = "
            "\"...\", rev/tag/branch = \"...\" }, or { registry = \"...\", "
            "version = \"...\" }, or { artifact = \".../package.artifact\" }, "
            "optionally with features = [...], optional = true, or "
            "default_features = false";
    return false;
  }
  bool has_path = false;
  bool has_workspace = false;
  bool has_git = false;
  bool has_registry = false;
  bool has_artifact = false;
  bool has_version = false;
  bool has_features = false;
  bool has_optional = false;
  bool has_default_features = false;
  bool workspace_value = false;
  int selector_count = 0;
  for (const auto &[key, raw_value] : entries) {
    if (key == "path") {
      if (has_path) {
        error = "duplicate dependency '" + std::string(name) + "' path key";
        return false;
      }
      has_path = true;
      if (!manifest_toml::parseString(raw_value, dependency.path)) {
        error = "dependency '" + std::string(name) + "' path expects a string";
        return false;
      }
    } else if (key == "workspace") {
      if (has_workspace) {
        error =
            "duplicate dependency '" + std::string(name) + "' workspace key";
        return false;
      }
      has_workspace = true;
      if (!manifest_toml::parseBool(raw_value, workspace_value)) {
        error = "dependency '" + std::string(name) +
                "' workspace expects a boolean";
        return false;
      }
      if (!workspace_value) {
        error = "dependency '" + std::string(name) +
                "' workspace value must be true";
        return false;
      }
    } else if (key == "git") {
      if (has_git) {
        error = "duplicate dependency '" + std::string(name) + "' git key";
        return false;
      }
      has_git = true;
      if (!manifest_toml::parseString(raw_value, dependency.git_url)) {
        error = "dependency '" + std::string(name) + "' git expects a string";
        return false;
      }
      if (dependency.git_url.empty()) {
        error =
            "dependency '" + std::string(name) + "' git URL cannot be empty";
        return false;
      }
    } else if (key == "registry") {
      if (has_registry) {
        error = "duplicate dependency '" + std::string(name) + "' registry key";
        return false;
      }
      has_registry = true;
      if (!manifest_toml::parseString(raw_value, dependency.registry)) {
        error =
            "dependency '" + std::string(name) + "' registry expects a string";
        return false;
      }
      if (dependency.registry.empty()) {
        error =
            "dependency '" + std::string(name) + "' registry cannot be empty";
        return false;
      }
    } else if (key == "artifact") {
      if (has_artifact) {
        error = "duplicate dependency '" + std::string(name) + "' artifact key";
        return false;
      }
      has_artifact = true;
      if (!manifest_toml::parseString(raw_value, dependency.path)) {
        error =
            "dependency '" + std::string(name) + "' artifact expects a string";
        return false;
      }
    } else if (key == "version") {
      if (has_version) {
        error = "duplicate dependency '" + std::string(name) + "' version key";
        return false;
      }
      has_version = true;
      if (!manifest_toml::parseString(raw_value,
                                      dependency.version_requirement)) {
        error =
            "dependency '" + std::string(name) + "' version expects a string";
        return false;
      }
      if (dependency.version_requirement.empty()) {
        error =
            "dependency '" + std::string(name) + "' version cannot be empty";
        return false;
      }
    } else if (key == "rev" || key == "tag" || key == "branch") {
      std::string selector_value;
      if (!manifest_toml::parseString(raw_value, selector_value)) {
        error = "dependency '" + std::string(name) + "' " + key +
                " expects a string";
        return false;
      }
      if (selector_value.empty()) {
        error = "dependency '" + std::string(name) + "' " + key +
                " cannot be empty";
        return false;
      }
      ++selector_count;
      dependency.git_selector_value = std::move(selector_value);
      if (key == "rev") {
        dependency.git_selector_kind = ProjectGitSelectorKind::Rev;
      } else if (key == "tag") {
        dependency.git_selector_kind = ProjectGitSelectorKind::Tag;
      } else {
        dependency.git_selector_kind = ProjectGitSelectorKind::Branch;
      }
    } else if (key == "features") {
      if (has_features) {
        error = "duplicate dependency '" + std::string(name) + "' features key";
        return false;
      }
      has_features = true;
      if (!manifest_toml::parseStringArray(raw_value, dependency.features)) {
        error = "dependency '" + std::string(name) +
                "' features expects a string array";
        return false;
      }
      if (!validateDependencyFeatures(dependency.features, name, error)) {
        return false;
      }
    } else if (key == "optional") {
      if (has_optional) {
        error = "duplicate dependency '" + std::string(name) + "' optional key";
        return false;
      }
      has_optional = true;
      if (!manifest_toml::parseBool(raw_value, dependency.optional)) {
        error =
            "dependency '" + std::string(name) + "' optional expects a boolean";
        return false;
      }
    } else if (key == "default_features") {
      if (has_default_features) {
        error = "duplicate dependency '" + std::string(name) +
                "' default_features key";
        return false;
      }
      has_default_features = true;
      dependency.default_features_specified = true;
      if (!manifest_toml::parseBool(raw_value,
                                    dependency.enable_default_features)) {
        error = "dependency '" + std::string(name) +
                "' default_features expects a boolean";
        return false;
      }
    } else {
      error =
          "unknown dependency '" + std::string(name) + "' key '" + key + "'";
      return false;
    }
  }
  const int source_count = (has_path ? 1 : 0) + (has_workspace ? 1 : 0) +
                           (has_git ? 1 : 0) + (has_registry ? 1 : 0) +
                           (has_artifact ? 1 : 0);
  if (source_count > 1) {
    error = "dependency '" + std::string(name) +
            "' cannot combine path, workspace, git, registry, and artifact";
    return false;
  }
  if (!has_registry && has_version) {
    error = "dependency '" + std::string(name) + "' version requires registry";
    return false;
  }
  if (!has_git && selector_count > 0) {
    error = "dependency '" + std::string(name) +
            "' rev, tag, or branch requires git";
    return false;
  }
  if (has_path) {
    if (dependency.path.empty()) {
      error = "dependency '" + std::string(name) + "' path cannot be empty";
      return false;
    }
    dependency.kind = ProjectDependencyKind::Path;
    return true;
  }
  if (has_workspace) {
    dependency.kind = ProjectDependencyKind::Workspace;
    dependency.path.clear();
    return true;
  }
  if (has_git) {
    if (selector_count != 1) {
      error = "dependency '" + std::string(name) +
              "' git source requires exactly one of rev, tag, or branch";
      return false;
    }
    dependency.kind = ProjectDependencyKind::Git;
    dependency.path.clear();
    return true;
  }
  if (has_registry) {
    if (!has_version) {
      error = "dependency '" + std::string(name) +
              "' registry source requires registry and version";
      return false;
    }
    dependency.kind = ProjectDependencyKind::Registry;
    dependency.path.clear();
    return true;
  }
  if (has_artifact) {
    if (dependency.path.empty()) {
      error = "dependency '" + std::string(name) + "' artifact cannot be empty";
      return false;
    }
    dependency.kind = ProjectDependencyKind::Artifact;
    return true;
  }
  error = "dependency '" + std::string(name) +
          "' only supports { path = \"...\" }, { workspace = true }, { git = "
          "\"...\", rev/tag/branch = \"...\" }, or { registry = \"...\", "
          "version = \"...\" }, or { artifact = \".../package.artifact\" }, "
          "optionally with features = [...], optional = true, or "
          "default_features = false";
  return false;
}

bool parseRegistryInlineTable(std::string_view value, std::string_view name,
                              ProjectRegistryConfig &registry,
                              std::string &error) {
  std::vector<std::pair<std::string, std::string>> entries;
  if (!manifest_toml::parseInlineTable(value, entries)) {
    error = "registry '" + std::string(name) +
            "' only supports { index = \"...\" }";
    return false;
  }
  bool has_index = false;
  bool has_trusted_keys = false;
  bool has_root_keys = false;
  bool has_root_threshold = false;
  bool has_origin = false;
  bool has_publish = false;
  bool has_ca_bundle = false;
  bool has_witnesses = false;
  bool has_witness_keys = false;
  bool has_witness_threshold = false;
  bool has_witness_ca_bundle = false;
  for (const auto &[key, raw_value] : entries) {
    if (key == "origin") {
      if (has_origin ||
          !manifest_toml::parseString(raw_value, registry.origin)) {
        error = "registry '" + std::string(name) +
                "' origin expects one HTTPS origin";
        return false;
      }
      has_origin = true;
      const auto authority = std::string_view(registry.origin).substr(8);
      if (!registry.origin.starts_with("https://") || authority.empty() ||
          authority.find_first_of("/@?#\t\r\n ") != std::string_view::npos) {
        error = "registry '" + std::string(name) +
                "' origin requires an HTTPS origin with no path or credentials";
        return false;
      }
      continue;
    }
    if (key == "url" || key == "remote") {
      error = unsupportedManifestSurfaceDiagnostic(
          UnsupportedManifestSurfaceUseKind::HttpRegistry, std::string(name),
          "registry '" + std::string(name) + "' " + key);
      return false;
    }
    if (key == "token" || key == "auth" || key == "credential" ||
        key == "source" || key == "verify") {
      error = unsupportedManifestSurfaceDiagnostic(
          UnsupportedManifestSurfaceUseKind::RegistryAuth, std::string(name),
          "registry '" + std::string(name) + "' " + key);
      return false;
    }
    if (key == "trusted_keys") {
      if (has_trusted_keys ||
          !manifest_toml::parseStringArray(raw_value, registry.trusted_keys)) {
        error = "registry '" + std::string(name) +
                "' trusted_keys expects one string array";
        return false;
      }
      has_trusted_keys = true;
      for (const auto &trusted_key : registry.trusted_keys) {
        if (!isValidRegistryPublicKey(trusted_key) ||
            std::count(registry.trusted_keys.begin(),
                       registry.trusted_keys.end(), trusted_key) != 1) {
          error = "registry '" + std::string(name) +
                  "' trusted_keys contains an invalid or duplicate key";
          return false;
        }
      }
      continue;
    }
    if (key == "root_keys") {
      if (has_root_keys ||
          !manifest_toml::parseStringArray(raw_value, registry.root_keys)) {
        error = "registry '" + std::string(name) +
                "' root_keys expects one string array";
        return false;
      }
      has_root_keys = true;
      for (const auto &root_key : registry.root_keys) {
        if (!isValidRegistryPublicKey(root_key) ||
            std::count(registry.root_keys.begin(), registry.root_keys.end(),
                       root_key) != 1) {
          error = "registry '" + std::string(name) +
                  "' root_keys contains an invalid or duplicate key";
          return false;
        }
      }
      continue;
    }
    if (key == "root_threshold") {
      std::uint64_t threshold = 0;
      if (has_root_threshold ||
          !manifest_toml::parseUnsigned(raw_value, threshold) ||
          threshold == 0 || threshold > UINT32_MAX) {
        error = "registry '" + std::string(name) +
                "' root_threshold expects a positive 32-bit integer";
        return false;
      }
      has_root_threshold = true;
      registry.root_threshold = static_cast<std::uint32_t>(threshold);
      continue;
    }
    if (key == "publish") {
      if (has_publish ||
          !manifest_toml::parseString(raw_value, registry.publish_url)) {
        error = "registry '" + std::string(name) +
                "' publish expects one HTTPS URL";
        return false;
      }
      has_publish = true;
      const auto authority_end = registry.publish_url.find('/', 8);
      const auto authority = std::string_view(registry.publish_url)
                                 .substr(8, authority_end == std::string::npos
                                                ? std::string_view::npos
                                                : authority_end - 8);
      if (!registry.publish_url.starts_with("https://") || authority.empty() ||
          authority.find('@') != std::string_view::npos ||
          registry.publish_url.find_first_of("\t\r\n ") != std::string::npos) {
        error =
            "registry '" + std::string(name) +
            "' publish requires an HTTPS URL with a host and no credentials";
        return false;
      }
      continue;
    }
    if (key == "ca_bundle") {
      if (has_ca_bundle ||
          !manifest_toml::parseString(raw_value, registry.ca_bundle) ||
          registry.ca_bundle.empty()) {
        error = "registry '" + std::string(name) +
                "' ca_bundle expects one non-empty path";
        return false;
      }
      has_ca_bundle = true;
      continue;
    }
    if (key == "witnesses") {
      if (has_witnesses ||
          !manifest_toml::parseStringArray(raw_value, registry.witness_urls) ||
          registry.witness_urls.empty() || registry.witness_urls.size() > 16) {
        error = "registry '" + std::string(name) +
                "' witnesses expects 1 to 16 HTTPS URLs";
        return false;
      }
      has_witnesses = true;
      for (const auto &url : registry.witness_urls) {
        const auto authority_end = url.find('/', 8);
        const auto authority = std::string_view(url).substr(
            8, authority_end == std::string::npos ? std::string_view::npos
                                                  : authority_end - 8);
        if (!url.starts_with("https://") || authority.empty() ||
            authority.find('@') != std::string_view::npos ||
            url.find_first_of("\t\r\n ") != std::string::npos ||
            std::count(registry.witness_urls.begin(),
                       registry.witness_urls.end(), url) != 1) {
          error = "registry '" + std::string(name) +
                  "' witnesses contains an invalid or duplicate HTTPS URL";
          return false;
        }
      }
      continue;
    }
    if (key == "witness_keys") {
      if (has_witness_keys ||
          !manifest_toml::parseStringArray(raw_value, registry.witness_keys) ||
          registry.witness_keys.empty() || registry.witness_keys.size() > 16) {
        error = "registry '" + std::string(name) +
                "' witness_keys expects 1 to 16 public keys";
        return false;
      }
      has_witness_keys = true;
      for (const auto &witness_key : registry.witness_keys) {
        if (!isValidRegistryPublicKey(witness_key) ||
            std::count(registry.witness_keys.begin(),
                       registry.witness_keys.end(), witness_key) != 1) {
          error = "registry '" + std::string(name) +
                  "' witness_keys contains an invalid or duplicate key";
          return false;
        }
      }
      continue;
    }
    if (key == "witness_threshold") {
      std::uint64_t threshold = 0;
      if (has_witness_threshold ||
          !manifest_toml::parseUnsigned(raw_value, threshold) ||
          threshold == 0 || threshold > UINT32_MAX) {
        error = "registry '" + std::string(name) +
                "' witness_threshold expects a positive 32-bit integer";
        return false;
      }
      has_witness_threshold = true;
      registry.witness_threshold = static_cast<std::uint32_t>(threshold);
      continue;
    }
    if (key == "witness_ca_bundle") {
      if (has_witness_ca_bundle ||
          !manifest_toml::parseString(raw_value, registry.witness_ca_bundle) ||
          registry.witness_ca_bundle.empty()) {
        error = "registry '" + std::string(name) +
                "' witness_ca_bundle expects one non-empty path";
        return false;
      }
      has_witness_ca_bundle = true;
      continue;
    }
    if (key != "index") {
      error = "unknown registry '" + std::string(name) + "' key '" + key + "'";
      return false;
    }
    if (has_index) {
      error = "duplicate registry '" + std::string(name) + "' index key";
      return false;
    }
    has_index = true;
    if (!manifest_toml::parseString(raw_value, registry.index)) {
      error = "registry '" + std::string(name) + "' index expects a string";
      return false;
    }
    if (registry.index.empty()) {
      error = "registry '" + std::string(name) + "' index cannot be empty";
      return false;
    }
  }
  if (!has_index) {
    error = "registry '" + std::string(name) + "' requires index";
    return false;
  }
  if (!classifyProjectRegistryIndex(registry.index, registry.index_kind,
                                    error)) {
    error = "registry '" + std::string(name) + "' " + error;
    return false;
  }
  if (has_trusted_keys && (has_root_keys || has_root_threshold)) {
    error = "registry '" + std::string(name) +
            "' cannot combine trusted_keys with root trust metadata";
    return false;
  }
  if (has_root_keys != has_root_threshold ||
      (has_root_keys && registry.root_threshold > registry.root_keys.size())) {
    error = "registry '" + std::string(name) +
            "' requires root_keys and a satisfiable root_threshold together";
    return false;
  }
  const bool has_witness_policy =
      has_witnesses || has_witness_keys || has_witness_threshold;
  if (has_witness_policy &&
      (!has_witnesses || !has_witness_keys || !has_witness_threshold ||
       registry.witness_threshold > registry.witness_urls.size() ||
       registry.witness_threshold > registry.witness_keys.size() ||
       registry.witness_threshold <= registry.witness_keys.size() / 2)) {
    error = "registry '" + std::string(name) +
            "' requires witnesses, witness_keys, and an intersecting "
            "witness_threshold together";
    return false;
  }
  if (has_witness_policy && (!has_origin || !has_root_keys)) {
    error = "registry '" + std::string(name) +
            "' witness policy requires origin and root trust metadata";
    return false;
  }
  if (has_witness_ca_bundle && !has_witness_policy) {
    error = "registry '" + std::string(name) +
            "' witness_ca_bundle requires a witness policy";
    return false;
  }
  if (registry.index_kind == ProjectRegistryIndexKind::RemoteGit &&
      (!has_root_keys || has_trusted_keys)) {
    error =
        "remote registry '" + std::string(name) +
        "' requires root_keys/root_threshold and rejects legacy trusted_keys";
    return false;
  }
  registry.name = std::string(name);
  return true;
}

} // namespace

std::optional<ProjectManifest>
parseProjectManifestText(std::string_view text, std::string manifest_path,
                         std::string &error) {
  ProjectManifest manifest;
  manifest.manifest_path = std::move(manifest_path);
  manifest.root_directory = manifestRoot(manifest.manifest_path);

  auto assignments = manifest_toml::parseAssignments(
      text,
      {"package", "build", "native", "target", "c", "dependencies", "features",
       "registries"},
      "manifest", error, projectUnsupportedTableReason);
  if (!assignments) {
    return std::nullopt;
  }

  for (const auto &assignment : *assignments) {
    const auto full_key = assignment.fullKey();
    if (assignment.table == "dependencies") {
      if (assignment.key.empty()) {
        error = "dependency name cannot be empty at line " +
                std::to_string(assignment.line);
        return std::nullopt;
      }
      if (dependencyExists(manifest.dependencies, assignment.key)) {
        error = "duplicate dependency '" + assignment.key + "' at line " +
                std::to_string(assignment.line);
        return std::nullopt;
      }
      ProjectDependency dependency;
      dependency.name = assignment.key;
      if (!parseDependencyInlineTable(assignment.value, assignment.key,
                                      dependency, error)) {
        return std::nullopt;
      }
      manifest.dependencies.push_back(std::move(dependency));
    } else if (assignment.table == "features") {
      if (!validateFeatureName(assignment.key, "feature name", error)) {
        error += " at line " + std::to_string(assignment.line);
        return std::nullopt;
      }
      if (featureExists(manifest.features, assignment.key)) {
        error = "duplicate feature '" + assignment.key + "' at line " +
                std::to_string(assignment.line);
        return std::nullopt;
      }
      ProjectFeature feature;
      feature.name = assignment.key;
      if (!manifest_toml::parseStringArray(assignment.value,
                                           feature.requirements)) {
        error = "feature '" + assignment.key + "' expects a string array";
        return std::nullopt;
      }
      if (!validateFeatureRequirements(feature.requirements, assignment.key,
                                       error)) {
        return std::nullopt;
      }
      manifest.features.push_back(std::move(feature));
    } else if (assignment.table == "registries") {
      if (assignment.key.empty()) {
        error = "registry name cannot be empty at line " +
                std::to_string(assignment.line);
        return std::nullopt;
      }
      if (registryExists(manifest.registries, assignment.key)) {
        error = "duplicate registry '" + assignment.key + "' at line " +
                std::to_string(assignment.line);
        return std::nullopt;
      }
      ProjectRegistryConfig registry;
      if (!parseRegistryInlineTable(assignment.value, assignment.key, registry,
                                    error)) {
        return std::nullopt;
      }
      manifest.registries.push_back(std::move(registry));
    } else if (full_key == "package.name") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.package_name)) {
        error = "manifest key 'package.name' expects a string";
        return std::nullopt;
      }
    } else if (full_key == "package.language") {
      if (manifest.language_version) {
        error = "duplicate manifest key 'package.language'";
        return std::nullopt;
      }
      std::string version;
      if (!manifest_toml::parseString(assignment.value, version)) {
        error = "manifest key 'package.language' expects a string";
        return std::nullopt;
      }
      manifest.language_version = LanguageVersion::parse(version);
      if (!manifest.language_version) {
        error = "manifest key 'package.language' expects MAJOR.MINOR";
        return std::nullopt;
      }
      if (!isSupportedLanguageVersion(*manifest.language_version)) {
        error = "unsupported Chtholly language version '" + version + "'";
        return std::nullopt;
      }
    } else if (full_key == "build.entry") {
      if (!manifest_toml::parseString(assignment.value, manifest.entry)) {
        error = "manifest key 'build.entry' expects a string";
        return std::nullopt;
      }
    } else if (full_key == "build.module_paths") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.module_paths)) {
        error = "manifest key 'build.module_paths' expects a string array";
        return std::nullopt;
      }
      if (containsEmpty(manifest.module_paths)) {
        error = "module_paths cannot contain empty paths";
        return std::nullopt;
      }
    } else if (full_key == "native.library_paths") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.native_library_paths)) {
        error = "manifest key 'native.library_paths' expects a string array";
        return std::nullopt;
      }
      if (containsEmpty(manifest.native_library_paths)) {
        error = "library_paths cannot contain empty paths";
        return std::nullopt;
      }
    } else if (full_key == "native.link_libraries") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.native_link_libraries)) {
        error = "manifest key 'native.link_libraries' expects a string array";
        return std::nullopt;
      }
    } else if (full_key == "c.include_paths") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.c_include_paths)) {
        error = "manifest key 'c.include_paths' expects a string array";
        return std::nullopt;
      }
      if (containsEmpty(manifest.c_include_paths)) {
        error = "c.include_paths cannot contain empty paths";
        return std::nullopt;
      }
    } else if (full_key == "c.defines") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.c_defines)) {
        error = "manifest key 'c.defines' expects a string array";
        return std::nullopt;
      }
      if (containsEmpty(manifest.c_defines)) {
        error = "c.defines cannot contain empty values";
        return std::nullopt;
      }
    } else if (full_key == "target.triple") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.target_triple)) {
        error = "manifest key 'target.triple' expects a string";
        return std::nullopt;
      }
      if (manifest.target_triple.empty()) {
        error = "target.triple cannot be empty";
        return std::nullopt;
      }
    } else if (full_key == "target.sysroot") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.target_sysroot)) {
        error = "manifest key 'target.sysroot' expects a string";
        return std::nullopt;
      }
      if (manifest.target_sysroot.empty()) {
        error = "target.sysroot cannot be empty";
        return std::nullopt;
      }
    } else if (full_key == "target.linker") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.target_linker)) {
        error = "manifest key 'target.linker' expects a string";
        return std::nullopt;
      }
      if (manifest.target_linker.empty()) {
        error = "target.linker cannot be empty";
        return std::nullopt;
      }
    } else {
      error = "unknown manifest key '" + full_key + "' at line " +
              std::to_string(assignment.line);
      return std::nullopt;
    }
  }
  if (!manifest.package_name.empty() && !manifest.language_version) {
    error = "manifest package requires package.language = \"" +
            DefaultLanguageVersion.str() + "\"";
    return std::nullopt;
  }
  return manifest;
}

std::optional<ProjectManifest>
loadProjectManifestFile(const std::string &manifest_path, std::string &error) {
  auto text = readTextFile(manifest_path, error);
  if (!text) {
    return std::nullopt;
  }
  return parseProjectManifestText(*text, manifest_path, error);
}

} // namespace chtholly
