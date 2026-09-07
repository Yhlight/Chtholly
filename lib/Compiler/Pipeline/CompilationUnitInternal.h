#pragma once

#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/LLVM.h"
#include "chtholly/Compiler/LowIR.h"
#include "chtholly/Compiler/ParseTree.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SharedValueStores.h"
#include "chtholly/Compiler/TokenBuffer.h"

namespace chtholly::compiler {

struct UnitImport {
  CheckIRId local_unit;
  PublicInterfaceId public_interface;
  ModuleIdentity provider;
  bool is_export = false;
  IdentifierId alias;
};

struct CompilationUnit::Impl {
  Impl(CheckIRId check_ir_id, SourceInput source_input,
       CompilationUnitKind unit_kind,
       std::shared_ptr<SharedValueStores> shared_values,
       std::shared_ptr<PublicInterfaceRegistry> shared_interfaces,
       std::shared_ptr<interop::ArtifactRegistry> shared_interop)
      : id(check_ir_id), kind(unit_kind), values(std::move(shared_values)),
        public_interfaces(std::move(shared_interfaces)),
        interop_registry(std::move(shared_interop)),
        source(std::move(source_input)) {}

  CheckIRId id;
  CompilationUnitKind kind;
  IdentifierId module_name;
  std::shared_ptr<SharedValueStores> values;
  std::shared_ptr<PublicInterfaceRegistry> public_interfaces;
  std::shared_ptr<interop::ArtifactRegistry> interop_registry;
  core::Arena arena;
  SourceBuffer source;
  DiagnosticEmitter diagnostics;
  std::optional<TokenBuffer> tokens;
  std::optional<ParseTree> parse_tree;
  std::optional<CFDLSyntaxFile> cfdl_syntax;
  std::optional<SemIR> sem_ir;
  PublicInterfaceId public_interface;
  NativeDefinitionExportClosure native_definition_exports;
  std::optional<LowIR> low_ir;
  std::optional<LLVMModule> llvm_module;
  std::string object_bytes;
  std::vector<NominalTypeSpecificArtifact> nominal_type_specifics;
  std::vector<NominalSemanticWitnessArtifact> nominal_semantic_witnesses;
  std::vector<NominalTypeLayoutArtifact> nominal_type_layouts;
  std::vector<ComponentExportLoweringPlan> component_exports;
  std::vector<UnitImport> imports;
  bool reused = false;
  bool completed = false;
};

struct CompilationSession::Impl {
  explicit Impl(std::string triple, std::string package,
                std::vector<std::string> features,
                StableFingerprint compile_toolchain,
                PackageProvenance package_provenance, LanguageContract contract,
                std::vector<CompilerIntrinsicBinding> intrinsics,
                std::optional<CFFIReceiptIdentity> cffi)
      : target_triple(std::move(triple)), package_name(std::move(package)),
        resolved_features(std::move(features)),
        compile_toolchain_fingerprint(compile_toolchain),
        provenance(std::move(package_provenance)), language_contract(contract),
        cffi_identity(std::move(cffi)),
        compiler_intrinsics(std::move(intrinsics)),
        values(std::make_shared<SharedValueStores>()),
        public_interfaces(std::make_shared<PublicInterfaceRegistry>(*values)),
        interop_registry(std::make_shared<interop::ArtifactRegistry>()) {
    std::ranges::sort(resolved_features);
    resolved_features.erase(
        std::unique(resolved_features.begin(), resolved_features.end()),
        resolved_features.end());
    package_name_id = values->internIdentifier(package_name);
  }

  std::string target_triple;
  std::string package_name;
  IdentifierId package_name_id;
  std::vector<std::string> resolved_features;
  StableFingerprint compile_toolchain_fingerprint;
  PackageProvenance provenance;
  LanguageContract language_contract;
  std::optional<CFFIReceiptIdentity> cffi_identity;
  std::vector<CompilerIntrinsicBinding> compiler_intrinsics;
  std::vector<std::pair<std::string, std::string>> runtime_symbol_mappings;
  std::string component_identity;
  std::shared_ptr<SharedValueStores> values;
  std::shared_ptr<PublicInterfaceRegistry> public_interfaces;
  std::shared_ptr<interop::ArtifactRegistry> interop_registry;
  std::vector<std::unique_ptr<CompilationUnit>> units;
  std::optional<IncrementalCompilationPlan> compilation_plan;
  std::optional<CompilerPackageCheckArtifact> package_check_artifact;
  std::optional<CompilerPackageArtifactManifest> package_manifest;
  bool compilation_started = false;
};

} // namespace chtholly::compiler
