#include "chtholly/Compiler/CompilationUnit.h"

#include "chtholly/Core/Arena.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/IncrementalDependencies.h"
#include "chtholly/Compiler/InteropArtifact.h"
#include "chtholly/Compiler/LLVM.h"
#include "chtholly/Compiler/Lexer.h"
#include "chtholly/Compiler/LowIR.h"
#include "chtholly/Compiler/LowerToLowIR.h"
#include "chtholly/Compiler/ParseTree.h"
#include "chtholly/Compiler/Parser.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/Semantics.h"
#include "chtholly/Compiler/SharedValueStores.h"
#include "chtholly/Compiler/Source.h"
#include "chtholly/Compiler/Token.h"
#include "chtholly/Compiler/TokenBuffer.h"
#include "chtholly/Compiler/TypeConcurrency.h"
#include "chtholly/Support/FileSystem.h"

#include "CompilationUnitInternal.h"

#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler {

namespace {
std::string modulePathText(const ParseTree &tree, NodeId path) {
  if (tree.kind(path) != NodeKind::ModulePath)
    return {};
  std::string result;
  for (const auto component : tree.children(path)) {
    if (tree.kind(component) != NodeKind::Name)
      return {};
    if (!result.empty())
      result += "::";
    result += tree.tokens().text(tree.token(component));
  }
  return result;
}

#include "CompilationUnitNominalArtifacts.inc"
} // namespace

CompilationUnit::CompilationUnit(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CompilationUnit::CompilationUnit(CompilationUnit &&) noexcept = default;
CompilationUnit &
CompilationUnit::operator=(CompilationUnit &&) noexcept = default;
CompilationUnit::~CompilationUnit() = default;

bool CompilationUnit::success() const {
  return impl_->completed;
}
bool CompilationUnit::wasReused() const {
  return impl_->reused;
}
bool CompilationUnit::hasEntryPoint() const {
  if (!impl_->parse_tree)
    return false;
  const auto root =
      NodeId(static_cast<std::uint32_t>(impl_->parse_tree->size() - 1));
  for (const auto declaration : impl_->parse_tree->children(root)) {
    if (impl_->parse_tree->kind(declaration) != NodeKind::FunctionDecl)
      continue;
    const auto children = impl_->parse_tree->children(declaration);
    for (const auto child : children) {
      if (impl_->parse_tree->kind(child) != NodeKind::Name)
        continue;
      if (impl_->parse_tree->tokens().text(impl_->parse_tree->token(child)) ==
          "main")
        return true;
      break;
    }
  }
  return false;
}
CompilationUnitKind CompilationUnit::kind() const {
  return impl_->kind;
}
CheckIRId CompilationUnit::checkIRId() const {
  return impl_->id;
}
std::string_view CompilationUnit::sourcePath() const {
  return impl_->source.filename();
}
const SourceBuffer &CompilationUnit::source() const {
  return impl_->source;
}
const ParseTree *CompilationUnit::parseTree() const {
  return impl_->parse_tree ? &*impl_->parse_tree : nullptr;
}
const CFDLSyntaxFile *CompilationUnit::cfdlSyntax() const {
  return impl_->cfdl_syntax ? &*impl_->cfdl_syntax : nullptr;
}
std::span<const Diagnostic> CompilationUnit::diagnostics() const {
  return impl_->diagnostics.diagnostics();
}
std::string_view CompilationUnit::moduleName() const {
  return impl_->module_name.hasValue()
             ? impl_->values->identifier(impl_->module_name)
             : std::string_view{};
}
const SemIR *CompilationUnit::semIR() const {
  return impl_->sem_ir ? &*impl_->sem_ir : nullptr;
}
PublicInterfaceId CompilationUnit::publicInterfaceId() const {
  return impl_->public_interface;
}
const PublicInterface *CompilationUnit::publicInterface() const {
  return impl_->public_interfaces->tryGet(impl_->public_interface);
}
StableFingerprint CompilationUnit::publicInterfaceFingerprint() const {
  const auto *interface = publicInterface();
  return interface ? interface->fingerprint() : StableFingerprint{};
}
const LowIR *CompilationUnit::lowIR() const {
  return impl_->low_ir ? &*impl_->low_ir : nullptr;
}
std::span<const NominalTypeSpecificArtifact>
CompilationUnit::nominalTypeSpecificArtifacts() const {
  return impl_->nominal_type_specifics;
}
std::span<const NominalSemanticWitnessArtifact>
CompilationUnit::nominalSemanticWitnessArtifacts() const {
  return impl_->nominal_semantic_witnesses;
}
std::span<const NominalTypeLayoutArtifact>
CompilationUnit::nominalTypeLayoutArtifacts() const {
  return impl_->nominal_type_layouts;
}
std::span<const ComponentExportLoweringPlan>
CompilationUnit::componentExports() const {
  return impl_->component_exports;
}

CompilationSession::CompilationSession(
    std::string target_triple, std::string package_name,
    std::vector<std::string> resolved_features,
    StableFingerprint compile_toolchain_fingerprint,
    PackageProvenance provenance, LanguageContract language_contract,
    std::vector<CompilerIntrinsicBinding> compiler_intrinsics,
    std::optional<CFFIReceiptIdentity> cffi_identity)
    : impl_(std::make_unique<Impl>(
          std::move(target_triple), std::move(package_name),
          std::move(resolved_features), compile_toolchain_fingerprint,
          std::move(provenance), language_contract,
          std::move(compiler_intrinsics), std::move(cffi_identity))) {}
CompilationSession::CompilationSession(CompilationSession &&) noexcept =
    default;
CompilationSession &
CompilationSession::operator=(CompilationSession &&) noexcept = default;
CompilationSession::~CompilationSession() = default;

bool CompilationSession::setRuntimeSymbolMappings(
    std::vector<std::pair<std::string, std::string>> mappings,
    std::string &error) {
  error.clear();
  if (impl_->compilation_started) {
    error = "runtime symbol mappings must be configured before compilation";
    return false;
  }
  std::ranges::sort(mappings);
  std::unordered_set<std::string> source_symbols;
  std::unordered_set<std::string> runtime_symbols;
  for (const auto &mapping : mappings) {
    if (mapping.first.empty() || mapping.second.empty()) {
      error = "runtime symbol mappings cannot contain an empty symbol";
      return false;
    }
    if (!source_symbols.insert(mapping.first).second ||
        !runtime_symbols.insert(mapping.second).second) {
      error = "runtime symbol mappings must be unique";
      return false;
    }
  }
  impl_->runtime_symbol_mappings = std::move(mappings);
  return true;
}

bool CompilationSession::loadInteropBundle(const std::string &path,
                                           std::string_view expected_package,
                                           std::string &error) {
  error.clear();
  if (impl_->compilation_started) {
    error = "cannot load an Interop bundle after compilation starts";
    return false;
  }
  interop::ArtifactBundle bundle;
  if (!interop::readArtifactBundle(path, bundle, error))
    return false;
  for (const auto &record : bundle.records) {
    if (record.reference.canonical_package != expected_package) {
      error = "Interop bundle contains a reference for a different package";
      return false;
    }
  }
  return impl_->interop_registry->registerBundle(bundle, error);
}

bool CompilationSession::exportInteropBundle(const std::string &path,
                                             std::string &error) const {
  error.clear();
  if (!impl_->compilation_started) {
    error = "cannot export an Interop bundle before compilation starts";
    return false;
  }
  auto bundle = impl_->interop_registry->exportBundle(error);
  if (!error.empty())
    return false;
  std::erase_if(bundle.records, [&](const auto &record) {
    return record.reference.canonical_package != impl_->package_name;
  });
  std::error_code file_error;
  const auto parent = pathForFileSystem(path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, file_error);
  if (file_error) {
    error = "failed to create Interop sidecar directory: " +
            file_error.message();
    return false;
  }
  return interop::writeArtifactBundle(bundle, path, error);
}

CheckIRId CompilationSession::addUnit(SourceInput source,
                                      CompilationUnitKind kind) {
  if (impl_->compilation_started)
    throw std::logic_error(
        "cannot add a unit after session compilation starts");
  const auto id = CheckIRId(static_cast<std::uint32_t>(impl_->units.size()));
  if (kind >= CompilationUnitKind::Count)
    throw std::invalid_argument("invalid compilation unit kind");
  auto data = std::make_unique<CompilationUnit::Impl>(
      id, std::move(source), kind, impl_->values, impl_->public_interfaces,
      impl_->interop_registry);
  impl_->units.push_back(
      std::unique_ptr<CompilationUnit>(new CompilationUnit(std::move(data))));
  return id;
}

#include "CompilationUnitSessionCompile.inc"

CompilationOutcome
CompilationSession::compileRequest(std::string &error,
                                   const CompilationRequest &request) {
  const auto success = compile(error, request);
  if (success)
    return CompilationOutcome::Success;
  if (request.is_cancelled && request.is_cancelled()) {
    error.clear();
    return CompilationOutcome::Cancelled;
  }
  return CompilationOutcome::Failed;
}

const CompilationUnit &CompilationSession::unit(CheckIRId id) const {
  if (!id.hasValue() || id.index >= impl_->units.size())
    throw std::out_of_range("invalid compilation unit ID");
  return *impl_->units[id.index];
}

std::size_t CompilationSession::unitCount() const {
  return impl_->units.size();
}

const PublicInterfaceRegistry &CompilationSession::publicInterfaces() const {
  return *impl_->public_interfaces;
}

const IncrementalCompilationPlan &CompilationSession::compilationPlan() const {
  if (!impl_->compilation_plan)
    throw std::logic_error(
        "compilation plan is unavailable before planning completes");
  return *impl_->compilation_plan;
}

const CompilerPackageCheckArtifact &
CompilationSession::packageCheckArtifact() const {
  if (!impl_->package_check_artifact)
    throw std::logic_error(
        "package check artifact is unavailable before successful checking");
  return *impl_->package_check_artifact;
}

const CompilerPackageArtifactManifest &CompilationSession::packageManifest() const {
  if (!impl_->package_manifest)
    throw std::logic_error(
        "compilation state is unavailable before successful compilation");
  return *impl_->package_manifest;
}

std::optional<ComponentContractArtifact>
CompilationSession::componentContract(std::string &error) const {
  ComponentContractArtifact contract;
  for (const auto &unit : impl_->units) {
    for (const auto &entry : unit->componentExports()) {
      if (contract.identity.empty()) {
        // Export IDs are identity-bound; the session caller supplies and
        // verifies the textual identity after construction.
      }
      contract.exports.push_back(entry.artifact);
    }
  }
  if (contract.exports.empty()) {
    error = "compilation session has no component exports";
    return std::nullopt;
  }
  // Recovering the identity from an ID is intentionally impossible. The
  // compile request stores it on each unit through a canonical session field.
  contract.identity = impl_->component_identity;
  contract.canonicalize();
  if (!contract.verify(error))
    return std::nullopt;
  return contract;
}

} // namespace chtholly::compiler
