#include "chtholly/Compiler/Outcome.h"

#include <algorithm>
#include "test_check.h"
#include <string>

namespace {

void verifiesCanonicalConstructors() {
  using namespace chtholly::compiler;
  std::string error;

  auto result = makeResultOutcome();
  CHTHOLLY_TEST_CHECK(result.verify(error));
  CHTHOLLY_TEST_CHECK(result.cardinality == OutcomeCardinality::OneShot);
  CHTHOLLY_TEST_CHECK(result.arms.size() == 2);
  CHTHOLLY_TEST_CHECK(result.fingerprint().hasValue());

  auto task = makeTaskOutcome(true);
  CHTHOLLY_TEST_CHECK(task.verify(error));
  CHTHOLLY_TEST_CHECK(task.cancellation_is_terminal);
  CHTHOLLY_TEST_CHECK(task.arms.size() == 3);
  CHTHOLLY_TEST_CHECK(task.transitions.size() == 3);

  auto channel = makeChannelOutcome();
  CHTHOLLY_TEST_CHECK(channel.verify(error));
  CHTHOLLY_TEST_CHECK(channel.cardinality == OutcomeCardinality::MultiSubmit);
  CHTHOLLY_TEST_CHECK(channel.transitions.size() == 4);

  auto completion_set = makeCompletionSetOutcome();
  CHTHOLLY_TEST_CHECK(completion_set.verify(error));
  CHTHOLLY_TEST_CHECK(completion_set.arms.size() == 3);
}

void rejectsMalformedDescriptors() {
  using namespace chtholly::compiler;
  std::string error;

  OutcomeDescriptor empty;
  CHTHOLLY_TEST_CHECK(!empty.verify(error));
  CHTHOLLY_TEST_CHECK(error == "outcome descriptor has invalid cardinality");

  auto duplicate = makeResultOutcome();
  duplicate.arms.push_back(OutcomeArmKind::Failure);
  CHTHOLLY_TEST_CHECK(!duplicate.verify(error));
  CHTHOLLY_TEST_CHECK(error == "outcome descriptor has duplicate terminal arms");

  auto token_oneshot = makeResultOutcome();
  token_oneshot.transitions.front().consumes_token = true;
  CHTHOLLY_TEST_CHECK(!token_oneshot.verify(error));
  CHTHOLLY_TEST_CHECK(error == "one-shot outcome cannot consume a multi-submit token");

  auto missing_commit = makeChannelOutcome();
  missing_commit.transitions.erase(missing_commit.transitions.begin() + 1);
  CHTHOLLY_TEST_CHECK(!missing_commit.verify(error));
  CHTHOLLY_TEST_CHECK(error == "outcome descriptor has no reachable success state");
}

void fingerprintsAreStructural() {
  using namespace chtholly::compiler;
  std::string error;
  auto first = makeResultOutcome();
  auto second = makeResultOutcome();
  CHTHOLLY_TEST_CHECK(first.fingerprint() == second.fingerprint());
  second.cancellation_is_terminal = true;
  CHTHOLLY_TEST_CHECK(first.fingerprint() != second.fingerprint());

  auto ordered = makeChannelOutcome();
  auto shuffled = ordered;
  std::swap(shuffled.arms[0], shuffled.arms[3]);
  std::reverse(shuffled.transitions.begin(), shuffled.transitions.end());
  CHTHOLLY_TEST_CHECK(ordered.fingerprint() == shuffled.fingerprint());

  auto reordered = makeChannelOutcome();
  std::reverse(reordered.transitions.begin(), reordered.transitions.end());
  CHTHOLLY_TEST_CHECK(reordered.verify(error));
}

void rejectsInconsistentMultiSubmitTokens() {
  using namespace chtholly::compiler;
  std::string error;
  auto channel = makeChannelOutcome();
  channel.transitions.back().token_lane = 1;
  CHTHOLLY_TEST_CHECK(!channel.verify(error));
  CHTHOLLY_TEST_CHECK(error == "multi-submit transitions use inconsistent token lanes");

  auto malformed = makeChannelOutcome();
  malformed.transitions[1].phase = OutcomePhase::Consume;
  CHTHOLLY_TEST_CHECK(!malformed.verify(error));
  CHTHOLLY_TEST_CHECK(error == "outcome transition phase does not match its state edge");

  auto oneshot = makeResultOutcome();
  oneshot.transitions.pop_back();
  CHTHOLLY_TEST_CHECK(!oneshot.verify(error));
  CHTHOLLY_TEST_CHECK(error == "one-shot outcome must have exactly one consume transition");
}

void derivesTransitionEffects() {
  using namespace chtholly::compiler;
  std::string error;
  auto channel = makeChannelOutcome();
  auto prepare = channel.transitions[0];
  auto commit = channel.transitions[1];
  auto cancel = channel.transitions[2];
  CHTHOLLY_TEST_CHECK(channel.transitionEffect(prepare)->source ==
         OutcomeEffect::PreserveSource);
  CHTHOLLY_TEST_CHECK(channel.transitionEffect(commit)->source == OutcomeEffect::MoveSource);
  CHTHOLLY_TEST_CHECK(channel.transitionEffect(cancel)->source ==
         OutcomeEffect::PreserveSource);
  OutcomeTransition unknown;
  CHTHOLLY_TEST_CHECK(!channel.transitionEffect(unknown));
  auto malformed = channel;
  malformed.transitions.front().token_lane = 7;
  CHTHOLLY_TEST_CHECK(!malformed.transitionEffect(malformed.transitions.front()));
  auto stale = makeResultOutcome();
  stale.semantic_epoch = 1;
  CHTHOLLY_TEST_CHECK(!stale.verify(error));
  CHTHOLLY_TEST_CHECK(error == "outcome descriptor has unsupported semantic epoch");
}

} // namespace

int main() {
  verifiesCanonicalConstructors();
  rejectsMalformedDescriptors();
  fingerprintsAreStructural();
  rejectsInconsistentMultiSubmitTokens();
  derivesTransitionEffects();
  return 0;
}
