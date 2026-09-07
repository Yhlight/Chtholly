#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace chtholly::compiler {

enum class CallableOwnershipViolationKind : std::uint8_t {
  BorrowEscape,
  NonConvergent,
  ArtifactMismatch,
  ContractMismatch,
};

enum class OwnershipEvidenceKind : std::uint8_t {
  BorrowOrigin,
  ConflictingAccess,
  MoveOrigin,
  CallEffect,
  ReturnProvenance,
  CleanupBoundary,
  ContractBoundary,
  WidenedRegion,
};

struct OwnershipEvidence {
  InstId instruction;
  OwnershipEvidenceKind kind = OwnershipEvidenceKind::BorrowOrigin;
  std::uint32_t parameter_index = 0;
  std::string detail;
};

struct CallableOwnershipViolation {
  InstId instruction;
  CallableOwnershipViolationKind kind =
      CallableOwnershipViolationKind::BorrowEscape;
  std::vector<OwnershipEvidence> evidence;
};

// Read-only projection of the callable ownership CFG. The ownership pass
// remains the single implementation of loop/defer edge construction; iterator
// ergonomics tests consume this projection to compare cleanup facts.
struct CallableControlFlowEdge {
  InstId from;
  InstId to;
};

struct CallableControlFlowGraph {
  std::vector<InstId> instructions;
  std::vector<InstId> entries;
  std::vector<CallableControlFlowEdge> edges;
};

[[nodiscard]] CallableControlFlowGraph
buildCallableControlFlowGraph(const SemIR &sem_ir, FunctionId function);

// Returns the summary attached to either a local callable or its canonical
// imported public entity.
[[nodiscard]] const CallableOwnershipSummary *
callableOwnershipSummary(const SemIR &sem_ir, FunctionRefId reference);

// Conservative boundary for a Chtholly function value whose target is erased.
[[nodiscard]] CallableOwnershipSummary
indirectCallOwnershipSummary(const SemIR &sem_ir, TypeId function_type);

[[nodiscard]] CallableOwnershipSummary
effectiveCallableOwnershipSummary(CallableOwnershipSummary summary);

[[nodiscard]] bool
callableOwnershipSubstitutes(const CallableOwnershipSummary &source,
                             const CallableOwnershipSummary &contract);

[[nodiscard]] bool callbackAdapterOwnershipSubstitutes(const SemIR &sem_ir,
                                                       FunctionRefId source,
                                                       TypeId callback_type);
[[nodiscard]] std::optional<CallableOwnershipSummary>
callbackAdapterCallOwnershipSummary(const SemIR &sem_ir, TypeId adapter_type);
[[nodiscard]] std::optional<CallableOwnershipSummary>
callbackRegistrationCallOwnershipSummary(const SemIR &sem_ir,
                                         InstId instruction);
[[nodiscard]] std::optional<CallableOwnershipSummary>
foreignOutcomeCallOwnershipSummary(const SemIR &sem_ir, FunctionRefId function,
                                   TypeId projected_result);

// Computes exact parameter-region effects after every generic specific has
// been materialized. Calls are propagated to a fixed point over the local call
// graph; imported summaries are immutable boundary facts.
[[nodiscard]] std::vector<CallableOwnershipViolation>
analyzeCallableOwnership(SemIR &sem_ir);
[[nodiscard]] std::vector<CallableOwnershipViolation>
analyzeCallableOwnership(SemIR &sem_ir, FixedPointAnalysisMetrics *metrics);

} // namespace chtholly::compiler
