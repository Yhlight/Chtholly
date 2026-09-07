#include "SemanticCallResolution.h"
#include "SemanticControlFlow.h"
#include "SemanticLiteral.h"
#include "SemanticNameScopes.h"
#include "SemanticWitnessResolution.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"
#include "chtholly/Compiler/LowIR.h"
#include "chtholly/Compiler/TypeConcurrency.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"

#include "test_check.h"
#include <cmath>
#include <iostream>
#include <string>

namespace {

using chtholly::compiler::CanonicalType;
using chtholly::compiler::CanonicalTypeKind;
using chtholly::compiler::NominalTypeSpecificArtifact;
using chtholly::compiler::PublicEntityKind;
using chtholly::compiler::PublicEntityReferenceArtifact;
using chtholly::compiler::PublicNominalFieldArtifact;
using chtholly::compiler::PublicType;
using chtholly::compiler::TypeConcurrencySummary;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::FunctionId;
using chtholly::compiler::GenericValueStores;
using chtholly::compiler::NameId;
using chtholly::compiler::SourceInput;
using chtholly::compiler::semantics_internal::blockFallsThrough;
using chtholly::compiler::semantics_internal::NumericSuffix;
using chtholly::compiler::semantics_internal::parseFloatLiteral;
using chtholly::compiler::semantics_internal::parseIntegerLiteral;
using chtholly::compiler::semantics_internal::selectCallCandidate;
using chtholly::compiler::semantics_internal::selectSemanticWitness;
using chtholly::compiler::semantics_internal::SemanticBinding;
using chtholly::compiler::semantics_internal::SemanticBindingKind;
using chtholly::compiler::semantics_internal::SemanticCallCandidateRank;
using chtholly::compiler::semantics_internal::SemanticCallSelectionKind;
using chtholly::compiler::semantics_internal::SemanticNameScopes;
using chtholly::compiler::semantics_internal::SemanticObligationState;
using chtholly::compiler::semantics_internal::SemanticObligationWorklist;
using chtholly::compiler::semantics_internal::SemanticObligationWorklistResult;
using chtholly::compiler::semantics_internal::SemanticWitnessCandidate;
using chtholly::compiler::semantics_internal::SemanticWitnessLookupKind;

void testLiterals() {
  const auto hexadecimal = parseIntegerLiteral("0xff_u32");
  CHTHOLLY_TEST_CHECK(hexadecimal && hexadecimal->magnitude == 255 &&
         hexadecimal->suffix == NumericSuffix::U32);
  const auto binary = parseIntegerLiteral("0b1010usize");
  CHTHOLLY_TEST_CHECK(binary && binary->magnitude == 10 &&
         binary->suffix == NumericSuffix::USize);
  CHTHOLLY_TEST_CHECK(!parseIntegerLiteral("0xg1"));

  NumericSuffix suffix = NumericSuffix::None;
  const auto floating = parseFloatLiteral("1_2.5f32", suffix);
  CHTHOLLY_TEST_CHECK(floating && std::abs(*floating - 12.5) < 0.001 &&
         suffix == NumericSuffix::F32);
  CHTHOLLY_TEST_CHECK(!parseFloatLiteral("1.0u32", suffix));
}

void testTypedChannelIntrinsicRoles() {
  using chtholly::compiler::CompilerIntrinsicRole;
  using chtholly::compiler::compilerIntrinsicParameterCount;
  using chtholly::compiler::parseCompilerIntrinsicRole;
  const auto make = parseCompilerIntrinsicRole("channel.make");
  const auto send = parseCompilerIntrinsicRole("channel.send-prepare");
  const auto receive = parseCompilerIntrinsicRole("channel.receive-commit");
  const auto init = parseCompilerIntrinsicRole("channel.init");
  const auto send_public = parseCompilerIntrinsicRole("channel.send");
  const auto receive_public = parseCompilerIntrinsicRole("channel.receive");
  const auto drop_public = parseCompilerIntrinsicRole("channel.drop");
  CHTHOLLY_TEST_CHECK(make && *make == CompilerIntrinsicRole::ChannelMake);
  CHTHOLLY_TEST_CHECK(send && *send == CompilerIntrinsicRole::ChannelSendPrepare);
  CHTHOLLY_TEST_CHECK(receive && *receive == CompilerIntrinsicRole::ChannelReceiveCommit);
  CHTHOLLY_TEST_CHECK(init && *init == CompilerIntrinsicRole::ChannelInit);
  CHTHOLLY_TEST_CHECK(send_public && *send_public == CompilerIntrinsicRole::ChannelSend);
  CHTHOLLY_TEST_CHECK(receive_public &&
         *receive_public == CompilerIntrinsicRole::ChannelReceive);
  CHTHOLLY_TEST_CHECK(drop_public && *drop_public == CompilerIntrinsicRole::ChannelDrop);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*make) == 2);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*send) == 2);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*receive) == 2);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*init) == 2);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*send_public) == 2);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*receive_public) == 1);
  CHTHOLLY_TEST_CHECK(compilerIntrinsicParameterCount(*drop_public) == 1);
}

void testNameScopes() {
  SemanticNameScopes scopes;
  scopes.push();
  const auto outer = NameId(1);
  const auto inner = NameId(2);
  CHTHOLLY_TEST_CHECK(scopes.insert(
      outer, {SemanticBindingKind::Local, 10, chtholly::compiler::NodeId(1)}));
  CHTHOLLY_TEST_CHECK(scopes.lookup(outer)->target == 10);

  scopes.pushIsolated();
  CHTHOLLY_TEST_CHECK(scopes.lookup(outer) == nullptr);
  CHTHOLLY_TEST_CHECK(scopes.lookupOutsideIsolation(outer)->target == 10);
  CHTHOLLY_TEST_CHECK(scopes.insert(
      inner, {SemanticBindingKind::Constant, 20, chtholly::compiler::NodeId(2)}));
  CHTHOLLY_TEST_CHECK(scopes.lookup(inner)->target == 20);
  scopes.pop();

  CHTHOLLY_TEST_CHECK(scopes.lookup(inner) == nullptr);
  CHTHOLLY_TEST_CHECK(scopes.lookup(outer)->target == 10);
  scopes.pop();
}

void testCallResolution() {
  const SemanticCallCandidateRank ranks[] = {
      {2, 4, false}, {1, 8, true}, {1, 6, true}, {1, 6, false}};
  const auto selected = selectCallCandidate(ranks);
  CHTHOLLY_TEST_CHECK(selected.kind == SemanticCallSelectionKind::Selected);
  CHTHOLLY_TEST_CHECK(selected.index == 3);

  const SemanticCallCandidateRank ambiguous_ranks[] = {{1, 2, false},
                                                       {1, 2, false}};
  CHTHOLLY_TEST_CHECK(selectCallCandidate(ambiguous_ranks).kind ==
         SemanticCallSelectionKind::Ambiguous);
  CHTHOLLY_TEST_CHECK(selectCallCandidate({}).kind == SemanticCallSelectionKind::None);
}

void testWitnessResolution() {
  const SemanticWitnessCandidate candidates[] = {
      {chtholly::compiler::InterfaceWitnessId(9),
       chtholly::compiler::StableFingerprint::fromCanonicalBytes("second")},
      {chtholly::compiler::InterfaceWitnessId(3),
       chtholly::compiler::StableFingerprint::fromCanonicalBytes("first")},
  };
  const auto ambiguous = selectSemanticWitness(candidates);
  CHTHOLLY_TEST_CHECK(ambiguous.kind == SemanticWitnessLookupKind::Ambiguous);
  const auto selected = selectSemanticWitness(
      std::span<const SemanticWitnessCandidate>(candidates, 1));
  CHTHOLLY_TEST_CHECK(selected.kind == SemanticWitnessLookupKind::Found &&
         selected.witness == chtholly::compiler::InterfaceWitnessId(9));
  CHTHOLLY_TEST_CHECK(selectSemanticWitness({}).kind == SemanticWitnessLookupKind::Missing);
}

void testObligationWorklist() {
  SemanticObligationWorklist ordered(3);
  bool first = false;
  bool second = false;
  const auto result = ordered.run([&](std::size_t index) {
    if (index == 0) {
      first = true;
      return SemanticObligationState::Resolved;
    }
    if (index == 1) {
      if (!first)
        return SemanticObligationState::Deferred;
      second = true;
      return SemanticObligationState::Resolved;
    }
    return second ? SemanticObligationState::Resolved
                  : SemanticObligationState::Deferred;
  });
  CHTHOLLY_TEST_CHECK(result == SemanticObligationWorklistResult::Complete);

  SemanticObligationWorklist stalled(2);
  CHTHOLLY_TEST_CHECK(stalled.run([](std::size_t) {
    return SemanticObligationState::Deferred;
  }) == SemanticObligationWorklistResult::Stalled);

  SemanticObligationWorklist failed(1);
  CHTHOLLY_TEST_CHECK(failed.run([](std::size_t) {
    return SemanticObligationState::Failed;
  }) == SemanticObligationWorklistResult::Failed);
}

void testCanonicalReferenceStability() {
  GenericValueStores values;
  const auto retained =
      values.internType({.kind = CanonicalTypeKind::Integer, .arg0 = 7});
  const auto duplicate =
      values.internType({.kind = CanonicalTypeKind::Integer, .arg0 = 7});
  CHTHOLLY_TEST_CHECK(duplicate == retained);
  const auto *address = &values.type(retained);
  for (std::uint32_t index = 8; index < 20000; ++index)
    (void)values.internType(
        CanonicalType{.kind = CanonicalTypeKind::Integer, .arg0 = index});
  CHTHOLLY_TEST_CHECK(&values.type(retained) == address);
  CHTHOLLY_TEST_CHECK(address->kind == CanonicalTypeKind::Integer && address->arg0 == 7);
}

void testTypeConcurrencySummary() {
  std::string error;
  TypeConcurrencySummary summary([](const PublicType &) {
    return static_cast<const NominalTypeSpecificArtifact *>(nullptr);
  });
  CHTHOLLY_TEST_CHECK(summary.summarize(PublicType::integer(32, true), error).transferable);
  CHTHOLLY_TEST_CHECK(summary.summarize(PublicType::integer(32, true), error).shareable);
  CHTHOLLY_TEST_CHECK(summary.summarize(PublicType::slice(PublicType::integer(8, false)),
                           error) == chtholly::compiler::TypeConcurrencyFacts{});

  NominalTypeSpecificArtifact scalar;
  scalar.fields.push_back({"value", PublicType::integer(32, true)});
  const PublicEntityReferenceArtifact entity{
      PublicEntityKind::NominalType, "test", "test", "Scalar", {}};
  const PublicType scalar_type(entity);
  TypeConcurrencySummary nominal_summary([&](const PublicType &type) {
    return type == scalar_type ? &scalar : nullptr;
  });
  const auto scalar_facts = nominal_summary.summarize(scalar_type, error);
  CHTHOLLY_TEST_CHECK(scalar_facts.transferable && scalar_facts.shareable);

  NominalTypeSpecificArtifact borrowed;
  borrowed.fields.push_back(
      {"view", PublicType::slice(PublicType::integer(8, false))});
  TypeConcurrencySummary borrowed_summary([&](const PublicType &type) {
    return type == scalar_type ? &borrowed : nullptr;
  });
  const auto borrowed_facts = borrowed_summary.summarize(scalar_type, error);
  CHTHOLLY_TEST_CHECK(!borrowed_facts.transferable && !borrowed_facts.shareable);

  TypeConcurrencySummary foreign_summary(
      {}, [&](const PublicType &type) -> std::optional<chtholly::compiler::TypeConcurrencyFacts> {
        return type == scalar_type
                   ? std::optional(chtholly::compiler::TypeConcurrencyFacts{true, true})
                   : std::nullopt;
      });
  const auto foreign_facts = foreign_summary.summarize(scalar_type, error);
  CHTHOLLY_TEST_CHECK(foreign_facts.transferable && foreign_facts.shareable);

  TypeConcurrencySummary bounded_summary(
      [](const PublicType &) {
        return static_cast<const NominalTypeSpecificArtifact *>(nullptr);
      },
      {}, 1);
  (void)bounded_summary.summarize(
      PublicType::tuple({PublicType::integer(32, true),
                         PublicType::integer(64, false)}),
      error);
  CHTHOLLY_TEST_CHECK(error.find("worklist") != std::string::npos);
}

void testControlFlow() {
  CompilationSession session(chtholly_test::targetTriple, "semantic-support");
  const auto unit = session.addUnit(SourceInput("support.cns", R"cns(
module support;
fn branch_body(): void { if (true) { let value = 1; } return; }
fn returns(): i32 { return 1; }
fn loops_forever(): void { while (true) {} }
)cns"));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << error << "\n";
    std::abort();
  }
  const auto *sem_ir = session.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  bool saw_fallthrough_arm = false;
  bool saw_return = false;
  bool saw_infinite_loop = false;
  for (std::uint32_t index = 0; index < sem_ir->functionCount(); ++index) {
    const auto function = FunctionId(index);
    const auto name =
        sem_ir->identifier(sem_ir->name(sem_ir->function(function).name).text);
    const auto falls = blockFallsThrough(
        *sem_ir, sem_ir->instBlock(sem_ir->function(function).body));
    if (name == "branch_body") {
      CHTHOLLY_TEST_CHECK(!falls);
      for (const auto instruction :
           sem_ir->instBlock(sem_ir->function(function).body)) {
        const auto &inst = sem_ir->inst(instruction);
        if (inst.kind != chtholly::compiler::SemInstKind::If)
          continue;
        for (const auto arm :
             sem_ir->instBlock(chtholly::compiler::InstBlockId(inst.arg1)))
          saw_fallthrough_arm |= blockFallsThrough(
              *sem_ir, sem_ir->instBlock(chtholly::compiler::InstBlockId(
                           sem_ir->inst(arm).arg0)));
      }
    }
    if (name == "returns")
      saw_return = !falls;
    if (name == "loops_forever")
      saw_infinite_loop = !falls;
  }
  CHTHOLLY_TEST_CHECK(saw_fallthrough_arm && saw_return && saw_infinite_loop);
}

void testInitializeContract() {
  CompilationSession session(chtholly_test::targetTriple,
                             "semantic-initialize");
  const auto unit = session.addUnit(SourceInput("initialize.cns", R"cns(
module initialize;
fn fill(destination: i32&): void contract {
  initializes destination;
  ensures initialized destination;
}
fn main(): i32 {
  var value: i32;
  fill(value);
  return value;
}
)cns"));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "initialize contract compile failed: " << error << "\n";
    std::abort();
  }
}

void testOwnerSafeSemIRRefs() {
  CompilationSession first(chtholly_test::targetTriple, "owner-first");
  const auto first_unit = first.addUnit(SourceInput("first.cns", R"cns(
module owner_first;
fn main(): i32 { return 0; }
)cns"));
  std::string error;
  CHTHOLLY_TEST_CHECK(first.compile(error));
  const auto *first_low = first.unit(first_unit).lowIR();
  CHTHOLLY_TEST_CHECK(first_low != nullptr);
  const auto reference = first_low->semIRRef(chtholly::compiler::InstId(0));
  CHTHOLLY_TEST_CHECK(first_low->owns(reference));
  CHTHOLLY_TEST_CHECK(reference.checked(first_low->semIR()).hasValue());
  const auto invalid_reference = chtholly::compiler::makeSemIRRef(
      first_low->semIR(), chtholly::compiler::InstId::invalid());
  CHTHOLLY_TEST_CHECK(!first_low->owns(invalid_reference));

  CompilationSession second(chtholly_test::targetTriple, "owner-second");
  const auto second_unit = second.addUnit(SourceInput("second.cns", R"cns(
module owner_second;
fn main(): i32 { return 0; }
)cns"));
  CHTHOLLY_TEST_CHECK(second.compile(error));
  const auto *second_low = second.unit(second_unit).lowIR();
  CHTHOLLY_TEST_CHECK(second_low != nullptr && !second_low->owns(reference));
  CHTHOLLY_TEST_CHECK(!second_low->owns(invalid_reference));
}

} // namespace

int main() {
  testLiterals();
  testTypedChannelIntrinsicRoles();
  testNameScopes();
  testCallResolution();
  testWitnessResolution();
  testObligationWorklist();
  testCanonicalReferenceStability();
  testTypeConcurrencySummary();
  testControlFlow();
  testInitializeContract();
  testOwnerSafeSemIRRefs();
  return 0;
}
