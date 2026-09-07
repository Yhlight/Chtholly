#pragma once

#include "chtholly/Basic/Diagnostic.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

enum class WorkspaceArtifactCacheStatus {
  Disabled,
  Miss,
  Hit,
  Write,
  Stale,
};

enum class WorkspaceArtifactReleaseGateStatus {
  NotApplicable,
  Pass,
  Blocked,
};

inline constexpr std::string_view workspaceArtifactReleaseGateStatusSpelling(
    WorkspaceArtifactReleaseGateStatus status) {
  switch (status) {
  case WorkspaceArtifactReleaseGateStatus::NotApplicable:
    return "not-applicable";
  case WorkspaceArtifactReleaseGateStatus::Pass:
    return "pass";
  case WorkspaceArtifactReleaseGateStatus::Blocked:
    return "blocked";
  }
  return "unknown";
}

inline bool workspaceArtifactReportHasField(std::string_view report,
                                            std::string_view field) {
  std::size_t offset = 0;
  while (offset < report.size()) {
    while (offset < report.size() &&
           (report[offset] == ' ' || report[offset] == '\t' ||
            report[offset] == '\r' || report[offset] == '\n')) {
      ++offset;
    }
    const auto begin = offset;
    while (offset < report.size() && report[offset] != ' ' &&
           report[offset] != '\t' && report[offset] != '\r' &&
           report[offset] != '\n') {
      ++offset;
    }
    if (report.substr(begin, offset - begin) == field) {
      return true;
    }
  }
  return false;
}

inline WorkspaceArtifactReleaseGateStatus
workspaceArtifactReleaseGateStatusFromReport(std::string_view report) {
  if (workspaceArtifactReportHasField(report, "release-gate=pass")) {
    return WorkspaceArtifactReleaseGateStatus::Pass;
  }
  if (workspaceArtifactReportHasField(report, "release-gate=blocked")) {
    return WorkspaceArtifactReleaseGateStatus::Blocked;
  }
  return WorkspaceArtifactReleaseGateStatus::NotApplicable;
}

struct WorkspaceArtifactResult {
  struct InvalidationExplanation {
    std::string module;
    std::string query_kind;
    std::string query_key;
    std::string result;
    std::string reason;
    std::string provider;
    std::string record_id;
    std::string old_digest;
    std::string new_digest;
    std::vector<std::string> chain;
    std::size_t evaluated_records = 0;
    std::size_t hit_records = 0;
    std::size_t miss_records = 0;
  };
  std::string package_name;
  std::string output_path;
  std::vector<std::string> dependencies;
  std::vector<std::string> semantic_interface_paths;
  std::vector<std::string> object_paths;
  std::vector<std::string> compiled_modules;
  std::vector<std::string> reused_modules;
  std::vector<std::string> native_link_libraries;
  std::vector<InvalidationExplanation> invalidation_explanations;
  std::string package_artifact_identity;
  std::string package_artifact_manifest_path;
  std::string package_artifact_manifest_digest;
  bool package_default_features = true;
  std::string backend_report_line;
  std::string error;
  std::optional<Diagnostic> diagnostic;
  WorkspaceArtifactCacheStatus cache_status =
      WorkspaceArtifactCacheStatus::Disabled;
  WorkspaceArtifactReleaseGateStatus release_gate_status =
      WorkspaceArtifactReleaseGateStatus::NotApplicable;
  bool success = false;
  bool cache_hit = false;
  bool relinked = false;
};

struct WorkspaceArtifactExecutionOptions {
  bool enable_artifact_cache = false;
  std::string artifact_cache_dir;
};

} // namespace chtholly
