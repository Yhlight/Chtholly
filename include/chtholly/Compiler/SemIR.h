#pragma once

#include "chtholly/Core/Arena.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/AnalysisMetrics.h"
#include "chtholly/Compiler/CompilationIds.h"
#include "chtholly/Compiler/ComponentABI2Protocol.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"
#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Compiler/ImportIR.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/Outcome.h"
#include "chtholly/Compiler/ParseTree.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chtholly::compiler {

namespace internal {
class SemIRVerificationContext;
}

struct InstId : core::IndexBase<InstId> {
  using IndexBase::IndexBase;
};
struct TypeId : core::IndexBase<TypeId> {
  using IndexBase::IndexBase;
};
struct NameId : core::IndexBase<NameId> {
  using IndexBase::IndexBase;
};
struct FunctionId : core::IndexBase<FunctionId> {
  using IndexBase::IndexBase;
};
struct FunctionRefId : core::IndexBase<FunctionRefId> {
  using IndexBase::IndexBase;
};
struct LocalId : core::IndexBase<LocalId> {
  using IndexBase::IndexBase;
};
struct ConstantId : core::IndexBase<ConstantId> {
  using IndexBase::IndexBase;
};
struct ConstantEntityId : core::IndexBase<ConstantEntityId> {
  using IndexBase::IndexBase;
};
struct SemPlaceId : core::IndexBase<SemPlaceId> {
  using IndexBase::IndexBase;
};

using InstBlockId = core::BlockId<struct InstBlockTag>;
using TypeBlockId = core::BlockId<struct TypeBlockTag>;
using LocalBlockId = core::BlockId<struct LocalBlockTag>;
using ConstantBlockId = core::BlockId<struct ConstantBlockTag>;

enum class SemInstKind : std::uint32_t {
#define CHTHOLLY_COMPILER_SEM_INST(Name, Arg0, Arg1) Name,
#include "chtholly/Compiler/SemIRKind.def"
  Count,
};

enum class SemArgKind : std::uint8_t {
  None,
  Inst,
  Type,
  Name,
  Function,
  FunctionRef,
  Local,
  Integer,
  String,
  Block,
  Constant,
};

struct NoSemArg : core::AnyId {
  constexpr NoSemArg() : AnyId(InvalidIndex) {}
  explicit constexpr NoSemArg(std::uint32_t raw) : AnyId(raw) {}
};

template <SemArgKind Kind> struct SemArgType;
template <> struct SemArgType<SemArgKind::None> {
  using type = NoSemArg;
};
template <> struct SemArgType<SemArgKind::Inst> {
  using type = InstId;
};
template <> struct SemArgType<SemArgKind::Type> {
  using type = TypeId;
};
template <> struct SemArgType<SemArgKind::Name> {
  using type = NameId;
};
template <> struct SemArgType<SemArgKind::Function> {
  using type = FunctionId;
};
template <> struct SemArgType<SemArgKind::FunctionRef> {
  using type = FunctionRefId;
};
template <> struct SemArgType<SemArgKind::Local> {
  using type = LocalId;
};
template <> struct SemArgType<SemArgKind::Integer> {
  using type = IntegerId;
};
template <> struct SemArgType<SemArgKind::String> {
  using type = StringLiteralId;
};
template <> struct SemArgType<SemArgKind::Block> {
  using type = InstBlockId;
};
template <> struct SemArgType<SemArgKind::Constant> {
  using type = ConstantEntityId;
};

template <SemArgKind Kind> using SemArgTypeT = typename SemArgType<Kind>::type;

template <SemInstKind KindValue, SemArgKind Arg0KindValue,
          SemArgKind Arg1KindValue>
struct TypedSemInst {
  using Arg0Type = SemArgTypeT<Arg0KindValue>;
  using Arg1Type = SemArgTypeT<Arg1KindValue>;
  static constexpr SemInstKind Kind = KindValue;
  static constexpr SemArgKind Arg0Kind = Arg0KindValue;
  static constexpr SemArgKind Arg1Kind = Arg1KindValue;

  TypeId type;
  Arg0Type arg0;
  Arg1Type arg1;
};

#define CHTHOLLY_COMPILER_SEM_INST(Name, Arg0, Arg1)                               \
  using Sem##Name =                                                            \
      TypedSemInst<SemInstKind::Name, SemArgKind::Arg0, SemArgKind::Arg1>;
#include "chtholly/Compiler/SemIRKind.def"

enum class SemTypeKind : std::uint32_t {
  Invalid,
  Void,
  Bool,
  Integer,
  Float,
  String,
  Array,
  Function,
  AsyncFunction,
  TypeParameter,
  Nominal,
  Reference,
  RawPointer,
  CFunctionPointer,
  CVariadicFunctionPointer,
  CallbackAdapter,
  CallbackRegistration,
  CallbackCompletion,
  CallbackWake,
  ForeignCompletion,
  ForeignWake,
  CoroutineExecutor,
  CoroutineScope,
  CoroutineTask,
  CoroutineTaskOutcome,
  CoroutineTaskCompletion,
  CoroutineTaskCompletionSet,
  CoroutineTaskSelection,
  CoroutineChecked,
  Never,
  Tuple,
  Slice,
  TypeProjection,
  Char,
  Count,
};

enum class SemTypeProjectionKind : std::uint8_t {
  Element,
  Pointee,
  Count,
};

struct SemTypeQueryArtifact {
  enum class Kind : std::uint8_t {
    TypeSame,
    TypeIs,
    TypeHas,
    ArrayExtent,
    TupleArity,
    Count,
  };

  Kind kind = Kind::Count;
  TypeId source;
  TypeId other;
  std::string property;
};

struct ForeignOperationStateOwner {
  PublicEntityReferenceArtifact operation;
  ForeignOperationStateKind state = ForeignOperationStateKind::Count;

  friend bool operator==(const ForeignOperationStateOwner &,
                         const ForeignOperationStateOwner &) = default;
};

enum class SemExprCategory : std::uint8_t {
  Error,
  Value,
  Place,
  Temporary,
  Diverging,
};

// Runtime ABI v1 reasons for compiler-generated coroutine protocol faults.
enum class CoroutineRuntimeFaultReason : std::uint32_t {
  TaskCreate = 1,
  CompletionArm = 2,
  TaskJoin = 3,
  TaskQuery = 4,
  TakeResult = 5,
  TakeError = 6,
  UnexpectedTerminal = 7,
  TaskGroup = 8,
};

enum class CallbackReleaseAuthority : std::uint8_t {
  Retained,
  Transferred,
  Count,
};

enum class SemReferenceMutability : std::uint8_t {
  ReadOnly,
  Mutable,
};

enum class SemLoanCarrierCapability : std::uint8_t {
  None,
  Shared,
  Mutable,
};

enum class SemReferenceProvenanceKind : std::uint8_t {
  Erased,
  Parameter,
};

struct SemInst {
  SemInstKind kind = SemInstKind::Invalid;
  std::uint32_t type = core::AnyId::InvalidIndex;
  std::uint32_t arg0 = core::AnyId::InvalidIndex;
  std::uint32_t arg1 = core::AnyId::InvalidIndex;
};

struct SemType {
  SemTypeKind kind = SemTypeKind::Invalid;
  std::uint32_t arg0 = core::AnyId::InvalidIndex;
  std::uint32_t arg1 = core::AnyId::InvalidIndex;
  std::uint32_t reserved = core::AnyId::InvalidIndex;

  friend bool operator==(const SemType &, const SemType &) = default;
};

struct SemTypeHash {
  std::size_t operator()(const SemType &type) const noexcept;
};

struct SemName {
  IdentifierId text;

  friend bool operator==(const SemName &, const SemName &) = default;
};

struct SemNameHash {
  std::size_t operator()(const SemName &name) const noexcept;
};

struct SemFunction {
  NameId name;
  TypeId type;
  LocalBlockId parameters;
  InstBlockId body;
  std::uint32_t flags = 0;
  GenericId generic;
  SpecificId specific;
  NominalTypeId semantic_owner;
  std::uint8_t semantic_role = 0;
  NameId semantic_projector;
  CompilerIntrinsicRole intrinsic_role = CompilerIntrinsicRole::None;
};

enum SemFunctionFlags : std::uint32_t {
  SemFunctionPublic = 1U << 0U,
  SemFunctionTemplate = 1U << 1U,
  SemFunctionSpecific = 1U << 2U,
  SemFunctionCoroutineScaffold = 1U << 3U,
  SemFunctionAsync = 1U << 4U,
  SemFunctionCoroutineExecutionEntry = 1U << 5U,
  SemFunctionCoroutineTaskDriver = 1U << 6U,
  SemFunctionConst = 1U << 7U,
  // A typed body materialized from a dependency artifact for constant
  // evaluation. It is never a native definition in the consuming module.
  SemFunctionEvaluatorArtifact = 1U << 8U,
  SemFunctionInterfaceMember = 1U << 9U,
};

enum SemLocalFlags : std::uint32_t {
  SemLocalMutable = 1U << 0U,
  SemLocalTemporary = 1U << 1U,
  SemLocalUninitialized = 1U << 2U,
};

enum class SemCallableDeclarationKind : std::uint8_t {
  Definition,
  Forward,
  Foreign,
  Count,
};

struct SemCallableDeclaration {
  SemCallableDeclarationKind kind = SemCallableDeclarationKind::Definition;
  bool is_unsafe = false;
  bool is_const = false;
  IdentifierId foreign_abi;
  IdentifierId external_symbol;
  std::optional<ForeignAbiSignature> foreign_signature;
  std::optional<interop::ArtifactReference> interop_artifact;
  std::optional<CallableOwnershipSummary> declared_contract;
  std::vector<IdentifierId> parameter_names;
  std::vector<ConstantEntityId> default_arguments;
};

struct SemFunctionRef {
  FunctionId local_function;
  ImportIRInstId import_ir_inst;
  TypeId local_type;
  PublicEntityId public_entity;
  GenericId generic;
  SpecificId specific;
};

// Describes the concrete signature a compiler intrinsic is allowed to expose
// after an imported public entity has been specialized. The type ids are
// local, already-materialized SemIR types; the helper compares them after
// canonical external materialization so callers cannot lower against a
// generic evaluator signature by accident.
struct CanonicalIntrinsicShapeSpec {
  std::span<const TypeId> parameter_types;
  TypeId return_type = TypeId::invalid();
  TypeId error_type = TypeId::invalid();
  bool has_error_type = false;
  std::optional<bool> is_async;
};

struct CanonicalIntrinsicResolution {
  FunctionRefId reference;
  TypeId function_type = TypeId::invalid();
  std::vector<TypeId> parameter_types;
  TypeId return_type = TypeId::invalid();
  TypeId error_type = TypeId::invalid();
  bool has_error_type = false;
};

struct SemLocal {
  NameId name;
  TypeId type;
  NodeId declaration;
  std::uint32_t flags = 0;
};

enum class ConstantValueKind : std::uint8_t {
  Integer,
  Float,
  Bool,
  String,
  Null,
  Array,
  Aggregate,
  Union,
  Enum,
  Tuple,
  ForeignEnum,
};

struct ConstantValue {
  ConstantValueKind kind = ConstantValueKind::Integer;
  TypeId type;
  std::uint64_t payload = 0;
  ConstantBlockId elements;
  bool target_dependent = false;

  friend bool operator==(const ConstantValue &,
                         const ConstantValue &) = default;
};

struct ConstantValueHash {
  std::size_t operator()(const ConstantValue &value) const noexcept;
};

enum class ConstantEvalState : std::uint8_t {
  Concrete,
  Symbolic,
  NotConstant,
  Error,
};

struct ConstantEvalResult {
  ConstantEvalState state = ConstantEvalState::NotConstant;
  ConstantId value;

  [[nodiscard]] bool isConcrete() const {
    return state == ConstantEvalState::Concrete && value.hasValue();
  }
};

struct SemConstant {
  NameId name;
  TypeId type;
  InstBlockId initializer;
  InstId value;
  NodeId declaration;
  std::uint32_t flags = 0;
  ConstantEvalResult result;
  StableFingerprint public_fingerprint;
  IdentifierId canonical_package;
  IdentifierId canonical_module;
  IdentifierId canonical_name;
};

struct SemForeignConstant {
  NameId name;
  TypeId type;
  PublicConstantValueKind kind = PublicConstantValueKind::Integer;
  std::uint64_t payload = 0;
  NodeId declaration;
};

enum SemConstantFlags : std::uint32_t {
  SemConstantPublic = 1U << 0U,
  SemConstantModule = 1U << 1U,
  SemConstantStatic = 1U << 2U,
  SemConstantImported = 1U << 3U,
};

struct SemNominalField {
  NameId name;
  TypeId type;
  NodeId declaration;
  std::uint32_t flags = 0;
  std::vector<NameId> storage_path;
  PublicObjectProjectionKind projection_kind =
      PublicObjectProjectionKind::StableAddress;
  NameId projector_name;
  std::vector<NameId> projection_region_path;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;
};

enum class SemEnumPayloadShape : std::uint8_t {
  Unit,
  Tuple,
  Struct,
};

struct SemEnumVariant {
  NameId name;
  std::vector<SemNominalField> fields;
  NodeId declaration;
  SemEnumPayloadShape shape = SemEnumPayloadShape::Unit;
  std::int64_t discriminant = 0;
};

struct SemObjectProjector {
  NameId name;
  GenericId generic;
  TypeId owner_pattern;
  FunctionId load_function;
  FunctionId store_function;
  FunctionId take_function;
  FunctionId init_function;
  FunctionId borrow_function;
  FunctionId borrow_mut_function;
};

struct SemForeignResourceOperation {
  NameId name;
  ForeignResourceRoleKind role = ForeignResourceRoleKind::Count;
  FunctionRefId target;
  std::uint32_t resource_parameter = core::AnyId::InvalidIndex;
  std::uint32_t completion_parameter = core::AnyId::InvalidIndex;
  TypeId source_result;

  friend bool operator==(const SemForeignResourceOperation &,
                         const SemForeignResourceOperation &) = default;
};

struct SemNominalMemberFunction {
  NameId name;
  FunctionRefId target;
  std::uint32_t flags = 0;

  friend bool operator==(const SemNominalMemberFunction &,
                         const SemNominalMemberFunction &) = default;
};

enum SemNominalMemberFunctionFlags : std::uint32_t {
  SemNominalMemberFunctionPublic = 1U << 0U,
  SemNominalMemberFunctionAssociated = 1U << 1U,
};

enum class SemNominalCompletionState : std::uint8_t {
  Unloaded,
  Completing,
  Complete,
  Failed,
};

struct SemNominalType {
  NameId name;
  GenericId generic;
  std::vector<SemNominalField> fields;
  NodeId declaration;
  std::uint32_t flags = 0;
  PublicEntityId canonical_entity;
  std::uint8_t lifecycle_copy = 0;
  std::uint8_t lifecycle_move = 0;
  std::uint8_t lifecycle_drop = 0;
  FunctionId lifecycle_copy_function;
  FunctionId lifecycle_drop_function;
  FunctionId representation_pack_function;
  FunctionId representation_init_function;
  GenericId lifecycle_copy_generic;
  GenericId lifecycle_drop_generic;
  GenericId representation_generic;
  TypeId lifecycle_copy_pattern;
  TypeId lifecycle_drop_pattern;
  TypeId representation_pattern;
  TypeId value_repr_pattern;
  TypeId object_repr_pattern;
  std::vector<SemObjectProjector> object_projectors;
  FunctionId object_init_function;
  FunctionId object_copy_init_function;
  FunctionId object_move_init_function;
  FunctionId object_drop_function;
  NominalRepresentationPolicy representation_policy =
      NominalRepresentationPolicy::Opaque;
  NominalKind kind = NominalKind::Struct;
  std::vector<SemEnumVariant> variants;
  bool is_value_enum = false;
  TypeId foreign_representation;
  ForeignResourceInvalidState foreign_invalid_state =
      ForeignResourceInvalidState::Count;
  std::int64_t foreign_invalid_integer = 0;
  TypeId foreign_handle_type;
  TypeId foreign_completion_handle_type;
  TypeId foreign_callback_type;
  TypeId foreign_waker_type;
  TypeId foreign_registration_storage_type;
  TypeId foreign_completion_storage_type;
  TypeId foreign_wake_storage_type;
  SemNominalCompletionState completion_state =
      SemNominalCompletionState::Unloaded;
  ForeignResourceProtocolId foreign_resource_protocol;
  std::vector<SemForeignResourceOperation> foreign_resource_operations;
  std::vector<SemNominalMemberFunction> member_functions;
};

enum class SemInterfaceRequirementKind : std::uint8_t {
  Function,
  AssociatedAlias,
};

struct SemInterfaceRequirement {
  SemInterfaceRequirementKind kind = SemInterfaceRequirementKind::Function;
  NameId name;
  TypeId type;
  FunctionRefId function;
  std::uint32_t binding_index = core::AnyId::InvalidIndex;
  bool has_default = false;
  NodeId declaration;
};

struct SemInterfaceConstraint {
  TypeId subject;
  InterfaceId interface_id;
  TypeBlockId arguments;
  InterfaceWitnessId witness;

  friend bool operator==(const SemInterfaceConstraint &,
                         const SemInterfaceConstraint &) = default;
};

struct SemGenericSubstitution {
  GenericId generic;
  std::vector<CanonicalTypeId> arguments;
};

struct SemInterface {
  NameId name;
  GenericId generic;
  std::uint32_t explicit_parameter_count = 0;
  std::vector<SemInterfaceConstraint> constraints;
  std::vector<SemInterfaceRequirement> requirements;
  NodeId declaration;
  std::uint32_t flags = 0;
  StableFingerprint fingerprint;
  PublicEntityId canonical_entity;
};

enum SemInterfaceFlags : std::uint32_t {
  SemInterfacePublic = 1U << 0U,
};

enum class SemInterfaceWitnessState : std::uint8_t {
  Declared,
  Complete,
  Invalid,
};

struct SemInterfaceWitnessEntry {
  std::uint32_t requirement = core::AnyId::InvalidIndex;
  FunctionRefId function;
  TypeId associated_type;
};

struct SemInterfaceWitness {
  InterfaceId interface_id;
  TypeId self_type;
  TypeBlockId interface_arguments;
  std::vector<SemInterfaceWitnessEntry> entries;
  NodeId declaration;
  SemInterfaceWitnessState state = SemInterfaceWitnessState::Declared;
  StableFingerprint fingerprint;
  GenericId generic;
  std::vector<SemInterfaceConstraint> constraints;
};

// A concrete container callback contract is produced while a generic body is
// materialized.  It deliberately stores semantic identities and witness
// fingerprints, rather than LLVM function pointers; code generation resolves
// those identities for the current target.  This keeps the specialization
// cache target-independent while still making the native bridge fail closed
// when a witness or layout does not match.
struct SemConcreteContainerVTable {
  TypeId container_type;
  TypeId key_type;
  TypeId value_type;
  bool hash_map = true;
  // These references are session-local lowering handles. Their stable
  // identities are carried separately by the fingerprints below; function
  // pointers are never serialized into an artifact.
  FunctionRefId key_hash_function;
  FunctionRefId key_equal_function;
  StableFingerprint key_type_fingerprint;
  StableFingerprint value_type_fingerprint;
  StableFingerprint layout_fingerprint;
  std::vector<StableFingerprint> constraint_witnesses;

  friend bool operator==(const SemConcreteContainerVTable &,
                         const SemConcreteContainerVTable &) = default;
};

// Session-local descriptor for a typed-channel payload. Stable fingerprints
// and canonical lifecycle identities are persisted separately in concrete
// specialization artifacts; FunctionRefIds are only lowering handles.
struct SemTypedChannelDescriptor {
  FunctionId owner_function;
  TypeId payload_type;
  FunctionRefId move_function;
  FunctionRefId drop_function;
  StableFingerprint payload_type_fingerprint;
  StableFingerprint layout_fingerprint;
  StableFingerprint lifecycle_fingerprint;
  TypeRepresentationFacts representation;
  TypeConcurrencyFacts concurrency;
  std::uint32_t runtime_abi_epoch = 1;
  std::optional<PublicEntityReferenceArtifact> move_target;
  std::optional<PublicEntityReferenceArtifact> drop_target;
  // Canonical protocol identity for repeated send/receive submissions. The
  // runtime ABI remains unchanged; this is a semantic consistency anchor.
  StableFingerprint outcome_fingerprint;
  std::string component_identity;
  std::string operation_identity;
  ComponentAbi2OperationKind operation_kind =
      ComponentAbi2OperationKind::Send;
  ComponentAbi2LeasePolicy lease_policy = ComponentAbi2LeasePolicy::Exclusive;
  std::uint8_t ownership_flags = 0;
  StableFingerprint component_descriptor_digest;

  friend bool operator==(const SemTypedChannelDescriptor &,
                         const SemTypedChannelDescriptor &) = default;
};

enum class SemTypeAliasState : std::uint8_t {
  Declared,
  Resolving,
  Complete,
  Invalid,
};

struct SemTypeAlias {
  NameId name;
  GenericId generic;
  TypeId target;
  std::vector<SemInterfaceConstraint> constraints;
  NodeId declaration;
  std::uint32_t flags = 0;
  SemTypeAliasState state = SemTypeAliasState::Declared;
  StableFingerprint fingerprint;
  PublicEntityId canonical_entity;
};

enum SemTypeAliasFlags : std::uint32_t {
  SemTypeAliasPublic = 1U << 0U,
};

struct SemObjectProjectionStep {
  TypeId aggregate_type;
  std::uint32_t field_index = 0;

  friend bool operator==(const SemObjectProjectionStep &,
                         const SemObjectProjectionStep &) = default;
};

enum class SemLifecycleCopyPolicy : std::uint8_t {
  Default,
  Delete,
  Custom,
};

enum class SemLifecycleMovePolicy : std::uint8_t {
  Default,
  Delete,
};

enum class SemLifecycleDropPolicy : std::uint8_t {
  Default,
  Custom,
};

using SemCanonicalFunctionRole = CallableSemanticRole;

struct SemCallableSemanticContract {
  CallableSemanticDomain domain = CallableSemanticDomain::Ordinary;
  CallableSemanticRole role = CallableSemanticRole::None;
  std::uint16_t capability = CallableCapabilityNone;
  NominalTypeId owner;
  std::uint32_t projector_field = core::AnyId::InvalidIndex;
  bool whole_carrier = false;
  std::vector<std::uint32_t> carrier_path;
  bool has_bit_range = false;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;

  friend bool operator==(const SemCallableSemanticContract &,
                         const SemCallableSemanticContract &) = default;
};

enum SemNominalTypeFlags : std::uint32_t {
  SemNominalTypePublic = 1U << 0U,
  SemNominalTypeArtifactDependency = 1U << 1U,
  SemNominalTypeClosureEnvironment = 1U << 2U,
  SemNominalTypeBoundMethodEnvironment = 1U << 3U,
};

enum class SemCallableEnvironmentCapability : std::uint8_t {
  ReadOnly,
  Mutable,
  Consuming,
  Count,
};

enum class SemCallableEnvironmentKind : std::uint8_t {
  Closure,
  BoundMethod,
  Count,
};

struct SemCallableEnvironmentInfo {
  SemCallableEnvironmentKind kind = SemCallableEnvironmentKind::Count;
  TypeId environment;
  // Target used to invoke the environment value. Bound methods additionally
  // retain their receiver-selected formation target below.
  FunctionRefId target;
  FunctionRefId formation_target;
  SemCallableEnvironmentCapability capability =
      SemCallableEnvironmentCapability::ReadOnly;
  StableFingerprint identity;
};

enum class SemSymbolOccurrenceKind : std::uint8_t {
  Declaration,
  Reference,
};

enum class SemSymbolTargetKind : std::uint8_t {
  Local,
  Function,
  Constant,
};

struct SemSymbolOccurrence {
  NodeId location;
  SemSymbolOccurrenceKind kind = SemSymbolOccurrenceKind::Reference;
  SemSymbolTargetKind target_kind = SemSymbolTargetKind::Local;
  std::uint32_t target = core::AnyId::InvalidIndex;
};

enum class SemMemberAccessKind : std::uint8_t {
  Instance,
  Associated,
};

struct SemMemberAccessContext {
  NodeId location;
  NominalTypeId owner;
  SemMemberAccessKind kind = SemMemberAccessKind::Instance;
};

struct SemModuleAccessContext {
  NodeId location;
  ImportIRId module;
};

enum class PlaceProjectionKind : std::uint8_t {
  CarrierView,
  Dereference,
  Field,
  Element,
  AnyElement,
  EnumPayload,
};

struct PlaceProjection {
  PlaceProjectionKind kind = PlaceProjectionKind::Field;
  std::uint32_t index = 0;
  std::uint32_t variant = core::AnyId::InvalidIndex;

  friend bool operator==(const PlaceProjection &,
                         const PlaceProjection &) = default;
};

struct SemPlace {
  LocalId root;
  TypeId type;
  std::vector<PlaceProjection> projections;
};

enum class PlaceStateKind : std::uint8_t {
  Initialized,
  Moved,
  MaybeMoved,
  PartiallyMoved,
};

enum class PlaceObservationKind : std::uint8_t {
  Read,
  Borrow,
  Move,
  Copy,
  Reinitialize,
};

struct PlaceStateObservation {
  InstId instruction;
  SemPlaceId place;
  PlaceObservationKind kind = PlaceObservationKind::Read;
  PlaceStateKind before = PlaceStateKind::Initialized;
  PlaceStateKind after = PlaceStateKind::Initialized;
};

enum class PlaceCleanupKind : std::uint8_t {
  Destroy,
  DestroyIfInitialized,
  RunDefer,
  EndLifetime,
};

struct PlaceCleanupAction {
  SemPlaceId place;
  PlaceCleanupKind kind = PlaceCleanupKind::Destroy;
  InstBlockId block;
  LocalId local;
};

struct PlaceCleanupPlan {
  std::vector<PlaceCleanupAction> actions;
};

struct SuspensionCleanupPartition {
  PlaceCleanupPlan pre_commit;
  PlaceCleanupPlan transferred;
};

struct PlaceReinitializationPlan {
  SemPlaceId target;
  std::vector<PlaceCleanupAction> old_value_cleanups;
};

struct CanonicalResultShape {
  TypeId success;
  TypeId error;
  std::uint32_t ok_variant = core::AnyId::InvalidIndex;
  std::uint32_t err_variant = core::AnyId::InvalidIndex;
};

struct CanonicalReadOutcomeShape {
  TypeId data;
  std::uint32_t data_variant = core::AnyId::InvalidIndex;
  std::uint32_t eof_variant = core::AnyId::InvalidIndex;
};

class SemIR;

class PlaceStateQuery {
public:
  PlaceStateQuery() = default;
  PlaceStateQuery(
      std::vector<SemPlace> places,
      std::unordered_map<std::uint32_t, SemPlaceId> move_places,
      std::unordered_map<std::uint32_t, SemPlaceId> copy_places,
      std::unordered_map<std::uint32_t, PlaceCleanupPlan> return_cleanups,
      std::unordered_map<std::uint32_t, PlaceCleanupPlan> edge_cleanups,
      std::unordered_map<std::uint32_t, PlaceCleanupPlan>
          full_expression_cleanups,
      std::unordered_map<std::uint32_t, PlaceCleanupPlan> block_cleanups,
      std::unordered_map<std::uint32_t, SuspensionCleanupPartition>
          suspension_cleanups,
      std::unordered_map<std::uint32_t, PlaceCleanupPlan> cancellation_cleanups,
      std::unordered_map<std::uint32_t, PlaceReinitializationPlan>
          reinitializations,
      std::vector<PlaceStateObservation> observations)
      : places_(std::move(places)), move_places_(std::move(move_places)),
        copy_places_(std::move(copy_places)),
        return_cleanups_(std::move(return_cleanups)),
        edge_cleanups_(std::move(edge_cleanups)),
        full_expression_cleanups_(std::move(full_expression_cleanups)),
        block_cleanups_(std::move(block_cleanups)),
        suspension_cleanups_(std::move(suspension_cleanups)),
        cancellation_cleanups_(std::move(cancellation_cleanups)),
        reinitializations_(std::move(reinitializations)),
        observations_(std::move(observations)) {}

  [[nodiscard]] const SemPlace &place(SemPlaceId id) const {
    return places_.at(id.index);
  }
  [[nodiscard]] std::size_t placeCount() const {
    return places_.size();
  }
  [[nodiscard]] SemPlaceId movePlace(InstId instruction) const;
  [[nodiscard]] SemPlaceId copyPlace(InstId instruction) const;
  [[nodiscard]] const PlaceCleanupPlan &returnCleanup(InstId instruction) const;
  [[nodiscard]] const PlaceCleanupPlan &edgeCleanup(InstId instruction) const;
  [[nodiscard]] const PlaceCleanupPlan &
  fullExpressionCleanup(InstId instruction) const;
  [[nodiscard]] const PlaceCleanupPlan &blockCleanup(InstBlockId block) const;
  [[nodiscard]] const SuspensionCleanupPartition &
  suspensionCleanup(InstId instruction) const;
  [[nodiscard]] const PlaceCleanupPlan &
  cancellationCleanup(InstId instruction) const;
  [[nodiscard]] const PlaceReinitializationPlan &
  reinitialization(InstId instruction) const;
  [[nodiscard]] std::span<const PlaceStateObservation> observations() const {
    return observations_;
  }
  [[nodiscard]] bool verify(const SemIR &sem_ir, std::string &error) const;

private:
  std::vector<SemPlace> places_;
  std::unordered_map<std::uint32_t, SemPlaceId> move_places_;
  std::unordered_map<std::uint32_t, SemPlaceId> copy_places_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> return_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> edge_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> full_expression_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> block_cleanups_;
  std::unordered_map<std::uint32_t, SuspensionCleanupPartition>
      suspension_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> cancellation_cleanups_;
  std::unordered_map<std::uint32_t, PlaceReinitializationPlan>
      reinitializations_;
  std::vector<PlaceStateObservation> observations_;
};

class SemIR {
public:
  SemIR(core::Arena &arena, SharedValueStores &values, CheckIRId check_ir_id,
        IdentifierId module_name,
        const PublicInterfaceRegistry &public_interfaces,
        const interop::ArtifactRegistry &interop_registry,
        std::span<const ImportIR> imports = {},
        LanguageVersion language_version = DefaultLanguageVersion);

  [[nodiscard]] TypeId voidType() const {
    return void_type_;
  }
  [[nodiscard]] TypeId boolType() const {
    return bool_type_;
  }
  [[nodiscard]] TypeId charType() const { return char_type_; }
  [[nodiscard]] TypeId i32Type() const {
    return i32_type_;
  }
  [[nodiscard]] TypeId f64Type() const {
    return f64_type_;
  }
  [[nodiscard]] TypeId addIntegerType(std::uint32_t width, bool is_signed) {
    return addType({SemTypeKind::Integer, width, is_signed ? 1U : 0U});
  }
  [[nodiscard]] TypeId addFloatType(std::uint32_t width) {
    return addType({SemTypeKind::Float, width, 0});
  }
  [[nodiscard]] TypeId stringType() const {
    return string_type_;
  }
  [[nodiscard]] TypeId neverType() const {
    return never_type_;
  }
  [[nodiscard]] TypeId coroutineExecutorType() const {
    return coroutine_executor_type_;
  }
  [[nodiscard]] TypeId coroutineScopeType() const {
    return coroutine_scope_type_;
  }
  [[nodiscard]] TypeId coroutineTaskCompletionType() const {
    return coroutine_task_completion_type_;
  }
  [[nodiscard]] TypeId
  addCoroutineTaskCompletionSetType(std::uint32_t capacity) {
    return addCompletionSetType(coroutine_task_completion_type_, capacity);
  }
  [[nodiscard]] TypeId addCompletionSetType(TypeId completion,
                                            std::uint32_t capacity) {
    return addType(
        {SemTypeKind::CoroutineTaskCompletionSet, completion.index, capacity});
  }
  [[nodiscard]] TypeId addCoroutineTaskSelectionType(std::uint32_t capacity) {
    return addCompletionSelectionType(coroutine_task_completion_type_,
                                      capacity);
  }
  [[nodiscard]] TypeId addCompletionSelectionType(TypeId completion,
                                                  std::uint32_t capacity) {
    return addType(
        {SemTypeKind::CoroutineTaskSelection, completion.index, capacity});
  }
  [[nodiscard]] std::uint32_t
  coroutineTaskCompletionCapacity(TypeId type) const {
    assert(this->type(type).kind == SemTypeKind::CoroutineTaskCompletionSet ||
           this->type(type).kind == SemTypeKind::CoroutineTaskSelection);
    return this->type(type).arg1;
  }
  [[nodiscard]] TypeId completionSetElementType(TypeId type) const {
    assert(this->type(type).kind == SemTypeKind::CoroutineTaskCompletionSet ||
           this->type(type).kind == SemTypeKind::CoroutineTaskSelection);
    return TypeId(this->type(type).arg0);
  }
  [[nodiscard]] bool isCompletionAggregationProvider(TypeId type) const;

  [[nodiscard]] TypeId addType(SemType type);
  [[nodiscard]] TypeId addFunctionType(std::span<const TypeId> parameters,
                                       TypeId result) {
    return addType(
        {SemTypeKind::Function, addTypeBlock(parameters).index, result.index});
  }
  [[nodiscard]] TypeId
  addAsyncFunctionType(std::span<const TypeId> parameters, TypeId success,
                       std::optional<TypeId> error = std::nullopt);
  [[nodiscard]] TypeId
  addCoroutineTaskType(TypeId success,
                       std::optional<TypeId> error = std::nullopt);
  [[nodiscard]] TypeId
  addCoroutineTaskOutcomeType(TypeId success,
                              std::optional<TypeId> error = std::nullopt);
  [[nodiscard]] TypeId addCoroutineCheckedType(TypeId payload);
  [[nodiscard]] TypeId coroutineTaskSuccessType(TypeId type) const;
  [[nodiscard]] std::optional<TypeId> coroutineTaskErrorType(TypeId type) const;
  [[nodiscard]] TypeId coroutineCheckedPayloadType(TypeId type) const;
  [[nodiscard]] TypeId asyncSuccessType(TypeId type) const;
  [[nodiscard]] std::optional<TypeId> asyncErrorType(TypeId type) const;
  [[nodiscard]] TypeId addTypeParameter(GenericId generic,
                                        std::uint32_t binding_index);
  [[nodiscard]] TypeId addNominalType(NominalTypeId nominal,
                                      std::span<const TypeId> arguments = {});
  [[nodiscard]] TypeId addTupleType(std::span<const TypeId> elements);
  [[nodiscard]] TypeId addCUnionType(std::span<const TypeId> members);
  [[nodiscard]] bool isCUnionType(TypeId type) const;
  [[nodiscard]] TypeId addSliceType(TypeId element, bool mutable_view);
  [[nodiscard]] TypeId
  addReferenceType(TypeId pointee, SemReferenceMutability mutability,
                   SemReferenceProvenanceKind provenance_kind =
                       SemReferenceProvenanceKind::Erased,
                   std::uint32_t provenance_index = core::AnyId::InvalidIndex);
  [[nodiscard]] TypeId addRawPointerType(TypeId pointee, bool pointee_const) {
    return addType(
        {SemTypeKind::RawPointer, pointee.index, pointee_const ? 1U : 0U});
  }
  [[nodiscard]] TypeId addCFunctionPointerType(
      std::span<const TypeId> parameters, TypeId result, bool is_variadic,
      CallableOwnershipSummary contract = {},
      std::uint32_t context_parameter = core::AnyId::InvalidIndex,
      ForeignCallingConvention convention = ForeignCallingConvention::C);
  [[nodiscard]] ForeignCallingConvention
  cFunctionCallingConvention(TypeId type) const;
  [[nodiscard]] TypeId addCallbackAdapterType(TypeId entry, TypeId context,
                                              TypeId release);
  [[nodiscard]] TypeId addCallbackCompletionType(
      TypeId callback, TypeId handle, TypeId token, TypeId wait_type,
      TypeId poll_type, CallbackReleaseAuthority authority,
      TypeId arm_type = TypeId::invalid(),
      TypeId detach_type = TypeId::invalid(),
      std::array<std::uint32_t, 4> arm_parameters = {core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex},
      std::array<std::uint32_t, 3> detach_parameters = {
          core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
          core::AnyId::InvalidIndex});
  [[nodiscard]] TypeId addCallbackWakeType(TypeId completion);
  [[nodiscard]] TypeId addForeignCompletionType(NominalTypeId resource);
  [[nodiscard]] TypeId addForeignWakeType(NominalTypeId resource);
  [[nodiscard]] TypeId
  markForeignOperationState(TypeId storage,
                            PublicEntityReferenceArtifact operation,
                            ForeignOperationStateKind state);
  [[nodiscard]] std::optional<ForeignOperationStateOwner>
  foreignOperationStateOwner(TypeId type) const;
  [[nodiscard]] TypeId materializePublicTypeForImport(const PublicType &type,
                                                      GenericId generic,
                                                      NodeId location,
                                                      std::string &error) {
    return materializePublicType(type, generic, location, error);
  }
  [[nodiscard]] TypeId addCallbackRegistrationType(
      TypeId callback, TypeId handle, TypeId register_type,
      TypeId unregister_type, TypeId cancel_type,
      CallbackReleaseAuthority authority, std::uint32_t entry_parameter,
      std::uint32_t userdata_parameter, std::uint32_t release_parameter,
      std::vector<CallbackRegistrationBinding> bindings = {},
      TypeId cancel_async_type = TypeId::invalid(),
      TypeId wait_type = TypeId::invalid(),
      TypeId poll_type = TypeId::invalid(), TypeId arm_type = TypeId::invalid(),
      TypeId detach_type = TypeId::invalid(),
      std::array<std::uint32_t, 4> arm_parameters = {core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex},
      std::array<std::uint32_t, 3> detach_parameters = {
          core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
          core::AnyId::InvalidIndex});
  [[nodiscard]] CallbackReleaseAuthority
  callbackCompletionAuthority(TypeId type) const;
  [[nodiscard]] ForeignResourceProtocolId
  foreignResourceProtocolId(TypeId type) const;
  [[nodiscard]] const CanonicalForeignResourceProtocol &
  foreignResourceProtocol(TypeId type) const;
  [[nodiscard]] CallbackReleaseAuthority
  callbackRegistrationAuthority(TypeId type) const;
  [[nodiscard]] std::array<std::uint32_t, 3>
  callbackRegistrationParameters(TypeId type) const;
  [[nodiscard]] std::span<const CallbackRegistrationBinding>
  callbackRegistrationBindings(TypeId type) const;
  [[nodiscard]] std::array<std::uint32_t, 4>
  callbackArmParameters(TypeId type) const;
  [[nodiscard]] std::array<std::uint32_t, 3>
  callbackDetachParameters(TypeId type) const;
  [[nodiscard]] std::uint32_t callbackContextParameter(TypeId type) const;
  [[nodiscard]] const CallableOwnershipSummary &
  callbackContract(TypeId type) const;
  [[nodiscard]] TypeId rawPointerPointee(TypeId pointer) const;
  [[nodiscard]] bool rawPointerPointeeConst(TypeId pointer) const;
  [[nodiscard]] TypeId referencePointee(TypeId reference) const;
  [[nodiscard]] std::uint32_t tupleArity(TypeId tuple) const;
  [[nodiscard]] TypeId tupleElementType(TypeId tuple,
                                        std::uint32_t index) const;
  [[nodiscard]] TypeId sliceElementType(TypeId slice) const;
  [[nodiscard]] bool sliceMutable(TypeId slice) const;
  [[nodiscard]] SemReferenceMutability
  referenceMutability(TypeId reference) const;
  [[nodiscard]] SemLoanCarrierCapability
  loanCarrierCapability(TypeId type) const;
  [[nodiscard]] std::vector<std::vector<CallableReturnSource::CarrierStep>>
  loanCarrierPaths(TypeId type) const;
  [[nodiscard]] SemReferenceProvenanceKind
  referenceProvenanceKind(TypeId reference) const;
  [[nodiscard]] std::uint32_t referenceProvenanceIndex(TypeId reference) const;
  [[nodiscard]] TypeRepresentationFacts typeRepresentation(TypeId type) const;
  [[nodiscard]] TypeId valueRepresentationType(TypeId type) const;
  [[nodiscard]] TypeId objectRepresentationType(TypeId type) const;
  [[nodiscard]] TypeId foreignRepresentationType(TypeId type) const;
  [[nodiscard]] TypeId nominalFieldType(TypeId nominal_type,
                                        std::uint32_t field_index) const;
  [[nodiscard]] TypeId enumPayloadFieldType(TypeId nominal_type,
                                            std::uint32_t variant_index,
                                            std::uint32_t field_index) const;
  [[nodiscard]] std::span<const SemObjectProjectionStep>
  objectFieldProjection(TypeId type, std::uint32_t field_index) const;
  [[nodiscard]] const NominalSemanticWitnessArtifact *
  nominalSemanticWitness(TypeId type) const;
  [[nodiscard]] bool bindNominalSemanticWitness(
      TypeId type, NominalSemanticWitnessArtifact witness, std::string &error);
  [[nodiscard]] CanonicalTypeId canonicalType(TypeId id) const {
    return CanonicalTypeId(type(id).reserved);
  }
  [[nodiscard]] bool isCallAbiType(TypeId type) const;
  [[nodiscard]] std::optional<CanonicalResultShape>
  canonicalResultShape(TypeId type) const;
  [[nodiscard]] std::optional<CanonicalReadOutcomeShape>
  canonicalReadOutcomeShape(TypeId type) const;
  [[nodiscard]] TypeId canonicalResultOutcomeType(TypeId type) const;
  [[nodiscard]] bool isGenericArgumentType(CanonicalTypeId type) const;
  [[nodiscard]] bool matchesPublicType(TypeId local,
                                       const PublicType &external) const;
  [[nodiscard]] TypeId materializeType(CanonicalTypeId canonical);
  [[nodiscard]] std::uint32_t addTypeQuery(SemTypeQueryArtifact query);
  [[nodiscard]] const SemTypeQueryArtifact &
  typeQuery(std::uint32_t index) const;
  [[nodiscard]] std::size_t typeQueryCount() const {
    return type_queries_.size();
  }
  [[nodiscard]] InstBlockId cloneSpecificBody(
      InstBlockId body, GenericId generic,
      std::span<const CanonicalTypeId> arguments, NodeId location,
      std::span<const SemGenericSubstitution> dependent_substitutions,
      std::unordered_map<std::uint32_t, LocalId> &locals,
      const std::function<FunctionRefId(FunctionRefId, std::span<const InstId>,
                                        NodeId)> &specialize_callee,
      const std::function<FunctionRefId(std::uint64_t, std::span<const InstId>,
                                        NodeId)> &resolve_interface_call,
      std::string &error);
  [[nodiscard]] bool materializeTemplateBody(
      FunctionId function, const GenericTemplateArtifact &generic_template,
      GenericId generic, NameId name, TypeId function_type, NodeId location,
      const std::function<TypeId(const PublicType &)> &materialize_type,
      const std::function<FunctionRefId(PublicEntityId, std::span<const InstId>,
                                        std::span<const PublicType>, NodeId)>
          &resolve_callee,
      std::string &error);
  [[nodiscard]] bool materializeConcreteSpecificBody(
      FunctionId function, const ConcreteSpecificNodeArtifact &node,
      NameId name, TypeId function_type, NodeId location,
      const std::function<FunctionRefId(const ConcreteCallTargetArtifact &,
                                        std::span<const InstId>, NodeId)>
          &resolve_callee,
      std::string &error);
  [[nodiscard]] NameId addName(IdentifierId text);
  [[nodiscard]] IntegerId addInteger(std::int64_t value) const;
  [[nodiscard]] LocalId addLocal(SemLocal local);
  [[nodiscard]] ConstantEntityId addConstantEntity(SemConstant constant);
  void addForeignConstant(SemForeignConstant constant) {
    foreign_constants_.push_back(std::move(constant));
  }
  [[nodiscard]] std::span<const SemForeignConstant> foreignConstants() const {
    return foreign_constants_;
  }
  void setConstantEntity(ConstantEntityId id, SemConstant constant) {
    constants_.get(id) = std::move(constant);
  }
  [[nodiscard]] ConstantId addConstantValue(ConstantValue value) {
    return constant_values_.add(std::move(value));
  }
  [[nodiscard]] ConstantBlockId
  addConstantBlock(std::span<const ConstantId> values) {
    return constant_blocks_.addCanonical(values);
  }
  [[nodiscard]] NominalTypeId addNominalTypeDecl(SemNominalType nominal);
  void setNominalType(NominalTypeId id, SemNominalType nominal);
  [[nodiscard]] InterfaceId addInterface(SemInterface interface_value) {
    return interfaces_.add(std::move(interface_value));
  }
  void setInterface(InterfaceId id, SemInterface interface_value) {
    interfaces_.get(id) = std::move(interface_value);
  }
  [[nodiscard]] InterfaceWitnessId
  addInterfaceWitness(SemInterfaceWitness witness) {
    return interface_witnesses_.add(std::move(witness));
  }
  void setInterfaceWitness(InterfaceWitnessId id, SemInterfaceWitness witness) {
    interface_witnesses_.get(id) = std::move(witness);
  }
  [[nodiscard]] TypeAliasId addTypeAlias(SemTypeAlias alias) {
    return type_aliases_.add(std::move(alias));
  }
  void setTypeAlias(TypeAliasId id, SemTypeAlias alias) {
    type_aliases_.get(id) = std::move(alias);
  }
  [[nodiscard]] FunctionId addFunction(SemFunction function);
  [[nodiscard]] FunctionRefId addFunctionRef(SemFunctionRef function_ref);
  [[nodiscard]] FunctionRefId addCanonicalExternalFunctionRef(
      PublicEntityId entity, std::string &error,
      std::span<const PublicType> concrete_arguments = {});
  [[nodiscard]] std::optional<CanonicalIntrinsicResolution>
  resolveCanonicalIntrinsic(PublicEntityId entity,
                            std::span<const PublicType> concrete_arguments,
                            const CanonicalIntrinsicShapeSpec &shape,
                            std::string &error);
  [[nodiscard]] std::optional<FunctionRefId>
  resolveCanonicalIntrinsic(FunctionRefId reference, std::string &error);
  [[nodiscard]] bool isConcreteReverseTarget(FunctionRefId target) const;
  [[nodiscard]] std::vector<StableFingerprint>
  reverseTargetWitnesses(FunctionRefId target) const;
  [[nodiscard]] bool bindCoroutineConstructorEntity(FunctionId scaffold,
                                                    PublicEntityId entity,
                                                    std::string &error);
  void clearCoroutineConstructorEntity(FunctionId scaffold) {
    if (scaffold.hasValue() &&
        scaffold.index < coroutine_constructor_entities_.size())
      coroutine_constructor_entities_[scaffold.index] =
          PublicEntityId::invalid();
  }
  [[nodiscard]] PublicEntityId
  coroutineConstructorEntity(FunctionId scaffold) const {
    return scaffold.index < coroutine_constructor_entities_.size()
               ? coroutine_constructor_entities_[scaffold.index]
               : PublicEntityId::invalid();
  }
  void addLocalOccurrence(NodeId location, SemSymbolOccurrenceKind kind,
                          LocalId local);
  void addFunctionOccurrence(NodeId location, SemSymbolOccurrenceKind kind,
                             FunctionRefId function);
  void addConstantOccurrence(NodeId location, SemSymbolOccurrenceKind kind,
                             ConstantEntityId constant);
  void addMemberAccessContext(NodeId location, NominalTypeId owner,
                              SemMemberAccessKind kind) {
    member_access_contexts_.push_back({location, owner, kind});
  }
  void addModuleAccessContext(NodeId location, ImportIRId module) {
    const auto duplicate =
        std::ranges::any_of(module_access_contexts_, [&](const auto &context) {
          return context.location == location && context.module == module;
        });
    if (!duplicate)
      module_access_contexts_.push_back({location, module});
  }
  [[nodiscard]] ImportIRInstId addImportIRInst(ImportIRInst import_inst) {
    return imports_.addInst(import_inst);
  }
  void setFunction(FunctionId id, SemFunction function);
  template <typename InstT>
  [[nodiscard]] InstId addInst(InstT inst, NodeId location)
    requires requires {
      InstT::Kind;
      InstT::Arg0Kind;
      InstT::Arg1Kind;
    }
  {
    return addRawInst(
        {InstT::Kind, inst.type.index, inst.arg0.index, inst.arg1.index},
        location);
  }
  [[nodiscard]] InstBlockId addInstBlock(std::span<const InstId> insts,
                                         bool canonical = false);
  [[nodiscard]] TypeBlockId addTypeBlock(std::span<const TypeId> types);
  [[nodiscard]] LocalBlockId addLocalBlock(std::span<const LocalId> locals);
  void setTopBlock(InstBlockId block) {
    top_block_ = block;
  }
  void setPlaceStateQuery(PlaceStateQuery query) {
    place_state_query_ = std::move(query);
  }

  [[nodiscard]] const SemInst &inst(InstId id) const {
    return insts_.get(id);
  }
  void setInstType(InstId id, TypeId type) {
    insts_.get(id).type = type.index;
  }
  template <typename InstT>
  void replaceInst(InstId id, InstT inst)
    requires requires {
      InstT::Kind;
      InstT::Arg0Kind;
      InstT::Arg1Kind;
    }
  {
    insts_.get(id) = {InstT::Kind, inst.type.index, inst.arg0.index,
                      inst.arg1.index};
  }
  template <typename InstT> [[nodiscard]] InstT getAs(InstId id) const {
    const auto &raw = inst(id);
    assert(raw.kind == InstT::Kind);
    return {TypeId(raw.type), typename InstT::Arg0Type(raw.arg0),
            typename InstT::Arg1Type(raw.arg1)};
  }
  [[nodiscard]] NodeId location(InstId id) const {
    return locations_.get(id);
  }
  [[nodiscard]] const SemType &type(TypeId id) const {
    return types_.get(id);
  }
  [[nodiscard]] const SemName &name(NameId id) const {
    return names_.get(id);
  }
  [[nodiscard]] const SemLocal &local(LocalId id) const {
    return locals_.get(id);
  }
  [[nodiscard]] const SemConstant &constantEntity(ConstantEntityId id) const {
    return constants_.get(id);
  }
  [[nodiscard]] std::size_t constantEntityCount() const {
    return constants_.size();
  }
  [[nodiscard]] const ConstantValue &constantValue(ConstantId id) const {
    return constant_values_.get(id);
  }
  [[nodiscard]] std::size_t constantValueCount() const {
    return constant_values_.size();
  }
  [[nodiscard]] std::span<const ConstantId>
  constantBlock(ConstantBlockId id) const {
    return constant_blocks_.get(id);
  }
  [[nodiscard]] const SemNominalType &nominalType(NominalTypeId id) const {
    return nominal_types_.get(id);
  }
  [[nodiscard]] const SemInterface &interface(InterfaceId id) const {
    return interfaces_.get(id);
  }
  [[nodiscard]] std::size_t interfaceCount() const {
    return interfaces_.size();
  }
  [[nodiscard]] const SemInterfaceWitness &
  interfaceWitness(InterfaceWitnessId id) const {
    return interface_witnesses_.get(id);
  }
  [[nodiscard]] std::size_t interfaceWitnessCount() const {
    return interface_witnesses_.size();
  }
  void recordConcreteContainerVTable(SemConcreteContainerVTable value) {
    const auto duplicate = std::ranges::find(
        concrete_container_vtables_, value) != concrete_container_vtables_.end();
    if (!duplicate)
      concrete_container_vtables_.push_back(std::move(value));
  }
  [[nodiscard]] std::span<const SemConcreteContainerVTable>
  concreteContainerVTables() const {
    return concrete_container_vtables_;
  }
  void recordTypedChannelDescriptor(SemTypedChannelDescriptor value) {
    const auto duplicate = std::ranges::find(
        typed_channel_descriptors_, value) != typed_channel_descriptors_.end();
    if (!duplicate)
      typed_channel_descriptors_.push_back(std::move(value));
  }
  [[nodiscard]] std::span<const SemTypedChannelDescriptor>
  typedChannelDescriptors() const {
    return typed_channel_descriptors_;
  }
  [[nodiscard]] const SemTypeAlias &typeAlias(TypeAliasId id) const {
    return type_aliases_.get(id);
  }
  [[nodiscard]] std::size_t typeAliasCount() const {
    return type_aliases_.size();
  }
  [[nodiscard]] std::size_t nominalTypeCount() const {
    return nominal_types_.size();
  }
  [[nodiscard]] const SemFunction &function(FunctionId id) const {
    return functions_.get(id);
  }
  [[nodiscard]] const CallableOwnershipSummary &
  functionOwnership(FunctionId id) const {
    return function_ownership_summaries_.at(id.index);
  }
  void setFunctionOwnership(FunctionId id, CallableOwnershipSummary summary) {
    function_ownership_summaries_.at(id.index) = std::move(summary);
  }
  void expectFunctionOwnership(FunctionId id,
                               CallableOwnershipSummary summary) {
    expected_function_ownership_summaries_.at(id.index) = std::move(summary);
  }
  [[nodiscard]] const SemCallableDeclaration &
  functionDeclaration(FunctionId id) const {
    return function_declarations_.at(id.index);
  }
  void setFunctionDeclaration(FunctionId id,
                              SemCallableDeclaration declaration) {
    function_declarations_.at(id.index) = std::move(declaration);
  }
  [[nodiscard]] std::span<const SemInterfaceConstraint>
  functionConstraints(FunctionId id) const {
    return function_constraints_.at(id.index);
  }
  void setFunctionConstraints(FunctionId id,
                              std::vector<SemInterfaceConstraint> constraints) {
    function_constraints_.at(id.index) = std::move(constraints);
  }
  [[nodiscard]] const std::optional<CallableOwnershipSummary> &
  expectedFunctionOwnership(FunctionId id) const {
    return expected_function_ownership_summaries_.at(id.index);
  }
  [[nodiscard]] const SemCallableSemanticContract &
  functionSemanticContract(FunctionId id) const {
    return function_semantic_contracts_.at(id.index);
  }
  void setFunctionSemanticContract(FunctionId id,
                                   SemCallableSemanticContract contract) {
    function_semantic_contracts_.at(id.index) = std::move(contract);
  }
  [[nodiscard]] const SemFunctionRef &functionRef(FunctionRefId id) const {
    return function_refs_.get(id);
  }
  [[nodiscard]] CompilerIntrinsicRole
  functionIntrinsicRole(FunctionRefId id) const;
  [[nodiscard]] std::span<const PublicType>
  functionRefConcreteArguments(FunctionRefId id) const {
    return function_ref_concrete_arguments_.at(id.index);
  }
  void setFunctionRefConcreteArguments(FunctionRefId id,
                                       std::vector<PublicType> arguments) {
    function_ref_concrete_arguments_.at(id.index) = std::move(arguments);
  }
  void setFunctionRef(FunctionRefId id, SemFunctionRef reference) {
    function_refs_.get(id) = std::move(reference);
  }
  void setLocal(LocalId id, SemLocal local) {
    locals_.get(id) = std::move(local);
  }
  void setCallableEnvironment(SemCallableEnvironmentInfo info) {
    callable_environments_.insert_or_assign(info.environment.index,
                                            std::move(info));
  }
  [[nodiscard]] const SemCallableEnvironmentInfo *
  tryGetCallableEnvironment(TypeId environment) const {
    const auto found = callable_environments_.find(environment.index);
    return found == callable_environments_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const ImportIRTable &importIRs() const {
    return imports_;
  }
  [[nodiscard]] std::int64_t integer(IntegerId id) const {
    return values_->integer(id);
  }
  [[nodiscard]] std::string_view identifier(IdentifierId id) const {
    return values_->identifier(id);
  }
  [[nodiscard]] std::string_view string(StringLiteralId id) const {
    return values_->stringLiteral(id);
  }
  [[nodiscard]] GenericValueStores &genericValues() {
    return values_->generics();
  }
  [[nodiscard]] const GenericValueStores &genericValues() const {
    return values_->generics();
  }
  [[nodiscard]] std::span<const InstId> instBlock(InstBlockId id) const {
    return inst_blocks_.get(id);
  }
  [[nodiscard]] std::span<const TypeId> typeBlock(TypeBlockId id) const {
    return type_blocks_.get(id);
  }
  [[nodiscard]] std::span<const LocalId> localBlock(LocalBlockId id) const {
    return local_blocks_.get(id);
  }
  [[nodiscard]] InstBlockId topBlock() const {
    return top_block_;
  }
  [[nodiscard]] const PlaceStateQuery &placeStates() const {
    assert(place_state_query_.has_value());
    return *place_state_query_;
  }
  [[nodiscard]] bool hasPlaceStates() const {
    return place_state_query_.has_value();
  }
  [[nodiscard]] std::size_t instCount() const {
    return insts_.size();
  }
  [[nodiscard]] std::size_t instBlockCount() const {
    return inst_blocks_.size();
  }
  [[nodiscard]] std::size_t typeCount() const {
    return types_.size();
  }
  [[nodiscard]] std::size_t functionCount() const {
    return functions_.size();
  }
  [[nodiscard]] std::size_t functionRefCount() const {
    return function_refs_.size();
  }
  [[nodiscard]] std::size_t localCount() const {
    return locals_.size();
  }
  [[nodiscard]] std::size_t integerCount() const {
    return values_->integerCount();
  }
  void setTargetLayout(TargetLayoutConfig target) {
    target_layout_ = std::move(target);
  }
  [[nodiscard]] const TargetLayoutConfig &targetLayout() const {
    return target_layout_;
  }
  [[nodiscard]] std::span<const SemSymbolOccurrence> symbolOccurrences() const {
    return symbol_occurrences_;
  }
  [[nodiscard]] std::span<const SemMemberAccessContext>
  memberAccessContexts() const {
    return member_access_contexts_;
  }
  [[nodiscard]] std::span<const SemModuleAccessContext>
  moduleAccessContexts() const {
    return module_access_contexts_;
  }
  [[nodiscard]] std::size_t stringCount() const {
    return values_->stringLiteralCount();
  }
  [[nodiscard]] CheckIRId checkIRId() const {
    return check_ir_id_;
  }
  [[nodiscard]] IdentifierId moduleName() const {
    return module_name_;
  }
  [[nodiscard]] LanguageVersion languageVersion() const {
    return language_version_;
  }
  void setSourceMetadata(std::string path,
                         std::vector<LineColumn> node_locations) {
    source_path_ = std::move(path);
    node_locations_ = std::move(node_locations);
  }
  [[nodiscard]] std::string_view sourcePath() const {
    return source_path_;
  }
  [[nodiscard]] LineColumn sourceLocation(NodeId id) const {
    return id.hasValue() && id.index < node_locations_.size()
               ? node_locations_[id.index]
               : LineColumn{};
  }
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] StableFingerprint specificDependencyFingerprint() const;
  void recordSpecializationLookup(bool hit);
  void recordSpecializationSemanticRejection();
  void recordSpecializationComponent(
      ConcreteSpecializationComponentArtifact artifact, bool rebuilt);
  [[nodiscard]] std::span<const ConcreteSpecializationComponentArtifact>
  specializationComponents() const {
    return specialization_components_;
  }
  [[nodiscard]] std::span<const ConcreteSpecializationReference>
  specializationReferences() const {
    return specialization_references_;
  }
  [[nodiscard]] const ConcreteSpecializationCacheStats &
  specializationCacheStats() const {
    return specialization_cache_stats_;
  }
  [[nodiscard]] std::string print() const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;
  [[nodiscard]] AnalysisMetrics &analysisMetrics() {
    return analysis_metrics_;
  }
  [[nodiscard]] const AnalysisMetrics &analysisMetrics() const {
    return analysis_metrics_;
  }

private:
  friend class internal::SemIRVerificationContext;
  [[nodiscard]] TypeId substituteNominalMemberType(TypeId owner,
                                                   TypeId member) const;
  [[nodiscard]] TypeId materializePublicType(const PublicType &type,
                                             GenericId generic, NodeId location,
                                             std::string &error);
  [[nodiscard]] InstId addRawInst(SemInst inst, NodeId location);
  [[nodiscard]] bool containsArg(SemArgKind kind, std::uint32_t raw) const;
  [[nodiscard]] bool verifyCodeBlock(InstBlockId block, bool require_return,
                                     TypeId return_type,
                                     std::string &error) const;
  [[nodiscard]] bool verifyTypeRecords(std::string &error) const;

  SharedValueStores *values_;
  CheckIRId check_ir_id_;
  IdentifierId module_name_;
  LanguageVersion language_version_;
  AnalysisMetrics analysis_metrics_;
  std::string source_path_;
  std::vector<LineColumn> node_locations_;
  ImportIRTable imports_;
  core::CanonicalValueStore<TypeId, SemType, SemTypeHash> types_;
  core::CanonicalValueStore<NameId, SemName, SemNameHash> names_;
  core::StableValueStore<LocalId, SemLocal> locals_;
  std::vector<SemTypeQueryArtifact> type_queries_;
  core::StableValueStore<ConstantEntityId, SemConstant> constants_;
  std::vector<SemForeignConstant> foreign_constants_;
  core::CanonicalValueStore<ConstantId, ConstantValue, ConstantValueHash>
      constant_values_;
  core::BlockStore<ConstantBlockId, ConstantId> constant_blocks_;
  core::StableValueStore<NominalTypeId, SemNominalType> nominal_types_;
  core::StableValueStore<InterfaceId, SemInterface> interfaces_;
  core::StableValueStore<InterfaceWitnessId, SemInterfaceWitness>
      interface_witnesses_;
  core::StableValueStore<TypeAliasId, SemTypeAlias> type_aliases_;
  core::StableValueStore<FunctionId, SemFunction> functions_;
  std::deque<SemCallableSemanticContract> function_semantic_contracts_;
  std::deque<CallableOwnershipSummary> function_ownership_summaries_;
  std::deque<std::optional<CallableOwnershipSummary>>
      expected_function_ownership_summaries_;
  std::deque<SemCallableDeclaration> function_declarations_;
  std::deque<std::vector<SemInterfaceConstraint>> function_constraints_;
  core::StableValueStore<FunctionRefId, SemFunctionRef> function_refs_;
  std::vector<std::vector<PublicType>> function_ref_concrete_arguments_;
  std::unordered_map<std::uint32_t, SemCallableEnvironmentInfo>
      callable_environments_;
  std::vector<PublicEntityId> coroutine_constructor_entities_;
  std::vector<SemSymbolOccurrence> symbol_occurrences_;
  std::vector<SemMemberAccessContext> member_access_contexts_;
  std::vector<SemModuleAccessContext> module_access_contexts_;
  core::StableValueStore<InstId, SemInst> insts_;
  core::StableValueStore<InstId, NodeId> locations_;
  core::BlockStore<InstBlockId, InstId> inst_blocks_;
  core::BlockStore<TypeBlockId, TypeId> type_blocks_;
  core::BlockStore<LocalBlockId, LocalId> local_blocks_;
  TypeId void_type_;
  TypeId bool_type_;
  TypeId char_type_;
  TypeId i32_type_;
  TypeId f64_type_;
  TypeId string_type_;
  TypeId never_type_;
  TypeId coroutine_executor_type_;
  TypeId coroutine_scope_type_;
  TypeId coroutine_task_completion_type_;
  InstBlockId top_block_;
  std::optional<PlaceStateQuery> place_state_query_;
  std::unordered_map<std::uint32_t, NominalSemanticWitnessArtifact>
      nominal_semantic_witnesses_;
  std::unordered_map<std::uint32_t, TypeId> value_repr_carriers_;
  std::unordered_map<std::uint32_t, TypeId> object_repr_carriers_;
  std::unordered_map<std::uint32_t, ForeignOperationStateOwner>
      foreign_operation_state_owners_;
  std::unordered_map<std::uint32_t,
                     std::vector<std::vector<SemObjectProjectionStep>>>
      object_field_projections_;
  std::vector<ConcreteSpecializationComponentArtifact>
      specialization_components_;
  std::vector<ConcreteSpecializationReference> specialization_references_;
  ConcreteSpecializationCacheStats specialization_cache_stats_;
  std::vector<SemConcreteContainerVTable> concrete_container_vtables_;
  std::vector<SemTypedChannelDescriptor> typed_channel_descriptors_;
  TargetLayoutConfig target_layout_{"unknown-64", 64, NominalLayoutAbiEpoch};
};

[[nodiscard]] std::string_view semInstKindName(SemInstKind kind);
[[nodiscard]] SemArgKind semInstArgKind(SemInstKind kind, std::size_t index);
[[nodiscard]] std::string_view semTypeKindName(SemTypeKind kind);
[[nodiscard]] SemExprCategory expressionCategory(const SemIR &sem_ir,
                                                 InstId instruction);

template <typename Fn> void visitSemInst(const SemInst &inst, Fn &&fn) {
  switch (inst.kind) {
#define CHTHOLLY_COMPILER_SEM_INST(Name, Arg0, Arg1)                               \
  case SemInstKind::Name:                                                      \
    std::forward<Fn>(fn)(Sem##Name{TypeId(inst.type),                          \
                                   Sem##Name::Arg0Type(inst.arg0),             \
                                   Sem##Name::Arg1Type(inst.arg1)});           \
    return;
#include "chtholly/Compiler/SemIRKind.def"
  case SemInstKind::Count:
    return;
  }
}

static_assert(sizeof(SemInst) == 16);
static_assert(sizeof(SemInvalid) == 12);
static_assert(sizeof(SemType) == 16);
static_assert(sizeof(SemFunction) == 44);
static_assert(sizeof(SemFunctionRef) == 24);
static_assert(sizeof(SemLocal) == 16);

} // namespace chtholly::compiler
