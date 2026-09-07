#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"
#include "chtholly/Compiler/IteratorDesugaring.h"

#include <algorithm>
#include "test_check.h"
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>

namespace {
using chtholly::compiler::buildIteratorDesugaring;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::FunctionId;
using chtholly::compiler::IteratorDesugaringEdgeKind;
using chtholly::compiler::PlaceCleanupKind;
using chtholly::compiler::SourceInput;

CompilationSession makeV13Session(std::string package) {
  auto contract = chtholly::CurrentLanguageContract;
  contract.source = chtholly::FrozenV13LanguageVersion;
  return CompilationSession(
      chtholly_test::targetTriple, std::move(package), {},
      chtholly::compiler::defaultCompileToolchainFingerprint(), {}, contract);
}

void expectSuccess(std::string_view name, std::string_view source) {
  auto session = makeV13Session(std::string("labeled-") + std::string(name));
  std::string error;
  CHTHOLLY_TEST_CHECK(session.addUnit(SourceInput(std::string(name), std::string(source)))
             .hasValue());
  if (!session.compile(error)) {
    std::cerr << "labeled loop fixture failed: " << name << "\n"
              << error << "\n";
    std::abort();
  }
}

void expectFailure(std::string_view name, std::string_view source) {
  auto session = makeV13Session(std::string("labeled-") + std::string(name));
  std::string error;
  CHTHOLLY_TEST_CHECK(session.addUnit(SourceInput(std::string(name), std::string(source)))
             .hasValue());
  if (session.compile(error)) {
    std::cerr << "labeled loop fixture unexpectedly compiled: " << name << "\n";
    std::abort();
  }
}

void expectRealSemIRGraphFacts() {
  constexpr std::string_view source = R"cns(
module labeled_graph;
fn main(): i32 {
  outer: while (true) {
    inner: for (; false; ) {
      defer { let marker = 1; }
      continue outer;
    }
    break outer;
  }
  return 0;
}
)cns";
  auto session = makeV13Session("labeled-graph");
  std::string error;
  const auto unit =
      session.addUnit(SourceInput("labeled-graph.cns", std::string(source)));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  if (!session.compile(error)) {
    std::cerr << "real SemIR graph fixture failed:\n" << error << "\n";
    std::abort();
  }
  const auto *sem_ir = session.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  FunctionId main_function = FunctionId::invalid();
  for (std::uint32_t index = 0; index < sem_ir->functionCount(); ++index) {
    const auto function = FunctionId(index);
    if (sem_ir->identifier(
            sem_ir->name(sem_ir->function(function).name).text) == "main") {
      main_function = function;
      break;
    }
  }
  CHTHOLLY_TEST_CHECK(main_function.hasValue());
  const auto graph = buildIteratorDesugaring(*sem_ir, main_function);
  bool saw_while = false;
  bool saw_for = false;
  bool saw_continue = false;
  bool saw_break = false;
  bool saw_labeled_continue = false;
  for (const auto &fact : graph.semir_facts) {
    saw_while |= fact.kind == chtholly::compiler::SemInstKind::While;
    saw_for |= fact.kind == chtholly::compiler::SemInstKind::For;
    if (fact.kind == chtholly::compiler::SemInstKind::Continue) {
      saw_continue = true;
      saw_labeled_continue |= fact.loop_distance == 1;
    }
    if (fact.kind == chtholly::compiler::SemInstKind::Break)
      saw_break = true;
  }
  CHTHOLLY_TEST_CHECK(saw_while && saw_for && saw_continue && saw_break);
  CHTHOLLY_TEST_CHECK(saw_labeled_continue);
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(graph.semir_edges, [](const auto &edge) {
    return edge.kind == IteratorDesugaringEdgeKind::Continue &&
           edge.loop_distance == 1;
  }));
}

void expectNonTrivialDropFacts() {
  constexpr std::string_view source = R"cns(
module drop_graph;
lifecycle(copy = delete, move = default, drop = custom)
struct Tracker { pub value: i32; }
impl Tracker {
  fn drop(self: Tracker&): void { return; }
}
fn main(): i32 {
  var value = Tracker { .value = 1 };
  while (true) {
    defer { let marker = 1; }
    break;
  }
  return value.value;
}
)cns";
  auto session = makeV13Session("drop-graph");
  std::string error;
  const auto unit =
      session.addUnit(SourceInput("drop-graph.cns", std::string(source)));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  if (!session.compile(error)) {
    std::cerr << "non-trivial drop graph fixture failed:\n" << error << "\n";
    std::abort();
  }
  const auto *sem_ir = session.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  FunctionId main_function = FunctionId::invalid();
  for (std::uint32_t index = 0; index < sem_ir->functionCount(); ++index) {
    const auto function = FunctionId(index);
    if (sem_ir->identifier(
            sem_ir->name(sem_ir->function(function).name).text) == "main") {
      main_function = function;
      break;
    }
  }
  CHTHOLLY_TEST_CHECK(main_function.hasValue());
  const auto graph = buildIteratorDesugaring(*sem_ir, main_function);
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(graph.semir_facts, [](const auto &fact) {
    if (fact.kind != chtholly::compiler::SemInstKind::Return)
      return false;
    return std::ranges::find(fact.cleanup_kinds, PlaceCleanupKind::Destroy) !=
               fact.cleanup_kinds.end() ||
           std::ranges::find(fact.cleanup_kinds,
                             PlaceCleanupKind::DestroyIfInitialized) !=
               fact.cleanup_kinds.end();
  }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(graph.semir_edges, [](const auto &edge) {
    return edge.kind == IteratorDesugaringEdgeKind::Break &&
           std::ranges::find(edge.cleanup_kinds, PlaceCleanupKind::RunDefer) !=
               edge.cleanup_kinds.end();
  }));
}
} // namespace

int main() {
  expectSuccess("nested-break-continue", R"cns(
module labeled_nested;
fn main(): i32 {
  let flag = false;
  outer: while (true) {
    inner: for (; flag; ) {
      continue outer;
    }
    break outer;
  }
  return 0;
}
)cns");

  expectSuccess("outer-break-cleanup", R"cns(
module labeled_outer_break;
fn main(): i32 {
  let flag = false;
  outer: while (true) {
    inner: while (flag) {
      defer { let marker = 1; }
      break outer;
    }
    break outer;
  }
  return 0;
}
)cns");

  expectSuccess("do-while-label", R"cns(
module labeled_do;
fn main(): i32 {
  outer: do { continue outer; } while (true);
  return 0;
}
)cns");

  expectSuccess("defer-cleanup", R"cns(
module labeled_defer;
fn main(): i32 {
  outer: while (false) {
    defer { let marker = 1; }
    break outer;
  }
  return 0;
}
)cns");

  expectSuccess("defer-local-loop", R"cns(
module labeled_defer_local;
fn main(): i32 {
  defer { local: while (false) { break local; } }
  return 0;
}
)cns");

  expectFailure("unknown-label", R"cns(
module labeled_unknown;
fn main(): i32 { while (true) { break missing; } return 0; }
)cns");
  expectFailure("duplicate-label", R"cns(
module labeled_duplicate;
fn main(): i32 {
  outer: while (true) { outer: while (true) { break outer; } }
  return 0;
}
)cns");
  expectFailure("label-outside-loop", R"cns(
module labeled_invalid;
fn main(): i32 { bad: if (true) { return 0; } return 0; }
)cns");
  expectFailure("defer-escape", R"cns(
module labeled_escape;
fn main(): i32 {
  outer: while (true) { defer { break outer; } }
  return 0;
}
)cns");
  expectRealSemIRGraphFacts();
  expectNonTrivialDropFacts();
  return 0;
}
