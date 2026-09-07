#pragma once

#include "chtholly/Basic/LanguageVersion.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

enum class ProjectDependencyKind {
  Path,
  Workspace,
  Git,
  Registry,
  Artifact,
};

enum class ProjectGitSelectorKind {
  None,
  Rev,
  Tag,
  Branch,
};

struct ProjectDependency {
  std::string name;
  std::string path;
  std::string git_url;
  ProjectGitSelectorKind git_selector_kind = ProjectGitSelectorKind::None;
  std::string git_selector_value;
  std::string registry;
  std::string version_requirement;
  std::string artifact_identity;
  std::string artifact_manifest_digest;
  std::vector<std::string> features;
  bool optional = false;
  bool enable_default_features = true;
  bool default_features_specified = false;
  ProjectDependencyKind kind = ProjectDependencyKind::Path;
};

struct ProjectFeature {
  std::string name;
  std::vector<std::string> requirements;
};

enum class ProjectRegistryIndexKind {
  LocalPath,
  RemoteGit,
};

struct ProjectRegistryConfig {
  std::string name;
  std::string index;
  std::vector<std::string> trusted_keys;
  std::vector<std::string> root_keys;
  std::uint32_t root_threshold = 0;
  std::string origin;
  std::string publish_url;
  std::string ca_bundle;
  std::vector<std::string> witness_urls;
  std::vector<std::string> witness_keys;
  std::uint32_t witness_threshold = 0;
  std::string witness_ca_bundle;
  ProjectRegistryIndexKind index_kind = ProjectRegistryIndexKind::LocalPath;
};

struct ProjectManifest {
  std::string manifest_path;
  std::string root_directory;
  std::string package_name;
  std::optional<LanguageVersion> language_version;
  std::string entry;
  std::vector<std::string> module_paths;
  std::vector<std::string> native_library_paths;
  std::vector<std::string> native_link_libraries;
  std::vector<std::string> c_include_paths;
  std::vector<std::string> c_defines;
  std::vector<ProjectDependency> dependencies;
  std::vector<ProjectFeature> features;
  std::vector<ProjectRegistryConfig> registries;
  std::string target_triple;
  std::string target_sysroot;
  std::string target_linker;
};

std::optional<ProjectManifest>
parseProjectManifestText(std::string_view text, std::string manifest_path,
                         std::string &error);
std::optional<ProjectManifest>
loadProjectManifestFile(const std::string &manifest_path, std::string &error);
bool classifyProjectRegistryIndex(std::string_view index,
                                  ProjectRegistryIndexKind &kind,
                                  std::string &error);

} // namespace chtholly
