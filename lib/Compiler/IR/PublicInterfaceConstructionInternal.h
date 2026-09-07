#pragma once

#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SemIR.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chtholly::compiler::internal {

class PublicTypeMappingScope {
public:
  PublicTypeMappingScope(
      GenericId &generic_slot,
      std::span<const SemInterfaceConstraint> &constraint_slot,
      GenericId generic,
      std::span<const SemInterfaceConstraint> constraints = {});
  ~PublicTypeMappingScope();

  PublicTypeMappingScope(const PublicTypeMappingScope &) = delete;
  PublicTypeMappingScope &operator=(const PublicTypeMappingScope &) = delete;

private:
  GenericId &generic_slot_;
  std::span<const SemInterfaceConstraint> &constraint_slot_;
  GenericId previous_generic_;
  std::span<const SemInterfaceConstraint> previous_constraints_;
};

struct PublicInterfaceTypeConstructionCallbacks {
  using EntityFingerprintFn = std::function<StableFingerprint(
      std::string_view, std::string_view, std::string_view,
      const std::optional<PublicEntityReferenceArtifact> &,
      PublicFunctionArtifact::MemberKind, std::uint32_t,
      std::span<const PublicType>, PublicType,
      const std::optional<PublicType> &, PublicFunctionExecutionKind,
      const PublicCoroutineConstructorABI &,
      const PublicNominalConstructorABI &, const CallableSemanticContract &,
      CompilerIntrinsicRole, const CallableOwnershipSummary &,
      const std::optional<GenericTemplateArtifact> &,
      PublicCallableDeclarationKind, bool, bool, std::string_view,
      const std::optional<ForeignAbiSignature> &, std::span<const std::string>,
      std::span<const std::optional<PublicConstantValue>>,
      std::span<const PublicInterfaceConstraintArtifact>,
      const std::optional<interop::ArtifactReference> &, std::string_view)>;

  EntityFingerprintFn entity_fingerprint;
  std::function<std::vector<std::string>(std::size_t,
                                         std::span<const std::string>)>
      canonical_parameter_names;
  std::function<std::vector<std::optional<PublicConstantValue>>(
      std::size_t, std::span<const std::optional<PublicConstantValue>>)>
      canonical_default_arguments;
};

class PublicInterfaceTypeConstructionContext {
public:
  PublicInterfaceTypeConstructionContext(
      const SemIR &sem_ir, PublicInterfaceRegistry &registry,
      IdentifierId package_name, std::string &error,
      PublicInterfaceTypeConstructionCallbacks callbacks);
  ~PublicInterfaceTypeConstructionContext();

  PublicInterfaceTypeConstructionContext(
      const PublicInterfaceTypeConstructionContext &) = delete;
  PublicInterfaceTypeConstructionContext &
  operator=(const PublicInterfaceTypeConstructionContext &) = delete;

  [[nodiscard]] std::optional<PublicType> mapType(TypeId type);
  [[nodiscard]] std::optional<PublicConstantValue> mapConstant(ConstantId id);
  [[nodiscard]] std::optional<PublicEntityReferenceArtifact>
  mapFunctionReference(FunctionRefId id);
  [[nodiscard]] bool buildNominals();

  [[nodiscard]] const std::vector<std::optional<PublicNominalTypeArtifact>> &
  nominalArtifacts() const;
  [[nodiscard]] GenericId &mappedGeneric();
  [[nodiscard]] std::span<const SemInterfaceConstraint> &mappedConstraints();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct PublicInterfaceClosureConstructionService {
  [[nodiscard]] static PublicInterfaceId finalizeAndRegister(
      const SemIR &sem_ir, PublicInterfaceRegistry &registry,
      IdentifierId package_name, std::string &error,
      NativeDefinitionExportClosure *native_exports,
      std::vector<PublicFunctionBindingSpec> &functions,
      std::vector<PublicNominalTypeArtifact> &nominals,
      std::vector<PublicValueArtifact> &values,
      std::vector<PublicInterfaceDeclarationArtifact> &interfaces,
      std::vector<PublicTypeAliasArtifact> &aliases,
      std::vector<PublicInterfaceWitnessArtifact> &witnesses);
};

struct PublicInterfaceDeclarationConstructionCallbacks {
  std::function<std::optional<PublicEntityReferenceArtifact>(FunctionRefId)>
      function_reference;
  std::function<StableFingerprint(const PublicInterfaceDeclarationArtifact &)>
      interface_fingerprint;
  std::function<StableFingerprint(const PublicTypeAliasArtifact &)>
      alias_fingerprint;
  std::function<StableFingerprint(const PublicInterfaceWitnessArtifact &)>
      witness_fingerprint;
  std::function<StableFingerprint(const PublicValueArtifact &)>
      value_fingerprint;
};

struct PublicInterfaceDeclarationConstructionService {
  [[nodiscard]] static bool
  build(const SemIR &sem_ir, PublicInterfaceRegistry &registry,
        IdentifierId package_name, std::string &error,
        NativeDefinitionExportClosure *native_exports,
        PublicInterfaceTypeConstructionContext &types,
        const std::unordered_map<std::uint32_t, std::size_t>
            &local_function_specs,
        PublicInterfaceDeclarationConstructionCallbacks callbacks,
        std::vector<PublicFunctionBindingSpec> &functions,
        std::vector<PublicValueArtifact> &values,
        std::vector<PublicInterfaceDeclarationArtifact> &interfaces,
        std::vector<PublicTypeAliasArtifact> &aliases,
        std::vector<PublicInterfaceWitnessArtifact> &witnesses);
};

struct PublicInterfaceFunctionConstructionResult {
  std::vector<PublicFunctionBindingSpec> functions;
  std::vector<PublicNominalTypeArtifact> nominals;
  std::vector<PublicValueArtifact> values;
  std::vector<PublicInterfaceDeclarationArtifact> interfaces;
  std::vector<PublicTypeAliasArtifact> aliases;
  std::vector<PublicInterfaceWitnessArtifact> witnesses;
  std::unordered_map<std::uint32_t, std::size_t> local_function_specs;
};

struct PublicInterfaceFunctionConstructionCallbacks {
  PublicInterfaceTypeConstructionCallbacks::EntityFingerprintFn
      entity_fingerprint;
  std::function<std::optional<GenericTemplateArtifact>(
      const SemIR &, const SemFunction &, std::string_view,
      const std::function<std::optional<PublicType>(TypeId)> &,
      const std::unordered_map<std::uint32_t, IdentifierId> &, std::string &)>
      build_generic_template;
  std::function<CallableOwnershipSummary(CallableOwnershipSummary,
                                         std::span<const PublicType>)>
      normalize_ownership;
  std::function<StableFingerprint(std::span<const PublicType>, PublicType,
                                  const interop::ForeignOperationArtifact &)>
      foreign_operation_fingerprint;
};

struct PublicInterfaceFunctionConstructionService {
  [[nodiscard]] static std::optional<PublicInterfaceFunctionConstructionResult>
  build(const SemIR &sem_ir, PublicInterfaceRegistry &registry,
        interop::ArtifactRegistry &interop_registry, IdentifierId package_name,
        std::string &error, PublicInterfaceTypeConstructionContext &types,
        PublicInterfaceFunctionConstructionCallbacks callbacks);
};

struct PublicInterfaceGenericTemplateConstructionService {
  [[nodiscard]] static std::optional<GenericTemplateArtifact>
  build(const SemIR &sem_ir, const SemFunction &function,
        std::string_view package_name,
        const std::function<std::optional<PublicType>(TypeId)> &map_type,
        const std::unordered_map<std::uint32_t, IdentifierId>
            &hidden_evaluator_targets,
        std::string &error);
};

} // namespace chtholly::compiler::internal
