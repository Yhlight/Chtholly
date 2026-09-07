#pragma once

#include <string>
#include <vector>

namespace chtholly {

enum class PackageSourceKind {
  Unknown,
  Path,
  Workspace,
  Git,
  Registry,
  Artifact,
};

enum class PackageSourceContractFailure {
  None,
  PackageSource,
  PackageBoundaryConflict,
  WorkspaceDependency,
  RegistryPackage,
  StaleLockfile,
  DependencyCycle,
  DependencyNameMismatch,
  UnsupportedManifestSurface,
};

enum class PackageSourceContractUseKind {
  Unknown,
  DependencyResolution,
  PackageSourceResolution,
  RegistryResolution,
  LockfileResolution,
  WorkspaceSelection,
  ManifestSurface,
};

enum class PackageSourceContractFailureClass {
  None,
  SourceResolution,
  PackageBoundary,
  WorkspaceSelection,
  RegistryResolution,
  Lockfile,
  DependencyGraph,
  IdentityMismatch,
  ManifestSurface,
};

enum class UnsupportedManifestSurfaceKind {
  None,
  HttpRegistry,
  Publishing,
  WorkspaceFeatureTable,
  RemoteArtifactCache,
  RegistryAuth,
  MacroImport,
};

enum class UnsupportedManifestSurfaceUseKind {
  Unknown,
  HttpRegistry,
  Publishing,
  WorkspaceFeatureTable,
  RemoteArtifactCache,
  RegistryAuth,
  MacroImport,
};

struct PackageDependencyFacts {
  std::string dependency_name;
  PackageSourceKind dependency_kind = PackageSourceKind::Path;
  std::string request_spelling;
  std::string manifest_root;
  std::string resolved_root;
  std::string package_identity;
  std::string resolved_package_name;
  std::string source_identity;
  bool source_available = false;
  bool package_name_matches = true;
  bool source_kind_conflict = false;
};

struct PackageSourceResolutionFacts {
  std::string root_package;
  std::string package_name;
  PackageSourceKind source_kind = PackageSourceKind::Path;
  std::string source_identity;
  bool dependency_closure_known = false;
  bool package_boundary_conflict = false;
  bool dependency_cycle = false;
  std::vector<std::string> dependency_cycle_path;
};

struct RegistryPackageFacts {
  std::string dependency_name;
  std::string registry_name;
  std::string registry_index;
  std::string version_requirement;
  std::string selected_version;
  std::string entry_package_name;
  std::string entry_version;
  std::vector<std::string> available_versions;
  bool registry_known = false;
  bool entry_readable = false;
  bool version_satisfies_requirement = false;
  bool entry_package_matches = true;
  bool entry_version_matches = true;
};

struct LockfileResolutionFacts {
  std::string lockfile_path;
  bool lockfile_enabled = true;
  bool locked_mode = false;
  bool existing_lockfile_readable = true;
  std::string package_name;
  std::string expected_source_identity;
  std::string actual_source_identity;
  bool stale_lockfile = false;
};

struct WorkspaceSelectionFacts {
  std::string workspace_root;
  std::string selected_package;
  bool workspace_mode = false;
  bool member_known = false;
  bool dependency_closure_known = false;
  bool default_member_selected = false;
};

struct UnsupportedManifestSurfaceFacts {
  UnsupportedManifestSurfaceUseKind use_kind =
      UnsupportedManifestSurfaceUseKind::Unknown;
  std::string subject;
  std::string manifest_key;
};

struct PackageSourceContractDecision {
  bool valid = false;
  PackageSourceContractFailure failure = PackageSourceContractFailure::None;
  PackageSourceContractUseKind use_kind = PackageSourceContractUseKind::Unknown;
  PackageSourceKind source_kind = PackageSourceKind::Unknown;
  PackageSourceContractFailureClass failure_class =
      PackageSourceContractFailureClass::None;
  UnsupportedManifestSurfaceKind manifest_surface =
      UnsupportedManifestSurfaceKind::None;
  std::string stable_reason;
  std::string diagnostic_message;
  std::string normalized_package;
  std::string normalized_source_identity;
  std::string normalized_version;
};

struct UnsupportedManifestSurfacePlan {
  bool supported = false;
  bool rejected = false;
  UnsupportedManifestSurfaceUseKind use_kind =
      UnsupportedManifestSurfaceUseKind::Unknown;
  UnsupportedManifestSurfaceKind surface_kind =
      UnsupportedManifestSurfaceKind::None;
  PackageSourceContractFailure failure = PackageSourceContractFailure::None;
  PackageSourceContractFailureClass failure_class =
      PackageSourceContractFailureClass::None;
  std::string subject;
  std::string manifest_key;
  std::string stable_reason;
  std::string diagnostic_message;
  PackageSourceContractDecision decision;
};

class PackageSourceContractResolver {
public:
  static PackageSourceContractDecision
  resolveDependency(const PackageDependencyFacts &facts);

  static PackageSourceContractDecision
  resolvePackageSource(const PackageSourceResolutionFacts &facts);

  static PackageSourceContractDecision
  resolveRegistryPackage(const RegistryPackageFacts &facts);

  static PackageSourceContractDecision
  resolveLockfile(const LockfileResolutionFacts &facts);

  static PackageSourceContractDecision
  resolveWorkspaceSelection(const WorkspaceSelectionFacts &facts);

  static PackageSourceContractDecision
  resolveUnsupportedManifestSurface(
      const UnsupportedManifestSurfaceFacts &facts);

  static UnsupportedManifestSurfacePlan
  resolveUnsupportedManifestSurfacePlan(
      const UnsupportedManifestSurfaceFacts &facts);
};

std::string
packageSourceContractFailureSpelling(PackageSourceContractFailure failure);

std::string packageSourceKindSpelling(PackageSourceKind kind);

std::string
unsupportedManifestSurfaceStableReason(UnsupportedManifestSurfaceKind kind);

std::string unsupportedManifestSurfaceDiagnostic(
    const PackageSourceContractDecision &decision);

std::string unsupportedManifestSurfaceDiagnostic(
    const UnsupportedManifestSurfacePlan &plan);

std::string
unsupportedManifestSurfaceDiagnostic(
    const UnsupportedManifestSurfaceFacts &facts);

} // namespace chtholly
