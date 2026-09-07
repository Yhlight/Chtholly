#include "chtholly/Driver/CompilerBuildPlan.h"

#include "ManifestToml.h"
#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/ManifestDiscovery.h"
#include "chtholly/Driver/ResourceLocator.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <sstream>
#include <unordered_map>

namespace chtholly {
namespace {

struct CompilerDependencyInput {
  std::string name;
  std::string path;
  std::string artifact;
  std::string artifact_sha256;
  std::vector<std::string> features;
  bool workspace = false;
  bool optional = false;
  bool default_features = true;
};

struct CompilerFeatureInput {
  std::string name;
  std::vector<std::string> requirements;
};

struct CompilerManifestInput {
  std::string manifest_path;
  std::string root_directory;
  std::string package_name;
  std::optional<LanguageVersion> language_version;
  std::string entry;
  std::string interop_bundle;
  std::vector<std::string> module_paths;
  std::vector<std::string> native_library_paths;
  std::vector<std::string> native_link_libraries;
  std::string cffi_receipt;
  bool cffi_required = false;
  std::string target_triple;
  std::string target_sysroot;
  std::string target_linker;
  std::uint32_t component_abi = 0;
  std::string component_identity;
  std::vector<std::string> component_exports;
  std::vector<CompilerDependencyInput> dependencies;
  std::vector<CompilerFeatureInput> features;
};

struct CompilerWorkspaceInput {
  std::string manifest_path;
  std::string root_directory;
  std::vector<std::string> members;
  std::vector<std::string> default_members;
};

std::string normalized(const std::filesystem::path &path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  if (error)
    result = std::filesystem::absolute(path, error);
  return (error ? path : result).lexically_normal().string();
}

std::string resolvePath(const std::string &root, const std::string &path) {
  const std::filesystem::path candidate(path);
  return normalized(candidate.is_absolute()
                        ? candidate
                        : std::filesystem::path(root) / candidate);
}

bool uniqueNonempty(std::span<const std::string> values,
                    std::string_view subject, std::string &error) {
  std::set<std::string> seen;
  for (const auto &value : values) {
    if (value.empty()) {
      error = std::string(subject) + " cannot contain empty values";
      return false;
    }
    if (!seen.insert(value).second) {
      error = std::string(subject) + " contains duplicate '" + value + "'";
      return false;
    }
  }
  return true;
}

bool validPersistentIdentity(std::string_view value) {
  return !value.empty() &&
         value.find_first_of("\\\"\t\r\n") == std::string_view::npos;
}

bool validateCFFIReceipt(const std::string &path, std::string_view target,
                         compiler::CFFIReceiptIdentity &identity,
                         std::string &digest, std::string &error) {
  const auto text = readTextFile(path, error);
  if (!text)
    return false;
  auto parsed = compiler::parseCFFIReceipt(*text, error);
  if (!parsed) {
    error = "CFFI receipt is invalid: '" + path + "': " + error;
    return false;
  }
  if (!target.empty() && parsed->target != target) {
    error = "CFFI receipt targets '" + parsed->target +
            "' instead of the selected target '" + std::string(target) +
            "': '" + path + "'";
    return false;
  }
  identity = std::move(*parsed);
  digest = identity.fingerprint().hex();
  return true;
}

std::optional<CompilerDependencyInput>
parseDependency(const manifest_toml::Assignment &assignment,
                std::string &error) {
  std::vector<std::pair<std::string, std::string>> fields;
  if (!manifest_toml::parseInlineTable(assignment.value, fields)) {
    error = "compiler dependency '" + assignment.key +
            "' requires a path or workspace inline table";
    return std::nullopt;
  }
  CompilerDependencyInput dependency;
  dependency.name = assignment.key;
  bool source = false;
  std::set<std::string> keys;
  for (const auto &[key, value] : fields) {
    if (!keys.insert(key).second) {
      error = "duplicate dependency key '" + key + "'";
      return std::nullopt;
    }
    if (key == "path") {
      if (source || !manifest_toml::parseString(value, dependency.path) ||
          dependency.path.empty()) {
        error = "compiler dependency '" + dependency.name +
                "' requires one non-empty source";
        return std::nullopt;
      }
      source = true;
    } else if (key == "artifact") {
      if (!manifest_toml::parseString(value, dependency.artifact) ||
          dependency.artifact.empty()) {
        error = "compiler dependency '" + dependency.name +
                "' artifact expects a non-empty path";
        return std::nullopt;
      }
    } else if (key == "sha256") {
      if (!manifest_toml::parseString(value, dependency.artifact_sha256) ||
          dependency.artifact_sha256.size() != 64 ||
          !std::ranges::all_of(dependency.artifact_sha256, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          })) {
        error = "compiler dependency '" + dependency.name +
                "' sha256 expects 64 lowercase hexadecimal characters";
        return std::nullopt;
      }
    } else if (key == "workspace") {
      if (source || !manifest_toml::parseBool(value, dependency.workspace) ||
          !dependency.workspace) {
        error = "compiler dependency '" + dependency.name +
                "' requires workspace = true";
        return std::nullopt;
      }
      source = true;
    } else if (key == "features") {
      if (!manifest_toml::parseStringArray(value, dependency.features) ||
          !uniqueNonempty(dependency.features, "dependency features", error))
        return std::nullopt;
    } else if (key == "optional") {
      if (!manifest_toml::parseBool(value, dependency.optional)) {
        error = "dependency optional expects a Boolean";
        return std::nullopt;
      }
    } else if (key == "default_features") {
      if (!manifest_toml::parseBool(value, dependency.default_features)) {
        error = "dependency default_features expects a Boolean";
        return std::nullopt;
      }
    } else {
      error = "compiler dependency '" + dependency.name + "' rejects key '" + key +
              "'; only path/workspace dependencies are admitted";
      return std::nullopt;
    }
  }
  if (!source) {
    error = "compiler dependency '" + dependency.name +
            "' requires path or workspace = true";
    return std::nullopt;
  }
  if (!dependency.artifact.empty() && dependency.artifact_sha256.empty()) {
    error = "compiler dependency '" + dependency.name +
            "' requires sha256 when artifact is declared";
    return std::nullopt;
  }
  return dependency;
}

std::optional<CompilerManifestInput> loadManifest(const std::string &path,
                                              std::string &error) {
  auto text = readTextFile(path, error);
  if (!text)
    return std::nullopt;
  CompilerManifestInput manifest;
  manifest.manifest_path = normalized(path);
  manifest.root_directory =
      std::filesystem::path(manifest.manifest_path).parent_path().string();
  auto assignments = manifest_toml::parseAssignments(
      *text,
      {"package", "build", "component", "native", "target", "cffi",
       "dependencies", "features"},
      "compiler project manifest", error);
  if (!assignments)
    return std::nullopt;
  std::set<std::string> scalar_keys;
  std::set<std::string> dependency_names;
  std::set<std::string> feature_names;
  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if (assignment.table == "dependencies") {
      if (!validPersistentIdentity(assignment.key) ||
          !dependency_names.insert(assignment.key).second) {
        error = "invalid or duplicate compiler dependency name";
        return std::nullopt;
      }
      auto dependency = parseDependency(assignment, error);
      if (!dependency)
        return std::nullopt;
      manifest.dependencies.push_back(std::move(*dependency));
      continue;
    }
    if (assignment.table == "features") {
      if (!validPersistentIdentity(assignment.key) ||
          !feature_names.insert(assignment.key).second) {
        error = "invalid or duplicate compiler feature name";
        return std::nullopt;
      }
      CompilerFeatureInput feature{.name = assignment.key};
      if (!manifest_toml::parseStringArray(assignment.value,
                                           feature.requirements) ||
          !uniqueNonempty(feature.requirements, "feature requirements", error))
        return std::nullopt;
      manifest.features.push_back(std::move(feature));
      continue;
    }
    if (!scalar_keys.insert(key).second) {
      error = "duplicate compiler manifest key '" + key + "'";
      return std::nullopt;
    }
    if (key == "package.name") {
      if (!manifest_toml::parseString(assignment.value, manifest.package_name))
        error = "package.name expects a string";
    } else if (key == "package.language") {
      std::string version;
      if (manifest.language_version) {
        error = "duplicate package.language";
      } else if (!manifest_toml::parseString(assignment.value, version)) {
        error = "package.language expects a string";
      } else {
        manifest.language_version = LanguageVersion::parse(version);
        if (!manifest.language_version)
          error = "package.language expects MAJOR.MINOR";
        else if (!isSupportedLanguageVersion(*manifest.language_version))
          error = "unsupported Chtholly language version '" + version + "'";
      }
    } else if (key == "build.entry") {
      if (!manifest_toml::parseString(assignment.value, manifest.entry))
        error = "build.entry expects a string";
    } else if (key == "build.interop_bundle") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.interop_bundle) ||
          manifest.interop_bundle.empty())
        error = "build.interop_bundle expects a non-empty string";
    } else if (key == "build.module_paths") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.module_paths) ||
          !uniqueNonempty(manifest.module_paths, "module_paths", error))
        return std::nullopt;
    } else if (key == "component.abi") {
      std::uint64_t version = 0;
      if (!manifest_toml::parseUnsigned(assignment.value, version) ||
          version != 1)
        error = "component.abi currently requires 1";
      else
        manifest.component_abi = 1;
    } else if (key == "component.identity") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.component_identity) ||
          !validPersistentIdentity(manifest.component_identity))
        error = "component.identity expects a stable non-empty string";
    } else if (key == "component.exports") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.component_exports) ||
          !uniqueNonempty(manifest.component_exports, "component exports",
                          error))
        return std::nullopt;
    } else if (key == "native.library_paths") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.native_library_paths) ||
          !uniqueNonempty(manifest.native_library_paths, "library_paths",
                          error))
        return std::nullopt;
    } else if (key == "native.link_libraries") {
      if (!manifest_toml::parseStringArray(assignment.value,
                                           manifest.native_link_libraries) ||
          !uniqueNonempty(manifest.native_link_libraries, "link_libraries",
                          error))
        return std::nullopt;
    } else if (key == "cffi.receipt") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.cffi_receipt) ||
          manifest.cffi_receipt.empty())
        error = "cffi.receipt expects a non-empty string";
    } else if (key == "cffi.required") {
      if (!manifest_toml::parseBool(assignment.value, manifest.cffi_required))
        error = "cffi.required expects a boolean";
    } else if (key == "target.triple") {
      if (!manifest_toml::parseString(assignment.value, manifest.target_triple))
        error = "target.triple expects a string";
    } else if (key == "target.sysroot") {
      if (!manifest_toml::parseString(assignment.value,
                                      manifest.target_sysroot))
        error = "target.sysroot expects a string";
    } else if (key == "target.linker") {
      if (!manifest_toml::parseString(assignment.value, manifest.target_linker))
        error = "target.linker expects a string";
    } else {
      error = "unknown compiler manifest key '" + key + "'";
    }
    if (!error.empty())
      return std::nullopt;
  }
  if (!validPersistentIdentity(manifest.package_name)) {
    error = "compiler project manifest requires a valid package.name";
    return std::nullopt;
  }
  if (!manifest.language_version) {
    error = "compiler project manifest requires package.language = \"" +
            DefaultLanguageVersion.str() + "\"";
    return std::nullopt;
  }
  if (manifest.module_paths.empty())
    manifest.module_paths = {"src"};
  const auto has_component_fields = manifest.component_abi != 0 ||
                                    !manifest.component_identity.empty() ||
                                    !manifest.component_exports.empty();
  if (has_component_fields &&
      (manifest.component_abi != 1 || manifest.component_identity.empty() ||
       manifest.component_exports.empty())) {
    error = "component requires abi = 1, identity, and non-empty exports";
    return std::nullopt;
  }
  if (has_component_fields && !manifest.entry.empty()) {
    error = "component packages cannot declare build.entry";
    return std::nullopt;
  }
  if (manifest.cffi_required && manifest.cffi_receipt.empty()) {
    error = "cffi.required needs cffi.receipt";
    return std::nullopt;
  }
  return manifest;
}

std::optional<CompilerWorkspaceInput> loadWorkspace(const std::string &path,
                                                std::string &error) {
  auto text = readTextFile(path, error);
  if (!text)
    return std::nullopt;
  CompilerWorkspaceInput workspace;
  workspace.manifest_path = normalized(path);
  workspace.root_directory =
      std::filesystem::path(workspace.manifest_path).parent_path().string();
  auto assignments = manifest_toml::parseAssignments(
      *text, {"workspace"}, "compiler workspace manifest", error);
  if (!assignments)
    return std::nullopt;
  std::set<std::string> keys;
  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if (!keys.insert(key).second) {
      error = "duplicate compiler workspace key '" + key + "'";
      return std::nullopt;
    }
    auto *output = key == "workspace.members" ? &workspace.members
                   : key == "workspace.default_members"
                       ? &workspace.default_members
                       : nullptr;
    if (!output) {
      error = "unknown compiler workspace key '" + key + "'";
      return std::nullopt;
    }
    if (!manifest_toml::parseStringArray(assignment.value, *output) ||
        !uniqueNonempty(*output, key, error))
      return std::nullopt;
  }
  if (workspace.members.empty()) {
    error = "compiler workspace requires workspace.members";
    return std::nullopt;
  }
  for (const auto &member : workspace.default_members)
    if (std::ranges::find(workspace.members, member) ==
        workspace.members.end()) {
      error = "workspace default member '" + member + "' is not a member";
      return std::nullopt;
    }
  return workspace;
}

const CompilerFeatureInput *findFeature(const CompilerManifestInput &manifest,
                                    std::string_view name) {
  const auto it =
      std::ranges::find(manifest.features, name, &CompilerFeatureInput::name);
  return it == manifest.features.end() ? nullptr : &*it;
}

const CompilerDependencyInput *findDependency(const CompilerManifestInput &manifest,
                                          std::string_view name) {
  const auto it = std::ranges::find(manifest.dependencies, name,
                                    &CompilerDependencyInput::name);
  return it == manifest.dependencies.end() ? nullptr : &*it;
}

struct FeatureResolution {
  std::vector<std::string> features;
  std::map<std::string, std::vector<std::string>> dependency_features;
  std::set<std::string> active_optional_dependencies;
};

std::optional<FeatureResolution>
resolveFeatures(const CompilerManifestInput &manifest,
                std::vector<std::string> requested, bool defaults,
                std::string &error) {
  if (defaults && findFeature(manifest, "default"))
    requested.push_back("default");
  FeatureResolution result;
  std::set<std::string> queued(requested.begin(), requested.end());
  std::vector<std::string> work(queued.begin(), queued.end());
  while (!work.empty()) {
    const auto feature_name = std::move(work.back());
    work.pop_back();
    const auto *feature = findFeature(manifest, feature_name);
    if (!feature) {
      error = "unknown feature '" + feature_name + "' in package '" +
              manifest.package_name + "'";
      return std::nullopt;
    }
    result.features.push_back(feature_name);
    for (const auto &requirement : feature->requirements) {
      const auto slash = requirement.find('/');
      if (slash == std::string::npos) {
        if (findFeature(manifest, requirement)) {
          if (queued.insert(requirement).second)
            work.push_back(requirement);
        } else if (findDependency(manifest, requirement)) {
          result.active_optional_dependencies.insert(requirement);
        } else {
          error = "feature '" + feature_name + "' references unknown '" +
                  requirement + "'";
          return std::nullopt;
        }
      } else {
        const auto dependency = requirement.substr(0, slash);
        const auto dependency_feature = requirement.substr(slash + 1);
        if (!findDependency(manifest, dependency) ||
            dependency_feature.empty()) {
          error = "feature '" + feature_name +
                  "' has invalid dependency feature '" + requirement + "'";
          return std::nullopt;
        }
        result.active_optional_dependencies.insert(dependency);
        result.dependency_features[dependency].push_back(dependency_feature);
      }
    }
  }
  std::ranges::sort(result.features);
  result.features.erase(
      std::unique(result.features.begin(), result.features.end()),
      result.features.end());
  return result;
}

struct PlanBuilder {
  const CompilerInvocation &invocation;
  std::unordered_map<std::string, CompilerManifestInput> workspace_members;
  std::vector<CompilerBuildPlanPackage> packages;
  std::unordered_map<std::string, std::size_t> package_indices;
  std::string &error;

  struct PackageState {
    CompilerManifestInput manifest;
    std::set<std::string> requested_features;
    bool default_features = false;
    bool queued = false;
  };

  std::vector<PackageState> states;
  std::queue<std::size_t> pending;

  void enqueue(std::size_t index) {
    if (states[index].queued)
      return;
    states[index].queued = true;
    pending.push(index);
  }

  std::vector<std::string> qualifiedFeatures(std::string_view package) const {
    std::vector<std::string> result;
    for (const auto &selection : invocation.package_feature_selections)
      if (selection.package_name == package)
        result.push_back(selection.feature_name);
    return result;
  }

  bool defaultsEnabled(std::string_view package, bool requested) const {
    return requested &&
           std::ranges::find(invocation.default_feature_disabled_packages,
                             package) ==
               invocation.default_feature_disabled_packages.end();
  }

  std::optional<std::size_t> requestPackage(CompilerManifestInput manifest,
                                            std::vector<std::string> requested,
                                            bool defaults) {
    auto qualified = qualifiedFeatures(manifest.package_name);
    requested.insert(requested.end(), qualified.begin(), qualified.end());
    if (const auto existing = package_indices.find(manifest.package_name);
        existing != package_indices.end()) {
      const auto index = existing->second;
      if (states[index].manifest.manifest_path != manifest.manifest_path) {
        error = "duplicate compiler package name '" + manifest.package_name +
                "' resolves to different manifests";
        return std::nullopt;
      }
      bool changed = false;
      for (auto &feature : requested)
        changed = states[index]
                      .requested_features.insert(std::move(feature))
                      .second ||
                  changed;
      if (defaultsEnabled(manifest.package_name, defaults) &&
          !states[index].default_features) {
        states[index].default_features = true;
        changed = true;
      }
      if (changed)
        enqueue(index);
      return index;
    }

    const auto index = packages.size();
    package_indices.emplace(manifest.package_name, index);
    packages.push_back({.name = manifest.package_name,
                        .language_version = *manifest.language_version,
                        .manifest_path = manifest.manifest_path,
                        .root_directory = manifest.root_directory,
                        .entry_path = manifest.entry.empty()
                                          ? std::string{}
                                          : resolvePath(manifest.root_directory,
                                                        manifest.entry),
                        .interop_bundle_path = {},
                        .interop_bundle_digest = {},
                        .artifact_archive_path = {},
                        .artifact_archive_digest = {},
                        .cffi_receipt_path = {},
                        .cffi_receipt_digest = {},
                        .cffi_identity = std::nullopt,
                        .cffi_required = manifest.cffi_required,
                        .component_abi = manifest.component_abi,
                        .component_identity = manifest.component_identity,
                        .component_exports = manifest.component_exports});
    if (!manifest.cffi_receipt.empty()) {
      const auto receipt =
          resolvePath(manifest.root_directory, manifest.cffi_receipt);
      compiler::CFFIReceiptIdentity identity;
      if (!validateCFFIReceipt(receipt, manifest.target_triple, identity,
                               packages[index].cffi_receipt_digest, error))
        return std::nullopt;
      packages[index].cffi_receipt_path = receipt;
      packages[index].cffi_identity = std::move(identity);
    }
    if (!manifest.interop_bundle.empty()) {
      const auto path =
          resolvePath(manifest.root_directory, manifest.interop_bundle);
      const auto root = resolvePath(manifest.root_directory, ".");
      const auto normalized_path =
          std::filesystem::path(path).lexically_normal();
      const auto normalized_root =
          std::filesystem::path(root).lexically_normal();
      auto path_it = normalized_path.begin();
      bool within = true;
      for (auto root_it = normalized_root.begin();
           root_it != normalized_root.end(); ++root_it, ++path_it) {
        if (path_it == normalized_path.end() || *path_it != *root_it) {
          within = false;
          break;
        }
      }
      if (!within) {
        error = "compiler build.interop_bundle escapes package root";
        return std::nullopt;
      }
      const auto digest = sha256File(path);
      if (!digest) {
        error = "compiler build.interop_bundle is missing or unreadable: '" + path +
                "'";
        return std::nullopt;
      }
      packages[index].interop_bundle_path = path;
      packages[index].interop_bundle_digest = *digest;
    }
    for (const auto &path : manifest.module_paths)
      packages[index].module_roots.push_back(
          resolvePath(manifest.root_directory, path));
    for (const auto &path : manifest.native_library_paths)
      packages[index].native_library_paths.push_back(
          resolvePath(manifest.root_directory, path));
    packages[index].native_link_libraries = manifest.native_link_libraries;
    PackageState state{.manifest = std::move(manifest),
                       .default_features =
                           defaultsEnabled(packages[index].name, defaults)};
    state.requested_features.insert(std::make_move_iterator(requested.begin()),
                                    std::make_move_iterator(requested.end()));
    states.push_back(std::move(state));
    enqueue(index);
    return index;
  }

  bool resolve() {
    while (!pending.empty()) {
      const auto index = pending.front();
      pending.pop();
      states[index].queued = false;
      const auto manifest = states[index].manifest;
      auto features = resolveFeatures(
          manifest,
          std::vector<std::string>(states[index].requested_features.begin(),
                                   states[index].requested_features.end()),
          states[index].default_features, error);
      if (!features)
        return false;
      packages[index].resolved_features = features->features;

      for (const auto &dependency : manifest.dependencies) {
        const bool active =
            !dependency.optional ||
            features->active_optional_dependencies.contains(dependency.name);
        if (!active)
          continue;
        CompilerManifestInput dependency_manifest;
        if (dependency.workspace) {
          const auto member = workspace_members.find(dependency.name);
          if (member == workspace_members.end()) {
            error = "workspace dependency '" + dependency.name +
                    "' is not a workspace member";
            return false;
          }
          dependency_manifest = member->second;
        } else {
          auto loaded =
              loadManifest(resolvePath(manifest.root_directory,
                                       dependency.path + "/chtholly.toml"),
                           error);
          if (!loaded)
            return false;
          dependency_manifest = std::move(*loaded);
          if (dependency_manifest.package_name != dependency.name) {
            error = "dependency '" + dependency.name + "' resolves package '" +
                    dependency_manifest.package_name + "'";
            return false;
          }
        }
        auto dependency_features = dependency.features;
        if (const auto it = features->dependency_features.find(dependency.name);
            it != features->dependency_features.end())
          dependency_features.insert(dependency_features.end(),
                                     it->second.begin(), it->second.end());
        auto dependency_index = requestPackage(std::move(dependency_manifest),
                                               std::move(dependency_features),
                                               dependency.default_features);
        if (!dependency_index)
          return false;
        if (!dependency.artifact.empty()) {
          const auto archive_path =
              resolvePath(manifest.root_directory, dependency.artifact);
          const auto root_path =
              std::filesystem::path(resolvePath(manifest.root_directory, "."));
          const auto normalized_archive =
              std::filesystem::path(archive_path).lexically_normal();
          auto archive_it = normalized_archive.begin();
          bool within = true;
          for (auto root_it = root_path.begin(); root_it != root_path.end();
               ++root_it, ++archive_it) {
            if (archive_it == normalized_archive.end() ||
                *archive_it != *root_it) {
              within = false;
              break;
            }
          }
          if (!within) {
            error = "compiler dependency '" + dependency.name +
                    "' artifact path escapes package root";
            return false;
          }
          const auto digest = sha256File(archive_path);
          if (!digest || *digest != dependency.artifact_sha256) {
            error = "compiler dependency '" + dependency.name +
                    "' artifact is missing or has a SHA-256 mismatch: '" +
                    archive_path + "'";
            return false;
          }
          packages[*dependency_index].artifact_archive_path = archive_path;
          packages[*dependency_index].artifact_archive_digest = *digest;
        }
        if (std::ranges::find(packages[index].dependencies,
                              *dependency_index) ==
            packages[index].dependencies.end())
          packages[index].dependencies.push_back(*dependency_index);
      }
      std::ranges::sort(packages[index].dependencies);
    }

    std::vector<std::size_t> incoming(packages.size());
    std::queue<std::size_t> ready;
    for (std::size_t index = 0; index < packages.size(); ++index) {
      incoming[index] = packages[index].dependencies.size();
      if (incoming[index] == 0)
        ready.push(index);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
      const auto dependency = ready.front();
      ready.pop();
      ++visited;
      for (std::size_t index = 0; index < packages.size(); ++index)
        if (std::ranges::find(packages[index].dependencies, dependency) !=
                packages[index].dependencies.end() &&
            --incoming[index] == 0)
          ready.push(index);
    }
    if (visited != packages.size()) {
      error = "compiler package graph contains a dependency cycle";
      return false;
    }

    for (const auto &selection : invocation.package_feature_selections)
      if (!package_indices.contains(selection.package_name)) {
        error = "package-qualified feature selection references unreachable "
                "package '" +
                selection.package_name + "'";
        return false;
      }
    for (const auto &package : invocation.default_feature_disabled_packages)
      if (!package_indices.contains(package)) {
        error = "--no-default-feature references unreachable package '" +
                package + "'";
        return false;
      }
    return true;
  }
};

std::optional<std::filesystem::path>
projectManifestPath(const CompilerInvocation &invocation,
                    const ManifestDiscovery &discovery) {
  if (!invocation.manifest_path.empty())
    return invocation.manifest_path;
  if (!invocation.project_path.empty())
    return std::filesystem::path(invocation.project_path) / "chtholly.toml";
  return discovery.project_manifest;
}

} // namespace

std::optional<CompilerBuildPlan>
resolveNextBuildPlan(const CompilerInvocation &invocation, std::string &error) {
  error.clear();
  const auto discovery_start =
      !invocation.project_path.empty()
          ? std::filesystem::path(invocation.project_path)
      : !invocation.workspace_path.empty()
          ? std::filesystem::path(invocation.workspace_path)
      : !invocation.input_path.empty()
          ? std::filesystem::path(invocation.input_path).parent_path()
          : std::filesystem::current_path();
  const auto discovery = discoverManifests(discovery_start);
  std::optional<CompilerWorkspaceInput> workspace;
  if (!invocation.workspace_path.empty()) {
    const auto path = std::filesystem::path(invocation.workspace_path);
    workspace = loadWorkspace(
        (path.extension() == ".toml" ? path : path / "chtholly.workspace.toml")
            .string(),
        error);
  } else if (discovery.workspace_manifest) {
    workspace = loadWorkspace(discovery.workspace_manifest->string(), error);
  }
  if ((!invocation.workspace_path.empty() || discovery.workspace_manifest) &&
      !workspace)
    return std::nullopt;

  PlanBuilder builder{.invocation = invocation, .error = error};
  if (workspace) {
    for (const auto &member_path : workspace->members) {
      auto member = loadManifest(resolvePath(workspace->root_directory,
                                             member_path + "/chtholly.toml"),
                                 error);
      if (!member)
        return std::nullopt;
      if (!builder.workspace_members.emplace(member->package_name, *member)
               .second) {
        error = "duplicate workspace package '" + member->package_name + "'";
        return std::nullopt;
      }
    }
  }

  std::optional<CompilerManifestInput> root_manifest;
  std::string selected = invocation.package_name;
  if (workspace &&
      (!invocation.workspace_path.empty() ||
       (invocation.input_path.empty() && invocation.project_path.empty() &&
        invocation.manifest_path.empty()))) {
    if (selected.empty()) {
      if (workspace->default_members.size() != 1) {
        error = "compiler workspace requires exactly one selected package";
        return std::nullopt;
      }
      const auto member_path =
          resolvePath(workspace->root_directory,
                      workspace->default_members.front() + "/chtholly.toml");
      root_manifest = loadManifest(member_path, error);
    } else if (const auto it = builder.workspace_members.find(selected);
               it != builder.workspace_members.end()) {
      root_manifest = it->second;
    } else {
      error = "unknown compiler workspace package '" + selected + "'";
      return std::nullopt;
    }
  } else if (const auto path = projectManifestPath(invocation, discovery)) {
    root_manifest = loadManifest(path->string(), error);
  }

  CompilerBuildPlan plan;
  if (!root_manifest) {
    if (invocation.input_path.empty()) {
      if (error.empty())
        error = "compiler requires a direct input, project, or workspace";
      return std::nullopt;
    }
    CompilerBuildPlanPackage package;
    package.name = "main";
    package.root_directory =
        normalized(std::filesystem::path(invocation.input_path).parent_path());
    package.entry_path = normalized(invocation.input_path);
    package.module_roots = {package.root_directory};
    plan.packages.push_back(std::move(package));
  } else {
    std::vector<std::string> root_features = invocation.feature_selections;
    for (const auto &selection : invocation.package_feature_selections) {
      if (selection.package_name == root_manifest->package_name)
        root_features.push_back(selection.feature_name);
    }
    const bool defaults =
        invocation.enable_default_features &&
        std::ranges::find(invocation.default_feature_disabled_packages,
                          root_manifest->package_name) ==
            invocation.default_feature_disabled_packages.end();
    auto root = builder.requestPackage(*root_manifest, std::move(root_features),
                                       defaults);
    if (!root || !builder.resolve())
      return std::nullopt;
    plan.packages = std::move(builder.packages);
    plan.root_package = *root;
    if (!invocation.input_path.empty())
      plan.packages[plan.root_package].entry_path =
          normalized(invocation.input_path);
    plan.package_name = root_manifest->package_name;
    plan.project_manifest_path = root_manifest->manifest_path;
    plan.project_root = root_manifest->root_directory;
    if (workspace) {
      plan.workspace_manifest_path = workspace->manifest_path;
      plan.workspace_root = workspace->root_directory;
    }
    const auto lock_root =
        plan.workspace_root.empty() ? plan.project_root : plan.workspace_root;
    plan.lockfile_path =
        (std::filesystem::path(lock_root) / "chtholly.lock").string();
  }
  if (plan.package_name.empty())
    plan.package_name = plan.packages[plan.root_package].name;

  TargetConfigInput target_input;
  if (root_manifest) {
    target_input.root_directory = root_manifest->root_directory;
    target_input.triple = root_manifest->target_triple;
    target_input.sysroot = root_manifest->target_sysroot;
    target_input.linker = root_manifest->target_linker;
  }
  auto target = resolveTargetConfig(invocation, target_input, error);
  if (!target)
    return std::nullopt;
  plan.target = std::move(*target);
  for (const auto &package : plan.packages) {
    if (package.cffi_identity &&
        package.cffi_identity->target != plan.target.info.triple) {
      error = "package '" + package.name + "' CFFI receipt targets '" +
              package.cffi_identity->target +
              "' instead of the resolved build target '" +
              plan.target.info.triple + "'";
      return std::nullopt;
    }
  }

  auto resources = locateCompilerResources(invocation, error);
  if (!resources && !error.empty())
    return std::nullopt;
  if (resources) {
    plan.resource_dir = resources->resource_dir;
    plan.runtime_link_manifest_path = resources->runtime_link_manifest_path;
    for (const auto &mapping : resources->runtime_symbol_mappings)
      plan.runtime_symbol_mappings.emplace_back(mapping.source_symbol,
                                                mapping.runtime_symbol);
    if (invocation.action == DriverAction::EmitExecutable) {
      plan.runtime_library_path = resources->runtime_library_path;
      plan.runtime_link_libraries = resources->runtime_link_libraries;
    }
  }
  return plan;
}

bool verifyOrUpdateNextLockfile(const CompilerBuildPlan &plan, bool locked,
                                std::string &error) {
  if (plan.lockfile_path.empty())
    return true;
  std::ostringstream output;
  output << "format = 1\ncompiler = \"chtholly\"\nroot = \"" << plan.package_name
         << "\"\n";
  std::vector<const CompilerBuildPlanPackage *> packages;
  for (const auto &package : plan.packages)
    packages.push_back(&package);
  std::ranges::sort(packages, {}, &CompilerBuildPlanPackage::name);
  for (const auto *package : packages) {
    output << "package = \"" << package->name << "\"\nmanifest = \""
           << sha256Hex(package->manifest_path) << "\"\n";
    if (!package->artifact_archive_digest.empty())
      output << "artifact-sha256 = \"" << package->artifact_archive_digest
             << "\"\n";
    if (package->cffi_required)
      output << "cffi-required = true\n";
    if (!package->cffi_receipt_digest.empty())
      output << "cffi-receipt-sha256 = \"" << package->cffi_receipt_digest
             << "\"\n";
    for (const auto &feature : package->resolved_features)
      output << "feature = \"" << package->name << '/' << feature << "\"\n";
    std::vector<std::string> dependencies;
    dependencies.reserve(package->dependencies.size());
    for (const auto dependency : package->dependencies)
      dependencies.push_back(plan.packages[dependency].name);
    std::ranges::sort(dependencies);
    for (const auto &dependency : dependencies)
      output << "dependency = \"" << package->name << '/' << dependency
             << "\"\n";
  }
  auto current = readTextFile(plan.lockfile_path, error);
  if (current && *current == output.str())
    return true;
  if (locked) {
    error = current ? "compiler lockfile is out of date"
                    : "compiler locked build requires chtholly.lock";
    return false;
  }
  error.clear();
  return writeTextFile(plan.lockfile_path, output.str(), error);
}

} // namespace chtholly
