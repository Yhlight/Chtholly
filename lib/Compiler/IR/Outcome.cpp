#include "chtholly/Compiler/Outcome.h"

#include "chtholly/Support/Digest.h"

#include <algorithm>

namespace chtholly::compiler {
namespace {

template <typename T> void appendU32(std::string &out, T value) {
  const auto number = static_cast<std::uint32_t>(value);
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((number >> shift) & 0xffU));
}

void appendFingerprint(std::string &out, const StableFingerprint &value) {
  out.append(reinterpret_cast<const char *>(value.bytes().data()),
             value.bytes().size());
}

bool validArm(OutcomeArmKind value) {
  return value < OutcomeArmKind::Count;
}

bool validState(OutcomeState value) {
  return value < OutcomeState::Count;
}

bool validPhase(OutcomePhase value) {
  return value < OutcomePhase::Count;
}

void addTransition(OutcomeDescriptor &descriptor, OutcomeState from,
                   OutcomeState to, OutcomePhase phase, bool consumes = false,
                   std::uint32_t token_lane = core::AnyId::InvalidIndex) {
  descriptor.transitions.push_back(
      {from, to, phase, consumes, token_lane});
}

} // namespace

std::string_view outcomeCardinalityName(OutcomeCardinality value) {
  switch (value) {
  case OutcomeCardinality::OneShot:
    return "one-shot";
  case OutcomeCardinality::MultiSubmit:
    return "multi-submit";
  case OutcomeCardinality::Count:
    return "invalid";
  }
  return "invalid";
}

std::string_view outcomeArmName(OutcomeArmKind value) {
  switch (value) {
  case OutcomeArmKind::Success:
    return "success";
  case OutcomeArmKind::Failure:
    return "failure";
  case OutcomeArmKind::Cancelled:
    return "cancelled";
  case OutcomeArmKind::Data:
    return "data";
  case OutcomeArmKind::Eof:
    return "eof";
  case OutcomeArmKind::Closed:
    return "closed";
  case OutcomeArmKind::Count:
    return "invalid";
  }
  return "invalid";
}

std::string_view outcomePhaseName(OutcomePhase value) {
  switch (value) {
  case OutcomePhase::Prepare:
    return "prepare";
  case OutcomePhase::Invoke:
    return "invoke";
  case OutcomePhase::Classify:
    return "classify";
  case OutcomePhase::Publish:
    return "publish";
  case OutcomePhase::Commit:
    return "commit";
  case OutcomePhase::Cancel:
    return "cancel";
  case OutcomePhase::Consume:
    return "consume";
  case OutcomePhase::Count:
    return "invalid";
  }
  return "invalid";
}

std::string_view outcomeStateName(OutcomeState value) {
  switch (value) {
  case OutcomeState::Pending:
    return "pending";
  case OutcomeState::Prepared:
    return "prepared";
  case OutcomeState::Ready:
    return "ready";
  case OutcomeState::Committed:
    return "committed";
  case OutcomeState::Cancelled:
    return "cancelled";
  case OutcomeState::Failed:
    return "failed";
  case OutcomeState::Consumed:
    return "consumed";
  case OutcomeState::Count:
    return "invalid";
  }
  return "invalid";
}

bool OutcomeDescriptor::verify(std::string &error) const {
  if (cardinality >= OutcomeCardinality::Count) {
    error = "outcome descriptor has invalid cardinality";
    return false;
  }
  if (semantic_epoch != CurrentSemanticEpoch) {
    error = "outcome descriptor has unsupported semantic epoch";
    return false;
  }
  if (arms.empty()) {
    error = "outcome descriptor has no terminal arms";
    return false;
  }
  std::vector<OutcomeArmKind> unique_arms = arms;
  if (std::ranges::any_of(unique_arms,
                          [](auto value) { return !validArm(value); })) {
    error = "outcome descriptor has an invalid arm";
    return false;
  }
  std::ranges::sort(unique_arms);
  if (std::ranges::adjacent_find(unique_arms) != unique_arms.end()) {
    error = "outcome descriptor has duplicate terminal arms";
    return false;
  }
  const auto has_success =
      std::ranges::find(arms, OutcomeArmKind::Success) != arms.end();
  const auto has_data =
      std::ranges::find(arms, OutcomeArmKind::Data) != arms.end();
  const auto has_eof =
      std::ranges::find(arms, OutcomeArmKind::Eof) != arms.end();
  if (!has_success && !has_data && !has_eof) {
    error = "outcome descriptor has no successful terminal arm";
    return false;
  }
  std::vector<std::uint32_t> lanes;
  lanes.reserve(payloads.size());
  for (const auto &payload : payloads) {
    if (payload.lane == core::AnyId::InvalidIndex) {
      error = "outcome payload has invalid lane";
      return false;
    }
    if (!payload.type_fingerprint.hasValue()) {
      error = "outcome payload has no stable type fingerprint";
      return false;
    }
    lanes.push_back(payload.lane);
  }
  std::ranges::sort(lanes);
  if (std::ranges::adjacent_find(lanes) != lanes.end()) {
    error = "outcome descriptor has duplicate payload lanes";
    return false;
  }
  for (const auto &transition : transitions) {
    if (!validState(transition.from) || !validState(transition.to) ||
        !validPhase(transition.phase)) {
      error = "outcome descriptor has an invalid state transition";
      return false;
    }
    if (cardinality == OutcomeCardinality::OneShot &&
        transition.consumes_token) {
      error = "one-shot outcome cannot consume a multi-submit token";
      return false;
    }
    if (cardinality == OutcomeCardinality::MultiSubmit &&
        transition.consumes_token &&
        transition.token_lane == core::AnyId::InvalidIndex) {
      error = "multi-submit transition has no token lane";
      return false;
    }
    if (transition.from == OutcomeState::Consumed ||
        transition.from == OutcomeState::Cancelled ||
        transition.from == OutcomeState::Failed) {
      error = "outcome terminal state has an outgoing transition";
      return false;
    }
    const auto expected_edge = [&](OutcomePhase phase, OutcomeState from,
                                   OutcomeState to) {
      return transition.phase != phase ||
             (transition.from == from && transition.to == to);
    };
    if (!expected_edge(OutcomePhase::Prepare, OutcomeState::Pending,
                       OutcomeState::Prepared) ||
        !expected_edge(OutcomePhase::Commit, OutcomeState::Prepared,
                       OutcomeState::Committed) ||
        !expected_edge(OutcomePhase::Consume,
                       cardinality == OutcomeCardinality::MultiSubmit
                           ? OutcomeState::Committed
                           : OutcomeState::Ready,
                       OutcomeState::Consumed)) {
      error = "outcome transition phase does not match its state edge";
      return false;
    }
  }
  std::vector<OutcomeTransition> unique_transitions = transitions;
  std::ranges::sort(unique_transitions, [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.from, lhs.to, lhs.phase, lhs.consumes_token,
                    lhs.token_lane) <
           std::tie(rhs.from, rhs.to, rhs.phase, rhs.consumes_token,
                    rhs.token_lane);
  });
  if (std::ranges::adjacent_find(unique_transitions) !=
      unique_transitions.end()) {
    error = "outcome descriptor has duplicate state transitions";
    return false;
  }
  std::vector<OutcomeState> reachable{OutcomeState::Pending};
  for (std::size_t cursor = 0; cursor < reachable.size(); ++cursor)
    for (const auto &transition : transitions)
      if (transition.from == reachable[cursor] &&
          std::ranges::find(reachable, transition.to) == reachable.end())
        reachable.push_back(transition.to);
  for (const auto arm : arms) {
    const bool terminal = arm == OutcomeArmKind::Cancelled
                              ? cancellation_is_terminal
                              : true;
    if (!terminal)
      continue;
    const auto success_state = cardinality == OutcomeCardinality::MultiSubmit
                                   ? OutcomeState::Committed
                                   : OutcomeState::Ready;
    if (std::ranges::find(reachable, success_state) == reachable.end() &&
        arm != OutcomeArmKind::Cancelled) {
      error = "outcome descriptor has no reachable success state";
      return false;
    }
  }
  if (cardinality == OutcomeCardinality::MultiSubmit) {
    std::optional<std::uint32_t> token_lane;
    for (const auto &transition : transitions) {
      if (!transition.consumes_token)
        continue;
      if (!token_lane)
        token_lane = transition.token_lane;
      else if (*token_lane != transition.token_lane) {
        error = "multi-submit transitions use inconsistent token lanes";
        return false;
      }
    }
    const auto has_prepare = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Pending &&
                 t.to == OutcomeState::Prepared &&
                 t.phase == OutcomePhase::Prepare && t.consumes_token;
        });
    const auto has_commit = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Prepared &&
                 t.to == OutcomeState::Committed;
        });
    if (!has_prepare || !has_commit) {
      error = "multi-submit outcome lacks prepare/commit transitions";
      return false;
    }
    const auto has_cancel = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Prepared &&
                 t.to == OutcomeState::Cancelled &&
                 t.phase == OutcomePhase::Cancel;
        });
    const auto has_consume = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Committed &&
                 t.to == OutcomeState::Consumed &&
                 t.phase == OutcomePhase::Consume;
        });
    if (!has_cancel || !has_consume) {
      error = "multi-submit outcome lacks cancel/consume transitions";
      return false;
    }
    const auto has_commit_token = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Prepared &&
                 t.to == OutcomeState::Committed && t.phase == OutcomePhase::Commit &&
                 t.consumes_token;
        });
    const auto has_cancel_token = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Prepared &&
                 t.to == OutcomeState::Cancelled && t.phase == OutcomePhase::Cancel &&
                 t.consumes_token;
        });
    const auto has_consume_token = std::ranges::any_of(
        transitions, [](const auto &t) {
          return t.from == OutcomeState::Committed &&
                 t.to == OutcomeState::Consumed && t.phase == OutcomePhase::Consume &&
                 t.consumes_token;
        });
    if (!has_commit_token || !has_cancel_token || !has_consume_token) {
      error = "multi-submit terminal transitions must consume the token";
      return false;
    }
  } else {
    const auto consume_count = std::ranges::count_if(
        transitions, [](const auto &t) {
          return t.phase == OutcomePhase::Consume &&
                 t.from == OutcomeState::Ready &&
                 t.to == OutcomeState::Consumed;
        });
    if (consume_count != 1) {
      error = "one-shot outcome must have exactly one consume transition";
      return false;
    }
  }
  return true;
}

StableFingerprint OutcomeDescriptor::fingerprint() const {
  std::string canonical;
  canonical.append("chtholly.next.outcome.v2\n");
  canonical.push_back(static_cast<char>(cardinality));
  appendU32(canonical, semantic_epoch);
  canonical.push_back(retains_owner_until_commit ? 1 : 0);
  canonical.push_back(cancellation_is_terminal ? 1 : 0);
  auto canonical_arms = arms;
  std::ranges::sort(canonical_arms);
  appendU32(canonical, static_cast<std::uint32_t>(canonical_arms.size()));
  for (const auto arm : canonical_arms)
    canonical.push_back(static_cast<char>(arm));
  auto canonical_payloads = payloads;
  std::ranges::sort(canonical_payloads, [](const auto &lhs, const auto &rhs) {
    if (lhs.lane != rhs.lane)
      return lhs.lane < rhs.lane;
    if (lhs.type_fingerprint != rhs.type_fingerprint)
      return std::lexicographical_compare(
          lhs.type_fingerprint.bytes().begin(), lhs.type_fingerprint.bytes().end(),
          rhs.type_fingerprint.bytes().begin(), rhs.type_fingerprint.bytes().end());
    return std::tie(lhs.owned, lhs.initializes, lhs.invalidates) <
           std::tie(rhs.owned, rhs.initializes, rhs.invalidates);
  });
  appendU32(canonical, static_cast<std::uint32_t>(canonical_payloads.size()));
  for (const auto &payload : canonical_payloads) {
    appendU32(canonical, payload.lane);
    appendFingerprint(canonical, payload.type_fingerprint);
    canonical.push_back(payload.owned ? 1 : 0);
    canonical.push_back(payload.initializes ? 1 : 0);
    canonical.push_back(payload.invalidates ? 1 : 0);
  }
  auto canonical_transitions = transitions;
  std::ranges::sort(canonical_transitions, [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.from, lhs.to, lhs.phase, lhs.consumes_token,
                    lhs.token_lane) <
           std::tie(rhs.from, rhs.to, rhs.phase, rhs.consumes_token,
                    rhs.token_lane);
  });
  appendU32(canonical, static_cast<std::uint32_t>(canonical_transitions.size()));
  for (const auto &transition : canonical_transitions) {
    canonical.push_back(static_cast<char>(transition.from));
    canonical.push_back(static_cast<char>(transition.to));
    canonical.push_back(static_cast<char>(transition.phase));
    canonical.push_back(transition.consumes_token ? 1 : 0);
    appendU32(canonical, transition.token_lane);
  }
  return StableFingerprint::fromCanonicalBytes(canonical);
}

std::optional<OutcomeTransitionEffect>
OutcomeDescriptor::transitionEffect(const OutcomeTransition &transition) const {
  std::string error;
  if (!verify(error))
    return std::nullopt;
  if (std::ranges::find(transitions, transition) == transitions.end())
    return std::nullopt;
  OutcomeTransitionEffect effect;
  effect.consumes_token = transition.consumes_token;
  if (transition.phase == OutcomePhase::Prepare ||
      transition.phase == OutcomePhase::Invoke ||
      transition.phase == OutcomePhase::Classify ||
      transition.phase == OutcomePhase::Publish)
    effect.source = OutcomeEffect::PreserveSource;
  if (transition.phase == OutcomePhase::Commit)
    effect.source = OutcomeEffect::MoveSource;
  if (transition.phase == OutcomePhase::Cancel)
    effect.source = OutcomeEffect::PreserveSource;
  if (transition.phase == OutcomePhase::Consume)
    effect.source = OutcomeEffect::ConsumePayload;
  return effect;
}

std::optional<OutcomeTransitionEffect>
OutcomeDescriptor::transitionEffect(OutcomeState from, OutcomeState to,
                                     OutcomePhase phase) const {
  const auto found = std::ranges::find_if(
      transitions, [&](const auto &candidate) {
        return candidate.from == from && candidate.to == to &&
               candidate.phase == phase;
      });
  return found == transitions.end() ? std::nullopt
                                    : transitionEffect(*found);
}

OutcomeDescriptor makeResultOutcome() {
  OutcomeDescriptor result;
  result.cardinality = OutcomeCardinality::OneShot;
  result.arms = {OutcomeArmKind::Success, OutcomeArmKind::Failure};
  result.cancellation_is_terminal = false;
  addTransition(result, OutcomeState::Pending, OutcomeState::Ready,
                OutcomePhase::Publish);
  addTransition(result, OutcomeState::Ready, OutcomeState::Consumed,
                OutcomePhase::Consume);
  return result;
}

OutcomeDescriptor makeTaskOutcome(bool fallible) {
  OutcomeDescriptor result = makeResultOutcome();
  if (fallible)
    result.arms.push_back(OutcomeArmKind::Cancelled);
  else
    result.arms = {OutcomeArmKind::Success, OutcomeArmKind::Cancelled};
  result.cancellation_is_terminal = true;
  addTransition(result, OutcomeState::Pending, OutcomeState::Cancelled,
                OutcomePhase::Cancel);
  return result;
}

OutcomeDescriptor makeChannelOutcome() {
  OutcomeDescriptor result;
  result.cardinality = OutcomeCardinality::MultiSubmit;
  result.arms = {OutcomeArmKind::Success, OutcomeArmKind::Failure,
                 OutcomeArmKind::Closed, OutcomeArmKind::Cancelled};
  result.cancellation_is_terminal = true;
  addTransition(result, OutcomeState::Pending, OutcomeState::Prepared,
                OutcomePhase::Prepare, true, 0);
  addTransition(result, OutcomeState::Prepared, OutcomeState::Committed,
                OutcomePhase::Commit, true, 0);
  addTransition(result, OutcomeState::Prepared, OutcomeState::Cancelled,
                OutcomePhase::Cancel, true, 0);
  addTransition(result, OutcomeState::Committed, OutcomeState::Consumed,
                OutcomePhase::Consume, true, 0);
  return result;
}

OutcomeDescriptor makeCompletionSetOutcome() {
  OutcomeDescriptor result;
  result.cardinality = OutcomeCardinality::MultiSubmit;
  result.arms = {OutcomeArmKind::Success, OutcomeArmKind::Failure,
                 OutcomeArmKind::Cancelled};
  result.cancellation_is_terminal = true;
  addTransition(result, OutcomeState::Pending, OutcomeState::Prepared,
                OutcomePhase::Prepare, true, 0);
  addTransition(result, OutcomeState::Prepared, OutcomeState::Committed,
                OutcomePhase::Commit, true, 0);
  addTransition(result, OutcomeState::Prepared, OutcomeState::Cancelled,
                OutcomePhase::Cancel, true, 0);
  addTransition(result, OutcomeState::Committed, OutcomeState::Consumed,
                OutcomePhase::Consume, true, 0);
  return result;
}

OutcomeDescriptor makeForeignReadOutcome(bool has_eof) {
  OutcomeDescriptor result = makeResultOutcome();
  result.arms = {OutcomeArmKind::Data, OutcomeArmKind::Failure};
  if (has_eof)
    result.arms.push_back(OutcomeArmKind::Eof);
  return result;
}

StableFingerprint canonicalTypedChannelOutcomeFingerprint() {
  static const StableFingerprint value = makeChannelOutcome().fingerprint();
  return value;
}

} // namespace chtholly::compiler
