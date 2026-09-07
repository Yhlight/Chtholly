#pragma once

#include "chtholly/Core/Arena.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/Outcome.h"
#include "chtholly/Compiler/ComponentABI2Protocol.h"
#include "chtholly/Compiler/SemIRRef.h"
#include "chtholly/Compiler/SemIR.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace chtholly::compiler {

namespace internal {
class LowIRVerificationContext;
}

using SemIRInstRef = SemIRRef<InstId>;
using SemIRTypeRef = SemIRRef<TypeId>;
using SemIRBlockRef = SemIRRef<InstBlockId>;
using SemIRFunctionRef = SemIRRef<FunctionId>;
using SemIRConstantRef = SemIRRef<ConstantId>;

struct LowInstId : core::IndexBase<LowInstId> {
  using IndexBase::IndexBase;
};
struct LowBlockId : core::IndexBase<LowBlockId> {
  using IndexBase::IndexBase;
};
struct SlotId : core::IndexBase<SlotId> {
  using IndexBase::IndexBase;
};
struct LowPlaceId : core::IndexBase<LowPlaceId> {
  using IndexBase::IndexBase;
};
struct LowFunctionId : core::IndexBase<LowFunctionId> {
  using IndexBase::IndexBase;
};
struct TargetPairId : core::IndexBase<TargetPairId> {
  using IndexBase::IndexBase;
};
struct ParameterIndex : core::IndexBase<ParameterIndex> {
  using IndexBase::IndexBase;
};
struct FieldIndex : core::IndexBase<FieldIndex> {
  using IndexBase::IndexBase;
};
struct ForeignAbiLayoutId : core::IndexBase<ForeignAbiLayoutId> {
  using IndexBase::IndexBase;
};
struct ForeignAbiCallLayoutId : core::IndexBase<ForeignAbiCallLayoutId> {
  using IndexBase::IndexBase;
};
struct ForeignAbiThunkPlanId : core::IndexBase<ForeignAbiThunkPlanId> {
  using IndexBase::IndexBase;
};
struct CallbackAdapterPlanId : core::IndexBase<CallbackAdapterPlanId> {
  using IndexBase::IndexBase;
};
struct CallbackRegistrationPlanId
    : core::IndexBase<CallbackRegistrationPlanId> {
  using IndexBase::IndexBase;
};
struct CallbackCompletionPlanId : core::IndexBase<CallbackCompletionPlanId> {
  using IndexBase::IndexBase;
};
struct CallbackReadinessPlanId : core::IndexBase<CallbackReadinessPlanId> {
  using IndexBase::IndexBase;
};
struct CallbackWakePlanId : core::IndexBase<CallbackWakePlanId> {
  using IndexBase::IndexBase;
};
struct CoroutineFramePlanId : core::IndexBase<CoroutineFramePlanId> {
  using IndexBase::IndexBase;
};
struct CoroutineCleanupGraphId : core::IndexBase<CoroutineCleanupGraphId> {
  using IndexBase::IndexBase;
};
struct CoroutineTaskCreatePlanId : core::IndexBase<CoroutineTaskCreatePlanId> {
  using IndexBase::IndexBase;
};
struct CoroutineTaskCompletionArmPlanId
    : core::IndexBase<CoroutineTaskCompletionArmPlanId> {
  using IndexBase::IndexBase;
};
struct CoroutineTaskCompletionSetPlanId
    : core::IndexBase<CoroutineTaskCompletionSetPlanId> {
  using IndexBase::IndexBase;
};
struct CoroutineTaskCompletionCombinePlanId
    : core::IndexBase<CoroutineTaskCompletionCombinePlanId> {
  using IndexBase::IndexBase;
};
struct CoroutineSegmentId : core::IndexBase<CoroutineSegmentId> {
  using IndexBase::IndexBase;
};
struct CoroutineRegionId : core::IndexBase<CoroutineRegionId> {
  using IndexBase::IndexBase;
};
struct CoroutineFramePlaceId : core::IndexBase<CoroutineFramePlaceId> {
  using IndexBase::IndexBase;
};
struct ConstructPlanId : core::IndexBase<ConstructPlanId> {
  using IndexBase::IndexBase;
};

using LowInstBlockId = core::BlockId<struct LowInstBlockTag>;
using LowBlockListId = core::BlockId<struct LowBlockListTag>;
using SlotBlockId = core::BlockId<struct SlotBlockTag>;
using LowValueBlockId = core::BlockId<struct LowValueBlockTag>;
using LowPlaceProjectionBlockId =
    core::BlockId<struct LowPlaceProjectionBlockTag>;

enum class LowPlaceProjectionKind : std::uint8_t {
  Dereference,
  StructField,
  ArrayElement,
  EnumPayload,
};

struct LowPlaceProjection {
  LowPlaceProjectionKind kind = LowPlaceProjectionKind::StructField;
  TypeId aggregate_type;
  std::uint32_t index = 0;
  std::uint32_t variant = core::AnyId::InvalidIndex;

  friend bool operator==(const LowPlaceProjection &,
                         const LowPlaceProjection &) = default;
};

enum class LowInstKind : std::uint32_t {
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1) Name,
#include "chtholly/Compiler/LowIRKind.def"
  Count,
};

enum class LowArgKind : std::uint8_t {
  None,
  Value,
  Slot,
  Place,
  Block,
  Targets,
  Integer,
  String,
  Constant,
  ValueBlock,
  FunctionRef,
  ForeignAbiLayout,
  ForeignAbiCallLayout,
  ForeignCallOutcomePlan,
  ForeignAbiThunkPlan,
  CallbackAdapterPlan,
  CallbackRegistrationPlan,
  CallbackCompletionPlan,
  CallbackReadinessPlan,
  CallbackWakePlan,
  ForeignOperationCompletionPlan,
  CoroutineTaskCreatePlan,
  CoroutineTaskCompletionArmPlan,
  CoroutineTaskCompletionSetPlan,
  CoroutineTaskCompletionCombinePlan,
  ConstructPlan,
  Parameter,
  Field,
};

struct NoLowArg : core::AnyId {
  constexpr NoLowArg() : AnyId(InvalidIndex) {}
  explicit constexpr NoLowArg(std::uint32_t raw) : AnyId(raw) {}
};

template <LowArgKind Kind> struct LowArgType;
template <> struct LowArgType<LowArgKind::None> {
  using type = NoLowArg;
};
template <> struct LowArgType<LowArgKind::Value> {
  using type = LowInstId;
};
template <> struct LowArgType<LowArgKind::Slot> {
  using type = SlotId;
};
template <> struct LowArgType<LowArgKind::Place> {
  using type = LowPlaceId;
};
template <> struct LowArgType<LowArgKind::Block> {
  using type = LowBlockId;
};
template <> struct LowArgType<LowArgKind::Targets> {
  using type = TargetPairId;
};
template <> struct LowArgType<LowArgKind::Integer> {
  using type = IntegerId;
};
template <> struct LowArgType<LowArgKind::String> {
  using type = StringLiteralId;
};
template <> struct LowArgType<LowArgKind::Constant> {
  using type = ConstantEntityId;
};
template <> struct LowArgType<LowArgKind::ValueBlock> {
  using type = LowValueBlockId;
};
template <> struct LowArgType<LowArgKind::FunctionRef> {
  using type = FunctionRefId;
};
template <> struct LowArgType<LowArgKind::ForeignAbiLayout> {
  using type = ForeignAbiLayoutId;
};
template <> struct LowArgType<LowArgKind::ForeignAbiCallLayout> {
  using type = ForeignAbiCallLayoutId;
};
template <> struct LowArgType<LowArgKind::ForeignCallOutcomePlan> {
  using type = ForeignCallOutcomePlanId;
};
template <> struct LowArgType<LowArgKind::ForeignAbiThunkPlan> {
  using type = ForeignAbiThunkPlanId;
};
template <> struct LowArgType<LowArgKind::CallbackAdapterPlan> {
  using type = CallbackAdapterPlanId;
};
template <> struct LowArgType<LowArgKind::CallbackRegistrationPlan> {
  using type = CallbackRegistrationPlanId;
};
template <> struct LowArgType<LowArgKind::CallbackCompletionPlan> {
  using type = CallbackCompletionPlanId;
};
template <> struct LowArgType<LowArgKind::CallbackReadinessPlan> {
  using type = CallbackReadinessPlanId;
};
template <> struct LowArgType<LowArgKind::CallbackWakePlan> {
  using type = CallbackWakePlanId;
};
template <> struct LowArgType<LowArgKind::ForeignOperationCompletionPlan> {
  using type = ForeignOperationCompletionPlanId;
};
template <> struct LowArgType<LowArgKind::CoroutineTaskCreatePlan> {
  using type = CoroutineTaskCreatePlanId;
};
template <> struct LowArgType<LowArgKind::CoroutineTaskCompletionArmPlan> {
  using type = CoroutineTaskCompletionArmPlanId;
};
template <> struct LowArgType<LowArgKind::CoroutineTaskCompletionSetPlan> {
  using type = CoroutineTaskCompletionSetPlanId;
};
template <> struct LowArgType<LowArgKind::CoroutineTaskCompletionCombinePlan> {
  using type = CoroutineTaskCompletionCombinePlanId;
};
template <> struct LowArgType<LowArgKind::ConstructPlan> {
  using type = ConstructPlanId;
};
template <> struct LowArgType<LowArgKind::Parameter> {
  using type = ParameterIndex;
};
template <> struct LowArgType<LowArgKind::Field> {
  using type = FieldIndex;
};

template <LowArgKind Kind> using LowArgTypeT = typename LowArgType<Kind>::type;

template <LowInstKind KindValue, LowArgKind Arg0KindValue,
          LowArgKind Arg1KindValue>
struct TypedLowInst {
  using Arg0Type = LowArgTypeT<Arg0KindValue>;
  using Arg1Type = LowArgTypeT<Arg1KindValue>;
  static constexpr LowInstKind Kind = KindValue;
  static constexpr LowArgKind Arg0Kind = Arg0KindValue;
  static constexpr LowArgKind Arg1Kind = Arg1KindValue;

  TypeId type;
  Arg0Type arg0;
  Arg1Type arg1;
};

#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1)                               \
  using Low##Name =                                                            \
      TypedLowInst<LowInstKind::Name, LowArgKind::Arg0, LowArgKind::Arg1>;
#include "chtholly/Compiler/LowIRKind.def"

struct LowInst {
  LowInstKind kind = LowInstKind::Invalid;
  std::uint32_t type = core::AnyId::InvalidIndex;
  std::uint32_t arg0 = core::AnyId::InvalidIndex;
  std::uint32_t arg1 = core::AnyId::InvalidIndex;
};

struct LowSlot {
  TypeId type;
  LocalId semantic_local;
  std::uint32_t flags = 0;
  std::uint32_t reserved = 0;
};

enum LowSlotFlags : std::uint32_t {
  LowSlotSynthetic = 1U << 31U,
};

struct LowPlace {
  SlotId root;
  LowPlaceProjectionBlockId projections;
  LowPlaceProjectionBlockId logical_projections;
  TypeId type;
  std::uint32_t flags = 0;
};

// A constructor call whose result is written directly into a caller-owned
// place. The target and argument block are verified against the constructor's
// semantic function type before target lowering.
struct ConstructPlan {
  LowPlaceId destination;
  FunctionRefId target;
  LowValueBlockId arguments;
};

enum LowPlaceFlags : std::uint32_t {
  LowPlaceAddressable = 1U << 0U,
};

struct TargetPair {
  LowBlockId true_block;
  LowBlockId false_block;
};

struct LowFunction {
  FunctionId semantic_function;
  LowBlockId entry;
  LowBlockListId blocks;
  SlotBlockId slots;
};

struct LowObjectFieldProjection {
  ObjectFieldProjectionKind kind = ObjectFieldProjectionKind::StableAddress;
  std::vector<SemObjectProjectionStep> physical_steps;
  std::vector<std::uint32_t> region_indices;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;
  std::uint16_t capabilities = 0;
  FunctionRefId load_target;
  FunctionRefId store_target;
  FunctionRefId take_target;
  FunctionRefId init_target;
  FunctionRefId borrow_target;
  FunctionRefId borrow_mut_target;

  friend bool operator==(const LowObjectFieldProjection &,
                         const LowObjectFieldProjection &) = default;
};

struct LowTypeRepresentation {
  TypeRepresentationFacts facts;
  TypeId object_type;
  TypeId value_type;
  FunctionRefId pack_target;
  FunctionRefId init_target;
  FunctionRefId copy_target;
  FunctionRefId destroy_target;
  FunctionRefId object_init_target;
  FunctionRefId object_copy_init_target;
  FunctionRefId object_move_init_target;
  FunctionRefId object_drop_target;
  std::vector<TypeId> object_fields;
  std::vector<LowObjectFieldProjection> field_projections;

  friend bool operator==(const LowTypeRepresentation &,
                         const LowTypeRepresentation &) = default;
};

struct LowNominalLayoutBinding {
  TypeId type;
  std::uint32_t layout_index = core::AnyId::InvalidIndex;
  StableFingerprint expected_type_specific_fingerprint;
};

struct LowNominalFieldLayout {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
};

struct LowNominalVariantLayout {
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::vector<LowNominalFieldLayout> fields;
};

struct LowNominalLayout {
  NominalKind kind = NominalKind::Struct;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::vector<LowNominalFieldLayout> fields;
  std::uint32_t tag_size = 0;
  std::uint64_t payload_offset = 0;
  std::vector<LowNominalVariantLayout> variants;
};

struct LowEnumVariantLayout {
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::vector<std::uint64_t> field_offsets;
};

struct LowEnumLayout {
  std::uint32_t tag_size = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::vector<LowEnumVariantLayout> variants;
};

enum class ForeignAbiTargetKind : std::uint8_t {
  Unsupported,
  WindowsX64,
  SysVAMD64,
  AAPCS64,
  Count,
};
enum class ForeignPassKind : std::uint8_t {
  Ignore,
  Scalar,
  Direct,
  Indirect,
  Count,
};
enum class ForeignPhysicalKind : std::uint8_t {
  Pointer,
  Integer,
  Float32,
  Float64,
  Float32Vector2,
  HomogeneousFloat,
  Count,
};
enum class ForeignExtensionKind : std::uint8_t {
  None,
  Sign,
  Zero,
  Count,
};

struct ForeignAbiLane {
  ForeignPhysicalKind kind = ForeignPhysicalKind::Count;
  std::uint32_t width = 0;
  std::uint32_t elements = 1;
  std::uint64_t offset = 0;
  friend bool operator==(const ForeignAbiLane &,
                         const ForeignAbiLane &) = default;
};
struct ForeignAbiValueLayout {
  ForeignPassKind kind = ForeignPassKind::Ignore;
  TypeId semantic_type;
  std::vector<ForeignAbiLane> lanes;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  ForeignExtensionKind extension = ForeignExtensionKind::None;
  bool by_value = false;
  friend bool operator==(const ForeignAbiValueLayout &,
                         const ForeignAbiValueLayout &) = default;
};
struct ForeignAbiFunctionLayout {
  FunctionRefId target;
  TypeId callback_type;
  ForeignAbiTargetKind target_kind = ForeignAbiTargetKind::Unsupported;
  ForeignAbiValueLayout result;
  std::vector<ForeignAbiValueLayout> parameters;
  bool is_variadic = false;
  ForeignCallingConvention calling_convention = ForeignCallingConvention::C;
  std::uint32_t abi_epoch = 11;
  friend bool operator==(const ForeignAbiFunctionLayout &,
                         const ForeignAbiFunctionLayout &) = default;
};
struct ForeignAbiCallLayout {
  ForeignAbiLayoutId function_layout;
  std::vector<TypeId> source_suffix_types;
  std::vector<ForeignAbiValueLayout> suffix;
  std::uint32_t abi_epoch = 11;
  friend bool operator==(const ForeignAbiCallLayout &,
                         const ForeignAbiCallLayout &) = default;
};

enum class ForeignAbiThunkParameterKind : std::uint8_t {
  Scalar,
  DirectLanes,
  IndirectObject,
  Count,
};
enum class ForeignAbiThunkResultKind : std::uint8_t {
  Ignore,
  Scalar,
  DirectLanes,
  IndirectReturnSlot,
  Count,
};
struct ForeignAbiThunkParameterPlan {
  TypeId semantic_type;
  ForeignAbiThunkParameterKind kind = ForeignAbiThunkParameterKind::Count;
  bool semantic_uses_object_pointer = false;

  friend bool operator==(const ForeignAbiThunkParameterPlan &,
                         const ForeignAbiThunkParameterPlan &) = default;
};
struct ForeignAbiThunkResultPlan {
  TypeId semantic_type;
  ForeignAbiThunkResultKind kind = ForeignAbiThunkResultKind::Count;
  bool semantic_uses_return_slot = false;

  friend bool operator==(const ForeignAbiThunkResultPlan &,
                         const ForeignAbiThunkResultPlan &) = default;
};

enum class ReverseThunkTargetKind : std::uint8_t {
  OrdinaryFunction,
  InterfaceWitness,
  Count,
};

enum class ReverseThunkRole : std::uint8_t {
  Direct,
  ContextEntry,
  ContextRelease,
  Count,
};

struct ForeignAbiThunkPlan {
  FunctionRefId source;
  TypeId callback_type;
  ForeignAbiLayoutId callback_layout;
  std::vector<ForeignAbiThunkParameterPlan> parameters;
  ForeignAbiThunkResultPlan result;
  ReverseThunkTargetKind target_kind = ReverseThunkTargetKind::Count;
  ReverseThunkRole role = ReverseThunkRole::Count;
  std::uint32_t context_parameter = core::AnyId::InvalidIndex;
  TypeId context_carrier;
  std::vector<StableFingerprint> canonical_witnesses;
  std::uint32_t abi_epoch = 10;

  friend bool operator==(const ForeignAbiThunkPlan &,
                         const ForeignAbiThunkPlan &) = default;
};

struct CallbackAdapterPlan {
  TypeId adapter_type;
  ForeignAbiCallLayoutId entry_call_layout;
  ForeignAbiCallLayoutId release_call_layout;
  std::uint32_t context_parameter = core::AnyId::InvalidIndex;
  TypeId context_carrier;
  CallbackReleaseAuthority local_release_authority =
      CallbackReleaseAuthority::Retained;
  std::uint32_t abi_epoch = 10;

  friend bool operator==(const CallbackAdapterPlan &,
                         const CallbackAdapterPlan &) = default;
};

struct CallbackCompletionPlan {
  TypeId completion_type;
  ForeignResourceProtocolId protocol;
  CallbackAdapterPlanId callback_plan;
  ForeignAbiCallLayoutId wait_call_layout;
  CallbackReleaseAuthority authority = CallbackReleaseAuthority::Count;
  std::uint32_t semantic_epoch = ForeignResourceProtocol::CurrentSemanticEpoch;
  std::uint32_t abi_epoch = 12;

  friend bool operator==(const CallbackCompletionPlan &,
                         const CallbackCompletionPlan &) = default;
};

struct CallbackReadinessPlan {
  TypeId completion_type;
  ForeignResourceProtocolId protocol;
  CallbackCompletionPlanId completion_plan;
  ForeignAbiCallLayoutId poll_call_layout;
  std::uint32_t semantic_epoch = ForeignResourceProtocol::CurrentSemanticEpoch;
  std::uint32_t abi_epoch = 13;

  friend bool operator==(const CallbackReadinessPlan &,
                         const CallbackReadinessPlan &) = default;
};

struct CallbackWakePlan {
  TypeId completion_type;
  ForeignResourceProtocolId protocol;
  CallbackCompletionPlanId completion_plan;
  CallbackReadinessPlanId readiness_plan;
  ForeignAbiCallLayoutId arm_call_layout;
  ForeignAbiCallLayoutId detach_call_layout;
  ForeignAbiCallLayoutId wake_release_call_layout;
  std::array<std::uint32_t, 4> arm_parameters{
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex};
  std::array<std::uint32_t, 3> detach_parameters{core::AnyId::InvalidIndex,
                                                 core::AnyId::InvalidIndex,
                                                 core::AnyId::InvalidIndex};
  CallbackReleaseAuthority authority = CallbackReleaseAuthority::Count;
  std::uint32_t semantic_epoch = ForeignResourceProtocol::CurrentSemanticEpoch;
  std::uint32_t abi_epoch = 14;

  friend bool operator==(const CallbackWakePlan &,
                         const CallbackWakePlan &) = default;
};

struct CallbackRegistrationPlan {
  TypeId registration_type;
  ForeignResourceProtocolId protocol;
  CallbackAdapterPlanId callback_plan;
  ForeignAbiCallLayoutId register_call_layout;
  ForeignAbiCallLayoutId unregister_call_layout;
  ForeignAbiCallLayoutId cancel_call_layout;
  ForeignAbiCallLayoutId cancel_async_call_layout;
  CallbackCompletionPlanId completion_plan;
  std::uint32_t entry_parameter = core::AnyId::InvalidIndex;
  std::uint32_t userdata_parameter = core::AnyId::InvalidIndex;
  std::uint32_t release_parameter = core::AnyId::InvalidIndex;
  std::vector<std::uint32_t> binding_parameters;
  CallbackReleaseAuthority authority = CallbackReleaseAuthority::Count;
  std::uint32_t semantic_epoch = ForeignResourceProtocol::CurrentSemanticEpoch;
  std::uint32_t abi_epoch = 11;

  friend bool operator==(const CallbackRegistrationPlan &,
                         const CallbackRegistrationPlan &) = default;
};

enum class ForeignOperationPhase : std::uint8_t {
  Prepare,
  Invoke,
  Classify,
  Resolve,
  Publish,
  Count,
};

// LowIR operation plans are source-independent.  They are re-derived by the
// verifier from the public operation facts before LLVM lowering, so a stale
// plan or forged artifact cannot change the physical ABI call.
struct ForeignOperationPlan {
  StableFingerprint operation_fingerprint;
  std::vector<ForeignOperationPhase> phases{
      ForeignOperationPhase::Prepare, ForeignOperationPhase::Invoke,
      ForeignOperationPhase::Classify, ForeignOperationPhase::Resolve,
      ForeignOperationPhase::Publish};
  std::vector<std::uint32_t> input_lanes;
  std::vector<std::uint32_t> callback_lanes;
  std::vector<std::uint32_t> output_lanes;
  std::vector<std::uint32_t> success_lanes;
  std::vector<std::uint32_t> failure_lanes;
  interop::ForeignCompletionProjectionKind completion_projection =
      interop::ForeignCompletionProjectionKind::None;
  interop::ForeignCompletionInputEffect completion_input_effect =
      interop::ForeignCompletionInputEffect::Borrow;
  std::uint32_t completion_carrier_lane = core::AnyId::InvalidIndex;
  std::uint32_t completion_result_lane = core::AnyId::InvalidIndex;
  std::vector<std::uint32_t> wake_callback_lanes;
  std::uint8_t success_payload_kind = 0;
  std::uint8_t failure_payload_kind = 0;
  bool retains_consumed_inputs_until_classify = true;
  std::uint32_t abi_epoch = 1;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();

  friend bool operator==(const ForeignOperationPlan &,
                         const ForeignOperationPlan &) = default;
};

struct PayloadOperationPlan {
  StableFingerprint plan_fingerprint;
  StableFingerprint descriptor_digest;
  StableFingerprint payload_type_digest;
  StableFingerprint layout_digest;
  StableFingerprint lifecycle_digest;
  StableFingerprint contract_digest;
  ComponentAbi2OperationKind operation_kind = ComponentAbi2OperationKind::Send;
  ComponentAbi2LeasePolicy lease_policy = ComponentAbi2LeasePolicy::Exclusive;
  std::uint32_t source_lane = core::AnyId::InvalidIndex;
  std::uint32_t destination_lane = core::AnyId::InvalidIndex;
  std::uint32_t token_lane = core::AnyId::InvalidIndex;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();
  bool source_preserved_until_commit = true;
  bool destination_initializes_on_commit = false;
  friend bool operator==(const PayloadOperationPlan &, const PayloadOperationPlan &) = default;
};

struct ForeignCallOutcomePlan {
  using ArgumentSourceKind =
      interop::ForeignOperationArtifact::ArgumentSourceKind;
  using ArgumentSource = interop::ForeignOperationArtifact::ArgumentSource;
  ForeignAbiCallLayoutId call_layout;
  TypeId raw_result_type;
  // Physical carrier inspected by the error extractor. fread's `ferror`
  // accessor returns i32 independently of fread's size_t result.
  TypeId error_physical_type;
  TypeId projected_result_type;
  TypeId error_type;
  interop::ForeignOperationArtifact::ErrorExtractor extractor =
      interop::ForeignOperationArtifact::ErrorExtractor::None;
  interop::ForeignOperationArtifact::ErrorPredicate predicate =
      interop::ForeignOperationArtifact::ErrorPredicate::None;
  interop::ForeignOperationArtifact::ErrorSuccessPayload success_payload =
      interop::ForeignOperationArtifact::ErrorSuccessPayload::None;
  std::vector<interop::ForeignOperationArtifact::ErrorInterval> intervals;
  std::uint32_t predicate_width = 0;
  bool predicate_signed = false;
  bool predicate_inverted = false;
  interop::ForeignOperationArtifact::OutcomeProjection outcome_projection =
      interop::ForeignOperationArtifact::OutcomeProjection::None;
  TypeId outcome_type;
  TypeId slice_type;
  TypeId element_type;
  std::uint32_t outcome_buffer_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_capacity_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_count_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_context_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_size_lane = core::AnyId::InvalidIndex;
  std::string outcome_eof_symbol;
  std::string outcome_ferror_symbol;
  TypeId outcome_count_type;
  std::vector<ArgumentSource> argument_sources;
  std::uint32_t data_variant = core::AnyId::InvalidIndex;
  std::uint32_t eof_variant = core::AnyId::InvalidIndex;
  StableFingerprint operation_fingerprint;
  std::uint32_t abi_epoch = 1;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();

  friend bool operator==(const ForeignCallOutcomePlan &,
                         const ForeignCallOutcomePlan &) = default;
};

struct ForeignOperationCallbackPlan {
  ForeignOperationPlanId operation;
  CallbackAdapterPlanId adapter;
  ForeignAbiThunkPlanId entry_thunk;
  ForeignAbiThunkPlanId context_thunk;
  ForeignAbiThunkPlanId release_thunk;
  std::array<std::uint32_t, 3> lanes{core::AnyId::InvalidIndex,
                                     core::AnyId::InvalidIndex,
                                     core::AnyId::InvalidIndex};
  CallbackReleaseAuthority authority = CallbackReleaseAuthority::Count;
  std::uint32_t abi_epoch = 1;

  friend bool operator==(const ForeignOperationCallbackPlan &,
                         const ForeignOperationCallbackPlan &) = default;
};

enum class ForeignOperationCompletionRole : std::uint8_t {
  Wait,
  Poll,
  Arm,
  Detach,
  CancelAsync,
  Count,
};

struct ForeignOperationCompletionPlan {
  ForeignOperationPlanId operation;
  StableFingerprint operation_fingerprint;
  TypeId completion_carrier;
  std::vector<ForeignOperationCompletionRole> roles;
  CallbackWakePlanId wake_plan;
  interop::ForeignCompletionProjectionKind projection =
      interop::ForeignCompletionProjectionKind::None;
  interop::ForeignCompletionInputEffect input_effect =
      interop::ForeignCompletionInputEffect::Borrow;
  std::uint32_t carrier_lane = core::AnyId::InvalidIndex;
  std::uint32_t result_lane = core::AnyId::InvalidIndex;
  std::uint32_t readiness_success_literal = 1;
  std::vector<std::uint32_t> wake_callback_lanes;
  std::uint32_t family = 0;
  std::uint32_t abi_epoch = 1;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();

  friend bool operator==(const ForeignOperationCompletionPlan &,
                         const ForeignOperationCompletionPlan &) = default;
};

enum class CoroutineRegionEntryKind : std::uint8_t {
  Initial,
  Resume,
  ControlFlow,
  Count,
};

enum class CoroutineRegionEdgeKind : std::uint8_t {
  ControlFlow,
  Suspend,
  Success,
  Error,
  Cancelled,
  Count,
};

struct CoroutineSegment {
  LowBlockId block;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  CoroutineRegionId region;

  friend bool operator==(const CoroutineSegment &,
                         const CoroutineSegment &) = default;
};

struct CoroutineRegionEdge {
  CoroutineRegionEdgeKind kind = CoroutineRegionEdgeKind::Count;
  CoroutineRegionId source;
  CoroutineRegionId target;
  LowInstId suspension;
  std::uint32_t resume_state = 0;

  friend bool operator==(const CoroutineRegionEdge &,
                         const CoroutineRegionEdge &) = default;
};

struct CoroutineRegion {
  CoroutineRegionEntryKind entry_kind = CoroutineRegionEntryKind::Count;
  CoroutineSegmentId entry;
  std::vector<CoroutineSegmentId> segments;
  std::vector<CoroutineRegionEdge> exits;
  std::vector<LowInstId> live_values_in;
  std::vector<LowInstId> live_values_out;
  std::vector<SlotId> live_places_in;
  std::vector<SlotId> live_places_out;

  friend bool operator==(const CoroutineRegion &,
                         const CoroutineRegion &) = default;
};

struct CoroutineCleanupGraph {
  FunctionId function;
  InstId semantic_origin;
  LowBlockId entry;
  LowBlockListId blocks;
  std::vector<SlotId> local_slots;

  friend bool operator==(const CoroutineCleanupGraph &,
                         const CoroutineCleanupGraph &) = default;
};

struct CoroutineCancellationCleanup {
  LowInstId instruction;
  CoroutineCleanupGraphId cleanup;

  friend bool operator==(const CoroutineCancellationCleanup &,
                         const CoroutineCancellationCleanup &) = default;
};

enum class CoroutineTaskGroupExitIntent : std::uint8_t {
  Normal,
  SelectedError,
  SelectedCancellation,
  Count,
};

enum class CoroutineCancellationCausePolicy : std::uint8_t {
  OwnerRequestOnly,
  OwnerRequestOrUnexpectedChild,
  Count,
};

enum class CoroutineChildCancellationPolicy : std::uint8_t {
  EscalateUnexpected,
  PreserveSelectedExit,
  Count,
};

enum class CoroutineCancellationAcknowledgement : std::uint8_t {
  AtThisDrain,
  EnclosingTaskScope,
  Count,
};

struct CoroutineResumeState {
  enum class SuspensionKind : std::uint8_t {
    CallbackWake,
    TaskCompletion,
    TaskCompletionSet,
    TaskGroupDrain,
  };

  std::uint32_t state = 0;
  InstId semantic_suspension;
  LowInstId suspension;
  CoroutineRegionId suspension_region;
  CoroutineRegionId continuation_region;
  SlotId wake_slot;
  LowInstId result_value;
  SuspensionKind suspension_kind = SuspensionKind::CallbackWake;
  CallbackWakePlanId wake_plan;
  LowInstId task_group;
  CoroutineTaskGroupExitIntent task_group_exit_intent =
      CoroutineTaskGroupExitIntent::Normal;
  CoroutineCancellationCausePolicy cancellation_cause_policy =
      CoroutineCancellationCausePolicy::OwnerRequestOnly;
  CoroutineChildCancellationPolicy child_cancellation_policy =
      CoroutineChildCancellationPolicy::PreserveSelectedExit;
  CoroutineCancellationAcknowledgement cancellation_acknowledgement =
      CoroutineCancellationAcknowledgement::AtThisDrain;
  std::vector<LowInstId> live_values;
  std::vector<SlotId> live_places;
  std::vector<CoroutineFramePlaceId> cleanup_order;
  CoroutineCleanupGraphId pre_commit_cleanup;
  CoroutineCleanupGraphId transferred_cleanup;

  friend bool operator==(const CoroutineResumeState &,
                         const CoroutineResumeState &) = default;
};

struct CoroutineFramePlace {
  LowPlaceId place;
  std::uint32_t initialization_bit = core::AnyId::InvalidIndex;

  friend bool operator==(const CoroutineFramePlace &,
                         const CoroutineFramePlace &) = default;
};

enum class CoroutineStartPolicy : std::uint8_t {
  Eager,
  Count,
};

enum class CoroutineEvaluationPolicy : std::uint8_t {
  LeftToRightExactlyOnce,
  Count,
};

enum class CoroutineExecutorBindingPolicy : std::uint8_t {
  InheritAtCreation,
  Count,
};

enum class CoroutineExecutorSwitchPolicy : std::uint8_t {
  Persistent,
  Count,
};

enum class CoroutineDeadlineClockPolicy : std::uint8_t {
  RuntimeMonotonic,
  Count,
};

enum class CoroutineDeadlineRepresentation : std::uint8_t {
  AbsoluteNormalized,
  Count,
};

enum class CoroutineDeadlineInheritancePolicy : std::uint8_t {
  EarliestActive,
  Count,
};

enum class CoroutineDeadlineOutcomePolicy : std::uint8_t {
  StickyCancellation,
  Count,
};

enum class CoroutineDeadlineCausePrecedence : std::uint8_t {
  FirstLinearized,
  Count,
};

enum class CoroutineTerminalPrecedence : std::uint8_t {
  CancellationBeforeTerminalCommit,
  Count,
};

enum CoroutineCancellationPoint : std::uint32_t {
  CoroutineCancellationAtEntry = 1U << 0U,
  CoroutineCancellationAtSuspensionCommit = 1U << 1U,
  CoroutineCancellationAfterResume = 1U << 2U,
  CoroutineCancellationAtExplicitCheck = 1U << 3U,
  CoroutineCancellationAtExecutorSwitch = 1U << 4U,
};

inline constexpr std::uint32_t FrozenCoroutineCancellationPoints =
    CoroutineCancellationAtEntry | CoroutineCancellationAtSuspensionCommit |
    CoroutineCancellationAfterResume | CoroutineCancellationAtExplicitCheck |
    CoroutineCancellationAtExecutorSwitch;

struct CoroutineFramePlan {
  FunctionId function;
  PublicEntityId constructor_entity;
  std::uint32_t constructor_abi_epoch = 1;
  TypeId result_type;
  std::optional<TypeId> error_type;
  CoroutineRegionId initial_region;
  std::vector<CoroutineSegment> segments;
  std::vector<CoroutineRegion> regions;
  std::vector<LowInstId> frame_values;
  std::vector<SlotId> lifted_slots;
  std::vector<CoroutineFramePlace> frame_places;
  std::vector<CoroutineFramePlaceId> cleanup_order;
  std::vector<CoroutineResumeState> resume_states;
  std::vector<CoroutineCancellationCleanup> cancellation_cleanups;
  bool execution_entry = false;
  CoroutineStartPolicy start_policy = CoroutineStartPolicy::Eager;
  CoroutineEvaluationPolicy evaluation_policy =
      CoroutineEvaluationPolicy::LeftToRightExactlyOnce;
  CoroutineExecutorBindingPolicy executor_binding_policy =
      CoroutineExecutorBindingPolicy::InheritAtCreation;
  CoroutineExecutorSwitchPolicy executor_switch_policy =
      CoroutineExecutorSwitchPolicy::Persistent;
  CoroutineDeadlineClockPolicy deadline_clock_policy =
      CoroutineDeadlineClockPolicy::RuntimeMonotonic;
  CoroutineDeadlineRepresentation deadline_representation =
      CoroutineDeadlineRepresentation::AbsoluteNormalized;
  CoroutineDeadlineInheritancePolicy deadline_inheritance_policy =
      CoroutineDeadlineInheritancePolicy::EarliestActive;
  CoroutineDeadlineOutcomePolicy deadline_outcome_policy =
      CoroutineDeadlineOutcomePolicy::StickyCancellation;
  CoroutineDeadlineCausePrecedence deadline_cause_precedence =
      CoroutineDeadlineCausePrecedence::FirstLinearized;
  CoroutineTerminalPrecedence terminal_precedence =
      CoroutineTerminalPrecedence::CancellationBeforeTerminalCommit;
  std::uint32_t cancellation_points = FrozenCoroutineCancellationPoints;
  std::uint32_t abi_version = 11;

  friend bool operator==(const CoroutineFramePlan &,
                         const CoroutineFramePlan &) = default;
};

enum class CoroutineTaskCreateMode : std::uint8_t { Root, Child };

struct CoroutineTaskCreatePlan {
  FunctionRefId target;
  FunctionId scaffold;
  PublicEntityId constructor_entity;
  std::uint32_t constructor_abi_epoch = 1;
  TypeId task_type;
  CoroutineTaskCreateMode mode = CoroutineTaskCreateMode::Root;
  std::vector<TypeId> parameter_types;

  friend bool operator==(const CoroutineTaskCreatePlan &,
                         const CoroutineTaskCreatePlan &) = default;
};

struct CoroutineTaskCompletionArmPlan {
  FunctionId scaffold;
  TypeId task_type;
  TypeId completion_type;
  std::uint32_t abi_epoch = 1;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();

  friend bool operator==(const CoroutineTaskCompletionArmPlan &,
                         const CoroutineTaskCompletionArmPlan &) = default;
};

enum class CompletionProviderKind : std::uint8_t {
  Task,
  Operation,
  ForeignResource = Operation,
  Count,
};

struct CompletionProviderPlan {
  CompletionProviderKind kind = CompletionProviderKind::Count;
  TypeId completion_type;
  NominalTypeId resource_owner;
  ForeignResourceProtocolId protocol;
  CallbackWakePlanId wake_plan;
  ForeignOperationCompletionPlanId operation_completion;

  friend bool operator==(const CompletionProviderPlan &,
                         const CompletionProviderPlan &) = default;
};

struct CoroutineTaskCompletionSetPlan {
  FunctionId scaffold;
  TypeId set_type;
  CompletionProviderPlan provider;
  std::uint32_t operand_count = 0;
  std::uint32_t bitmap_word_count = 0;
  std::vector<InstId> ordered_operands;
  std::uint32_t abi_epoch = 1;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();

  friend bool operator==(const CoroutineTaskCompletionSetPlan &,
                         const CoroutineTaskCompletionSetPlan &) = default;
};

enum class CoroutineTaskCompletionCombineKind : std::uint8_t {
  WaitAll,
  Select,
  Race,
  Count,
};

enum class CoroutineTaskCompletionWinnerPolicy : std::uint8_t {
  None,
  LowestCanonicalIndex,
  Count,
};

enum class CoroutineTaskCompletionLoserPolicy : std::uint8_t {
  ConsumeAll,
  TransferRemaining,
  ReleaseRemaining,
  Count,
};

[[nodiscard]] constexpr std::uint32_t
coroutineTaskCompletionBitmapWordCount(std::uint32_t operand_count) {
  return operand_count / 64U + (operand_count % 64U != 0 ? 1U : 0U);
}

struct CoroutineTaskCompletionCombinePlan {
  FunctionId scaffold;
  CoroutineTaskCompletionCombineKind operation =
      CoroutineTaskCompletionCombineKind::Count;
  TypeId set_type;
  TypeId result_type;
  CompletionProviderPlan provider;
  std::uint32_t operand_count = 0;
  std::uint32_t bitmap_word_count = 0;
  std::vector<std::uint32_t> canonical_operand_order;
  CoroutineTaskCompletionWinnerPolicy winner_policy =
      CoroutineTaskCompletionWinnerPolicy::None;
  CoroutineTaskCompletionLoserPolicy loser_policy =
      CoroutineTaskCompletionLoserPolicy::ConsumeAll;
  InstId semantic_suspension;
  LowInstId continuation;
  std::uint32_t abi_epoch = 1;
  OutcomeDescriptorId outcome = OutcomeDescriptorId::invalid();

  friend bool operator==(const CoroutineTaskCompletionCombinePlan &,
                         const CoroutineTaskCompletionCombinePlan &) = default;
};

class LowIR;

// An OutcomeDescriptorId is meaningful only in the LowIR that owns its
// descriptor store. This wrapper mirrors SemIRRef and prevents a descriptor
// from one lowering session being accidentally queried through another.
struct OutcomeDescriptorRef {
  const LowIR *owner = nullptr;
  OutcomeDescriptorId id = OutcomeDescriptorId::invalid();

  [[nodiscard]] bool hasOwner() const { return owner != nullptr; }
  [[nodiscard]] bool hasId() const { return id.hasValue(); }
  [[nodiscard]] bool matches(const LowIR &candidate) const {
    return owner == &candidate;
  }
  [[nodiscard]] bool valid(const LowIR &candidate) const {
    return hasOwner() && hasId() && matches(candidate);
  }
  [[nodiscard]] const OutcomeDescriptor *checked(const LowIR &candidate) const;
};

class LowIR {
public:
  LowIR(core::Arena &arena, const SemIR &sem_ir,
        std::string normalized_target_triple = {},
        std::span<const NominalTypeLayoutArtifact> nominal_layouts = {},
        std::span<const LowNominalLayoutBinding> nominal_layout_bindings = {});

  template <typename InstT>
  [[nodiscard]] LowInstId addInst(InstT inst, InstId origin)
    requires requires {
      InstT::Kind;
      InstT::Arg0Kind;
      InstT::Arg1Kind;
    }
  {
    return addRawInst(
        {InstT::Kind, inst.type.index, inst.arg0.index, inst.arg1.index},
        origin);
  }
  [[nodiscard]] SlotId addSlot(LowSlot slot);
  [[nodiscard]] LowPlaceId addPlace(LowPlace place);
  [[nodiscard]] LowPlaceProjectionBlockId
  addPlaceProjectionBlock(std::span<const LowPlaceProjection> projections);
  [[nodiscard]] TargetPairId addTargets(TargetPair targets);
  [[nodiscard]] LowValueBlockId
  addValueBlock(std::span<const LowInstId> values);
  [[nodiscard]] LowBlockId addBlock(std::span<const LowInstId> instructions);
  [[nodiscard]] LowBlockListId addBlockList(std::span<const LowBlockId> blocks);
  [[nodiscard]] SlotBlockId addSlotBlock(std::span<const SlotId> slots);
  [[nodiscard]] LowFunctionId addFunction(LowFunction function);

  [[nodiscard]] const LowInst &inst(LowInstId id) const {
    return insts_.get(id);
  }
  template <typename InstT> [[nodiscard]] InstT getAs(LowInstId id) const {
    const auto &raw = inst(id);
    assert(raw.kind == InstT::Kind);
    return {TypeId(raw.type), typename InstT::Arg0Type(raw.arg0),
            typename InstT::Arg1Type(raw.arg1)};
  }
  [[nodiscard]] InstId origin(LowInstId id) const {
    return origins_.get(id);
  }
  [[nodiscard]] const LowSlot &slot(SlotId id) const {
    return slots_.get(id);
  }
  [[nodiscard]] const LowPlace &place(LowPlaceId id) const {
    return places_.get(id);
  }
  [[nodiscard]] std::span<const LowPlaceProjection>
  placeProjections(LowPlaceProjectionBlockId id) const {
    return place_projection_blocks_.get(id);
  }
  [[nodiscard]] std::span<const LowPlaceProjection>
  logicalPlaceProjections(LowPlaceId id) const {
    return place_projection_blocks_.get(place(id).logical_projections);
  }
  [[nodiscard]] const TargetPair &targets(TargetPairId id) const {
    return targets_.get(id);
  }
  [[nodiscard]] const LowFunction &function(LowFunctionId id) const {
    return functions_.get(id);
  }
  [[nodiscard]] std::span<const LowInstId> block(LowBlockId id) const {
    return inst_blocks_.get(blocks_.get(id));
  }
  [[nodiscard]] std::span<const LowBlockId> blockList(LowBlockListId id) const {
    return block_lists_.get(id);
  }
  [[nodiscard]] std::span<const SlotId> slotBlock(SlotBlockId id) const {
    return slot_blocks_.get(id);
  }
  [[nodiscard]] std::span<const LowInstId>
  valueBlock(LowValueBlockId id) const {
    return value_blocks_.get(id);
  }
  [[nodiscard]] std::size_t instCount() const {
    return insts_.size();
  }
  [[nodiscard]] std::size_t blockCount() const {
    return blocks_.size();
  }
  [[nodiscard]] std::size_t slotCount() const {
    return slots_.size();
  }
  [[nodiscard]] std::size_t placeCount() const {
    return places_.size();
  }
  [[nodiscard]] std::size_t functionCount() const {
    return functions_.size();
  }
  [[nodiscard]] const SemIR &semIR() const {
    return *sem_ir_;
  }
  template <typename Id> [[nodiscard]] SemIRRef<Id> semIRRef(Id id) const {
    return makeSemIRRef(*sem_ir_, id);
  }
  template <typename Id> [[nodiscard]] bool owns(SemIRRef<Id> ref) const {
    return ref.valid(*sem_ir_);
  }
  [[nodiscard]] const LowTypeRepresentation &
  typeRepresentation(TypeId type) const {
    return type_representations_.at(type.index);
  }
  [[nodiscard]] const LowEnumLayout *enumLayout(TypeId type) const {
    return type.index < enum_layouts_.size() && enum_layouts_[type.index]
               ? &*enum_layouts_[type.index]
               : nullptr;
  }
  [[nodiscard]] const LowNominalLayout *nominalLayout(TypeId type) const {
    return type.index < nominal_layouts_.size() && nominal_layouts_[type.index]
               ? &*nominal_layouts_[type.index]
               : nullptr;
  }
  [[nodiscard]] bool requiresNominalLayouts() const {
    return nominal_layouts_required_;
  }
  [[nodiscard]] const ForeignAbiFunctionLayout &
  foreignAbiLayout(ForeignAbiLayoutId id) const {
    return foreign_abi_layouts_.at(id.index);
  }
  [[nodiscard]] const ForeignAbiFunctionLayout &
  foreignAbiLayout(ForeignAbiCallLayoutId id) const {
    return foreignAbiLayout(foreignAbiCallLayout(id).function_layout);
  }
  [[nodiscard]] ForeignAbiLayoutId
  foreignAbiLayoutFor(FunctionRefId target) const {
    return target.index < foreign_abi_layout_by_target_.size()
               ? foreign_abi_layout_by_target_[target.index]
               : ForeignAbiLayoutId::invalid();
  }
  [[nodiscard]] ForeignAbiLayoutId
  foreignAbiLayoutForCallback(TypeId type) const {
    return type.index < foreign_abi_layout_by_callback_type_.size()
               ? foreign_abi_layout_by_callback_type_[type.index]
               : ForeignAbiLayoutId::invalid();
  }
  [[nodiscard]] std::size_t foreignAbiLayoutCount() const {
    return foreign_abi_layouts_.size();
  }
  [[nodiscard]] const ForeignAbiCallLayout &
  foreignAbiCallLayout(ForeignAbiCallLayoutId id) const {
    return foreign_abi_call_layouts_.at(id.index);
  }
  [[nodiscard]] ForeignAbiCallLayoutId
  addForeignAbiCallLayout(FunctionRefId target,
                          std::span<const TypeId> source_suffix_types,
                          std::string &error);
  [[nodiscard]] ForeignAbiCallLayoutId
  addForeignAbiCallLayout(TypeId callback_type,
                          std::span<const TypeId> source_suffix_types,
                          std::string &error);
  [[nodiscard]] std::size_t foreignAbiCallLayoutCount() const {
    return foreign_abi_call_layouts_.size();
  }
  [[nodiscard]] const ForeignAbiThunkPlan &
  foreignAbiThunkPlan(ForeignAbiThunkPlanId id) const {
    return foreign_abi_thunk_plans_.at(id.index);
  }
  [[nodiscard]] ForeignAbiThunkPlanId
  addForeignAbiThunkPlan(FunctionRefId source, TypeId callback_type,
                         std::string &error);
  [[nodiscard]] std::size_t foreignAbiThunkPlanCount() const {
    return foreign_abi_thunk_plans_.size();
  }
  [[nodiscard]] const CallbackAdapterPlan &
  callbackAdapterPlan(CallbackAdapterPlanId id) const {
    return callback_adapter_plans_.at(id.index);
  }
  [[nodiscard]] CallbackAdapterPlanId
  callbackAdapterPlanFor(TypeId adapter_type) const {
    return adapter_type.index < callback_adapter_plan_by_type_.size()
               ? callback_adapter_plan_by_type_[adapter_type.index]
               : CallbackAdapterPlanId::invalid();
  }
  [[nodiscard]] std::size_t callbackAdapterPlanCount() const {
    return callback_adapter_plans_.size();
  }
  [[nodiscard]] const CallbackRegistrationPlan &
  callbackRegistrationPlan(CallbackRegistrationPlanId id) const {
    return callback_registration_plans_.at(id.index);
  }
  [[nodiscard]] CallbackRegistrationPlanId
  callbackRegistrationPlanFor(TypeId type) const {
    return type.index < callback_registration_plan_by_type_.size()
               ? callback_registration_plan_by_type_[type.index]
               : CallbackRegistrationPlanId::invalid();
  }
  [[nodiscard]] std::size_t callbackRegistrationPlanCount() const {
    return callback_registration_plans_.size();
  }
  [[nodiscard]] const CallbackCompletionPlan &
  callbackCompletionPlan(CallbackCompletionPlanId id) const {
    return callback_completion_plans_.at(id.index);
  }
  [[nodiscard]] CallbackCompletionPlanId
  callbackCompletionPlanFor(TypeId type) const {
    return type.index < callback_completion_plan_by_type_.size()
               ? callback_completion_plan_by_type_[type.index]
               : CallbackCompletionPlanId::invalid();
  }
  [[nodiscard]] std::size_t callbackCompletionPlanCount() const {
    return callback_completion_plans_.size();
  }
  [[nodiscard]] const CallbackReadinessPlan &
  callbackReadinessPlan(CallbackReadinessPlanId id) const {
    return callback_readiness_plans_.at(id.index);
  }
  [[nodiscard]] CallbackReadinessPlanId
  callbackReadinessPlanFor(TypeId type) const {
    return type.index < callback_readiness_plan_by_type_.size()
               ? callback_readiness_plan_by_type_[type.index]
               : CallbackReadinessPlanId::invalid();
  }
  [[nodiscard]] std::size_t callbackReadinessPlanCount() const {
    return callback_readiness_plans_.size();
  }
  [[nodiscard]] const CallbackWakePlan &
  callbackWakePlan(CallbackWakePlanId id) const {
    return callback_wake_plans_.at(id.index);
  }
  [[nodiscard]] CallbackWakePlanId callbackWakePlanFor(TypeId type) const {
    return type.index < callback_wake_plan_by_type_.size()
               ? callback_wake_plan_by_type_[type.index]
               : CallbackWakePlanId::invalid();
  }
  [[nodiscard]] std::size_t callbackWakePlanCount() const {
    return callback_wake_plans_.size();
  }
  [[nodiscard]] ForeignOperationPlanId
  addForeignOperationPlan(ForeignOperationPlan plan) {
    plan.outcome = addOutcomeDescriptor(makeResultOutcome());
    return foreign_operation_plans_.add(std::move(plan));
  }
  [[nodiscard]] PayloadOperationPlanId
  addPayloadOperationPlan(PayloadOperationPlan plan) {
    if (!plan.outcome.hasValue())
      plan.outcome = addOutcomeDescriptor(makeChannelOutcome());
    return payload_operation_plans_.add(std::move(plan));
  }
  [[nodiscard]] const PayloadOperationPlan &
  payloadOperationPlan(PayloadOperationPlanId id) const {
    return payload_operation_plans_.get(id);
  }
  [[nodiscard]] std::size_t payloadOperationPlanCount() const {
    return payload_operation_plans_.size();
  }
  [[nodiscard]] std::span<const PayloadOperationPlan>
  payloadOperationPlans() const { return payload_operation_plans_.values(); }
  [[nodiscard]] ForeignCallOutcomePlanId
  addForeignCallOutcomePlan(ForeignCallOutcomePlan plan) {
    plan.outcome = addOutcomeDescriptor(
        plan.outcome_projection ==
                interop::ForeignOperationArtifact::OutcomeProjection::None
            ? makeResultOutcome()
            : makeForeignReadOutcome(true));
    return foreign_call_outcome_plans_.add(std::move(plan));
  }
  [[nodiscard]] const ForeignCallOutcomePlan &
  foreignCallOutcomePlan(ForeignCallOutcomePlanId id) const {
    return foreign_call_outcome_plans_.get(id);
  }
  [[nodiscard]] std::size_t foreignCallOutcomePlanCount() const {
    return foreign_call_outcome_plans_.size();
  }
  [[nodiscard]] const ForeignOperationPlan &
  foreignOperationPlan(ForeignOperationPlanId id) const {
    return foreign_operation_plans_.get(id);
  }
  [[nodiscard]] std::size_t foreignOperationPlanCount() const {
    return foreign_operation_plans_.size();
  }
  [[nodiscard]] ForeignOperationCallbackPlanId
  addForeignOperationCallbackPlan(ForeignOperationCallbackPlan plan) {
    return foreign_operation_callback_plans_.add(std::move(plan));
  }
  [[nodiscard]] const ForeignOperationCallbackPlan &
  foreignOperationCallbackPlan(ForeignOperationCallbackPlanId id) const {
    return foreign_operation_callback_plans_.get(id);
  }
  [[nodiscard]] std::size_t foreignOperationCallbackPlanCount() const {
    return foreign_operation_callback_plans_.size();
  }
  [[nodiscard]] ForeignOperationCompletionPlanId
  addForeignOperationCompletionPlan(ForeignOperationCompletionPlan plan) {
    plan.outcome = addOutcomeDescriptor(makeTaskOutcome(true));
    return foreign_operation_completion_plans_.add(std::move(plan));
  }
  [[nodiscard]] const ForeignOperationCompletionPlan &
  foreignOperationCompletionPlan(ForeignOperationCompletionPlanId id) const {
    return foreign_operation_completion_plans_.get(id);
  }
  [[nodiscard]] std::size_t foreignOperationCompletionPlanCount() const {
    return foreign_operation_completion_plans_.size();
  }
  [[nodiscard]] ForeignOperationCompletionPlanId
  foreignOperationCompletionPlanFor(StableFingerprint fingerprint) const {
    for (std::uint32_t index = 0;
         index < foreign_operation_completion_plans_.size(); ++index) {
      const auto id = ForeignOperationCompletionPlanId(index);
      if (foreign_operation_completion_plans_.get(id).operation_fingerprint ==
          fingerprint)
        return id;
    }
    return ForeignOperationCompletionPlanId::invalid();
  }
  [[nodiscard]] CoroutineFramePlanId
  addCoroutineFramePlan(CoroutineFramePlan plan) {
    return coroutine_frame_plans_.add(std::move(plan));
  }
  [[nodiscard]] const CoroutineFramePlan &
  coroutineFramePlan(CoroutineFramePlanId id) const {
    return coroutine_frame_plans_.get(id);
  }
  [[nodiscard]] std::size_t coroutineFramePlanCount() const {
    return coroutine_frame_plans_.size();
  }
  [[nodiscard]] CoroutineCleanupGraphId
  addCoroutineCleanupGraph(CoroutineCleanupGraph graph) {
    return coroutine_cleanup_graphs_.add(std::move(graph));
  }
  [[nodiscard]] const CoroutineCleanupGraph &
  coroutineCleanupGraph(CoroutineCleanupGraphId id) const {
    return coroutine_cleanup_graphs_.get(id);
  }
  [[nodiscard]] std::size_t coroutineCleanupGraphCount() const {
    return coroutine_cleanup_graphs_.size();
  }
  [[nodiscard]] CoroutineTaskCreatePlanId
  addCoroutineTaskCreatePlan(CoroutineTaskCreatePlan plan) {
    return coroutine_task_create_plans_.add(std::move(plan));
  }
  [[nodiscard]] const CoroutineTaskCreatePlan &
  coroutineTaskCreatePlan(CoroutineTaskCreatePlanId id) const {
    return coroutine_task_create_plans_.get(id);
  }
  [[nodiscard]] std::size_t coroutineTaskCreatePlanCount() const {
    return coroutine_task_create_plans_.size();
  }
  [[nodiscard]] CoroutineTaskCompletionArmPlanId
  addCoroutineTaskCompletionArmPlan(CoroutineTaskCompletionArmPlan plan) {
    plan.outcome = addOutcomeDescriptor(makeTaskOutcome(true));
    return coroutine_task_completion_arm_plans_.add(std::move(plan));
  }
  [[nodiscard]] const CoroutineTaskCompletionArmPlan &
  coroutineTaskCompletionArmPlan(CoroutineTaskCompletionArmPlanId id) const {
    return coroutine_task_completion_arm_plans_.get(id);
  }
  [[nodiscard]] std::size_t coroutineTaskCompletionArmPlanCount() const {
    return coroutine_task_completion_arm_plans_.size();
  }
  [[nodiscard]] CoroutineTaskCompletionSetPlanId
  addCoroutineTaskCompletionSetPlan(CoroutineTaskCompletionSetPlan plan) {
    plan.outcome = addOutcomeDescriptor(makeCompletionSetOutcome());
    return coroutine_task_completion_set_plans_.add(std::move(plan));
  }
  [[nodiscard]] const CoroutineTaskCompletionSetPlan &
  coroutineTaskCompletionSetPlan(CoroutineTaskCompletionSetPlanId id) const {
    return coroutine_task_completion_set_plans_.get(id);
  }
  [[nodiscard]] std::size_t coroutineTaskCompletionSetPlanCount() const {
    return coroutine_task_completion_set_plans_.size();
  }
  [[nodiscard]] CoroutineTaskCompletionCombinePlanId
  addCoroutineTaskCompletionCombinePlan(
      CoroutineTaskCompletionCombinePlan plan) {
    plan.outcome = addOutcomeDescriptor(makeCompletionSetOutcome());
    return coroutine_task_completion_combine_plans_.add(std::move(plan));
  }
  [[nodiscard]] const CoroutineTaskCompletionCombinePlan &
  coroutineTaskCompletionCombinePlan(
      CoroutineTaskCompletionCombinePlanId id) const {
    return coroutine_task_completion_combine_plans_.get(id);
  }
  void bindCoroutineTaskCompletionCombineContinuation(
      CoroutineTaskCompletionCombinePlanId id, LowInstId continuation) {
    auto &plan = coroutine_task_completion_combine_plans_.get(id);
    assert(!plan.continuation.hasValue() && continuation.hasValue());
    plan.continuation = continuation;
  }
  [[nodiscard]] std::size_t coroutineTaskCompletionCombinePlanCount() const {
    return coroutine_task_completion_combine_plans_.size();
  }
  [[nodiscard]] ConstructPlanId addConstructPlan(ConstructPlan plan) {
    return construct_plans_.add(std::move(plan));
  }
  [[nodiscard]] const ConstructPlan &constructPlan(ConstructPlanId id) const {
    return construct_plans_.get(id);
  }
  [[nodiscard]] std::size_t constructPlanCount() const {
    return construct_plans_.size();
  }
  [[nodiscard]] CompletionProviderPlan
  completionProviderFor(TypeId aggregate_type) const;
  [[nodiscard]] OutcomeDescriptorId
  addOutcomeDescriptor(OutcomeDescriptor descriptor) {
    std::string error;
    if (!descriptor.verify(error))
      return OutcomeDescriptorId::invalid();
    const auto key = descriptor.fingerprint().hex();
    if (const auto found = outcome_descriptor_by_fingerprint_.find(key);
        found != outcome_descriptor_by_fingerprint_.end())
      return found->second;
    const auto id = OutcomeDescriptorId(
        static_cast<std::uint32_t>(outcome_descriptors_.size()));
    outcome_descriptor_by_fingerprint_.emplace(key, id);
    return outcome_descriptors_.add(std::move(descriptor));
  }
  [[nodiscard]] const OutcomeDescriptor &
  outcomeDescriptor(OutcomeDescriptorId id) const {
    return outcome_descriptors_.get(id);
  }
  [[nodiscard]] OutcomeDescriptorRef
  outcomeDescriptorRef(OutcomeDescriptorId id) const {
    return {this, id};
  }
  // Owner-safe access for verification and deferred semantic consumers. The
  // descriptor ID is session-local; callers must not turn an invalid or
  // foreign ID into an unchecked ValueStore access.
  [[nodiscard]] const OutcomeDescriptor *
  checkedOutcomeDescriptor(OutcomeDescriptorId id) const {
    if (!id.hasValue() || id.index >= outcome_descriptors_.size())
      return nullptr;
    return &outcome_descriptors_.get(id);
  }
  [[nodiscard]] const OutcomeDescriptor *
  checkedOutcomeDescriptor(OutcomeDescriptorRef ref) const {
    if (!ref.valid(*this))
      return nullptr;
    return checkedOutcomeDescriptor(ref.id);
  }
  [[nodiscard]] std::size_t outcomeDescriptorCount() const {
    return outcome_descriptors_.size();
  }
  void setCoroutineLoweringError(std::string error) {
    if (coroutine_lowering_error_.empty())
      coroutine_lowering_error_ = std::move(error);
  }
  [[nodiscard]] TypeId cDefaultPromotedType(TypeId type) const;
  [[nodiscard]] std::string_view targetTriple() const {
    return normalized_target_triple_;
  }

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string print() const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  friend class internal::LowIRVerificationContext;
  [[nodiscard]] LowInstId addRawInst(LowInst inst, InstId origin);
  [[nodiscard]] bool containsArg(LowArgKind kind, std::uint32_t raw) const;
  void buildForeignAbiLayouts();
  [[nodiscard]] std::optional<ForeignAbiFunctionLayout>
  buildForeignAbiLayout(FunctionRefId target, std::string &error) const;
  [[nodiscard]] std::optional<ForeignAbiFunctionLayout>
  buildCallbackAbiLayout(TypeId callback_type, std::string &error) const;
  [[nodiscard]] std::optional<ForeignAbiValueLayout>
  buildForeignAbiValueLayout(TypeId type, bool result,
                             std::string &error) const;
  [[nodiscard]] std::optional<ForeignAbiThunkPlan>
  buildForeignAbiThunkPlan(FunctionRefId source, TypeId callback_type,
                           std::string &error) const;
  [[nodiscard]] std::optional<CallbackAdapterPlan>
  buildCallbackAdapterPlan(TypeId adapter_type, std::string &error) const;
  [[nodiscard]] std::optional<CallbackRegistrationPlan>
  buildCallbackRegistrationPlan(TypeId type, std::string &error) const;
  [[nodiscard]] std::optional<CallbackCompletionPlan>
  buildCallbackCompletionPlan(TypeId type, std::string &error) const;
  [[nodiscard]] std::optional<CallbackReadinessPlan>
  buildCallbackReadinessPlan(TypeId type, std::string &error) const;
  [[nodiscard]] std::optional<CallbackWakePlan>
  buildCallbackWakePlan(TypeId type, std::string &error) const;
  [[nodiscard]] ForeignAbiCallLayoutId
  addForeignAbiCallLayout(ForeignAbiLayoutId fixed_id,
                          std::span<const TypeId> source_suffix_types,
                          std::string &error);
  const SemIR *sem_ir_;
  std::string normalized_target_triple_;
  std::string foreign_abi_error_;
  std::string nominal_layout_error_;
  bool nominal_layouts_required_ = false;
  std::vector<LowTypeRepresentation> type_representations_;
  std::vector<std::optional<LowEnumLayout>> enum_layouts_;
  std::vector<std::optional<LowNominalLayout>> nominal_layouts_;
  std::vector<ForeignAbiFunctionLayout> foreign_abi_layouts_;
  std::vector<ForeignAbiLayoutId> foreign_abi_layout_by_target_;
  std::vector<ForeignAbiLayoutId> foreign_abi_layout_by_callback_type_;
  std::vector<ForeignAbiCallLayout> foreign_abi_call_layouts_;
  std::vector<ForeignAbiThunkPlan> foreign_abi_thunk_plans_;
  std::vector<CallbackAdapterPlan> callback_adapter_plans_;
  std::vector<CallbackAdapterPlanId> callback_adapter_plan_by_type_;
  std::vector<CallbackRegistrationPlan> callback_registration_plans_;
  std::vector<CallbackRegistrationPlanId> callback_registration_plan_by_type_;
  std::vector<CallbackCompletionPlan> callback_completion_plans_;
  std::vector<CallbackCompletionPlanId> callback_completion_plan_by_type_;
  std::vector<CallbackReadinessPlan> callback_readiness_plans_;
  std::vector<CallbackReadinessPlanId> callback_readiness_plan_by_type_;
  std::vector<CallbackWakePlan> callback_wake_plans_;
  std::vector<CallbackWakePlanId> callback_wake_plan_by_type_;
  core::ValueStore<ForeignOperationPlanId, ForeignOperationPlan>
      foreign_operation_plans_;
  core::ValueStore<PayloadOperationPlanId, PayloadOperationPlan>
      payload_operation_plans_;
  core::ValueStore<ForeignCallOutcomePlanId, ForeignCallOutcomePlan>
      foreign_call_outcome_plans_;
  core::ValueStore<ForeignOperationCallbackPlanId, ForeignOperationCallbackPlan>
      foreign_operation_callback_plans_;
  core::ValueStore<ForeignOperationCompletionPlanId,
                   ForeignOperationCompletionPlan>
      foreign_operation_completion_plans_;
  core::ValueStore<OutcomeDescriptorId, OutcomeDescriptor>
      outcome_descriptors_;
  std::unordered_map<std::string, OutcomeDescriptorId>
      outcome_descriptor_by_fingerprint_;
  core::ValueStore<CoroutineFramePlanId, CoroutineFramePlan>
      coroutine_frame_plans_;
  core::ValueStore<CoroutineCleanupGraphId, CoroutineCleanupGraph>
      coroutine_cleanup_graphs_;
  core::ValueStore<CoroutineTaskCreatePlanId, CoroutineTaskCreatePlan>
      coroutine_task_create_plans_;
  core::ValueStore<CoroutineTaskCompletionArmPlanId,
                   CoroutineTaskCompletionArmPlan>
      coroutine_task_completion_arm_plans_;
  core::ValueStore<CoroutineTaskCompletionSetPlanId,
                   CoroutineTaskCompletionSetPlan>
      coroutine_task_completion_set_plans_;
  core::ValueStore<CoroutineTaskCompletionCombinePlanId,
                   CoroutineTaskCompletionCombinePlan>
      coroutine_task_completion_combine_plans_;
  core::ValueStore<ConstructPlanId, ConstructPlan> construct_plans_;
  std::string coroutine_lowering_error_;
  core::ValueStore<LowInstId, LowInst> insts_;
  core::ValueStore<LowInstId, InstId> origins_;
  core::ValueStore<SlotId, LowSlot> slots_;
  core::ValueStore<LowPlaceId, LowPlace> places_;
  core::ValueStore<TargetPairId, TargetPair> targets_;
  core::ValueStore<LowBlockId, LowInstBlockId> blocks_;
  core::ValueStore<LowFunctionId, LowFunction> functions_;
  core::BlockStore<LowInstBlockId, LowInstId> inst_blocks_;
  core::BlockStore<LowBlockListId, LowBlockId> block_lists_;
  core::BlockStore<SlotBlockId, SlotId> slot_blocks_;
  core::BlockStore<LowValueBlockId, LowInstId> value_blocks_;
  core::BlockStore<LowPlaceProjectionBlockId, LowPlaceProjection>
      place_projection_blocks_;
};

inline const OutcomeDescriptor *
OutcomeDescriptorRef::checked(const LowIR &candidate) const {
  return candidate.checkedOutcomeDescriptor(*this);
}

[[nodiscard]] std::string_view lowInstKindName(LowInstKind kind);
[[nodiscard]] LowArgKind lowInstArgKind(LowInstKind kind, std::size_t index);

template <typename Fn> void visitLowInst(const LowInst &inst, Fn &&fn) {
  switch (inst.kind) {
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1)                               \
  case LowInstKind::Name:                                                      \
    std::forward<Fn>(fn)(Low##Name{TypeId(inst.type),                          \
                                   Low##Name::Arg0Type(inst.arg0),             \
                                   Low##Name::Arg1Type(inst.arg1)});           \
    return;
#include "chtholly/Compiler/LowIRKind.def"
  case LowInstKind::Count:
    return;
  }
}

static_assert(sizeof(LowInst) == 16);
static_assert(sizeof(LowInvalid) == 12);
static_assert(sizeof(LowSlot) == 16);
static_assert(sizeof(LowPlace) == 20);
static_assert(sizeof(TargetPair) == 8);
static_assert(sizeof(LowFunction) == 16);

} // namespace chtholly::compiler
