#include "chtholly/Compiler/PlaceState.h"

#include "chtholly/Compiler/CallableOwnership.h"
#include "chtholly/Compiler/DenseWorklist.h"
#include "chtholly/Compiler/Outcome.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler {
namespace {

constexpr std::uint8_t InitializedBit = 1U;
constexpr std::uint8_t MovedBit = 2U;
constexpr std::uint8_t TaskOutstandingBit = 1U;
constexpr std::uint8_t TaskFulfilledBit = 2U;

enum class GuardValue : std::uint8_t { False, True, Unknown };

GuardValue evaluateGuardValue(const SemIR &sem_ir, InstId value) {
  const auto &inst = sem_ir.inst(value);
  if (inst.kind == SemInstKind::CallbackRegistrationBinding)
    return evaluateGuardValue(sem_ir, InstId(inst.arg1));
  if (inst.kind == SemInstKind::BoolLiteral)
    return sem_ir.integer(IntegerId(inst.arg0)) == 0 ? GuardValue::False
                                                     : GuardValue::True;
  if (inst.kind == SemInstKind::LogicalNot) {
    const auto operand = evaluateGuardValue(sem_ir, InstId(inst.arg0));
    return operand == GuardValue::True    ? GuardValue::False
           : operand == GuardValue::False ? GuardValue::True
                                          : GuardValue::Unknown;
  }
  if (inst.kind == SemInstKind::LogicalAnd) {
    const auto left = evaluateGuardValue(sem_ir, InstId(inst.arg0));
    const auto right = evaluateGuardValue(sem_ir, InstId(inst.arg1));
    if (left == GuardValue::False || right == GuardValue::False)
      return GuardValue::False;
    if (left == GuardValue::True && right == GuardValue::True)
      return GuardValue::True;
  }
  if (inst.kind == SemInstKind::LogicalOr) {
    const auto left = evaluateGuardValue(sem_ir, InstId(inst.arg0));
    const auto right = evaluateGuardValue(sem_ir, InstId(inst.arg1));
    if (left == GuardValue::True || right == GuardValue::True)
      return GuardValue::True;
    if (left == GuardValue::False && right == GuardValue::False)
      return GuardValue::False;
  }
  return GuardValue::Unknown;
}

bool guardMayMatch(const SemIR &sem_ir, const CallableReturnSource &source,
                   std::span<const InstId> arguments) {
  if (!source.condition.exact) return true;
  for (const auto &clause : source.condition.clauses) {
    bool clause_may_match = true;
    for (const auto &atom : clause.atoms) {
      if (atom.parameter_index >= arguments.size()) {
        clause_may_match = false;
        break;
      }
      if (atom.variant != core::AnyId::InvalidIndex) continue;
      const auto value =
          evaluateGuardValue(sem_ir, arguments[atom.parameter_index]);
      if ((value == GuardValue::True && !atom.expected) ||
          (value == GuardValue::False && atom.expected)) {
        clause_may_match = false;
        break;
      }
    }
    if (clause_may_match)
      return true;
  }
  return false;
}
constexpr std::uint32_t MaxTrackedElements = 4096;
constexpr std::size_t MaxOwnershipRegionPath = 256;
const PlaceCleanupPlan EmptyCleanupPlan;
const SuspensionCleanupPartition EmptySuspensionCleanupPartition;
const PlaceReinitializationPlan EmptyReinitializationPlan;

// The semantic analyzer consumes the same canonical descriptors as LowIR.
// These are immutable protocol facts; they are deliberately not source
// syntax or runtime ABI records.
const OutcomeDescriptor &resultOutcomeFacts() {
  static const OutcomeDescriptor value = makeResultOutcome();
  return value;
}

const OutcomeDescriptor &taskOutcomeFacts(bool fallible = true) {
  static const OutcomeDescriptor fallible_value = makeTaskOutcome(true);
  static const OutcomeDescriptor infallible_value = makeTaskOutcome(false);
  return fallible ? fallible_value : infallible_value;
}

const OutcomeDescriptor &channelOutcomeFacts() {
  static const OutcomeDescriptor value = makeChannelOutcome();
  return value;
}

bool isCoroutineSuspension(SemInstKind kind) {
  return kind == SemInstKind::CoroutineSuspend ||
         kind == SemInstKind::CoroutineTaskCompletionWaitAll ||
         kind == SemInstKind::CoroutineTaskCompletionSelect ||
         kind == SemInstKind::CoroutineTaskCompletionRace;
}

bool isPrefix(std::span<const PlaceProjection> prefix,
              std::span<const PlaceProjection> path) {
  if (prefix.size() > path.size())
    return false;
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    const auto lhs = prefix[index];
    const auto rhs = path[index];
    if (lhs.kind == PlaceProjectionKind::AnyElement ||
        rhs.kind == PlaceProjectionKind::AnyElement) {
      if ((lhs.kind != PlaceProjectionKind::AnyElement &&
           lhs.kind != PlaceProjectionKind::Element) ||
          (rhs.kind != PlaceProjectionKind::AnyElement &&
           rhs.kind != PlaceProjectionKind::Element))
        return false;
      continue;
    }
    if (lhs != rhs)
      return false;
  }
  return true;
}

std::string placeKey(LocalId root, std::span<const PlaceProjection> path) {
  std::string key;
  key.reserve(4 + path.size() * 9);
  const auto append_u32 = [&](std::uint32_t value) {
    for (std::uint32_t shift = 0; shift != 32; shift += 8)
      key.push_back(static_cast<char>((value >> shift) & 0xffU));
  };
  append_u32(root.index);
  for (const auto projection : path) {
    key.push_back(static_cast<char>(projection.kind));
    append_u32(projection.index);
    append_u32(projection.variant);
  }
  return key;
}

struct LeafState {
  std::vector<PlaceProjection> path;
  TypeId type;
  std::uint8_t possibilities = MovedBit;
};

struct UnionState {
  std::vector<PlaceProjection> path;
  TypeId type;
  std::vector<std::uint8_t> possible_members;
  bool unknown = false;
};

struct EnumState {
  std::vector<PlaceProjection> path;
  TypeId type;
  std::vector<std::uint8_t> possible_variants;
  bool unknown = false;
};

struct RootState {
  TypeId type;
  std::vector<LeafState> leaves;
  std::vector<UnionState> unions;
  std::vector<EnumState> enums;
  std::uint8_t task_obligation = 0;
  std::uint32_t task_scope = 0;
  bool ever_initialized = false;
  InstId uninitialized_declaration;
};

struct ActiveTaskScope {
  std::uint32_t id = 0;
  std::size_t local_depth = 0;
};

struct ResolvedPlace {
  LocalId root;
  TypeId type;
  std::vector<PlaceProjection> path;
  bool unsafe_union_access = false;
  bool borrowed_view = false;
};

#include "PlaceStateAnalyzer.inc"

} // namespace

#include "PlaceStateQuery.inc"

#include "PlaceStateAnalysisApi.inc"

} // namespace chtholly::compiler
