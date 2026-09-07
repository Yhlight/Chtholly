#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"
#include "chtholly/Compiler/IteratorDesugaring.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Support/FileSystem.h"

#include "test_check.h"
#include <filesystem>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using chtholly::compiler::CompilationRequest;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::CompilerIntrinsicBinding;
using chtholly::compiler::CompilerIntrinsicRole;
using chtholly::compiler::CallableReturnSource;
using chtholly::compiler::IteratorBindingKind;
using chtholly::compiler::IteratorDesugaringOptions;
using chtholly::compiler::SemReferenceMutability;
using chtholly::compiler::FunctionId;
using chtholly::compiler::buildHandWrittenIteratorExpansion;
using chtholly::compiler::buildIteratorDesugaring;
using chtholly::compiler::equivalentIteratorDesugarings;
using chtholly::compiler::validateIteratorProjectionLoans;
using chtholly::compiler::SourceInput;

bool hasIteratorCarrierPath(
    const chtholly::compiler::CallableOwnershipSummary &summary,
    std::initializer_list<CallableReturnSource::CarrierStep> path) {
  return std::ranges::any_of(summary.return_provenance, [&](const auto &source) {
    return std::ranges::equal(source.carrier_path, path);
  });
}

void expectIteratorCarrierPaths(
    const chtholly::compiler::CallableOwnershipSummary &summary) {
  using Kind = CallableReturnSource::CarrierStepKind;
  CHTHOLLY_TEST_CHECK(summary.return_provenance.size() == 2);
  CHTHOLLY_TEST_CHECK(hasIteratorCarrierPath(
      summary, {{Kind::EnumVariant, 0}, {Kind::Field, 0}}));
  CHTHOLLY_TEST_CHECK(hasIteratorCarrierPath(
      summary,
      {{Kind::EnumVariant, 0}, {Kind::Field, 1}, {Kind::Field, 0}}));
}

void expectPublicIteratorCarrierPaths(
    const chtholly::compiler::CompilerPackageArtifactManifest &manifest) {
  const auto *module = manifest.findModule("std::vec");
  CHTHOLLY_TEST_CHECK(module != nullptr);
  for (const auto role : {CompilerIntrinsicRole::VecIterNext,
                          CompilerIntrinsicRole::VecIterMutNext}) {
    const auto function = std::ranges::find_if(
        module->public_interface.functions(),
        [&](const auto &candidate) { return candidate.intrinsic_role == role; });
    CHTHOLLY_TEST_CHECK(function != module->public_interface.functions().end());
    expectIteratorCarrierPaths(function->ownership_summary);
  }
}

std::vector<CompilerIntrinsicBinding> stdContainerIntrinsics() {
  return {
      {"std::option", "Option.is_some", CompilerIntrinsicRole::OptionIsSome},
      {"std::option", "Option.is_none", CompilerIntrinsicRole::OptionIsNone},
      {"std::option", "Option.unwrap", CompilerIntrinsicRole::OptionUnwrap},
      {"std::vec", "Vec.init", CompilerIntrinsicRole::VecInit},
      {"std::vec", "Vec.len", CompilerIntrinsicRole::VecLen},
      {"std::vec", "Vec.capacity", CompilerIntrinsicRole::VecCapacity},
      {"std::vec", "Vec.reserve", CompilerIntrinsicRole::VecReserve},
      {"std::vec", "Vec.push", CompilerIntrinsicRole::VecPush},
      {"std::vec", "Vec.at", CompilerIntrinsicRole::VecAt},
      {"std::vec", "Vec.at_mut", CompilerIntrinsicRole::VecAtMut},
      {"std::vec", "Vec.pop", CompilerIntrinsicRole::VecPop},
      {"std::vec", "Vec.remove", CompilerIntrinsicRole::VecRemove},
      {"std::vec", "Vec.clear", CompilerIntrinsicRole::VecClear},
      {"std::vec", "Vec.drop", CompilerIntrinsicRole::VecDrop},
      {"std::vec", "Vec.iter", CompilerIntrinsicRole::VecIter},
      {"std::vec", "Vec.iter_mut", CompilerIntrinsicRole::VecIterMut},
      {"std::vec", "VecIterator.next", CompilerIntrinsicRole::VecIterNext},
      {"std::vec", "VecMutIterator.next", CompilerIntrinsicRole::VecIterMutNext},
  };
}

CompilationSession makeVecProvider() {
  return CompilationSession(
      chtholly_test::targetTriple, "std", {},
      chtholly::compiler::defaultCompileToolchainFingerprint(),
      {.kind = chtholly::compiler::PackageProvenanceKind::ToolchainStandardLibrary},
      chtholly::CurrentLanguageContract, stdContainerIntrinsics());
}

std::string readFixture(std::string_view name) {
  std::string error;
  const auto path = fs::path(CHTHOLLY_SOURCE_DIR) / "tests" / "fixtures" /
                    "chtholly-container-ownership" / std::string(name);
  auto source = chtholly::readTextFile(path.string(), error);
  if (!source) {
    std::cerr << "failed to read fixture " << path.string() << ": " << error
              << "\n";
    std::abort();
  }
  return *source;
}

std::string readStdModule(std::string_view name) {
  std::string error;
  const auto path = fs::path(CHTHOLLY_SOURCE_DIR) / "stdlib" /
                    (std::string(name) + ".cns");
  auto source = chtholly::readTextFile(path.string(), error);
  if (!source) {
    std::cerr << "failed to read std::" << name << ": " << error << "\n";
    std::abort();
  }
  return *source;
}

void addStdVec(CompilationSession &session) {
  CHTHOLLY_TEST_CHECK(session.addUnit(SourceInput("std::iter", readStdModule("iter")))
             .hasValue());
  CHTHOLLY_TEST_CHECK(session
             .addUnit(SourceInput("std::option", readStdModule("option")))
             .hasValue());
  CHTHOLLY_TEST_CHECK(session.addUnit(SourceInput("std::vec", readStdModule("vec")))
             .hasValue());
}

void compileStdVecProvider(CompilationSession &provider) {
  std::string error;
  addStdVec(provider);
  if (!provider.compile(error)) {
    std::cerr << "std::vec provider failed to compile:\n" << error << "\n";
    std::abort();
  }
  expectPublicIteratorCarrierPaths(provider.packageManifest());
  const auto encoded = provider.packageManifest().encode(error);
  CHTHOLLY_TEST_CHECK(!encoded.empty());
  const auto decoded =
      chtholly::compiler::CompilerPackageArtifactManifest::decode(encoded, error);
  CHTHOLLY_TEST_CHECK(decoded.has_value());
  expectPublicIteratorCarrierPaths(*decoded);
}

void expectSuccess(std::string_view name) {
  CompilationSession session(chtholly_test::targetTriple, "container-positive");
  std::string error;
  const auto unit =
      session.addUnit(SourceInput(std::string(name), readFixture(name)));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  if (!session.compile(error)) {
    std::cerr << "positive container fixture failed: " << name << "\n"
              << error << "\n";
    if (const auto *sem_ir = session.unit(unit).semIR()) {
      std::string verify_error;
      if (!sem_ir->verify(verify_error))
        std::cerr << "container verifier: " << verify_error << "\n";
    }
    std::abort();
  }
}

void expectVecSuccess(std::string_view name) {
  auto provider = makeVecProvider();
  compileStdVecProvider(provider);
  CompilationSession session(chtholly_test::targetTriple,
                             "vec-positive-" + std::string(name));
  std::string error;
  const auto unit =
      session.addUnit(SourceInput(std::string(name), readFixture(name)));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  CompilationRequest request;
  request.dependency_manifests = {&provider.packageManifest()};
  if (!session.compile(error, request)) {
    std::cerr << "positive Vec fixture failed: " << name << "\n"
              << error << "\n";
    if (const auto *sem_ir = session.unit(unit).semIR()) {
      std::string verify_error;
      if (!sem_ir->verify(verify_error))
        std::cerr << "consumer verifier: " << verify_error << "\n";
    }
    std::abort();
  }
  bool saw_concrete_intrinsic = false;
  const auto *sem_ir = session.unit(unit).semIR();
  for (std::uint32_t index = 0; index < sem_ir->functionRefCount(); ++index) {
    const auto reference = chtholly::compiler::FunctionRefId(index);
    const auto &value = sem_ir->functionRef(reference);
    if (!value.public_entity.hasValue() || value.local_function.hasValue() ||
        value.import_ir_inst.hasValue() || !value.generic.hasValue() ||
        !value.specific.hasValue() ||
        value.specific ==
            sem_ir->genericValues().generic(value.generic).self_specific ||
        sem_ir->functionIntrinsicRole(reference) == CompilerIntrinsicRole::None)
      continue;
    CHTHOLLY_TEST_CHECK(!sem_ir->functionRefConcreteArguments(reference).empty());
    saw_concrete_intrinsic = true;
  }
  CHTHOLLY_TEST_CHECK(saw_concrete_intrinsic);
  bool found_iterator_interface = false;
  bool found_iterator_witness = false;
  for (std::uint32_t index = 0; index < sem_ir->interfaceCount(); ++index) {
    const auto &interface_value =
        sem_ir->interface(chtholly::compiler::InterfaceId(index));
    if (interface_value.name.hasValue() &&
        sem_ir->identifier(sem_ir->name(interface_value.name).text) ==
            "Iterator") {
      CHTHOLLY_TEST_CHECK(interface_value.canonical_entity.hasValue());
      const auto *entity = sem_ir->importIRs().tryGetEntity(
          interface_value.canonical_entity);
      CHTHOLLY_TEST_CHECK(entity != nullptr);
      CHTHOLLY_TEST_CHECK(sem_ir->identifier(entity->package_name) == "std");
      CHTHOLLY_TEST_CHECK(sem_ir->identifier(entity->module_name) == "std::iter");
      found_iterator_interface = true;
    }
  }
  for (std::uint32_t index = 0; index < sem_ir->interfaceWitnessCount();
       ++index) {
    const auto &witness =
        sem_ir->interfaceWitness(chtholly::compiler::InterfaceWitnessId(index));
    const auto &interface_value = sem_ir->interface(witness.interface_id);
    if (interface_value.name.hasValue() &&
        sem_ir->identifier(sem_ir->name(interface_value.name).text) ==
            "Iterator")
      found_iterator_witness = true;
  }
  if (!found_iterator_interface || !found_iterator_witness) {
    std::cerr << "imported Vec omitted iterator protocol: interfaces="
              << sem_ir->interfaceCount()
              << " witnesses=" << sem_ir->interfaceWitnessCount() << "\n";
    std::abort();
  }
}

void expectFailure(std::string_view name, std::string_view diagnostic) {
  CompilationSession session(chtholly_test::targetTriple, "container-negative");
  std::string error;
  const auto unit_id =
      session.addUnit(SourceInput(std::string(name), readFixture(name)));
  CHTHOLLY_TEST_CHECK(unit_id.hasValue());
  if (session.compile(error)) {
    std::cerr << "negative container fixture unexpectedly compiled: " << name
              << "\n";
    std::abort();
  }
  if (error.find(diagnostic) == std::string::npos) {
    std::cerr << "fixture " << name << " produced unexpected diagnostic:\n"
              << error << "\nexpected: " << diagnostic << "\n";
    std::abort();
  }
  bool has_explanation = false;
  for (const auto &item : session.unit(unit_id).diagnostics())
    has_explanation |= !item.notes.empty();
  if (!has_explanation) {
    std::cerr << "fixture " << name
              << " did not retain structured ownership explanation\n";
    std::abort();
  }
}

void expectVecFailure(std::string_view name, std::string_view diagnostic) {
  auto provider = makeVecProvider();
  compileStdVecProvider(provider);
  CompilationSession session(chtholly_test::targetTriple,
                             "vec-negative-" + std::string(name));
  std::string error;
  const auto unit_id =
      session.addUnit(SourceInput(std::string(name), readFixture(name)));
  CHTHOLLY_TEST_CHECK(unit_id.hasValue());
  CompilationRequest request;
  request.dependency_manifests = {&provider.packageManifest()};
  if (session.compile(error, request)) {
    std::cerr << "negative Vec fixture unexpectedly compiled: " << name << "\n";
    std::abort();
  }
  if (error.find(diagnostic) == std::string::npos) {
    std::cerr << "Vec fixture " << name << " produced unexpected diagnostic:\n"
              << error << "\nexpected: " << diagnostic << "\n";
    std::abort();
  }
  bool has_explanation = false;
  for (const auto &item : session.unit(unit_id).diagnostics())
    has_explanation |= !item.notes.empty();
  if (!has_explanation) {
    std::cerr << "Vec fixture " << name
              << " did not retain structured ownership explanation\n";
    std::abort();
  }
}

void expectIteratorDesugaringPrototype() {
  auto provider = makeVecProvider();
  compileStdVecProvider(provider);
  CompilationSession session(chtholly_test::targetTriple, "iterator-prototype");
  std::string error;
  const auto unit = session.addUnit(SourceInput(
      "vec-iterator-expanded-control.cns",
      readFixture("vec-iterator-expanded-control.cns")));
  CHTHOLLY_TEST_CHECK(unit.hasValue());
  CompilationRequest request;
  request.dependency_manifests = {&provider.packageManifest()};
  if (!session.compile(error, request)) {
    std::cerr << "iterator prototype fixture failed:\n" << error << "\n";
    std::abort();
  }
  const auto *sem_ir = session.unit(unit).semIR();
  CHTHOLLY_TEST_CHECK(sem_ir != nullptr);
  FunctionId main_function = FunctionId::invalid();
  for (std::uint32_t index = 0; index < sem_ir->functionCount(); ++index) {
    const auto function = FunctionId(index);
    if (sem_ir->identifier(sem_ir->name(sem_ir->function(function).name).text) ==
        "main") {
      main_function = function;
      break;
    }
  }
  CHTHOLLY_TEST_CHECK(main_function.hasValue());

  for (const auto binding : {IteratorBindingKind::Shared,
                             IteratorBindingKind::Mutable}) {
    IteratorDesugaringOptions options;
    options.binding = binding;
    options.item_defer = true;
    const auto generated =
        buildIteratorDesugaring(*sem_ir, main_function, options);
    const auto reference =
        buildHandWrittenIteratorExpansion(*sem_ir, main_function, options);
    std::string graph_error;
    CHTHOLLY_TEST_CHECK(validateIteratorProjectionLoans(generated, graph_error));
    CHTHOLLY_TEST_CHECK(equivalentIteratorDesugarings(generated, reference, graph_error));
    const auto *projection = generated.findNode("item.projection");
    CHTHOLLY_TEST_CHECK(projection != nullptr);
    CHTHOLLY_TEST_CHECK(generated.loans.size() == 1);
    CHTHOLLY_TEST_CHECK(generated.loans.front().projection_node == projection->id);
    CHTHOLLY_TEST_CHECK(generated.loans.front().mutability ==
           (binding == IteratorBindingKind::Mutable
                ? SemReferenceMutability::Mutable
                : SemReferenceMutability::ReadOnly));
    CHTHOLLY_TEST_CHECK(generated.findNode("next.call")->source_inst.hasValue());
    CHTHOLLY_TEST_CHECK(generated.findNode("next.dispatch")->source_inst.hasValue());
    CHTHOLLY_TEST_CHECK(generated.findNode("item.arm")->source_inst.hasValue());
    CHTHOLLY_TEST_CHECK(generated.findNode("item.projection")->source_inst.hasValue());
    CHTHOLLY_TEST_CHECK(generated.findNode("done.arm")->source_inst.hasValue());
    CHTHOLLY_TEST_CHECK(!generated.semir_facts.empty());
    CHTHOLLY_TEST_CHECK(!generated.semir_edges.empty());
  }

  const auto owned = buildIteratorDesugaring(
      *sem_ir, main_function, {.binding = IteratorBindingKind::Owned});
  CHTHOLLY_TEST_CHECK(owned.loans.empty());
  CHTHOLLY_TEST_CHECK(owned.findNode("next.call")->source_inst.hasValue());

  auto invalid = buildIteratorDesugaring(
      *sem_ir, main_function, {.binding = IteratorBindingKind::Shared,
                .item_defer = false,
                .has_continue = true,
                .has_break = true,
                .has_return = true});
  invalid.loans.front().end_scope = 1;
  std::string graph_error;
  CHTHOLLY_TEST_CHECK(!validateIteratorProjectionLoans(invalid, graph_error));
  CHTHOLLY_TEST_CHECK(graph_error.find("Item scope") != std::string::npos);
}
} // namespace

int main() {
  expectVecSuccess("vec-api-load.cns");
  expectVecSuccess("vec-shared-iterator-read.cns");
  expectVecSuccess("vec-loan-ends-before-write.cns");
  expectVecSuccess("vec-mutable-iterator-next.cns");
  expectVecSuccess("vec-iterator-expanded-control.cns");
  expectVecFailure("vec-iterator-relocation.cns",
                   "chtholly.next.sem.borrow.region-conflict");
  expectVecFailure("vec-mutable-iterator-read.cns",
                   "chtholly.next.sem.borrow.region-conflict");
  expectVecFailure("vec-moved-iterator-relocation.cns",
                   "chtholly.next.sem.borrow.region-conflict");
  expectVecFailure("vec-iterator-escape.cns",
                   "chtholly.next.sem.ownership.borrow-return-escape");
  expectVecFailure("vec-element-relocation.cns",
                   "chtholly.next.sem.borrow.region-conflict");
  expectIteratorDesugaringPrototype();
  expectSuccess("array-static-move-ok.cns");
  expectSuccess("tuple-static-move-ok.cns");
  expectSuccess("slice-read-ok.cns");
  expectSuccess("slice-shared-borrows-ok.cns");
  expectFailure("slice-move-rejected.cns",
                "chtholly.next.sem.move.unavailable");
  expectFailure("slice-initialize-rejected.cns",
                "chtholly.next.sem.assign.invalid-place");
  expectFailure("array-dynamic-borrow-conflict.cns",
                "chtholly.next.sem.borrow.region-conflict");
  return 0;
}
