#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include "test_check.h"
#include <iostream>
#include <ranges>
#include <string>

namespace {

using chtholly::compiler::CallableReturnSource;
using chtholly::compiler::CallableOutcomeInitialize;
using chtholly::compiler::CallableOutcomePreserve;
using chtholly::compiler::CompilationRequest;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::PublicCallableDeclarationKind;
using chtholly::compiler::SourceInput;

bool hasCarrierPath(
    const chtholly::compiler::CallableOwnershipSummary &summary,
    std::initializer_list<CallableReturnSource::CarrierStep> path) {
  return std::ranges::any_of(
      summary.return_provenance, [&](const auto &source) {
        return std::ranges::equal(source.carrier_path, path);
      });
}

int mainImpl() {
  CompilationSession provider(chtholly_test::targetTriple,
                              "contract-provider-focused");
  const auto provider_unit =
      provider.addUnit(SourceInput("provider.cns",
                                   R"cns(module contract_provider_focused;
pub struct Box { pub value: i32; }
pub struct BorrowBox<T> { pub value: const T&; }

pub fn make(): Box {
  return Box { .value = 7 };
}

pub fn borrow(source: const Box&): const Box& contract {
  borrows shared source;
  returns borrow source;
}

pub fn update(target: Box&): void contract {
  writes target;
  ensures initialized target;
}

pub fn conditional_update(target: Box&, enabled: bool): void {
  if (enabled) {
    target.value = 9;
  }
  return;
}

pub fn forwarded_update(target: Box&, enabled: bool): void {
  conditional_update(target, enabled);
  return;
}

pub fn carry<T>(source: const T&): BorrowBox<T> {
  return BorrowBox<T> { .value = source };
}
)cns"));
  CHTHOLLY_TEST_CHECK(provider_unit.hasValue());
  std::string error;
  if (!provider.compile(error)) {
    std::cerr << "provider failed: " << error << "\n";
    return 1;
  }
  const auto *module =
      provider.packageManifest().findModule("contract_provider_focused");
  CHTHOLLY_TEST_CHECK(module != nullptr);
  chtholly::compiler::SharedValueStores registry_values;
  chtholly::compiler::PublicInterfaceRegistry registry(registry_values);
  const auto registered =
      registry.registerExternalArtifact(module->public_interface, error);
  CHTHOLLY_TEST_CHECK(registered.hasValue());
  CHTHOLLY_TEST_CHECK(registry.verify(error));
  const auto *borrow = module->public_interface.findFunction("borrow");
  const auto *update = module->public_interface.findFunction("update");
  const auto *conditional_update =
      module->public_interface.findFunction("conditional_update");
  const auto *forwarded_update =
      module->public_interface.findFunction("forwarded_update");
  const auto *carry = module->public_interface.findFunction("carry");
  CHTHOLLY_TEST_CHECK(borrow != nullptr && update != nullptr && conditional_update != nullptr &&
         forwarded_update != nullptr && carry != nullptr);
  CHTHOLLY_TEST_CHECK(borrow->declaration_kind == PublicCallableDeclarationKind::Forward);
  CHTHOLLY_TEST_CHECK(update->declaration_kind == PublicCallableDeclarationKind::Forward);
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      borrow->ownership_summary.effects, [](const auto &effect) {
        return effect.kind ==
                   chtholly::compiler::CallableEffectKind::BorrowShared &&
               effect.region.parameter_index == 0;
      }));
  CHTHOLLY_TEST_CHECK(!borrow->ownership_summary.return_provenance.empty());
  using CarrierKind = CallableReturnSource::CarrierStepKind;
  CHTHOLLY_TEST_CHECK(carry->ownership_summary.return_provenance.size() == 1);
  CHTHOLLY_TEST_CHECK(hasCarrierPath(carry->ownership_summary, {{CarrierKind::Field, 0}}));
  auto malformed_summary = carry->ownership_summary;
  malformed_summary.return_provenance.front().carrier_path.front().kind =
      CarrierKind::Count;
  std::string malformed_error;
  CHTHOLLY_TEST_CHECK(!malformed_summary.verify(1, malformed_error));
  malformed_summary = carry->ownership_summary;
  malformed_summary.return_provenance.front().carrier_path = {
      {CarrierKind::EnumVariant, 0}};
  malformed_error.clear();
  CHTHOLLY_TEST_CHECK(!malformed_summary.verify(1, malformed_error));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      update->ownership_summary.effects, [](const auto &effect) {
        return effect.kind == chtholly::compiler::CallableEffectKind::Write &&
               effect.region.parameter_index == 0;
      }));

  // A write guarded by a boolean parameter is published as two disjoint
  // compiler-owned postcondition facts: initialization on the true path and
  // preservation on the false path. This is the recovery-facing summary that
  // callers can replay without introducing source-level contract syntax.
  const auto has_condition = [](const auto &postcondition, bool expected,
                                std::uint8_t outcomes) {
    return postcondition.outcomes == outcomes &&
           postcondition.condition.clauses.size() == 1 &&
           postcondition.condition.clauses.front().atoms.size() == 1 &&
           postcondition.condition.clauses.front().atoms.front().parameter_index ==
               1 &&
           postcondition.condition.clauses.front().atoms.front().expected ==
               expected;
  };
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      conditional_update->ownership_summary.postconditions,
      [&](const auto &postcondition) {
        return has_condition(postcondition, true, CallableOutcomeInitialize);
      }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      forwarded_update->ownership_summary.postconditions,
      [&](const auto &postcondition) {
        return has_condition(postcondition, true, CallableOutcomeInitialize);
      }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      conditional_update->ownership_summary.postconditions,
      [&](const auto &postcondition) {
        return has_condition(postcondition, false, CallableOutcomePreserve);
      }));
  auto malformed_conditional = conditional_update->ownership_summary;
  malformed_conditional.postconditions.front().condition.clauses.front()
      .atoms.front().parameter_index = 99;
  std::string malformed_condition_error;
  CHTHOLLY_TEST_CHECK(!malformed_conditional.verify(2, malformed_condition_error));
  CHTHOLLY_TEST_CHECK(!std::ranges::any_of(
      update->ownership_summary.effects, [](const auto &effect) {
        return effect.kind == chtholly::compiler::CallableEffectKind::Initialize;
      }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      update->ownership_summary.postconditions, [](const auto &postcondition) {
        return postcondition.region.parameter_index == 0 &&
               (postcondition.outcomes &
                chtholly::compiler::CallableOutcomeInitialize) != 0;
      }));
  auto encoded = provider.packageManifest().encode(error);
  CHTHOLLY_TEST_CHECK(!encoded.empty());
  auto decoded =
      chtholly::compiler::CompilerPackageArtifactManifest::decode(encoded, error);
  CHTHOLLY_TEST_CHECK(decoded.has_value());
  const auto *decoded_module = decoded->findModule("contract_provider_focused");
  CHTHOLLY_TEST_CHECK(decoded_module != nullptr);
  const auto *decoded_borrow =
      decoded_module->public_interface.findFunction("borrow");
  const auto *decoded_carry =
      decoded_module->public_interface.findFunction("carry");
  const auto *decoded_conditional =
      decoded_module->public_interface.findFunction("conditional_update");
  CHTHOLLY_TEST_CHECK(decoded_borrow != nullptr && decoded_conditional != nullptr &&
         decoded_carry != nullptr);
  CHTHOLLY_TEST_CHECK(decoded_borrow->ownership_summary == borrow->ownership_summary);
  CHTHOLLY_TEST_CHECK(decoded_conditional->ownership_summary ==
         conditional_update->ownership_summary);
  CHTHOLLY_TEST_CHECK(decoded_carry->ownership_summary == carry->ownership_summary);

  CompilationSession consumer(chtholly_test::targetTriple,
                              "contract-consumer-focused");
  const auto consumer_unit =
      consumer.addUnit(SourceInput("consumer.cns",
R"cns(module contract_consumer_focused;
import contract_provider_focused;

fn main(): i32 {
  let box = contract_provider_focused::make();
  let view = contract_provider_focused::borrow(&box);
  let carried = contract_provider_focused::carry(&box);
  return view.value;
}
)cns"));
  CHTHOLLY_TEST_CHECK(consumer_unit.hasValue());
  CompilationRequest request;
  request.dependency_manifests = {&provider.packageManifest()};
  if (!consumer.compile(error, request)) {
    std::cerr << "consumer failed: " << error << "\n";
    return 1;
  }
  const auto llvm = consumer.unit(consumer_unit).printLLVM();
  CHTHOLLY_TEST_CHECK(llvm.find("declare") != std::string::npos);
  const auto *sem_ir = consumer.unit(consumer_unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  const chtholly::compiler::ConcreteSpecificNodeArtifact *carry_node = nullptr;
  const chtholly::compiler::ConcreteSpecializationComponentArtifact
      *carry_component = nullptr;
  for (const auto &component : sem_ir->specializationComponents())
    for (const auto &node : component.nodes())
      if (node.template_entity.canonical_name == "carry") {
        carry_node = &node;
        carry_component = &component;
      }
  CHTHOLLY_TEST_CHECK(carry_node != nullptr && carry_component != nullptr);
  CHTHOLLY_TEST_CHECK(carry_node->ownership_summary.return_provenance.size() == 1);
  CHTHOLLY_TEST_CHECK(
      hasCarrierPath(carry_node->ownership_summary, {{CarrierKind::Field, 0}}));
  const auto component_bytes = carry_component->encode(error);
  CHTHOLLY_TEST_CHECK(!component_bytes.empty());
  const auto decoded_component =
      chtholly::compiler::ConcreteSpecializationComponentArtifact::decode(
          component_bytes, error);
  CHTHOLLY_TEST_CHECK(decoded_component.has_value());
  const auto *decoded_node =
      decoded_component->findNode(carry_node->request_fingerprint);
  CHTHOLLY_TEST_CHECK(decoded_node != nullptr);
  CHTHOLLY_TEST_CHECK(decoded_node->ownership_summary == carry_node->ownership_summary);
  auto typed_node = *carry_node;
  chtholly::compiler::ConcreteTypedChannelDescriptor typed_descriptor;
  typed_descriptor.payload_type =
      chtholly::compiler::PublicType::integer(32, true);
  typed_descriptor.payload_type_fingerprint =
      chtholly::compiler::publicTypeFingerprint(typed_descriptor.payload_type);
  typed_descriptor.layout_fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("layout");
  typed_descriptor.lifecycle_fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("lifecycle");
  typed_descriptor.representation = {
      chtholly::compiler::ValueReprKind::Copy,
      chtholly::compiler::InitReprKind::ByCopy,
      chtholly::compiler::OwnershipReprKind::Owned,
      chtholly::compiler::CopyReprKind::Trivial,
      chtholly::compiler::MoveReprKind::Trivial,
      chtholly::compiler::DestroyReprKind::Trivial,
      chtholly::compiler::ObjectReprKind::NominalAggregate};
  typed_descriptor.concurrency = {true, true};
  typed_descriptor.runtime_abi_epoch = 1;
  typed_descriptor.component_identity = "chtholly.typed-channel";
  typed_descriptor.operation_identity = "payload.test";
  typed_descriptor.component_descriptor_digest =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("descriptor");
  typed_node.typed_channels.push_back(typed_descriptor);
  chtholly::compiler::ConcreteSpecializationComponentArtifact typed_component(
      carry_component->semanticOptionsFingerprint(), {std::move(typed_node)});
  const auto typed_bytes = typed_component.encode(error);
  CHTHOLLY_TEST_CHECK(!typed_bytes.empty());
  const auto typed_decoded =
      chtholly::compiler::ConcreteSpecializationComponentArtifact::decode(
          typed_bytes, error);
  CHTHOLLY_TEST_CHECK(typed_decoded.has_value());
  const auto *typed_decoded_node =
      typed_decoded->findNode(carry_node->request_fingerprint);
  CHTHOLLY_TEST_CHECK(typed_decoded_node != nullptr &&
         typed_decoded_node->typed_channels.size() == 1 &&
         typed_decoded_node->typed_channels.front() == typed_descriptor);
  typed_descriptor.outcome_fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("tampered-outcome");
  typed_node = *carry_node;
  typed_node.typed_channels.push_back(typed_descriptor);
  chtholly::compiler::ConcreteSpecializationComponentArtifact invalid_outcome(
      carry_component->semanticOptionsFingerprint(), {std::move(typed_node)});
  error.clear();
  CHTHOLLY_TEST_CHECK(invalid_outcome.encode(error).empty());
  CHTHOLLY_TEST_CHECK(error.find("typed channel descriptor") != std::string::npos);
  typed_descriptor.outcome_fingerprint =
      chtholly::compiler::canonicalTypedChannelOutcomeFingerprint();
  typed_descriptor.concurrency.transferable = false;
  typed_node = *carry_node;
  typed_node.typed_channels.push_back(typed_descriptor);
  chtholly::compiler::ConcreteSpecializationComponentArtifact invalid_typed(
      carry_component->semanticOptionsFingerprint(), {std::move(typed_node)});
  error.clear();
  CHTHOLLY_TEST_CHECK(invalid_typed.encode(error).empty());
  CHTHOLLY_TEST_CHECK(error.find("typed channel descriptor") != std::string::npos);
  auto stale_component_bytes = component_bytes;
  constexpr std::string_view ComponentMagic = "CHNXSCC51";
  stale_component_bytes[ComponentMagic.size()] = 41;
  error.clear();
  CHTHOLLY_TEST_CHECK(!chtholly::compiler::ConcreteSpecializationComponentArtifact::decode(
      stale_component_bytes, error));
  return 0;
}

} // namespace

int main() {
  return mainImpl();
}
