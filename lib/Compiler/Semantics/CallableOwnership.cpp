#include "chtholly/Compiler/CallableOwnership.h"
#include "chtholly/Compiler/DenseWorklist.h"
#include "chtholly/Compiler/Outcome.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace chtholly::compiler {
CallableOwnershipSummary indirectCallOwnershipSummary(const SemIR &sem_ir,
                                                       TypeId function_type) {
  CallableOwnershipSummary summary;
  const auto &signature = sem_ir.type(function_type);
  const auto parameters = sem_ir.typeBlock(TypeBlockId(signature.arg0));
  const auto return_paths = sem_ir.loanCarrierPaths(TypeId(signature.arg1));
  summary.returns_owned = return_paths.empty();
  for (std::uint32_t i = 0; i < parameters.size(); ++i) {
    if (sem_ir.type(parameters[i]).kind != SemTypeKind::Reference) continue;
    const OwnershipRegion region{.parameter_index = i};
    const bool mutable_reference = sem_ir.referenceMutability(parameters[i]) == SemReferenceMutability::Mutable;
    summary.effects.push_back({mutable_reference ? CallableEffectKind::Write : CallableEffectKind::Read, region});
    if (mutable_reference)
      summary.postconditions.push_back({region, CallableOutcomeAll});
    for (const auto &path : return_paths)
      summary.return_provenance.push_back({region, path});
  }
  summary.canonicalize();
  return summary;
}

namespace {

#include "CallableOwnershipGraph.inc"
} // namespace

CallableControlFlowGraph buildCallableControlFlowGraph(const SemIR &sem_ir,
                                                       FunctionId function) {
  const auto &record = sem_ir.function(function);
  const auto internal = buildFunctionGraph(sem_ir, record, record.body);
  CallableControlFlowGraph result;
  result.instructions = internal.instructions;
  result.entries = internal.entries;
  for (const auto &[from, successors] : internal.successors)
    for (const auto &successor : successors)
      result.edges.push_back({InstId(from), successor.instruction});
  std::ranges::sort(result.edges, [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.from.index, lhs.to.index) <
           std::tie(rhs.from.index, rhs.to.index);
  });
  return result;
}

namespace {

#include "CallableOwnershipRegions.inc"
} // namespace

#include "CallableOwnershipSummary.inc"

} // namespace chtholly::compiler
