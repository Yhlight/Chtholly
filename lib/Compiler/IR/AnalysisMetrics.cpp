#include "chtholly/Compiler/AnalysisMetrics.h"

#include <algorithm>
#include <sstream>

namespace chtholly::compiler {
namespace {

void appendJsonString(std::ostringstream &out, std::string_view value) {
  out << '"';
  for (const auto ch : value) {
    if (ch == '"' || ch == '\\')
      out << '\\';
    out << ch;
  }
  out << '"';
}

void appendFixedPoint(std::ostringstream &out,
                      const FixedPointAnalysisMetrics &value) {
  out << "{\"functions\":" << value.functions
      << ",\"cfg-instructions\":" << value.cfg_instructions
      << ",\"cfg-edges\":" << value.cfg_edges
      << ",\"provenance-work-items\":" << value.provenance_work_items
      << ",\"provenance-state-changes\":" << value.provenance_state_changes
      << ",\"postcondition-work-items\":" << value.postcondition_work_items
      << ",\"postcondition-state-changes\":"
      << value.postcondition_state_changes
      << ",\"scc-components\":" << value.scc_components
      << ",\"scc-function-evaluations\":" << value.scc_function_evaluations
      << ",\"region-widenings\":" << value.region_widenings
      << ",\"postcondition-widenings\":" << value.postcondition_widenings
      << ",\"max-worklist-depth\":" << value.max_worklist_depth
      << ",\"elapsed-us\":" << value.elapsed_us << '}';
}

void appendPlaceState(std::ostringstream &out,
                      const PlaceStateAnalysisMetrics &value) {
  out << "{\"place-work-items\":" << value.place_work_items
      << ",\"loan-flow-work-items\":" << value.loan_flow_work_items
      << ",\"liveness-work-items\":" << value.liveness_work_items
      << ",\"state-changes\":" << value.state_changes
      << ",\"call-conflict-checks\":" << value.call_conflict_checks
      << ",\"loan-region-widenings\":" << value.loan_region_widenings
      << ",\"max-worklist-depth\":" << value.max_worklist_depth
      << ",\"elapsed-us\":" << value.elapsed_us << '}';
}

void add(FixedPointAnalysisMetrics &target,
         const FixedPointAnalysisMetrics &source) {
#define CHTHOLLY_ADD_FIELD(Name) target.Name += source.Name
  CHTHOLLY_ADD_FIELD(functions);
  CHTHOLLY_ADD_FIELD(cfg_instructions);
  CHTHOLLY_ADD_FIELD(cfg_edges);
  CHTHOLLY_ADD_FIELD(provenance_work_items);
  CHTHOLLY_ADD_FIELD(provenance_state_changes);
  CHTHOLLY_ADD_FIELD(postcondition_work_items);
  CHTHOLLY_ADD_FIELD(postcondition_state_changes);
  CHTHOLLY_ADD_FIELD(scc_components);
  CHTHOLLY_ADD_FIELD(scc_function_evaluations);
  CHTHOLLY_ADD_FIELD(region_widenings);
  CHTHOLLY_ADD_FIELD(postcondition_widenings);
  CHTHOLLY_ADD_FIELD(elapsed_us);
#undef CHTHOLLY_ADD_FIELD
  target.max_worklist_depth =
      std::max(target.max_worklist_depth, source.max_worklist_depth);
}

void add(PlaceStateAnalysisMetrics &target,
         const PlaceStateAnalysisMetrics &source) {
  target.place_work_items += source.place_work_items;
  target.loan_flow_work_items += source.loan_flow_work_items;
  target.liveness_work_items += source.liveness_work_items;
  target.state_changes += source.state_changes;
  target.call_conflict_checks += source.call_conflict_checks;
  target.loan_region_widenings += source.loan_region_widenings;
  target.max_worklist_depth =
      std::max(target.max_worklist_depth, source.max_worklist_depth);
  target.elapsed_us += source.elapsed_us;
}

} // namespace

std::string analysisMetricsJson(std::span<const UnitAnalysisMetrics> units) {
  AnalysisMetrics totals;
  std::ostringstream out;
  out << "{\"schema\":\"chtholly-compiler-analysis-metrics-v1\",\"units\":[";
  for (std::size_t index = 0; index < units.size(); ++index) {
    if (index != 0)
      out << ',';
    const auto &unit = units[index];
    out << "{\"unit\":";
    appendJsonString(out, unit.unit);
    out << ",\"reused\":" << (unit.reused ? "true" : "false")
        << ",\"callable-ownership\":";
    const AnalysisMetrics empty;
    const auto &metrics = unit.metrics ? *unit.metrics : empty;
    appendFixedPoint(out, metrics.callable_ownership);
    out << ",\"place-state\":";
    appendPlaceState(out, metrics.place_state);
    out << '}';
    add(totals.callable_ownership, metrics.callable_ownership);
    add(totals.place_state, metrics.place_state);
  }
  out << "],\"totals\":{\"callable-ownership\":";
  appendFixedPoint(out, totals.callable_ownership);
  out << ",\"place-state\":";
  appendPlaceState(out, totals.place_state);
  out << "}}";
  return out.str();
}

} // namespace chtholly::compiler
