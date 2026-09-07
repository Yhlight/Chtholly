#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

// A compiler-owned description of a value or protocol outcome.  This is
// deliberately independent of the source-level Result enum and of any one
// runtime ABI.  It is the common vocabulary used by Result, task, channel,
// callback, and CFDL completion lowering.
enum class OutcomeCardinality : std::uint8_t {
  OneShot,
  MultiSubmit,
  Count,
};

enum class OutcomeArmKind : std::uint8_t {
  Success,
  Failure,
  Cancelled,
  Data,
  Eof,
  Closed,
  Count,
};

enum class OutcomePhase : std::uint8_t {
  Prepare,
  Invoke,
  Classify,
  Publish,
  Commit,
  Cancel,
  Consume,
  Count,
};

enum class OutcomeState : std::uint8_t {
  Pending,
  Prepared,
  Ready,
  Committed,
  Cancelled,
  Failed,
  Consumed,
  Count,
};

struct OutcomePayload {
  std::uint32_t lane = core::AnyId::InvalidIndex;
  StableFingerprint type_fingerprint;
  bool owned = false;
  bool initializes = false;
  bool invalidates = false;

  friend bool operator==(const OutcomePayload &, const OutcomePayload &) =
      default;
};

struct OutcomeTransition {
  OutcomeState from = OutcomeState::Count;
  OutcomeState to = OutcomeState::Count;
  OutcomePhase phase = OutcomePhase::Count;
  bool consumes_token = false;
  // Multi-submit transitions are tied to one logical token lane. The lane is
  // a compiler-owned identity (never a runtime address); keeping it on the
  // transition lets verification reject a commit/cancel pair that refers to
  // different tokens while preserving the existing runtime ABI.
  std::uint32_t token_lane = core::AnyId::InvalidIndex;

  friend bool operator==(const OutcomeTransition &, const OutcomeTransition &) =
      default;
};

// The ownership consequence of a verified transition.  This is deliberately
// a small compiler-owned vocabulary: source syntax and runtime ABIs continue
// to use their existing representations.
enum class OutcomeEffect : std::uint8_t {
  None,
  PreserveSource,
  MoveSource,
  InitializeDestination,
  ConsumePayload,
};

struct OutcomeTransitionEffect {
  OutcomeEffect source = OutcomeEffect::None;
  OutcomeEffect destination = OutcomeEffect::None;
  bool consumes_token = false;
};

[[nodiscard]] constexpr OutcomeEffect outcomeSourceEffect(OutcomePhase phase) {
  switch (phase) {
  case OutcomePhase::Commit:
    return OutcomeEffect::MoveSource;
  case OutcomePhase::Consume:
    return OutcomeEffect::ConsumePayload;
  case OutcomePhase::Prepare:
  case OutcomePhase::Invoke:
  case OutcomePhase::Classify:
  case OutcomePhase::Publish:
  case OutcomePhase::Cancel:
    return OutcomeEffect::PreserveSource;
  case OutcomePhase::Count:
    return OutcomeEffect::None;
  }
  return OutcomeEffect::None;
}

struct OutcomeDescriptor {
  // Token identity and terminal transition validation are part of the
  // descriptor contract; old epoch-1 records must not be replayed as if they
  // had those facts.
  static constexpr std::uint32_t CurrentSemanticEpoch = 2;

  OutcomeCardinality cardinality = OutcomeCardinality::Count;
  std::vector<OutcomeArmKind> arms;
  std::vector<OutcomePayload> payloads;
  std::vector<OutcomeTransition> transitions;
  bool retains_owner_until_commit = true;
  bool cancellation_is_terminal = false;
  std::uint32_t semantic_epoch = CurrentSemanticEpoch;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] StableFingerprint fingerprint() const;

  // Derive the ownership effect of a transition from the canonical outcome
  // facts. Callers must pass a transition that exists in this descriptor.
  [[nodiscard]] std::optional<OutcomeTransitionEffect>
  transitionEffect(const OutcomeTransition &transition) const;
  // Query a transition by its semantic edge rather than by a session-local
  // token spelling. This is the preferred API for ownership analyses.
  [[nodiscard]] std::optional<OutcomeTransitionEffect>
  transitionEffect(OutcomeState from, OutcomeState to,
                   OutcomePhase phase) const;

  friend bool operator==(const OutcomeDescriptor &, const OutcomeDescriptor &) =
      default;
};

[[nodiscard]] std::string_view outcomeCardinalityName(OutcomeCardinality value);
[[nodiscard]] std::string_view outcomeArmName(OutcomeArmKind value);
[[nodiscard]] std::string_view outcomePhaseName(OutcomePhase value);
[[nodiscard]] std::string_view outcomeStateName(OutcomeState value);

// Canonical constructors used by adapters at existing semantic boundaries.
[[nodiscard]] OutcomeDescriptor makeResultOutcome();
[[nodiscard]] OutcomeDescriptor makeTaskOutcome(bool fallible);
[[nodiscard]] OutcomeDescriptor makeChannelOutcome();
[[nodiscard]] OutcomeDescriptor makeCompletionSetOutcome();
[[nodiscard]] OutcomeDescriptor makeForeignReadOutcome(bool has_eof);

// Stable protocol identity used by persisted typed-channel specializations.
// The value includes only compiler-owned facts; session-local handles and
// runtime callback addresses are intentionally excluded.
[[nodiscard]] StableFingerprint canonicalTypedChannelOutcomeFingerprint();

} // namespace chtholly::compiler
