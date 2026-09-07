#include "chtholly/ToolingRules/PackageSourceContractRules.h"

#include <sstream>
#include <utility>

namespace chtholly {

namespace {

std::string joinPath(const std::vector<std::string> &path) {
  std::ostringstream out;
  for (std::size_t index = 0; index < path.size(); ++index) {
    if (index != 0) {
      out << " -> ";
    }
    out << path[index];
  }
  return out.str();
}

PackageSourceContractDecision valid() {
  PackageSourceContractDecision decision;
  decision.valid = true;
  decision.failure = PackageSourceContractFailure::None;
  decision.stable_reason =
      packageSourceContractFailureSpelling(decision.failure);
  return decision;
}

PackageSourceContractDecision
withContext(PackageSourceContractDecision decision,
            PackageSourceContractUseKind use_kind,
            PackageSourceKind source_kind,
            PackageSourceContractFailureClass failure_class =
                PackageSourceContractFailureClass::None) {
  decision.use_kind = use_kind;
  decision.source_kind = source_kind;
  decision.failure_class = failure_class;
  return decision;
}

PackageSourceContractDecision failure(PackageSourceContractFailure reason,
                                      std::string message = {}) {
  PackageSourceContractDecision decision;
  decision.valid = false;
  decision.failure = reason;
  decision.stable_reason = packageSourceContractFailureSpelling(reason);
  decision.diagnostic_message = std::move(message);
  return decision;
}

PackageSourceContractFailureClass
failureClassFor(PackageSourceContractFailure failure_kind) {
  switch (failure_kind) {
  case PackageSourceContractFailure::None:
    return PackageSourceContractFailureClass::None;
  case PackageSourceContractFailure::PackageSource:
    return PackageSourceContractFailureClass::SourceResolution;
  case PackageSourceContractFailure::PackageBoundaryConflict:
    return PackageSourceContractFailureClass::PackageBoundary;
  case PackageSourceContractFailure::WorkspaceDependency:
    return PackageSourceContractFailureClass::WorkspaceSelection;
  case PackageSourceContractFailure::RegistryPackage:
    return PackageSourceContractFailureClass::RegistryResolution;
  case PackageSourceContractFailure::StaleLockfile:
    return PackageSourceContractFailureClass::Lockfile;
  case PackageSourceContractFailure::DependencyCycle:
    return PackageSourceContractFailureClass::DependencyGraph;
  case PackageSourceContractFailure::DependencyNameMismatch:
    return PackageSourceContractFailureClass::IdentityMismatch;
  case PackageSourceContractFailure::UnsupportedManifestSurface:
    return PackageSourceContractFailureClass::ManifestSurface;
  }
  return PackageSourceContractFailureClass::SourceResolution;
}

UnsupportedManifestSurfaceKind
manifestSurfaceKindFor(UnsupportedManifestSurfaceUseKind use_kind) {
  switch (use_kind) {
  case UnsupportedManifestSurfaceUseKind::Unknown:
    return UnsupportedManifestSurfaceKind::None;
  case UnsupportedManifestSurfaceUseKind::HttpRegistry:
    return UnsupportedManifestSurfaceKind::HttpRegistry;
  case UnsupportedManifestSurfaceUseKind::Publishing:
    return UnsupportedManifestSurfaceKind::Publishing;
  case UnsupportedManifestSurfaceUseKind::WorkspaceFeatureTable:
    return UnsupportedManifestSurfaceKind::WorkspaceFeatureTable;
  case UnsupportedManifestSurfaceUseKind::RemoteArtifactCache:
    return UnsupportedManifestSurfaceKind::RemoteArtifactCache;
  case UnsupportedManifestSurfaceUseKind::RegistryAuth:
    return UnsupportedManifestSurfaceKind::RegistryAuth;
  case UnsupportedManifestSurfaceUseKind::MacroImport:
    return UnsupportedManifestSurfaceKind::MacroImport;
  }
  return UnsupportedManifestSurfaceKind::None;
}

std::string
manifestSurfaceDiagnosticSubject(const UnsupportedManifestSurfaceFacts &facts) {
  if (!facts.manifest_key.empty()) {
    return facts.manifest_key;
  }
  if (!facts.subject.empty()) {
    return facts.subject;
  }
  return "manifest surface";
}

} // namespace

std::string packageSourceContractFailureSpelling(
    PackageSourceContractFailure failure_kind) {
  switch (failure_kind) {
  case PackageSourceContractFailure::None:
    return "none";
  case PackageSourceContractFailure::PackageSource:
    return "package-source";
  case PackageSourceContractFailure::PackageBoundaryConflict:
    return "package-boundary-conflict";
  case PackageSourceContractFailure::WorkspaceDependency:
    return "workspace-dependency";
  case PackageSourceContractFailure::RegistryPackage:
    return "registry-package";
  case PackageSourceContractFailure::StaleLockfile:
    return "stale-lockfile";
  case PackageSourceContractFailure::DependencyCycle:
    return "dependency-cycle";
  case PackageSourceContractFailure::DependencyNameMismatch:
    return "dependency-name-mismatch";
  case PackageSourceContractFailure::UnsupportedManifestSurface:
    return "unsupported-manifest-surface";
  }
  return "package-source";
}

std::string packageSourceKindSpelling(PackageSourceKind kind) {
  switch (kind) {
  case PackageSourceKind::Unknown:
    return "unknown";
  case PackageSourceKind::Path:
    return "path";
  case PackageSourceKind::Workspace:
    return "workspace";
  case PackageSourceKind::Git:
    return "git";
  case PackageSourceKind::Registry:
    return "registry";
  case PackageSourceKind::Artifact:
    return "artifact";
  }
  return "unknown";
}

std::string
unsupportedManifestSurfaceStableReason(UnsupportedManifestSurfaceKind kind) {
  switch (kind) {
  case UnsupportedManifestSurfaceKind::None:
    return "unsupported-manifest-surface";
  case UnsupportedManifestSurfaceKind::HttpRegistry:
    return "registry-url-manifest-key-invalid";
  case UnsupportedManifestSurfaceKind::Publishing:
    return "package-publish-table-invalid";
  case UnsupportedManifestSurfaceKind::WorkspaceFeatureTable:
    return "workspace-feature-table-invalid";
  case UnsupportedManifestSurfaceKind::RemoteArtifactCache:
    return "remote-artifact-cache-table-invalid";
  case UnsupportedManifestSurfaceKind::RegistryAuth:
    return "registry-inline-credential-invalid";
  case UnsupportedManifestSurfaceKind::MacroImport:
    return "macro-import-table-invalid";
  }
  return "unsupported-manifest-surface";
}

std::string unsupportedManifestSurfaceDiagnostic(
    const PackageSourceContractDecision &decision) {
  if (decision.stable_reason.empty()) {
    return decision.diagnostic_message;
  }
  if (decision.diagnostic_message.empty()) {
    return "[" + decision.stable_reason + "]";
  }
  return decision.diagnostic_message + " [" + decision.stable_reason + "]";
}

std::string unsupportedManifestSurfaceDiagnostic(
    const UnsupportedManifestSurfacePlan &plan) {
  return unsupportedManifestSurfaceDiagnostic(plan.decision);
}

std::string
unsupportedManifestSurfaceDiagnostic(
    const UnsupportedManifestSurfaceFacts &facts) {
  return unsupportedManifestSurfaceDiagnostic(
      PackageSourceContractResolver::resolveUnsupportedManifestSurfacePlan(
          facts));
}

PackageSourceContractDecision PackageSourceContractResolver::resolveDependency(
    const PackageDependencyFacts &facts) {
  if (facts.source_kind_conflict) {
    return withContext(
        failure(PackageSourceContractFailure::PackageSource,
                "dependency '" + facts.dependency_name +
                    "' cannot combine path, workspace, git, registry, and "
                    "artifact"),
        PackageSourceContractUseKind::DependencyResolution,
        facts.dependency_kind,
        PackageSourceContractFailureClass::SourceResolution);
  }
  if (!facts.source_available) {
    return withContext(
        failure(PackageSourceContractFailure::PackageSource,
                "unable to resolve dependency '" + facts.dependency_name + "'"),
        PackageSourceContractUseKind::DependencyResolution,
        facts.dependency_kind,
        PackageSourceContractFailureClass::SourceResolution);
  }
  if (!facts.package_name_matches) {
    const auto found = facts.resolved_package_name.empty()
                           ? facts.package_identity
                           : facts.resolved_package_name;
    return withContext(
        failure(PackageSourceContractFailure::DependencyNameMismatch,
                "dependency '" + facts.dependency_name +
                    "' package name mismatch: found '" + found + "'"),
        PackageSourceContractUseKind::DependencyResolution,
        facts.dependency_kind,
        PackageSourceContractFailureClass::IdentityMismatch);
  }
  auto decision =
      withContext(valid(), PackageSourceContractUseKind::DependencyResolution,
                  facts.dependency_kind);
  decision.normalized_package = facts.dependency_name;
  decision.normalized_source_identity = facts.source_identity;
  return decision;
}

PackageSourceContractDecision
PackageSourceContractResolver::resolvePackageSource(
    const PackageSourceResolutionFacts &facts) {
  if (facts.dependency_cycle) {
    const auto cycle = facts.dependency_cycle_path.empty()
                           ? facts.package_name
                           : joinPath(facts.dependency_cycle_path);
    return withContext(failure(PackageSourceContractFailure::DependencyCycle,
                               "cyclic package dependency: " + cycle),
                       PackageSourceContractUseKind::PackageSourceResolution,
                       facts.source_kind,
                       PackageSourceContractFailureClass::DependencyGraph);
  }
  if (facts.package_boundary_conflict) {
    return withContext(
        failure(PackageSourceContractFailure::PackageBoundaryConflict,
                "package boundary conflict for package '" + facts.package_name +
                    "'"),
        PackageSourceContractUseKind::PackageSourceResolution,
        facts.source_kind, PackageSourceContractFailureClass::PackageBoundary);
  }
  if (!facts.dependency_closure_known) {
    return withContext(
        failure(PackageSourceContractFailure::PackageSource,
                "dependency closure is incomplete for package '" +
                    facts.package_name + "'"),
        PackageSourceContractUseKind::PackageSourceResolution,
        facts.source_kind, PackageSourceContractFailureClass::SourceResolution);
  }
  auto decision = withContext(
      valid(), PackageSourceContractUseKind::PackageSourceResolution,
      facts.source_kind);
  decision.normalized_package = facts.package_name;
  decision.normalized_source_identity = facts.source_identity;
  return decision;
}

PackageSourceContractDecision
PackageSourceContractResolver::resolveRegistryPackage(
    const RegistryPackageFacts &facts) {
  if (!facts.registry_known) {
    return withContext(failure(PackageSourceContractFailure::RegistryPackage,
                               "unknown registry '" + facts.registry_name +
                                   "' for dependency '" +
                                   facts.dependency_name + "'"),
                       PackageSourceContractUseKind::RegistryResolution,
                       PackageSourceKind::Registry,
                       PackageSourceContractFailureClass::RegistryResolution);
  }
  if (!facts.entry_readable) {
    return withContext(failure(PackageSourceContractFailure::RegistryPackage,
                               "registry dependency '" + facts.dependency_name +
                                   "' entry is not readable"),
                       PackageSourceContractUseKind::RegistryResolution,
                       PackageSourceKind::Registry,
                       PackageSourceContractFailureClass::RegistryResolution);
  }
  if (!facts.version_satisfies_requirement) {
    std::ostringstream out;
    out << "registry dependency '" << facts.dependency_name
        << "' has no version satisfying '" << facts.version_requirement
        << "' in registry '" << facts.registry_name << "'";
    if (!facts.available_versions.empty()) {
      out << "; candidates: ";
      for (std::size_t index = 0; index < facts.available_versions.size();
           ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << facts.available_versions[index];
      }
    }
    return withContext(
        failure(PackageSourceContractFailure::RegistryPackage, out.str()),
        PackageSourceContractUseKind::RegistryResolution,
        PackageSourceKind::Registry,
        PackageSourceContractFailureClass::RegistryResolution);
  }
  if (!facts.entry_package_matches) {
    return withContext(
        failure(PackageSourceContractFailure::DependencyNameMismatch,
                "registry entry package name mismatch for dependency '" +
                    facts.dependency_name + "': found '" +
                    facts.entry_package_name + "'"),
        PackageSourceContractUseKind::RegistryResolution,
        PackageSourceKind::Registry,
        PackageSourceContractFailureClass::IdentityMismatch);
  }
  if (!facts.entry_version_matches) {
    auto message = "registry entry version mismatch for dependency '" +
                   facts.dependency_name + "'";
    if (!facts.selected_version.empty() || !facts.entry_version.empty()) {
      message += ": file is '" + facts.selected_version +
                 "' but entry declares '" + facts.entry_version + "'";
    }
    return withContext(failure(PackageSourceContractFailure::RegistryPackage,
                               std::move(message)),
                       PackageSourceContractUseKind::RegistryResolution,
                       PackageSourceKind::Registry,
                       PackageSourceContractFailureClass::RegistryResolution);
  }
  auto decision =
      withContext(valid(), PackageSourceContractUseKind::RegistryResolution,
                  PackageSourceKind::Registry);
  decision.normalized_package = facts.dependency_name;
  decision.normalized_version = facts.selected_version;
  decision.normalized_source_identity = facts.registry_name + ":" +
                                        facts.dependency_name + "@" +
                                        facts.selected_version;
  return decision;
}

PackageSourceContractDecision PackageSourceContractResolver::resolveLockfile(
    const LockfileResolutionFacts &facts) {
  if (!facts.lockfile_enabled) {
    auto decision =
        withContext(valid(), PackageSourceContractUseKind::LockfileResolution,
                    PackageSourceKind::Unknown);
    decision.normalized_package = facts.package_name;
    return decision;
  }
  if (facts.locked_mode && !facts.existing_lockfile_readable) {
    return withContext(failure(PackageSourceContractFailure::StaleLockfile,
                               "registry dependency '" + facts.package_name +
                                   "' requires a lockfile"),
                       PackageSourceContractUseKind::LockfileResolution,
                       PackageSourceKind::Registry,
                       PackageSourceContractFailureClass::Lockfile);
  }
  if (facts.stale_lockfile) {
    return withContext(failure(PackageSourceContractFailure::StaleLockfile,
                               "stale lockfile: " + facts.lockfile_path),
                       PackageSourceContractUseKind::LockfileResolution,
                       PackageSourceKind::Registry,
                       PackageSourceContractFailureClass::Lockfile);
  }
  auto decision =
      withContext(valid(), PackageSourceContractUseKind::LockfileResolution,
                  PackageSourceKind::Registry);
  decision.normalized_package = facts.package_name;
  decision.normalized_source_identity = facts.expected_source_identity.empty()
                                            ? facts.actual_source_identity
                                            : facts.expected_source_identity;
  return decision;
}

PackageSourceContractDecision
PackageSourceContractResolver::resolveWorkspaceSelection(
    const WorkspaceSelectionFacts &facts) {
  if (!facts.workspace_mode) {
    return withContext(
        failure(PackageSourceContractFailure::WorkspaceDependency,
                "workspace dependency '" + facts.selected_package +
                    "' requires workspace mode"),
        PackageSourceContractUseKind::WorkspaceSelection,
        PackageSourceKind::Workspace,
        PackageSourceContractFailureClass::WorkspaceSelection);
  }
  if (!facts.member_known) {
    return withContext(
        failure(PackageSourceContractFailure::WorkspaceDependency,
                "workspace dependency '" + facts.selected_package +
                    "' is not a workspace member"),
        PackageSourceContractUseKind::WorkspaceSelection,
        PackageSourceKind::Workspace,
        PackageSourceContractFailureClass::WorkspaceSelection);
  }
  if (!facts.dependency_closure_known) {
    return withContext(
        failure(PackageSourceContractFailure::PackageSource,
                "workspace dependency closure is incomplete for '" +
                    facts.selected_package + "'"),
        PackageSourceContractUseKind::WorkspaceSelection,
        PackageSourceKind::Workspace,
        failureClassFor(PackageSourceContractFailure::PackageSource));
  }
  auto decision =
      withContext(valid(), PackageSourceContractUseKind::WorkspaceSelection,
                  PackageSourceKind::Workspace);
  decision.normalized_package = facts.selected_package;
  decision.normalized_source_identity = facts.workspace_root;
  return decision;
}

PackageSourceContractDecision
PackageSourceContractResolver::resolveUnsupportedManifestSurface(
    const UnsupportedManifestSurfaceFacts &facts) {
  const auto surface_kind = manifestSurfaceKindFor(facts.use_kind);
  auto decision = withContext(
      failure(PackageSourceContractFailure::UnsupportedManifestSurface,
              manifestSurfaceDiagnosticSubject(facts) +
                  " is not accepted by the manifest schema"),
      PackageSourceContractUseKind::ManifestSurface,
      PackageSourceKind::Unknown,
      PackageSourceContractFailureClass::ManifestSurface);
  decision.manifest_surface = surface_kind;
  decision.stable_reason = unsupportedManifestSurfaceStableReason(surface_kind);
  decision.normalized_package = facts.subject;
  return decision;
}

UnsupportedManifestSurfacePlan
PackageSourceContractResolver::resolveUnsupportedManifestSurfacePlan(
    const UnsupportedManifestSurfaceFacts &facts) {
  auto decision = resolveUnsupportedManifestSurface(facts);

  UnsupportedManifestSurfacePlan plan;
  plan.supported = decision.valid;
  plan.rejected =
      !decision.valid &&
      decision.failure ==
          PackageSourceContractFailure::UnsupportedManifestSurface;
  plan.use_kind = facts.use_kind;
  plan.surface_kind = decision.manifest_surface;
  plan.failure = decision.failure;
  plan.failure_class = decision.failure_class;
  plan.subject = facts.subject;
  plan.manifest_key = facts.manifest_key;
  plan.stable_reason = decision.stable_reason;
  plan.diagnostic_message = decision.diagnostic_message;
  plan.decision = std::move(decision);
  return plan;
}

} // namespace chtholly
