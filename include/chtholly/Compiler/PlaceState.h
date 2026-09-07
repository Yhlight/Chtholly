#pragma once

#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include <vector>

namespace chtholly::compiler {

enum class PlaceStateDiagnosticKind : std::uint8_t {
  InvalidMoveOperand,
  InvalidCopyOperand,
  InvalidAssignmentTarget,
  AssignmentToImmutablePlace,
  CopyUnavailable,
  MoveUnavailable,
  UseAfterMove,
  UseOfMaybeMovedPlace,
  UseOfPartiallyMovedPlace,
  BorrowConflict,
  InactiveUnionMember,
  UnknownActiveUnionMember,
  TaskNotConsumed,
  TaskAlreadyConsumed,
  TaskScopeEscape,
  UninitializedStorage,
  AnalysisNonConvergent,
};

struct PlaceStateDiagnostic {
  InstId instruction;
  PlaceStateDiagnosticKind kind = PlaceStateDiagnosticKind::InvalidMoveOperand;
  std::vector<OwnershipEvidence> evidence;
};

// Builds the immutable function-level place-state and cleanup-path query.
// Generic and cached specific bodies are analyzed after materialization so the
// result never depends on artifact-local instruction identities.
[[nodiscard]] std::vector<PlaceStateDiagnostic>
analyzePlaceStates(SemIR &sem_ir);
[[nodiscard]] std::vector<PlaceStateDiagnostic>
analyzePlaceStates(SemIR &sem_ir, PlaceStateAnalysisMetrics *metrics);

// Consumes converged callable summaries to propagate returned loans and check
// interprocedural effects against loans that are live across each call.
[[nodiscard]] std::vector<PlaceStateDiagnostic>
analyzeInterproceduralLoans(const SemIR &sem_ir);
[[nodiscard]] std::vector<PlaceStateDiagnostic>
analyzeInterproceduralLoans(const SemIR &sem_ir,
                            PlaceStateAnalysisMetrics *metrics);

} // namespace chtholly::compiler
