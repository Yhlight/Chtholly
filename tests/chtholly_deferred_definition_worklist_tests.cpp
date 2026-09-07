#include "chtholly/Compiler/DeferredDefinitionWorklist.h"
#include "chtholly/Compiler/DenseWorklist.h"
#include "chtholly/Compiler/SemIR.h"

#include "test_check.h"
#include <cstdint>

namespace {

struct Definition {
  std::uint32_t id = 0;
};

using chtholly::compiler::DeferredDefinitionTaskKind;
using Worklist = chtholly::compiler::DeferredDefinitionWorklist<Definition>;
using DenseWorklist = chtholly::compiler::DenseWorklist<
    chtholly::compiler::FunctionId>;
using DenseWorklistOrder = chtholly::compiler::DenseWorklistOrder;

void tracksDenseQueueOwnershipAndMetrics() {
  DenseWorklist worklist(4);
  worklist.push(chtholly::compiler::FunctionId(2));
  worklist.push(chtholly::compiler::FunctionId(2));
  worklist.push(chtholly::compiler::FunctionId(1));
  worklist.push(chtholly::compiler::FunctionId::invalid());
  worklist.push(chtholly::compiler::FunctionId(8));
  CHTHOLLY_TEST_CHECK(worklist.pendingCount() == 2);
  CHTHOLLY_TEST_CHECK(worklist.peakPendingCount() == 2);
  CHTHOLLY_TEST_CHECK(worklist.processedCount() == 0);

  const auto first = worklist.pop();
  CHTHOLLY_TEST_CHECK(first && first->index == 2);
  CHTHOLLY_TEST_CHECK(worklist.processedCount() == 1);
  // An ID can be queued again after it has been consumed. This is the
  // fixed-point behavior used when a state change invalidates a predecessor.
  worklist.push(chtholly::compiler::FunctionId(2));
  CHTHOLLY_TEST_CHECK(worklist.pendingCount() == 2);
  const auto second = worklist.pop();
  const auto third = worklist.pop();
  CHTHOLLY_TEST_CHECK(second && second->index == 1);
  CHTHOLLY_TEST_CHECK(third && third->index == 2);
  CHTHOLLY_TEST_CHECK(worklist.pendingCount() == 0);
  CHTHOLLY_TEST_CHECK(worklist.processedCount() == 3);
}

void preservesExplicitLifoTraversalOrder() {
  DenseWorklist worklist(4, DenseWorklistOrder::LIFO);
  worklist.push(chtholly::compiler::FunctionId(1));
  worklist.push(chtholly::compiler::FunctionId(3));
  worklist.push(chtholly::compiler::FunctionId(2));
  const auto first = worklist.pop();
  const auto second = worklist.pop();
  const auto third = worklist.pop();
  CHTHOLLY_TEST_CHECK(first && first->index == 2);
  CHTHOLLY_TEST_CHECK(second && second->index == 3);
  CHTHOLLY_TEST_CHECK(third && third->index == 1);
  CHTHOLLY_TEST_CHECK(worklist.pendingCount() == 0);
  CHTHOLLY_TEST_CHECK(worklist.processedCount() == 3);
}

void preservesNestedTaskOrder() {
  Worklist worklist;
  worklist.pushEnterNestedScope();
  worklist.pushDefinition({1});
  worklist.pushEnterNestedScope();
  worklist.pushDefinition({2}, DeferredDefinitionTaskKind::DefineThunk);
  CHTHOLLY_TEST_CHECK(worklist.pushLeaveNestedScope());
  worklist.pushDefinition({3});
  CHTHOLLY_TEST_CHECK(worklist.pushLeaveNestedScope());
  CHTHOLLY_TEST_CHECK(worklist.scopesBalanced());
  CHTHOLLY_TEST_CHECK(!worklist.pushLeaveNestedScope());

  const auto entries = worklist.entries();
  CHTHOLLY_TEST_CHECK(entries.size() == 7);
  CHTHOLLY_TEST_CHECK(entries[1].definition().id == 1 && entries[1].scope_depth == 1);
  CHTHOLLY_TEST_CHECK(entries[3].definition().id == 2 && entries[3].scope_depth == 2);
  CHTHOLLY_TEST_CHECK(entries[5].definition().id == 3 && entries[5].scope_depth == 1);

  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto task = worklist.popNext();
    CHTHOLLY_TEST_CHECK(task.has_value() && task->kind == entries[index].kind);
  }
  CHTHOLLY_TEST_CHECK(!worklist.popNext().has_value());
  CHTHOLLY_TEST_CHECK(worklist.pendingCount() == 0);
  CHTHOLLY_TEST_CHECK(worklist.processedCount() == entries.size());
  CHTHOLLY_TEST_CHECK(worklist.peakPendingCount() == entries.size());
}

void drainsTasksAppendedDuringChecking() {
  Worklist worklist;
  worklist.pushDefinition({1});
  const auto first = worklist.popNext();
  CHTHOLLY_TEST_CHECK(first && first->definition().id == 1);
  worklist.pushDefinition({2});
  const auto second = worklist.popNext();
  CHTHOLLY_TEST_CHECK(second && second->definition().id == 2);
  CHTHOLLY_TEST_CHECK(!worklist.popNext());
  CHTHOLLY_TEST_CHECK(worklist.processedCount() == 2);
  CHTHOLLY_TEST_CHECK(worklist.peakPendingCount() == 1);
}

} // namespace

int main() {
  tracksDenseQueueOwnershipAndMetrics();
  preservesExplicitLifoTraversalOrder();
  preservesNestedTaskOrder();
  drainsTasksAppendedDuringChecking();
  return 0;
}
