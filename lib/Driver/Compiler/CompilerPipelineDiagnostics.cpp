#include "CompilerPipelineInternal.h"

#include <algorithm>

namespace chtholly {

void CompilerPipelineDiagnosticsService::appendInvalidationExplanations(
    std::string_view package_name,
    const compiler::IncrementalCompilationPlan &compilation_plan,
    std::vector<WorkspaceArtifactResult::InvalidationExplanation> &output) {
  for (const auto &decision : compilation_plan.decisions()) {
    const auto append = [&](std::string_view reason,
                            std::string_view provider = {},
                            std::string_view binding = {}) {
      const auto qualified_module =
          std::string(package_name) + "/" + decision.module_name;
      output.push_back({.module = qualified_module,
                        .query_kind = "next-check-unit",
                        .query_key = qualified_module,
                        .result = std::string(
                            compiler::unitCompilationActionName(decision.action)),
                        .reason = std::string(reason),
                        .provider = std::string(provider),
                        .record_id = std::string(binding)});
    };
    if (decision.invalidations.empty()) {
      append("up-to-date");
      continue;
    }
    for (const auto &invalidation : decision.invalidations)
      append(compiler::unitInvalidationReasonName(invalidation.reason),
             invalidation.provider.package_name.empty()
                 ? std::string_view{}
                 : std::string_view(invalidation.provider.package_name + "/" +
                                    invalidation.provider.module_name),
             invalidation.binding_name);
  }
}

std::string CompilerPipelineDiagnosticsService::analysisMetricsJson(
    std::span<const PackageQueryResult> results) {
  std::vector<compiler::UnitAnalysisMetrics> units;
  for (const auto &result : results) {
    if (!result.session)
      continue;
    auto session_units = result.session->analysisMetricUnits();
    units.insert(units.end(), session_units.begin(), session_units.end());
  }
  std::ranges::sort(units, {}, &compiler::UnitAnalysisMetrics::unit);
  return compiler::analysisMetricsJson(units);
}

} // namespace chtholly
