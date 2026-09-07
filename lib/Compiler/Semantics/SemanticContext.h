#pragma once

#include "SemanticCallResolution.h"
#include "SemanticControlFlow.h"
#include "SemanticConversion.h"
#include "SemanticLiteral.h"
#include "SemanticNameScopes.h"
#include "SemanticWitnessResolution.h"
#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"
#include "chtholly/Compiler/CarrierView.h"
#include "chtholly/Compiler/ConstantEvaluation.h"
#include "chtholly/Compiler/DeferredDefinitionWorklist.h"
#include "chtholly/Compiler/ForeignDeclaration.h"
#include "chtholly/Compiler/NominalCompletion.h"
#include "chtholly/Compiler/OperatorProtocol.h"
#include "chtholly/Compiler/PlaceState.h"
#include "chtholly/Compiler/ProgramModel.h"
#include "chtholly/Compiler/Semantics.h"
#include "chtholly/Compiler/TypeLayout.h"
#include "chtholly/Compiler/UnsafeAuthority.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <llvm/ADT/APInt.h>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler::semantics_internal {

inline constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
inline constexpr std::uint32_t UnionFieldIndexMask = ~UnionFieldUnsafeBit;

using ConversionKind = SemanticConversionKind;
using ConversionPlan = SemanticConversionPlan;

struct CallbackParameterRoles {
  std::array<std::uint32_t, 4> synthesized{
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex};
  std::vector<CallbackRegistrationBinding> bindings;
};

class SemanticContext {

public:
  SemanticContext(const ParseTree &tree, core::Arena &arena,
                  SharedValueStores &values, DiagnosticEmitter &diagnostics,
                  const PublicInterfaceRegistry &public_interfaces,
                  const interop::ArtifactRegistry &interop_registry,
                  CheckIRId check_ir_id, IdentifierId module_name,
                  std::span<const ImportIR> imports,
                  StableFingerprint semantic_options_fingerprint,
                  ConcreteSpecializationLoader specialization_loader,
                  std::uint32_t pointer_width,
                  std::string_view normalized_target_triple,
                  std::span<const CompilerIntrinsicBinding> compiler_intrinsics,
                  LanguageVersion language_version);

  SemIR run();

private:
  struct CacheableSpecific {
    StableFingerprint request_fingerprint;
    PublicEntityReferenceArtifact entity;
    std::vector<PublicType> arguments;
    std::vector<StableFingerprint> constraint_witnesses;
  };

  void emit(DiagnosticKind kind, NodeId node);
  void emitOwnershipDiagnostic(DiagnosticKind kind, InstId instruction,
                               std::span<const OwnershipEvidence> evidence,
                               NodeId fallback);

  [[nodiscard]] bool requireVersion(LanguageVersion minimum,
                                    DiagnosticKind diagnostic, NodeId node);

  [[nodiscard]] bool hasUnsafeAuthority() const;

  [[nodiscard]] bool requireUnsafe(UnsafeOperationKind operation, NodeId node);

  [[nodiscard]] IdentifierId tokenString(NodeId node) const;

  [[nodiscard]] NameId nameFor(NodeId node);

  struct ActiveLoop {
    NodeId label;
  };

  void pushLoop(NodeId label);

  [[nodiscard]] InstBlockId checkLoopBlock(NodeId label, NodeId block);
  void checkLoopBlockInto(NodeId label, NodeId block,
                          std::vector<InstId> &instructions);

  std::optional<std::size_t> resolveLoopTarget(std::span<const NodeId> children,
                                               NodeId node, bool is_break);

  [[nodiscard]] StringLiteralId decodeString(NodeId node);
  [[nodiscard]] std::optional<std::uint32_t> decodeChar(NodeId node);

  [[nodiscard]] bool hasObjectRepresentation(TypeId type) const;

  [[nodiscard]] TypeId checkType(NodeId node);

  [[nodiscard]] NominalTypeId
  materializeImportedNominalShell(PublicEntityId entity_id, NodeId location);

  [[nodiscard]] NominalTypeId
  materializeImportedNominal(PublicEntityId entity_id, NodeId location);

  [[nodiscard]] PublicEntityId
  resolveArtifactEntity(const PublicEntityReferenceArtifact &reference) const;

  [[nodiscard]] InterfaceId
  materializeImportedInterface(PublicEntityId entity_id, NodeId location);

  [[nodiscard]] TypeAliasId
  materializeImportedTypeAlias(PublicEntityId entity_id, NodeId location);

  [[nodiscard]] bool completeImportedNominal(PublicEntityId entity_id,
                                             NominalTypeId local,
                                             NodeId location);

  [[nodiscard]] NominalTypeId resolveNominalType(NodeId node);

  [[nodiscard]] PublicEntityId
  resolveImportedTypeEntity(NodeId node, PublicEntityKind kind) const;

  template <typename InstT>
  [[nodiscard]] InstId
  appendInst(std::vector<InstId> &block, NodeId node, TypeId type,
             typename InstT::Arg0Type arg0 = typename InstT::Arg0Type{},
             typename InstT::Arg1Type arg1 = typename InstT::Arg1Type{}) {
    const auto id = sem_ir_.addInst(InstT{type, arg0, arg1}, node);
    block.push_back(id);
    return id;
  }

  [[nodiscard]] InstId appendInvalid(std::vector<InstId> &block, NodeId node);

  [[nodiscard]] bool isCurrentAsyncBody() const;

  [[nodiscard]] InstId
  appendCoroutineRuntimeFault(std::vector<InstId> &block, NodeId node,
                              CoroutineRuntimeFaultReason reason);

  [[nodiscard]] InstId
  takeCoroutineChecked(InstId checked, NodeId node,
                       CoroutineRuntimeFaultReason fault_reason,
                       std::vector<InstId> &block);

  [[nodiscard]] InstId appendAsyncCall(std::vector<InstId> &block, NodeId node,
                                       FunctionRefId target,
                                       std::span<const InstId> arguments);

  struct ResultTypeShape {
    TypeId type;
    CanonicalResultShape shape;
  };

  [[nodiscard]] std::optional<ResultTypeShape>
  canonicalResultTypeShape(TypeId success, TypeId error, NodeId node,
                           DiagnosticKind missing_diagnostic);
  [[nodiscard]] std::optional<ResultTypeShape> foreignErrorResultTypeShape(
      TypeId raw_result, const interop::ForeignOperationArtifact &artifact,
      NodeId node);

  [[nodiscard]] InstId makeResultVariant(std::vector<InstId> &block,
                                         NodeId node, TypeId result_type,
                                         std::uint32_t variant, InstId payload);

  [[nodiscard]] InstId appendCancellationTerminal(InstId outcome, NodeId node,
                                                  std::vector<InstId> &block);

  void appendReturnTerminal(InstId value, NodeId node,
                            std::vector<InstId> &block);

  [[nodiscard]] InstId takeTaskPayload(LocalId task, bool error, TypeId payload,
                                       NodeId node, std::vector<InstId> &block);

  [[nodiscard]] InstId buildWaitOutcome(LocalId task, InstId outcome,
                                        NodeId node,
                                        std::vector<InstId> &block);

  [[nodiscard]] InstId checkWaitExpression(NodeId node,
                                           std::vector<InstId> &block);

  [[nodiscard]] InstId materializeTemporary(InstId value, NodeId node,
                                            std::vector<InstId> &block);

  [[nodiscard]] InstId temporaryBorrowSource(InstId value) const;

  [[nodiscard]] bool typeContainsReference(TypeId type,
                                           std::uint32_t depth = 0) const;

  [[nodiscard]] InstId temporaryBorrowEscapeSource(InstId value) const;

  [[nodiscard]] TypeId instType(InstId id) const;

  [[nodiscard]] bool isPlace(InstId value) const;

  [[nodiscard]] bool isMutablePlace(InstId value) const;

  [[nodiscard]] bool containsRawPointerDereference(InstId value) const;

  [[nodiscard]] InstId acquireCheckedReference(InstId value, NodeId node,
                                               std::vector<InstId> &block);

  [[nodiscard]] bool adjustMethodReceiver(InstId &receiver, TypeId expected,
                                          NodeId receiver_node,
                                          std::vector<InstId> &block);

  [[nodiscard]] bool canBindMethodReceiver(InstId receiver, TypeId expected,
                                           NodeId receiver_node) const;

  [[nodiscard]] TypeId typeForSuffix(NumericSuffix suffix);

  [[nodiscard]] bool integerMagnitudeFits(std::uint64_t magnitude,
                                          TypeId target,
                                          bool negative = false) const;

  [[nodiscard]] static bool
  integerMagnitudeExactlyRepresentableAsFloat(std::uint64_t magnitude,
                                              std::uint32_t width);

  [[nodiscard]] bool losslessNumericConversion(TypeId source,
                                               TypeId target) const;

  [[nodiscard]] ConversionPlan queryConversion(InstId value,
                                               TypeId target) const;

  [[nodiscard]] bool applyConversion(InstId &value, TypeId target, NodeId node,
                                     std::vector<InstId> &block,
                                     SemanticConversionPlan plan);

  [[nodiscard]] bool adjustExpression(InstId &value, TypeId target, NodeId node,
                                      std::vector<InstId> &block);

  [[nodiscard]] bool canAdjustExpression(InstId value, TypeId target) const;

  [[nodiscard]] static bool isStatementNode(NodeKind kind);

  struct CheckedValueBlock {
    std::vector<InstId> instructions;
    InstId value;
    NodeId value_node;
    bool falls_through = true;
  };

  [[nodiscard]] CheckedValueBlock checkValueBlock(NodeId node, TypeId target);

  [[nodiscard]] bool isReferenceFieldInitialization(InstId target) const;

  [[nodiscard]] bool isCoreCarrierName(NodeId node) const;

  [[nodiscard]] InstId checkCarrierView(NodeId call,
                                        std::span<const NodeId> children,
                                        std::vector<InstId> &block);

  [[nodiscard]] CanonicalTypeId
  substituteCanonicalType(CanonicalTypeId type_id, GenericId generic,
                          std::span<const CanonicalTypeId> arguments);

  [[nodiscard]] TypeId
  substituteType(TypeId type, GenericId generic,
                 std::span<const CanonicalTypeId> arguments);

  [[nodiscard]] bool isCallbackTransportType(TypeId type,
                                             bool allow_void = false) const;

  [[nodiscard]] bool contextualizeLiteral(InstId instruction, TypeId target);

  void checkLifecycleAttribute(NodeId attribute, SemNominalType &nominal);

  void checkRepresentationAttribute(NodeId attribute, SemNominalType &nominal);

  void checkFieldRepresentationAttribute(NodeId attribute,
                                         SemNominalField &field);

  [[nodiscard]] std::string typePath(NodeId node) const;

  [[nodiscard]] bool isCanonicalReference(TypeId type, NominalTypeId owner,
                                          SemReferenceMutability mutability);

  [[nodiscard]] TypeId nominalFieldType(TypeId nominal_type,
                                        const SemNominalField &field);

  struct ResolvedEnumVariant {
    TypeId type;
    std::uint32_t variant = core::AnyId::InvalidIndex;
    NodeId location;
  };

  [[nodiscard]] NominalTypeId
  resolveNominalComponents(std::span<const NodeId> components, NodeId location);
  [[nodiscard]] TypeId
  resolveNominalTypeComponents(std::span<const NodeId> components, NodeId location);

  [[nodiscard]] std::optional<ResolvedEnumVariant>
  resolveQualifiedEnumVariant(NodeId path);

  [[nodiscard]] std::optional<ResolvedEnumVariant>
  resolveEnumLiteral(NodeId node);

  [[nodiscard]] TypeId foreignResourceTargetPointer(FunctionRefId target);

  void validateUnboundForeignContracts();

  void predeclareTypeAlias(NodeId node);

  void completeTypeAlias(NodeId node);

  void predeclareInterface(NodeId node);

  void completeInterface(NodeId node);

  [[nodiscard]] InterfaceId
  resolveInterfaceSpecific(NodeId node, std::vector<TypeId> &arguments);

  [[nodiscard]] std::vector<SemInterfaceConstraint>
  checkConstraintList(NodeId node);

  void recordFunctionConstraints(FunctionId function, NodeId node);

  struct SemanticGenericEnvironment {
    GenericId generic;
    std::vector<std::pair<IdentifierId, std::uint32_t>> bindings;
    std::vector<SemInterfaceConstraint> constraints;
  };

  void captureFunctionGenericEnvironment(FunctionId function);
  void restoreFunctionGenericEnvironment(FunctionId function);

  void materializeImportedInterfaceWitnesses(NodeId location);

  [[nodiscard]] std::optional<std::vector<CanonicalTypeId>>
  deduceInterfaceWitnessArguments(const SemInterfaceWitness &witness,
                                  TypeId subject,
                                  std::span<const TypeId> arguments);

  [[nodiscard]] SemanticWitnessLookupResult
  instantiateInterfaceWitness(InterfaceWitnessId pattern_id, TypeId subject,
                              std::span<const TypeId> arguments,
                              NodeId location);

  [[nodiscard]] SemanticWitnessLookupResult
  lookupInterfaceWitness(TypeId subject, InterfaceId interface_id,
                         std::span<const TypeId> arguments,
                         NodeId location = NodeId::invalid());

  [[nodiscard]] std::vector<FunctionRefId>
  collectInterfaceMemberCandidates(TypeId subject, NameId name,
                                   NodeId location);

  struct IteratorProtocolResolution {
    InterfaceWitnessId witness;
    FunctionRefId next;
    TypeId item_type;
    TypeId step_type;
    std::uint32_t item_variant = core::AnyId::InvalidIndex;
    std::uint32_t done_variant = core::AnyId::InvalidIndex;
    CompilerIntrinsicRole intrinsic_role = CompilerIntrinsicRole::None;
  };

  [[nodiscard]] std::optional<IteratorProtocolResolution>
  resolveIteratorProtocol(TypeId iterator_type, NodeId location);

  void predeclareNominalType(NodeId node);

  void completeNominalType(NodeId node);

  [[nodiscard]] bool completeNominalTypeDefinition(NodeId node);

  void validateNominalTypeCycles();

  [[nodiscard]] bool isAtomicStorageType(TypeId type) const;

  [[nodiscard]] bool hasCRepresentation(TypeId type) const;

  void validateCRepresentationPolicies();

  struct ValueBinding {
    LocalId local;
    ConstantEntityId constant;
  };

  [[nodiscard]] StableFingerprint
  callableEnvironmentIdentity(SemCallableEnvironmentKind kind,
                              std::uint32_t ordinal,
                              FunctionRefId target = FunctionRefId::invalid());

  // Callable interfaces use one tuple argument while closure bodies retain
  // their source-level parameter list. Synthesize a small checked adapter so
  // both paths share the existing hidden body ABI.
  void synthesizeCallableWitness(
      TypeId environment_type, TypeId result_type,
      std::span<const TypeId> source_parameters, FunctionRefId hidden_ref,
      GenericId witness_generic, SemCallableEnvironmentCapability capability,
      std::string_view interface_name, NodeId location,
      std::string_view interface_module = "std::callable",
      std::string_view requirement_name = "invoke",
      std::optional<std::string_view> output_requirement = "Output",
      bool direct_parameters = false);

  [[nodiscard]] InstId synthesizeBoundMethod(
      NodeId node, std::vector<InstId> &block, InstId receiver,
      FunctionRefId target_template, FunctionRefId target,
      SemCallableEnvironmentCapability capability,
      std::span<const TypeId> source_parameters, TypeId result_type,
      std::span<const CanonicalTypeId> explicit_member_arguments);

  [[nodiscard]] InstId checkBoundMethodValue(NodeId node, InstId receiver,
                                             NodeId receiver_node, NameId name,
                                             std::vector<InstId> &block);

  [[nodiscard]] LocalId lookup(NameId name) const;

  [[nodiscard]] ValueBinding lookupValue(NameId name) const;

  void bind(NameId name, LocalId local, NodeId node);

  void bindConstant(NameId name, ConstantEntityId constant, NodeId node);

  void bindClosureCapture(NameId name, std::uint32_t field, NodeId node);

  [[nodiscard]] FunctionRefId materializeLocalSpecific(
      FunctionRefId template_ref, SpecificId specific_id,
      std::span<const CanonicalTypeId> type_arguments, NodeId call_node,
      bool declaration_only = false, bool enqueue_definition = true,
      std::span<const SemGenericSubstitution> dependent_substitutions = {},
      TypeId cached_function_type = TypeId::invalid());

  [[nodiscard]] std::optional<PublicType>
  concretePublicType(CanonicalTypeId type_id,
                     GenericId symbolic_generic = GenericId::invalid());

  [[nodiscard]] std::optional<PublicEntityReferenceArtifact>
  publicEntityReference(PublicEntityId id) const;

  [[nodiscard]] FunctionRefId
  tryLoadConcreteSpecific(const CacheableSpecific &requested, NodeId location);

  void drainSpecificQueue();

  void materializeRepresentationSpecifics();

  [[nodiscard]] std::optional<ConcreteSpecificNodeArtifact>
  buildConcreteSpecificNode(
      SpecificId specific_id,
      const std::unordered_map<std::uint32_t, std::uint32_t> &component_nodes,
      std::string &error);

  void buildConcreteSpecializationComponents();
  void buildConcreteContainerVTables();
  void buildTypedChannelDescriptors();

  void finalizeSpecificFingerprints();

  [[nodiscard]] FunctionRefId
  specializeFunction(FunctionRefId template_ref,
                     std::span<const InstId> arguments, NodeId call_node,
                     bool diagnose = true,
                     std::span<const CanonicalTypeId> explicit_arguments = {},
                     std::size_t parameter_offset = 0);

  [[nodiscard]] InstId
  checkEnumConstruction(NodeId node, const ResolvedEnumVariant &resolved,
                        std::vector<InstId> &block);

  struct EnumPatternCoverage {
    std::vector<bool> covered;
    bool wildcard = false;
  };

  struct CheckedCastShape {
    TypeId result;
    TypeId error;
    std::uint32_t ok_variant = core::AnyId::InvalidIndex;
    std::uint32_t err_variant = core::AnyId::InvalidIndex;
  };

  struct OrderingShape {
    TypeId type;
    std::array<std::uint32_t, 4> variants{};
  };

  [[nodiscard]] std::optional<OrderingShape>
  inspectOrderingShape(TypeId type, NodeId location, DiagnosticKind diagnostic);

  [[nodiscard]] std::optional<OrderingShape> orderingShape(NodeId location);

  [[nodiscard]] std::optional<CheckedCastShape>
  checkedCastShape(TypeId target, NodeId location);

  [[nodiscard]] InstId checkTryExpression(NodeId node,
                                          std::vector<InstId> &block);

  [[nodiscard]] TypeId ensureResultOutcomeType(TypeId result_type);

  bool addEnumPatternCoverage(EnumPatternCoverage &coverage,
                              std::optional<std::uint32_t> variant,
                              NodeId location);

  [[nodiscard]] InstId checkSwitchExpression(NodeId node,
                                             std::vector<InstId> &block,
                                             TypeId expected_type);

  [[nodiscard]] TypeId convergeExpressionTypes(std::span<const InstId> values,
                                               TypeId expected_type,
                                               NodeId location);

  [[nodiscard]] static std::optional<BuiltinOperatorKind>
  binaryOperator(TokenKind token);

  [[nodiscard]] static std::optional<BuiltinOperatorKind>
  compoundOperator(TokenKind token);

  [[nodiscard]] InstId appendBuiltinUnary(std::vector<InstId> &block,
                                          NodeId node, TypeId type,
                                          BuiltinOperatorKind operation,
                                          InstId operand);

  [[nodiscard]] InstId appendBuiltinBinary(std::vector<InstId> &block,
                                           NodeId node, TypeId type,
                                           BuiltinOperatorKind operation,
                                           InstId left, InstId right);

  [[nodiscard]] std::optional<llvm::APInt>
  integerLiteralBits(InstId value) const;

  [[nodiscard]] bool validateConstantBinary(BuiltinOperatorKind operation,
                                            InstId left, InstId right,
                                            NodeId node);

  [[nodiscard]] InstId checkBuiltinBinary(NodeId node,
                                          BuiltinOperatorKind operation,
                                          InstId left, InstId right,
                                          std::vector<InstId> &block,
                                          bool destination_typed = false);

  struct ResolvedOperatorProtocol {
    InterfaceId interface_id;
    std::uint32_t requirement_index = core::AnyId::InvalidIndex;
  };

  [[nodiscard]] std::optional<ResolvedOperatorProtocol>
  resolveOperatorProtocol(const OperatorProtocol &protocol, NodeId location);

  [[nodiscard]] bool borrowOperatorOperand(InstId &operand, TypeId value_type,
                                           SemReferenceMutability mutability,
                                           NodeId location,
                                           std::vector<InstId> &block);

  [[nodiscard]] InstId boolOr(InstId left, InstId right, NodeId location,
                              std::vector<InstId> &block);

  [[nodiscard]] InstId
  finishOperatorProtocolResult(InstId value, const OperatorProtocol &protocol,
                               NodeId location, std::vector<InstId> &block);

  [[nodiscard]] InstId
  checkOperatorProtocol(NodeId node, const OperatorProtocol &protocol,
                        InstId left, std::optional<InstId> right,
                        NodeId left_node, std::optional<NodeId> right_node,
                        std::vector<InstId> &block);

  [[nodiscard]] InstId checkShortCircuit(NodeId node, bool is_and,
                                         std::vector<InstId> &block);

  [[nodiscard]] InstId checkIfExpression(NodeId node,
                                         std::vector<InstId> &block,
                                         TypeId expected_type);

  [[nodiscard]] InstId checkClosureExpression(NodeId node,
                                              std::vector<InstId> &block);

  [[nodiscard]] InstId checkCallableValue(NodeId node, InstId callee,
                                          std::vector<InstId> &block);
  [[nodiscard]] InstId checkMemberValue(NodeId node, InstId base,
                                        std::vector<InstId> &block);
  [[nodiscard]] InstId checkCallExpression(NodeId node,
                                           std::vector<InstId> &block);

  [[nodiscard]] InstId
  checkExpression(NodeId node, std::vector<InstId> &block,
                  TypeId expected_type = TypeId::invalid());

  struct PatternProjectionStep {
    enum class Kind : std::uint8_t { Field, StaticIndex, EnumPayload };
    Kind kind = Kind::Field;
    std::uint32_t index = 0;
    NodeId location;
    TypeId type;
  };

  struct CheckedPatternBinding {
    NodeId node;
    NodeId name;
    TokenKind action = TokenKind::Invalid;
    TypeId type;
    std::vector<PatternProjectionStep> steps;
    std::vector<std::uint32_t> canonical_path;
  };

  struct CheckedPattern {
    std::vector<CheckedPatternBinding> bindings;
    bool has_rest = false;
    bool valid = true;
  };

  [[nodiscard]] bool
  resolvePatternProjectionTail(std::span<const NodeId> projections,
                               std::size_t begin, TypeId &current_type,
                               std::vector<PatternProjectionStep> &steps,
                               std::vector<std::uint32_t> &canonical_path);

  [[nodiscard]] bool
  patternPathCoversType(TypeId type, std::vector<std::uint32_t> &prefix,
                        const std::vector<CheckedPatternBinding> &bindings,
                        std::uint32_t depth = 0);

  [[nodiscard]] CheckedPattern checkPatternBindings(
      NodeId pattern, std::size_t binding_begin, TypeId source_type,
      std::optional<std::uint32_t> enum_variant = std::nullopt);

  [[nodiscard]] InstId
  emitPatternProjection(InstId source, std::uint32_t enum_variant,
                        const CheckedPatternBinding &binding,
                        std::vector<InstId> &block);

  [[nodiscard]] InstId
  applyPatternTransfer(InstId value, const CheckedPatternBinding &binding,
                       std::vector<InstId> &block);

  bool bindStructuredPattern(NodeId pattern, InstId source,
                             std::vector<InstId> &block, NodeId location,
                             bool mutable_local);

  void checkForeachStatement(NodeId node, std::vector<InstId> &block);

  void checkStatementImpl(NodeId node, std::vector<InstId> &block);

  void endFullExpression(NodeId node, std::vector<InstId> &block);

  void checkStatement(NodeId node, std::vector<InstId> &block);

  void checkBlockInto(NodeId node, std::vector<InstId> &block);
  void checkCallableBodyInto(NodeId node, std::vector<InstId> &block);
  [[nodiscard]] InstId
  checkScopedBlockExpression(NodeId node, NodeId value_block,
                             std::vector<InstId> &block,
                             TypeId expected_type = TypeId::invalid());

  [[nodiscard]] InstBlockId checkBlock(NodeId node);

  [[nodiscard]] NodeId functionChild(NodeId node, NodeKind kind) const;

  struct FunctionParameterSyntax {
    NodeId name;
    NodeId type;
    NodeId default_value;
    NodeId pattern = NodeId::invalid();
  };

  [[nodiscard]] std::vector<FunctionParameterSyntax>
  functionParameters(NodeId parameter_list) const;

  [[nodiscard]] std::vector<NodeId> flattenedFunctionParameters(
      std::span<const FunctionParameterSyntax> parameters) const;

  [[nodiscard]] std::optional<OwnershipRegion>
  contractRegion(NodeId node, std::span<const NodeId> parameter_nodes,
                 std::span<const TypeId> parameter_types);

  [[nodiscard]] std::optional<CallableOwnershipSummary>
  declaredContract(NodeId node, std::span<const NodeId> parameter_nodes,
                   std::span<const TypeId> parameter_types, TypeId return_type);

  void predeclareModuleConstant(NodeId node);

  void checkModuleConstant(NodeId node, std::vector<InstId> &declarations);

  void validateConstants(bool module_constants);

  [[nodiscard]] bool sameSourceTypePattern(CanonicalTypeId lhs,
                                           CanonicalTypeId rhs) const;

  [[nodiscard]] bool
  sameSourceParameterPattern(std::span<const TypeId> lhs,
                             std::span<const TypeId> rhs) const;

  [[nodiscard]] CompilerIntrinsicRole
  compilerIntrinsicRole(NameId function_name,
                        NominalTypeId semantic_owner) const;

  [[nodiscard]] std::optional<std::uint8_t> memoryOrder(InstId argument);

  [[nodiscard]] const interop::ForeignOperationArtifact *
  foreignOperationArtifact(FunctionRefId function_ref) const;

  [[nodiscard]] bool hasInitializationContract(FunctionRefId function_ref,
                                               std::size_t parameter) const;

  [[nodiscard]] bool isMutableReferenceParameter(LocalId local,
                                                 TypeId expected) const;

  [[nodiscard]] bool
  mayDeclareInitializationParameter(FunctionRefId function_ref,
                                    TypeId expected) const;

  [[nodiscard]] bool prepareInitializationArgument(FunctionRefId function_ref,
                                                   std::size_t parameter,
                                                   InstId &argument,
                                                   TypeId expected, NodeId node,
                                                   std::vector<InstId> &block);

  [[nodiscard]] bool isInitializationArgument(FunctionRefId function_ref,
                                              std::size_t parameter,
                                              InstId argument,
                                              TypeId expected) const;

  [[nodiscard]] InstId appendResolvedCall(std::vector<InstId> &block,
                                          NodeId node,
                                          FunctionRefId function_ref,
                                          TypeId result_type,
                                          std::span<const InstId> arguments);

  void predeclareFunction(
      NodeId node, NominalTypeId semantic_owner = NominalTypeId::invalid(),
      SemCanonicalFunctionRole semantic_role = SemCanonicalFunctionRole::None,
      bool force_public = false, GenericId impl_generic = GenericId::invalid(),
      TypeId semantic_owner_type = TypeId::invalid(),
      NameId semantic_projector = NameId::invalid(),
      bool ordinary_method = false);

  [[nodiscard]] FunctionRefId functionReference(FunctionId function) const;

  void predeclareInterfaceImpl(NodeId node);

  void predeclareCanonicalImpl(NodeId node);

  void validateLifecyclePolicies();

  void validateRepresentationInitializers();

  void validateRepresentationCarrierCycles();

  void validateObjectRepresentationProjections();

  void checkFunctionBody(NodeId node, std::vector<InstId> &declarations,
                         bool templates, bool const_functions);

  [[nodiscard]] TypeId
  mapImportedPublicType(const PublicType &type, GenericId generic,
                        NodeId location, bool require_nominal_complete = true);

  [[nodiscard]] FunctionRefId
  resolvePublicEntity(PublicEntityId canonical, NodeId node,
                      ImportIRInstId import_inst = ImportIRInstId::invalid());

  [[nodiscard]] std::vector<FunctionRefId>
  resolveImportedFunctions(NodeId node, bool diagnose = true);

  [[nodiscard]] FunctionRefId resolveImportedFunction(NodeId node);

  [[nodiscard]] std::optional<ConstantId>
  materializePublicConstantValue(const PublicConstantValue &source,
                                 NodeId location);

  [[nodiscard]] ConstantEntityId
  materializeImportedDefaultArgument(const PublicConstantValue &source,
                                     TypeId parameter_type, NodeId location);

  [[nodiscard]] std::optional<InstId>
  resolveImportedValue(NodeId node, std::vector<InstId> &block);

  [[nodiscard]] std::vector<FunctionRefId>
  resolveImportedMemberFunctions(PublicEntityId owner, IdentifierId method_name,
                                 NodeId location);

  [[nodiscard]] FunctionRefId
  resolveImportedMemberFunction(PublicEntityId owner, IdentifierId method_name,
                                NodeId location);

  struct ResolvedAssociatedFunctionValue {
    FunctionRefId function;
    NodeId occurrence;
  };

  [[nodiscard]] std::optional<ResolvedAssociatedFunctionValue>
  resolveAssociatedFunctionValue(NodeId node,
                                 TypeId expected_type = TypeId::invalid());

  [[nodiscard]] InstId makeFunctionValue(NodeId node, NodeId occurrence,
                                         FunctionRefId function_ref,
                                         std::vector<InstId> &block);

  [[nodiscard]] InstId makeForeignFunctionReference(NodeId node,
                                                    NodeId occurrence,
                                                    FunctionRefId function_ref,
                                                    std::vector<InstId> &block);

  const ParseTree &tree_;
  SharedValueStores &values_;
  DiagnosticEmitter &diagnostics_;
  SemIR sem_ir_;
  SemanticConversionPlanner conversions_;
  StableFingerprint semantic_options_fingerprint_;
  ConcreteSpecializationLoader specialization_loader_;
  std::uint32_t pointer_width_ = 64;
  std::span<const CompilerIntrinsicBinding> compiler_intrinsics_;
  SemanticNameScopes lexical_scopes_;
  std::unordered_map<std::uint32_t, ConstantEntityId> module_constant_names_;
  std::unordered_map<std::uint32_t, ConstantEntityId> constant_nodes_;
  std::unordered_map<std::string, ConstantEntityId> imported_constant_entities_;
  std::uint32_t imported_default_counter_ = 0;
  std::unordered_map<std::uint32_t, std::vector<FunctionRefId>> function_names_;
  std::unordered_map<std::uint32_t, CallbackParameterRoles> parameter_roles_;
  std::unordered_map<std::uint32_t, NominalTypeId> nominal_names_;
  std::unordered_map<std::uint32_t, NominalTypeId> nominal_nodes_;
  std::unordered_map<std::uint32_t, TypeAliasId> type_alias_names_;
  std::unordered_map<std::uint32_t, TypeAliasId> type_alias_nodes_;
  std::unordered_map<std::uint32_t, InterfaceId> interface_names_;
  std::unordered_map<std::uint32_t, InterfaceId> interface_nodes_;
  std::unordered_set<std::string> conformance_keys_;
  std::unordered_map<std::uint32_t, FunctionId> function_nodes_;
  std::vector<NodeId> function_declarations_;
  std::unordered_map<std::uint32_t, FunctionRefId> imported_function_refs_;
  std::unordered_map<std::uint32_t, NominalTypeId> imported_nominal_types_;
  std::unordered_map<std::uint32_t, InterfaceId> imported_interfaces_;
  std::unordered_map<std::uint32_t, std::uint8_t> imported_interface_states_;
  std::unordered_map<std::uint32_t, TypeAliasId> imported_type_aliases_;
  std::unordered_set<std::string> imported_witness_fingerprints_;
  std::unordered_map<std::string, InterfaceWitnessId>
      instantiated_interface_witnesses_;
  std::unordered_set<std::string> resolving_interface_witnesses_;
  bool imported_witnesses_materializing_ = false;
  bool imported_witnesses_materialized_ = false;
  NominalCompletionService completion_;
  std::unordered_map<std::uint32_t, FunctionRefId> generic_template_refs_;
  std::unordered_map<std::uint32_t, FunctionRefId> specific_function_refs_;
  std::unordered_map<std::uint32_t, CacheableSpecific> cacheable_specifics_;
  std::unordered_map<std::string, FunctionRefId> request_function_refs_;
  std::unordered_map<std::uint32_t, StableFingerprint>
      specific_component_fingerprints_;
  struct SpecificRequest {
    FunctionRefId template_ref;
    FunctionRefId specific_ref;
    SpecificId specific;
    std::vector<CanonicalTypeId> arguments;
    NodeId location;
    std::vector<SemGenericSubstitution> dependent_substitutions;
  };
  DeferredDefinitionWorklist<SpecificRequest> deferred_definitions_;
  std::unordered_map<std::uint32_t, std::uint32_t> generic_bindings_;
  std::unordered_map<std::uint32_t, SemanticGenericEnvironment>
      function_generic_environments_;
  std::vector<SemInterfaceConstraint> active_resolved_constraints_;
  NodeId active_constraint_list_;
  GenericId current_generic_;
  NominalTypeId current_impl_owner_;
  TypeId current_impl_owner_type_;
  FunctionId current_function_;
  LocalId current_owner_parameter_;
  LocalId active_closure_environment_;
  TypeId active_closure_environment_type_;
  NameId initializing_closure_capture_;
  TypeId return_type_;
  std::uint32_t unsafe_depth_ = 0;
  std::vector<ActiveLoop> active_loops_;
  std::uint32_t defer_depth_ = 0;
  std::uint32_t task_scope_depth_ = 0;
  std::uint32_t defer_loop_base_ = 0;
  std::vector<std::vector<InstId>> full_expression_temporaries_;
  std::uint32_t temporary_counter_ = 0;
  std::uint32_t closure_counter_ = 0;
  bool checking_callback_context_ = false;
};

} // namespace chtholly::compiler::semantics_internal
