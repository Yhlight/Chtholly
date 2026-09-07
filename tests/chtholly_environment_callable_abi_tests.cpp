#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"
#include "chtholly/Compiler/SemIR.h"

#include <algorithm>
#include "test_check.h"
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chtholly::compiler::CompilationRequest;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::GenericTemplateArtifact;
using chtholly::compiler::GenericTemplateOpcode;
using chtholly::compiler::SemCallableEnvironmentCapability;
using chtholly::compiler::SemCallableEnvironmentKind;
using chtholly::compiler::SemInstKind;
using chtholly::compiler::SourceInput;
using chtholly::compiler::StableFingerprint;

CompilationSession makeV12Session(std::string package) {
  auto contract = chtholly::CurrentLanguageContract;
  contract.source = chtholly::FrozenV12LanguageVersion;
  return CompilationSession(
      chtholly_test::targetTriple, std::move(package), {},
      chtholly::compiler::defaultCompileToolchainFingerprint(), {}, contract);
}

void expectFailure(std::string_view name, std::string_view source) {
  auto session =
      makeV12Session(std::string("environment-callable-") + std::string(name));
  std::string error;
  CHTHOLLY_TEST_CHECK(session.addUnit(SourceInput(std::string(name), std::string(source)))
             .hasValue());
  if (session.compile(error)) {
    std::cerr << "environment callable fixture unexpectedly compiled: " << name
              << "\n";
    std::abort();
  }
}

void localClosureCapabilities() {
  auto session = makeV12Session("environment-callable-local-closures");
  const auto unit = session.addUnit(
      SourceInput("local-closures.cns", R"cns(module local_closures;
fn readonly(): i32 {
  let offset = 2;
  let add = fn [copy offset](value: i32): i32 {
    return value + offset;
  };
  return add(1) + add(2);
}
fn mutable_capture(): i32 {
  var counter = fn [var value = 0](): i32 {
    value += 1;
    return value;
  };
  let first = counter();
  return first + counter();
}
fn moved_field(): i32 {
  let payload = 7;
  var take = fn [move payload](): i32 { return move payload; };
  return take();
}
)cns"));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "local closure capabilities failed: " << error << "\n";
    std::abort();
  }
  const auto *sem_ir = session.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  std::size_t closures = 0;
  std::size_t readonly = 0;
  std::size_t mutable_callables = 0;
  for (std::uint32_t index = 0; index < sem_ir->instCount(); ++index) {
    const auto &inst = sem_ir->inst(chtholly::compiler::InstId(index));
    if (inst.kind != SemInstKind::Closure)
      continue;
    ++closures;
    const auto *info =
        sem_ir->tryGetCallableEnvironment(chtholly::compiler::TypeId(inst.type));
    CHTHOLLY_TEST_CHECK(info != nullptr);
    CHTHOLLY_TEST_CHECK(info->kind == SemCallableEnvironmentKind::Closure);
    CHTHOLLY_TEST_CHECK(info->target.hasValue() && info->formation_target == info->target);
    CHTHOLLY_TEST_CHECK(info->identity.hasValue());
    if (info->capability == SemCallableEnvironmentCapability::ReadOnly)
      ++readonly;
    if (info->capability == SemCallableEnvironmentCapability::Mutable)
      ++mutable_callables;
  }
  CHTHOLLY_TEST_CHECK(closures == 3);
  CHTHOLLY_TEST_CHECK(readonly == 1);
  CHTHOLLY_TEST_CHECK(mutable_callables == 2);
}

void localBoundMethodCapabilities() {
  auto session = makeV12Session("environment-callable-local-bound-methods");
  const auto unit = session.addUnit(
      SourceInput("local-bound-methods.cns", R"cns(module local_bound_methods;
struct Box { value: i32; }
impl Box {
  fn get(self: const Self&): i32 { return self.value; }
  fn replace(self: Self&, value: i32): void {
    self.value = value;
    return;
  }
  fn take(self: Self): i32 { return move self.value; }
}
fn readonly_bound(): i32 {
  let box = Box { .value = 3 };
  let get = box.get;
  return get() + get();
}
fn mutable_bound(): i32 {
  var box = Box { .value = 3 };
  var replace = box.replace;
  replace(9);
  return box.value;
}
fn consuming_bound(): i32 {
  let box = Box { .value = 3 };
  let take = (move box).take;
  return (move take)();
}
)cns"));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "local bound-method capabilities failed: " << error << "\n";
    std::abort();
  }
  const auto *sem_ir = session.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  bool saw_readonly = false;
  bool saw_mutable = false;
  bool saw_consuming = false;
  for (std::uint32_t index = 0; index < sem_ir->instCount(); ++index) {
    const auto &inst = sem_ir->inst(chtholly::compiler::InstId(index));
    if (inst.kind != SemInstKind::BoundMethod)
      continue;
    const auto *info =
        sem_ir->tryGetCallableEnvironment(chtholly::compiler::TypeId(inst.type));
    CHTHOLLY_TEST_CHECK(info != nullptr);
    CHTHOLLY_TEST_CHECK(info->kind == SemCallableEnvironmentKind::BoundMethod);
    CHTHOLLY_TEST_CHECK(info->target.hasValue() && info->formation_target.hasValue());
    CHTHOLLY_TEST_CHECK(info->identity.hasValue());
    saw_readonly |=
        info->capability == SemCallableEnvironmentCapability::ReadOnly;
    saw_mutable |=
        info->capability == SemCallableEnvironmentCapability::Mutable;
    saw_consuming |=
        info->capability == SemCallableEnvironmentCapability::Consuming;
  }
  CHTHOLLY_TEST_CHECK(saw_readonly && saw_mutable && saw_consuming);
}

constexpr std::string_view ProviderSource = R"cns(module callable_provider;
pub struct Box<T> { pub value: T; }
impl<T> Box<T> {
  pub fn take(self: Self): T { return move self.value; }
}
pub fn through_closure<T>(value: T): T {
  var closure = fn [move value](): T { return move value; };
  return closure();
}
pub fn through_bound<T>(value: T): T {
  let box = Box<T> { .value = move value };
  let bound = (move box).take;
  return (move bound)();
}
)cns";

struct ProviderArtifacts {
  CompilationSession session;
  const chtholly::compiler::PublicFunctionArtifact *closure = nullptr;
  const chtholly::compiler::PublicFunctionArtifact *bound = nullptr;

  explicit ProviderArtifacts(std::string identity)
      : session(makeV12Session(std::move(identity))) {
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput("provider.cns", std::string(ProviderSource)))
            .hasValue());
    std::string error;
    if (!session.compile(error)) {
      std::cerr << "callable provider failed: " << error << "\n";
      std::abort();
    }
    const auto *module =
        session.packageManifest().findModule("callable_provider");
    CHTHOLLY_TEST_CHECK(module != nullptr);
    closure = module->public_interface.findFunction("through_closure");
    bound = module->public_interface.findFunction("through_bound");
    CHTHOLLY_TEST_CHECK(closure != nullptr && closure->generic_template.has_value());
    CHTHOLLY_TEST_CHECK(bound != nullptr && bound->generic_template.has_value());
  }
};

const chtholly::compiler::GenericTemplateInstArtifact *
findOpcode(const GenericTemplateArtifact &artifact,
           GenericTemplateOpcode opcode, std::size_t *index = nullptr) {
  const auto found = std::ranges::find_if(
      artifact.definition.instructions,
      [&](const auto &inst) { return inst.opcode == opcode; });
  if (found == artifact.definition.instructions.end())
    return nullptr;
  if (index)
    *index = static_cast<std::size_t>(
        std::distance(artifact.definition.instructions.begin(), found));
  return &*found;
}

std::vector<StableFingerprint>
compileConsumer(const chtholly::compiler::CompilerPackageArtifactManifest &provider,
                std::string identity) {
  auto consumer = makeV12Session(std::move(identity));
  const auto unit = consumer.addUnit(
      SourceInput("consumer.cns", R"cns(module callable_consumer;
import callable_provider;
fn main(): i32 {
  return callable_provider::through_closure(7) +
         callable_provider::through_bound(8);
}
)cns"));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  CompilationRequest request;
  request.dependency_manifests = {&provider};
  std::string error;
  if (!consumer.compile(error, request)) {
    std::cerr << "callable consumer failed: " << error << "\n";
    std::abort();
  }
  const auto *sem_ir = consumer.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  std::vector<StableFingerprint> fingerprints;
  for (const auto &component : sem_ir->specializationComponents())
    fingerprints.push_back(component.fingerprint());
  CHTHOLLY_TEST_CHECK(!fingerprints.empty());
  std::ranges::sort(fingerprints, [](const auto &left, const auto &right) {
    return left.hex() < right.hex();
  });
  return fingerprints;
}

void genericArtifactRoundTripAndIdentity() {
  ProviderArtifacts first("environment-callable-provider");
  ProviderArtifacts second("environment-callable-provider");
  const auto &closure_template = *first.closure->generic_template;
  const auto &bound_template = *first.bound->generic_template;
  CHTHOLLY_TEST_CHECK(findOpcode(closure_template, GenericTemplateOpcode::Closure));
  CHTHOLLY_TEST_CHECK(findOpcode(bound_template, GenericTemplateOpcode::BoundMethod));
  CHTHOLLY_TEST_CHECK(first.closure->entity_fingerprint ==
         second.closure->entity_fingerprint);
  CHTHOLLY_TEST_CHECK(first.bound->entity_fingerprint == second.bound->entity_fingerprint);
  CHTHOLLY_TEST_CHECK(closure_template == *second.closure->generic_template);
  CHTHOLLY_TEST_CHECK(bound_template == *second.bound->generic_template);

  const auto first_components = compileConsumer(
      first.session.packageManifest(), "environment-callable-consumer");
  const auto second_components = compileConsumer(
      second.session.packageManifest(), "environment-callable-consumer");
  CHTHOLLY_TEST_CHECK(first_components == second_components);

  std::size_t closure_index = 0;
  CHTHOLLY_TEST_CHECK(findOpcode(closure_template, GenericTemplateOpcode::Closure,
                    &closure_index));
  auto bad_capability = closure_template;
  bad_capability.definition.instructions[closure_index].arg1 =
      static_cast<std::uint32_t>(SemCallableEnvironmentCapability::Count);
  std::string error;
  CHTHOLLY_TEST_CHECK(!bad_capability.verify(error));

  auto bad_target = closure_template;
  bad_target.definition.instructions[closure_index].arg0 =
      static_cast<std::uint32_t>(bad_target.callees.size());
  error.clear();
  CHTHOLLY_TEST_CHECK(!bad_target.verify(error));

  std::size_t bound_index = 0;
  CHTHOLLY_TEST_CHECK(findOpcode(bound_template, GenericTemplateOpcode::BoundMethod,
                    &bound_index));
  auto bad_receiver = bound_template;
  bad_receiver.definition.instruction_value_blocks[bound_index].clear();
  error.clear();
  CHTHOLLY_TEST_CHECK(!bad_receiver.verify(error));
}

} // namespace

int main() {
  localClosureCapabilities();
  localBoundMethodCapabilities();

  expectFailure("immutable-mutable-closure", R"cns(module immutable_mutable;
fn main(): i32 {
  let counter = fn [var value = 0](): i32 {
    value += 1;
    return value;
  };
  return counter();
}
)cns");
  expectFailure("reuse-moved-capture", R"cns(module reuse_moved_capture;
fn main(): i32 {
  let payload = 7;
  var take = fn [move payload](): i32 { return move payload; };
  let first = take();
  return first + take();
}
)cns");
  expectFailure("consuming-bound-without-move", R"cns(module bound_without_move;
struct Box { value: i32; }
impl Box { fn take(self: Self): i32 { return move self.value; } }
fn main(): i32 {
  let box = Box { .value = 3 };
  let take = (move box).take;
  return take();
}
)cns");
  expectFailure("mutable-bound-loan-conflict", R"cns(module bound_loan_conflict;
struct Box { value: i32; }
impl Box {
  fn replace(self: Self&, value: i32): void {
    self.value = value;
    return;
  }
}
fn main(): i32 {
  var box = Box { .value = 3 };
  var replace = box.replace;
  let observed = box.value;
  replace(9);
  return observed;
}
)cns");

  genericArtifactRoundTripAndIdentity();
  return 0;
}
