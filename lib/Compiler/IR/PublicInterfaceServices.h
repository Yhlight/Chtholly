#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <tuple>

namespace chtholly::compiler::internal {

class PublicInterfaceIdentityService {
public:
  [[nodiscard]] static std::string moduleKey(std::string_view package,
                                             std::string_view module);
  [[nodiscard]] static std::string entityKey(
      std::string_view package, std::string_view module, std::string_view name,
      PublicEntityKind kind = PublicEntityKind::Function);
  [[nodiscard]] static std::string overloadEntityKey(
      std::string_view package, std::string_view module, std::string_view name,
      const std::optional<PublicEntityReferenceArtifact> &member_owner,
      PublicFunctionArtifact::MemberKind member_kind,
      std::uint32_t generic_parameter_count,
      std::span<const PublicType> parameters);
  [[nodiscard]] static std::string memberFunctionBindingKey(
      const PublicEntityReferenceArtifact &owner, std::string_view name);
};

// Function/ABI policy predicates shared by artifact verification and registry
// binding. These helpers are pure views over one public-interface artifact;
// they do not retain state or materialize alternate ABI representations.
using ForeignNominalResolver =
    std::function<std::optional<PublicType>(const PublicType &)>;

[[nodiscard]] bool validCoroutineConstructorABI(
    PublicFunctionExecutionKind execution_kind,
    const PublicCoroutineConstructorABI &constructor);
[[nodiscard]] bool validNominalConstructorABI(
    const CallableSemanticContract &contract, const PublicType &return_type,
    const PublicNominalConstructorABI &constructor);
[[nodiscard]] bool validReferenceProvenance(const PublicType &type,
                                            std::size_t parameter_count,
                                            bool allow_parameter);
[[nodiscard]] CallableOwnershipSummary normalizePublicOwnershipSummary(
    CallableOwnershipSummary summary, std::span<const PublicType> parameters);
[[nodiscard]] bool memberSignatureMatchesOwner(
    const std::optional<PublicEntityReferenceArtifact> &owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::span<const PublicType> parameters);
[[nodiscard]] std::optional<ForeignAbiValue> classifyForeignType(
    const PublicType &type, bool result,
    const ForeignNominalResolver &resolve_nominal = {});
[[nodiscard]] bool foreignSignatureMatches(
    std::span<const PublicType> parameters, const PublicType &return_type,
    const std::optional<ForeignAbiSignature> &stored,
    const ForeignNominalResolver &resolve_nominal = {},
    bool allows_hidden_parameters = false);
[[nodiscard]] bool ownershipMatchesSignature(
    std::span<const PublicType> parameters, const PublicType &return_type,
    const CallableOwnershipSummary &summary);
[[nodiscard]] bool semanticContractMatchesSignature(
    std::span<const PublicType> parameters, const PublicType &return_type,
    const CallableSemanticContract &contract);
[[nodiscard]] bool semanticContractMatchesEffects(
    const CallableSemanticContract &contract,
    const CallableOwnershipSummary &summary);

[[nodiscard]] bool publicTypesMayOverlap(const PublicType &lhs,
                                         const PublicType &rhs);
[[nodiscard]] bool interfaceWitnessesMayOverlap(
    const PublicInterfaceWitnessArtifact &lhs,
    const PublicInterfaceWitnessArtifact &rhs);
[[nodiscard]] StableFingerprint interfaceDeclarationFingerprint(
    const PublicInterfaceDeclarationArtifact &declaration);
[[nodiscard]] StableFingerprint typeAliasFingerprint(
    const PublicTypeAliasArtifact &alias);
[[nodiscard]] StableFingerprint interfaceWitnessFingerprint(
    const PublicInterfaceWitnessArtifact &witness);
[[nodiscard]] std::vector<std::string> canonicalParameterNames(
    std::size_t parameter_count, std::span<const std::string> names);
[[nodiscard]] std::vector<std::optional<PublicConstantValue>>
canonicalDefaultArguments(
    std::size_t parameter_count,
    std::span<const std::optional<PublicConstantValue>> arguments);
[[nodiscard]] bool sameSignature(
    const PublicEntity &entity, const PublicFunctionBindingSpec &function);

struct OwnershipNominalDefinition {
  std::uint32_t generic_parameter_count = 0;
  std::span<const PublicNominalFieldArtifact> fields;
  std::span<const PublicEnumVariantArtifact> variants;
  StableFingerprint fingerprint;
};
using OwnershipNominalResolver = std::function<std::optional<
    OwnershipNominalDefinition>(const PublicEntityReferenceArtifact &)>;
[[nodiscard]] bool validReturnLoanTypes(
    std::span<const PublicType> parameters, const PublicType &return_type,
    const CallableOwnershipSummary &summary,
    const OwnershipNominalResolver &resolve_nominal);
[[nodiscard]] bool validOwnershipSummaryTypes(
    std::span<const PublicType> parameters,
    const CallableOwnershipSummary &summary,
    const OwnershipNominalResolver &resolve_nominal);
[[nodiscard]] bool validCallbackOwnershipTypes(
    const PublicType &type, const OwnershipNominalResolver &resolve_nominal);
[[nodiscard]] bool templateMatchesSignature(
    const GenericTemplateArtifact &generic_template,
    std::uint32_t generic_parameter_count, std::span<const PublicType> parameters,
    PublicType return_type);

class PublicInterfaceCanonicalizeService {
public:
  static void callableOwnership(CallableOwnershipSummary &summary);
  static void condition(CallableConditionDescriptor &condition);
  static void foreignProtocol(ForeignResourceProtocol &protocol);
};

class PublicInterfaceValidationService {
public:
  [[nodiscard]] static bool callableSemanticContract(
      const CallableSemanticContract &contract, std::uint32_t generic_count,
      std::string &error,
      const std::function<bool(const PublicType &, std::uint32_t)> &valid_type);
};

class PublicInterfaceTypeValidationService {
public:
  [[nodiscard]] static bool validPublicType(
      const PublicType &type, std::uint32_t generic_parameter_count,
      bool allow_void = false);
  [[nodiscard]] static bool validEntityReference(
      const PublicEntityReferenceArtifact &entity, PublicEntityKind expected);
  [[nodiscard]] static bool validInterfaceConstraint(
      const PublicInterfaceConstraintArtifact &constraint,
      std::uint32_t generic_parameter_count);
  [[nodiscard]] static bool validCallbackRegistrationContract(
      const PublicType &type);
  [[nodiscard]] static bool validCallbackCompletionContract(
      const PublicType &type);
  [[nodiscard]] static bool validCallbackWakeContract(const PublicType &type);
};

class PublicInterfaceForeignOperationService {
public:
  [[nodiscard]] static StableFingerprint fingerprint(
      std::span<const PublicType> parameters, PublicType result,
      interop::ForeignOperationArtifact operation);
  [[nodiscard]] static bool valid(
      const interop::ForeignOperationArtifact &operation,
      std::span<const PublicType> parameters, PublicType result);
};

class PublicInterfaceConstantValidationService {
public:
  [[nodiscard]] static bool validValue(const PublicConstantValue &value,
                                       std::uint32_t depth = 0);
  [[nodiscard]] static bool validShape(
      const PublicConstantValue &value,
      std::span<const PublicNominalTypeArtifact> nominal_types);
};

class PublicInterfaceResolverService {
public:
  [[nodiscard]] static std::optional<PublicType> foreignNominal(
      const PublicInterfaceArtifact &artifact, const PublicType &type);
  [[nodiscard]] static const PublicNominalTypeArtifact *ownershipNominal(
      const PublicInterfaceArtifact &artifact,
      const PublicEntityReferenceArtifact &reference);
};

class GenericTemplateValidationService {
public:
  [[nodiscard]] static bool validType(const PublicType &type,
                                      std::uint32_t generic_count,
                                      bool allow_void);
  [[nodiscard]] static bool validReferenceType(
      const PublicType &type, std::size_t parameter_count,
      bool allow_parameter);
};

class PublicInterfaceVerifyService {
public:
  [[nodiscard]] static bool artifact(const PublicInterfaceArtifact &artifact,
                                     std::string &error);
  struct ValueVerificationState {
    std::function<bool(const PublicType &, std::uint32_t, bool)> valid_type;
    std::function<bool(const PublicType &, std::size_t, bool)>
        valid_reference_provenance;
    std::function<bool(const PublicConstantValue &,
                       std::span<const PublicNominalTypeArtifact>)>
        valid_constant_shape;
    std::function<StableFingerprint(const PublicValueArtifact &)>
        value_fingerprint;
    std::function<std::string(const PublicEntityReferenceArtifact &,
                              std::string_view)>
        member_function_binding_key;
  };
  [[nodiscard]] static bool interfaceValue(const PublicInterface &interface_value,
                                           std::string &error,
                                           const ValueVerificationState &state);
};

class PublicInterfaceArtifactVerificationService {
public:
  struct FunctionValidationCallbacks {
    std::function<bool(const PublicType &, std::uint32_t, bool)> valid_type;
    std::function<bool(PublicFunctionExecutionKind,
                       const PublicCoroutineConstructorABI &)>
        valid_coroutine_constructor;
    std::function<bool(const CallableSemanticContract &, const PublicType &,
                       const PublicNominalConstructorABI &)>
        valid_nominal_constructor;
    std::function<bool(const PublicType &, std::size_t, bool)>
        valid_reference_provenance;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const CallableSemanticContract &)>
        semantic_contract_matches_signature;
    std::function<bool(const CallableSemanticContract &,
                       const CallableOwnershipSummary &)>
        semantic_contract_matches_effects;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const CallableOwnershipSummary &)>
        ownership_matches_signature;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const CallableOwnershipSummary &)>
        valid_return_loan_types;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const std::optional<ForeignAbiSignature> &, bool)>
        foreign_signature_matches;
    std::function<bool(const PublicConstantValue &)> valid_constant_shape;
    std::function<bool(const interop::ArtifactReference &,
                       std::span<const PublicType>, PublicType)>
        valid_foreign_operation;
    std::function<bool(
        const std::optional<PublicEntityReferenceArtifact> &,
        PublicFunctionArtifact::MemberKind, std::span<const PublicType>)>
        member_signature_matches_owner;
    std::function<bool(const GenericTemplateArtifact &, std::uint32_t,
                       std::span<const PublicType>, PublicType)>
        template_matches_signature;
    std::function<bool(const PublicInterfaceConstraintArtifact &,
                       std::uint32_t)>
        valid_constraint;
  };
  using FunctionOrderKey = std::tuple<
      bool, std::string_view, std::string_view, std::string_view,
      std::string_view, PublicFunctionArtifact::MemberKind, std::uint32_t,
      std::string>;
  using FunctionOrderKeyFn =
      std::function<FunctionOrderKey(const PublicFunctionArtifact &)>;
  using FunctionFingerprintFn =
      std::function<StableFingerprint(const PublicFunctionArtifact &)>;
  using FunctionVerifyFn =
      std::function<bool(const PublicFunctionArtifact &, std::string &)>;
  using NominalVerifyFn = std::function<bool(
      const PublicNominalTypeArtifact &, std::string &)>;
  using EntityReferenceFn = std::function<bool(
      const PublicEntityReferenceArtifact &, PublicEntityKind)>;
  using ConstraintVerifyFn = std::function<bool(
      const PublicInterfaceConstraintArtifact &, std::uint32_t)>;
  using TypeVerifyFn = std::function<bool(const PublicType &, std::uint32_t,
                                          bool)>;
  using InterfaceFingerprintFn = std::function<StableFingerprint(
      const PublicInterfaceDeclarationArtifact &)>;
  using ConstantShapeFn = std::function<bool(
      const PublicConstantValue &,
      std::span<const PublicNominalTypeArtifact>)>;
  using ValueFingerprintFn =
      std::function<StableFingerprint(const PublicValueArtifact &)>;
  using ArtifactFingerprintFn = std::function<StableFingerprint(
      std::string_view, std::string_view,
      std::span<const PublicFunctionArtifact>,
      std::span<const PublicNominalTypeArtifact>,
      std::span<const PublicValueArtifact>,
      std::span<const PublicInterfaceDeclarationArtifact>,
      std::span<const PublicTypeAliasArtifact>,
      std::span<const PublicInterfaceWitnessArtifact>)>;
  // Complete artifact verification is coordinated here. The callbacks carry
  // policy-level type/ABI predicates from the public-interface domain while
  // this service owns phase ordering, identity checks, and cross-section
  // invariants. All callbacks are non-owning and operate on the one artifact.
  struct VerificationCallbacks {
    FunctionValidationCallbacks function;
    FunctionOrderKeyFn order_key;
    FunctionFingerprintFn function_fingerprint;
    NominalVerifyFn verify_nominal;
    EntityReferenceFn valid_entity_reference;
    ConstraintVerifyFn valid_constraint;
    TypeVerifyFn valid_type;
    InterfaceFingerprintFn interface_fingerprint;
    std::function<StableFingerprint(const PublicTypeAliasArtifact &)>
        alias_fingerprint;
    std::function<StableFingerprint(const PublicInterfaceWitnessArtifact &)>
        witness_fingerprint;
    ConstantShapeFn valid_constant_shape;
    ValueFingerprintFn value_fingerprint;
    ArtifactFingerprintFn artifact_fingerprint;
  };
  [[nodiscard]] static bool identity(const PublicInterfaceArtifact &artifact,
                                     std::string &error);
  [[nodiscard]] static bool verify(const PublicInterfaceArtifact &artifact,
                                   std::string &error,
                                   const VerificationCallbacks &callbacks);
  [[nodiscard]] static std::optional<PublicType> resolveForeignNominal(
      const PublicInterfaceArtifact &artifact, const PublicType &type);
  [[nodiscard]] static bool verifyValuesAndFingerprint(
      const PublicInterfaceArtifact &artifact, std::string &error,
      const ConstantShapeFn &valid_constant_shape,
      const ValueFingerprintFn &value_fingerprint,
      const ArtifactFingerprintFn &artifact_fingerprint);
  [[nodiscard]] static bool verifyFunctionIdentity(
      const PublicFunctionArtifact &function, FunctionOrderKey &previous_key,
      bool &has_previous_key, const FunctionOrderKeyFn &order_key,
      const FunctionFingerprintFn &fingerprint, std::string &error);
  // Runs the function-binding phase in canonical order. Validation of one
  // binding is supplied by the caller so this service remains independent of
  // the artifact's private type/ownership helper callbacks.
  [[nodiscard]] static bool verifyFunctions(
      const PublicInterfaceArtifact &artifact, std::string &error,
      const FunctionVerifyFn &verify_function,
      const FunctionOrderKeyFn &order_key,
      const FunctionFingerprintFn &fingerprint);
  [[nodiscard]] static bool verifyFunctionContract(
      const PublicFunctionArtifact &function,
      const FunctionValidationCallbacks &callbacks, std::string &error);
  [[nodiscard]] static bool verifyNominalsAndInterfaces(
      const PublicInterfaceArtifact &artifact, std::string &error,
      const NominalVerifyFn &verify_nominal,
      const EntityReferenceFn &valid_entity_reference,
      const ConstraintVerifyFn &valid_constraint,
      const TypeVerifyFn &valid_type,
      const InterfaceFingerprintFn &interface_fingerprint);
  [[nodiscard]] static bool verifyAliasesAndWitnesses(
      const PublicInterfaceArtifact &artifact, std::string &error,
      const EntityReferenceFn &valid_entity_reference,
      const TypeVerifyFn &valid_type,
      const std::function<StableFingerprint(
          const PublicTypeAliasArtifact &)> &alias_fingerprint,
      const std::function<StableFingerprint(
          const PublicInterfaceWitnessArtifact &)> &witness_fingerprint,
      const std::function<bool(const PublicInterfaceConstraintArtifact &,
                               std::uint32_t)> &constraint_verify);
};

class PublicInterfaceRegistryService {
public:
  using NominalResolver =
      std::function<std::optional<PublicType>(const PublicType &)>;
  struct ValidationCallbacks {
    std::function<bool(const GenericTemplateArtifact &, std::uint32_t,
                       std::span<const PublicType>, PublicType)>
        template_matches_signature;
    std::function<bool(const PublicType &, std::uint32_t, bool)> valid_type;
    std::function<bool(PublicFunctionExecutionKind,
                       const PublicCoroutineConstructorABI &)>
        valid_coroutine_constructor;
    std::function<bool(const CallableSemanticContract &, const PublicType &,
                       const PublicNominalConstructorABI &)>
        valid_nominal_constructor;
    std::function<bool(const PublicType &, std::size_t, bool)>
        valid_reference_provenance;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const CallableSemanticContract &)>
        semantic_contract_matches_signature;
    std::function<bool(const CallableSemanticContract &,
                       const CallableOwnershipSummary &)>
        semantic_contract_matches_effects;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const CallableOwnershipSummary &)>
        ownership_matches_signature;
    std::function<bool(std::span<const PublicType>, const PublicType &,
                       const std::optional<ForeignAbiSignature> &,
                       const NominalResolver &, bool)>
        foreign_signature_matches;
    std::function<bool(const interop::ArtifactReference &,
                       std::span<const PublicType>, PublicType)>
        valid_foreign_operation;
    std::function<bool(const PublicInterfaceConstraintArtifact &,
                       std::uint32_t)>
        valid_constraint;
    std::function<std::string(const PublicEntity &, std::string_view,
                              std::string_view, std::string_view)>
        entity_key;
    std::function<StableFingerprint(const PublicEntity &, std::string_view,
                                    std::string_view, std::string_view)>
        entity_fingerprint;
    std::function<StableFingerprint(const PublicInterface &)>
        interface_fingerprint;
  };

  [[nodiscard]] static PublicInterfaceId artifact(
      PublicInterfaceRegistry &registry, CheckIRId check_ir_id,
      const PublicInterfaceArtifact &artifact, std::string &error);
  [[nodiscard]] static bool verify(
      const PublicInterfaceRegistry &registry, std::string &error,
      const ValidationCallbacks &callbacks);
  [[nodiscard]] static bool registerArtifactClosure(
      PublicInterfaceRegistry &registry,
      std::span<const PublicInterfaceArtifact *const> artifacts,
      std::string &error);
};

class PublicInterfaceArtifactBuildService {
public:
  [[nodiscard]] static PublicInterfaceArtifact build(
      const PublicInterfaceRegistry &registry, PublicInterfaceId id,
      std::string &error);
};

class PublicInterfaceRegistryConstructionService {
public:
  [[nodiscard]] static std::optional<std::vector<PublicValueArtifact>>
  collectUniqueValues(std::span<const PublicValueArtifact> candidates,
                      std::span<const PublicNominalTypeArtifact> nominals,
                      std::string &error,
                      const std::function<bool(
                          const PublicConstantValue &,
                          std::span<const PublicNominalTypeArtifact>)>
                          &valid_constant_shape,
                      const std::function<StableFingerprint(
                          const PublicValueArtifact &)> &value_fingerprint);
  static void registerNominalTypes(
      PublicInterfaceRegistry &registry, PublicInterface &interface_value,
      CheckIRId check_ir_id, std::string_view package, std::string_view module,
      std::span<const PublicNominalTypeArtifact> nominal_types);
  [[nodiscard]] static bool registerInterfaceDeclarations(
      PublicInterfaceRegistry &registry, CheckIRId check_ir_id,
      std::string_view package, std::string_view module,
      std::span<const PublicInterfaceDeclarationArtifact> declarations,
      std::string &error);
  [[nodiscard]] static bool registerTypeAliases(
      PublicInterfaceRegistry &registry, CheckIRId check_ir_id,
      std::string_view package, std::string_view module,
      std::span<const PublicTypeAliasArtifact> aliases, std::string &error);
  [[nodiscard]] static bool registerInterfaceWitnesses(
      PublicInterfaceRegistry &registry,
      std::span<const PublicInterfaceWitnessArtifact> witnesses,
      const std::function<bool(const PublicType &, std::uint32_t, bool)> &
          valid_type,
      const std::function<bool(const PublicInterfaceConstraintArtifact &,
                               std::uint32_t)> &valid_constraint,
      const std::function<StableFingerprint(
          const PublicInterfaceWitnessArtifact &)> &fingerprint,
      const std::function<bool(const PublicInterfaceWitnessArtifact &,
                               const PublicInterfaceWitnessArtifact &)>
          &may_overlap,
      std::string &error);
};

} // namespace chtholly::compiler::internal

namespace chtholly::compiler {

// Builds the default policy callbacks for one artifact. The callback object
// borrows the artifact's canonical stores and is consumed immediately by the
// verification service; it never owns or copies artifact state.
[[nodiscard]] internal::PublicInterfaceArtifactVerificationService::
    VerificationCallbacks
    makeArtifactVerificationCallbacks(const PublicInterfaceArtifact &artifact);

} // namespace chtholly::compiler
