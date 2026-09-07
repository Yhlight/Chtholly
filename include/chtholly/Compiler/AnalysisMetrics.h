#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace chtholly::compiler {

struct FixedPointAnalysisMetrics {
  std::uint64_t functions = 0;
  std::uint64_t cfg_instructions = 0;
  std::uint64_t cfg_edges = 0;
  std::uint64_t provenance_work_items = 0;
  std::uint64_t provenance_state_changes = 0;
  std::uint64_t postcondition_work_items = 0;
  std::uint64_t postcondition_state_changes = 0;
  std::uint64_t scc_components = 0;
  std::uint64_t scc_function_evaluations = 0;
  std::uint64_t region_widenings = 0;
  std::uint64_t postcondition_widenings = 0;
  std::uint64_t max_worklist_depth = 0;
  std::uint64_t elapsed_us = 0;
};

struct PlaceStateAnalysisMetrics {
  std::uint64_t place_work_items = 0;
  std::uint64_t loan_flow_work_items = 0;
  std::uint64_t liveness_work_items = 0;
  std::uint64_t state_changes = 0;
  std::uint64_t call_conflict_checks = 0;
  std::uint64_t loan_region_widenings = 0;
  std::uint64_t max_worklist_depth = 0;
  std::uint64_t elapsed_us = 0;
};

struct AnalysisMetrics {
  FixedPointAnalysisMetrics callable_ownership;
  PlaceStateAnalysisMetrics place_state;
};

struct UnitAnalysisMetrics {
  std::string_view unit;
  bool reused = false;
  const AnalysisMetrics *metrics = nullptr;
};

[[nodiscard]] std::string
analysisMetricsJson(std::span<const UnitAnalysisMetrics> units);

} // namespace chtholly::compiler
