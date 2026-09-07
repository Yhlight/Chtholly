#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/CallableOwnership.h"
#include "test_check.h"
#include <string>

int main() {
  using namespace chtholly::compiler;
  auto condition = CallableConditionDescriptor::always();
  for (unsigned i = 0; i < 9; ++i)
    condition = conditionAnd(condition, CallableConditionDescriptor::atom(i, true));
  CHTHOLLY_TEST_CHECK(!condition.exact && !condition.isAlways() && !condition.isNever());
  auto inverse = conditionNot(condition);
  CHTHOLLY_TEST_CHECK(!inverse.exact && !inverse.isNever());
  auto impossible = conditionAnd(CallableConditionDescriptor::enumVariant(0, 0),
                                 CallableConditionDescriptor::enumVariant(0, 1));
  CHTHOLLY_TEST_CHECK(impossible.isNever());
  CallableOwnershipSummary limited;
  limited.postconditions.push_back({OwnershipRegion{.parameter_index = 0}, CallableOutcomeInitialize,
                                    CallableConditionDescriptor::atom(1, true)});
  CallableOwnershipSummary required;
  required.postconditions.push_back({OwnershipRegion{.parameter_index = 0}, CallableOutcomeInitialize});
  CHTHOLLY_TEST_CHECK(!callableOwnershipSubstitutes(limited, required));
  CHTHOLLY_TEST_CHECK(callableOwnershipSubstitutes(required, limited));
  limited.postconditions[0].condition = condition;
  limited.canonicalize();
  CHTHOLLY_TEST_CHECK(limited.postconditions[0].outcomes == CallableOutcomeAll);
  CHTHOLLY_TEST_CHECK(!callableOwnershipSubstitutes(limited, required));
  std::string error;
  CHTHOLLY_TEST_CHECK(limited.verify(10, error));
}
