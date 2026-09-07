#pragma once

#include "chtholly/Core/Arena.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/CompilationIds.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"
#include "chtholly/Compiler/InteropArtifact.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chtholly::compiler {

class SemIR;
class SharedValueStores;

namespace internal {
class PublicInterfaceVerifyService;
class PublicInterfaceCanonicalizeService;
class PublicInterfaceRegistryService;
class PublicInterfaceArtifactVerificationService;
class PublicInterfaceArtifactBuildService;
class PublicInterfaceRegistryConstructionService;
} // namespace internal

struct PublicInterfaceId : core::IndexBase<PublicInterfaceId> {
  using IndexBase::IndexBase;
};

struct PublicBindingId : core::IndexBase<PublicBindingId> {
  using IndexBase::IndexBase;
};

using PublicTypeBlockId = core::BlockId<struct PublicTypeBlockTag>;

class StableFingerprint {
public:
  static constexpr std::size_t ByteCount = 32;

  StableFingerprint() = default;
  explicit StableFingerprint(std::array<std::uint8_t, ByteCount> bytes)
      : bytes_(bytes) {}

  [[nodiscard]] static StableFingerprint
  fromCanonicalBytes(std::string_view bytes);

  [[nodiscard]] std::span<const std::uint8_t, ByteCount> bytes() const {
    return bytes_;
  }
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool hasValue() const;

  friend bool operator==(const StableFingerprint &,
                         const StableFingerprint &) = default;

private:
  std::array<std::uint8_t, ByteCount> bytes_{};
};

enum class PublicEntityKind : std::uint8_t {
  Function,
  NominalType,
  Interface,
  TypeAlias,
  ForeignOperation,
  Count,
};

struct PublicEntityReferenceArtifact {
  PublicEntityKind kind = PublicEntityKind::Function;
  std::string canonical_package;
  std::string canonical_module;
  std::string canonical_name;
  StableFingerprint expected_fingerprint;

  friend bool operator==(const PublicEntityReferenceArtifact &,
                         const PublicEntityReferenceArtifact &) = default;
};

enum class PublicTypeKind : std::uint32_t {
  Void = 0,
  Bool = 1,
  Integer = 2,
  Float = 3,
  TypeParameter = 4,
  Nominal = 5,
  Reference = 6,
  RawPointer = 7,
  String = 8,
  Array = 9,
  CFunctionPointer = 10,
  CallbackAdapter = 11,
  CallbackRegistration = 12,
  CallbackCompletion = 13,
  CallbackWake = 14,
  ForeignCompletion = 15,
  ForeignWake = 16,
  Function = 17,
  Never = 18,
  Tuple = 19,
  Slice = 20,
  TypeProjection = 21,
  ForeignOperationState = 22,
  Char = 23,
  Count,
};

enum class ForeignOperationStateKind : std::uint8_t {
  Subscription,
  Completion,
  Wake,
  Count,
};

enum class PublicTypeProjectionKind : std::uint8_t {
  Element,
  Pointee,
  Associated,
  Count,
};

enum class PublicReferenceMutability : std::uint8_t {
  ReadOnly,
  Mutable,
  Count,
};

enum class PublicReferenceProvenanceKind : std::uint8_t {
  Erased,
  Parameter,
  Count,
};

enum class ForeignCallingConvention : std::uint8_t {
  C,
  Win64,
  SysV64,
  Count,
};

enum class OwnershipRegionStepKind : std::uint8_t {
  Field,
  StaticElement,
  AnyElement,
  Dereference,
  Count,
};

struct OwnershipRegionStep {
  OwnershipRegionStepKind kind = OwnershipRegionStepKind::Count;
  std::uint32_t index = 0;

  friend bool operator==(const OwnershipRegionStep &,
                         const OwnershipRegionStep &) = default;
  friend auto operator<=>(const OwnershipRegionStep &,
                          const OwnershipRegionStep &) = default;
};

struct OwnershipRegion {
  std::uint32_t parameter_index = core::AnyId::InvalidIndex;
  std::vector<OwnershipRegionStep> path;
  bool has_bit_range = false;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;

  friend bool operator==(const OwnershipRegion &,
                         const OwnershipRegion &) = default;
};

enum class CallableEffectKind : std::uint8_t {
  Read,
  Write,
  Take,
  BorrowShared,
  BorrowMutable,
  Initialize,
  Count,
};

enum class PublicCallableDeclarationKind : std::uint8_t {
  Definition,
  Foreign,
  Forward,
  Count,
};

struct CallableRegionEffect {
  CallableEffectKind kind = CallableEffectKind::Count;
  OwnershipRegion region;

  friend bool operator==(const CallableRegionEffect &,
                         const CallableRegionEffect &) = default;
};

enum CallablePostconditionOutcome : std::uint8_t {
  CallableOutcomePreserve = 1U << 0U,
  CallableOutcomeInitialize = 1U << 1U,
  CallableOutcomeInvalidate = 1U << 2U,
  CallableOutcomeAll = CallableOutcomePreserve | CallableOutcomeInitialize |
                       CallableOutcomeInvalidate,
};

struct CallableConditionAtom {
  std::uint32_t parameter_index = core::AnyId::InvalidIndex;
  bool expected = true;
  // InvalidIndex denotes a boolean parameter, otherwise an enum variant.
  std::uint32_t variant = core::AnyId::InvalidIndex;

  friend bool operator==(const CallableConditionAtom &,
                         const CallableConditionAtom &) = default;
  friend auto operator<=>(const CallableConditionAtom &,
                          const CallableConditionAtom &) = default;
};

struct CallableConditionClause {
  std::vector<CallableConditionAtom> atoms;

  friend bool operator==(const CallableConditionClause &,
                         const CallableConditionClause &) = default;
  friend auto operator<=>(const CallableConditionClause &,
                          const CallableConditionClause &) = default;
};

// Canonical DNF over boolean semantic parameters. No clauses is false and one
// empty clause is true. Operations widen to true when the persisted complexity
// limits would be exceeded.
struct CallableConditionDescriptor {
  std::vector<CallableConditionClause> clauses;
  bool exact = true;

  [[nodiscard]] static CallableConditionDescriptor always();
  [[nodiscard]] static CallableConditionDescriptor never();
  [[nodiscard]] static CallableConditionDescriptor unknown();
  [[nodiscard]] static CallableConditionDescriptor enumVariant(std::uint32_t parameter, std::uint32_t variant);
  [[nodiscard]] static CallableConditionDescriptor
  atom(std::uint32_t parameter_index, bool expected = true);
  [[nodiscard]] bool isAlways() const;
  [[nodiscard]] bool isNever() const {
    return exact && clauses.empty();
  }
  void canonicalize();
  [[nodiscard]] bool verify(std::uint32_t parameter_count,
                            std::string &error) const;

  friend bool operator==(const CallableConditionDescriptor &,
                         const CallableConditionDescriptor &) = default;
};

[[nodiscard]] CallableConditionDescriptor
conditionAnd(CallableConditionDescriptor lhs,
             const CallableConditionDescriptor &rhs);
[[nodiscard]] CallableConditionDescriptor
conditionOr(CallableConditionDescriptor lhs,
            const CallableConditionDescriptor &rhs);
[[nodiscard]] CallableConditionDescriptor
conditionNot(const CallableConditionDescriptor &condition);

struct CallableRegionPostcondition {
  OwnershipRegion region;
  std::uint8_t outcomes = CallableOutcomePreserve;
  // The postcondition is guaranteed only on paths satisfying this
  // compiler-owned predicate. An empty predicate is false; one empty clause
  // is true. Keeping the predicate on the summary (rather than in source
  // contracts) lets ordinary control flow and imported artifacts preserve
  // branch-sensitive ownership facts without adding a second contract
  // language.
  CallableConditionDescriptor condition =
      CallableConditionDescriptor::always();
  // A guarantee applies only when this returned enum variant is selected.
  std::uint32_t result_variant = core::AnyId::InvalidIndex;

  friend bool operator==(const CallableRegionPostcondition &,
                         const CallableRegionPostcondition &) = default;
};

struct CallableReturnSource {
  enum class CarrierStepKind : std::uint8_t {
    EnumVariant,
    Field,
    Count,
  };

  struct CarrierStep {
    CarrierStepKind kind = CarrierStepKind::Field;
    std::uint32_t index = core::AnyId::InvalidIndex;

    friend bool operator==(const CarrierStep &, const CarrierStep &) = default;
    friend auto operator<=>(const CarrierStep &, const CarrierStep &) = default;
  };

  OwnershipRegion region;
  // Empty means the returned value itself is the borrowed carrier. Non-empty
  // paths are restricted to statically known enum variants and fields.
  std::vector<CarrierStep> carrier_path;
  CallableConditionDescriptor condition = CallableConditionDescriptor::always();

  friend bool operator==(const CallableReturnSource &,
                         const CallableReturnSource &) = default;
};

[[nodiscard]] bool ownershipRegionCovers(const OwnershipRegion &allowed,
                                         const OwnershipRegion &actual);

struct CallableOwnershipSummary {
  std::vector<CallableRegionEffect> effects;
  std::vector<CallableRegionPostcondition> postconditions;
  bool returns_owned = true;
  std::vector<CallableReturnSource> return_provenance;

  void canonicalize();
  [[nodiscard]] bool verify(std::uint32_t parameter_count,
                            std::string &error) const;

  friend bool operator==(const CallableOwnershipSummary &,
                         const CallableOwnershipSummary &) = default;
};

struct PublicReferenceProvenance {
  PublicReferenceProvenanceKind kind = PublicReferenceProvenanceKind::Erased;
  std::uint32_t index = core::AnyId::InvalidIndex;

  friend bool operator==(const PublicReferenceProvenance &,
                         const PublicReferenceProvenance &) = default;
};

struct CallbackRegistrationBinding {
  std::string name;
  std::uint32_t parameter_index = core::AnyId::InvalidIndex;

  friend bool operator==(const CallbackRegistrationBinding &,
                         const CallbackRegistrationBinding &) = default;
};

enum class ForeignResourceRoleKind : std::uint8_t {
  AcquireOwned,
  Borrow,
  CloseQuiescent,
  CancelQuiescent,
  CancelAsync,
  WaitCompletion,
  InspectReady,
  ArmOneShot,
  DetachCompletion,
  Count,
};

enum class ForeignResourceParameterKind : std::uint8_t {
  Resource,
  Completion,
  CallbackEntry,
  CallbackUserdata,
  CallbackRelease,
  WakerEntry,
  WakerUserdata,
  WakerRelease,
  Bound,
  Count,
};

enum class ForeignResourceInvalidState : std::uint8_t {
  Null,
  Integer,
  PointerBitPattern,
  Count,
};

enum class ForeignResourceQuiescence : std::uint8_t {
  None,
  Quiescent,
  NonQuiescent,
  Count,
};

struct ForeignResourceParameterBinding {
  ForeignResourceParameterKind kind = ForeignResourceParameterKind::Count;
  std::uint32_t parameter_index = core::AnyId::InvalidIndex;
  std::string name;

  friend bool operator==(const ForeignResourceParameterBinding &,
                         const ForeignResourceParameterBinding &) = default;
  friend auto operator<=>(const ForeignResourceParameterBinding &,
                          const ForeignResourceParameterBinding &) = default;
};

struct ForeignResourceRole {
  ForeignResourceRoleKind kind = ForeignResourceRoleKind::Count;
  std::uint32_t callable_type_index = core::AnyId::InvalidIndex;
  ForeignResourceQuiescence quiescence = ForeignResourceQuiescence::None;
  std::vector<ForeignResourceParameterBinding> parameters;

  friend bool operator==(const ForeignResourceRole &,
                         const ForeignResourceRole &) = default;
};

struct ForeignResourceProtocol {
  static constexpr std::uint32_t CurrentSemanticEpoch = 5;

  std::uint32_t semantic_epoch = CurrentSemanticEpoch;
  bool completion_projection = false;
  std::uint32_t callback_type_index = core::AnyId::InvalidIndex;
  std::uint32_t resource_type_index = core::AnyId::InvalidIndex;
  std::uint32_t completion_type_index = core::AnyId::InvalidIndex;
  ForeignResourceInvalidState invalid_state = ForeignResourceInvalidState::Null;
  std::int64_t invalid_integer = 0;
  std::uint8_t release_authority = 0;
  std::vector<ForeignResourceRoleKind> cleanup_path;
  std::vector<ForeignResourceRoleKind> completion_cleanup_path;
  std::vector<ForeignResourceRoleKind> wake_cleanup_path;
  std::vector<ForeignResourceRole> roles;

  void canonicalize() {
    for (auto &role : roles) {
      std::ranges::sort(role.parameters, [](const auto &lhs, const auto &rhs) {
        return std::tie(lhs.kind, lhs.parameter_index, lhs.name) <
               std::tie(rhs.kind, rhs.parameter_index, rhs.name);
      });
    }
    std::ranges::sort(roles, [](const auto &lhs, const auto &rhs) {
      return lhs.kind < rhs.kind;
    });
  }
  [[nodiscard]] bool verify(std::uint32_t type_count, std::string &error) const;
  [[nodiscard]] const ForeignResourceRole *
  findRole(ForeignResourceRoleKind kind) const;

  friend bool operator==(const ForeignResourceProtocol &,
                         const ForeignResourceProtocol &) = default;
};

[[nodiscard]] std::string_view
foreignResourceRoleKindName(ForeignResourceRoleKind kind);

[[nodiscard]] ForeignResourceProtocol makeCallbackRegistrationProtocol(
    std::uint8_t authority, std::uint32_t entry_parameter,
    std::uint32_t userdata_parameter, std::uint32_t release_parameter,
    std::span<const CallbackRegistrationBinding> bindings,
    std::uint32_t argument_count, std::array<std::uint32_t, 4> arm_parameters,
    std::array<std::uint32_t, 3> detach_parameters);
[[nodiscard]] ForeignResourceProtocol
makeCallbackCompletionProtocol(std::uint8_t authority,
                               std::uint32_t argument_count,
                               std::array<std::uint32_t, 4> arm_parameters,
                               std::array<std::uint32_t, 3> detach_parameters);
[[nodiscard]] std::string
encodeForeignResourceProtocol(const ForeignResourceProtocol &protocol);
[[nodiscard]] std::optional<ForeignResourceProtocol>
decodeForeignResourceProtocol(std::string_view bytes, std::uint32_t type_count,
                              std::string &error);

struct PublicType {
  PublicTypeKind kind = PublicTypeKind::Count;
  std::uint32_t binding_index = core::AnyId::InvalidIndex;
  std::uint32_t scalar_width = 0;
  bool integer_signed = false;
  PublicEntityReferenceArtifact nominal_entity;
  std::vector<PublicType> arguments;
  std::uint32_t array_bound = 0;
  PublicReferenceMutability reference_mutability =
      PublicReferenceMutability::ReadOnly;
  PublicReferenceProvenance reference_provenance;
  bool pointer_const = false;
  bool slice_mutable = false;
  PublicTypeProjectionKind projection_kind = PublicTypeProjectionKind::Count;
  std::uint32_t projection_index = core::AnyId::InvalidIndex;
  bool callable_variadic = false;
  bool abi_union = false;
  ForeignCallingConvention foreign_calling_convention =
      ForeignCallingConvention::C;
  std::uint32_t callable_context_parameter = core::AnyId::InvalidIndex;
  CallableOwnershipSummary callable_contract;
  std::uint8_t registration_authority = 0;
  std::uint32_t registration_entry_parameter = core::AnyId::InvalidIndex;
  std::uint32_t registration_userdata_parameter = core::AnyId::InvalidIndex;
  std::uint32_t registration_release_parameter = core::AnyId::InvalidIndex;
  std::vector<CallbackRegistrationBinding> registration_bindings;
  std::array<std::uint32_t, 4> registration_arm_parameters{
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex};
  std::array<std::uint32_t, 3> registration_detach_parameters{
      core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
      core::AnyId::InvalidIndex};
  ForeignResourceProtocol foreign_resource_protocol;
  ForeignOperationStateKind foreign_operation_state =
      ForeignOperationStateKind::Count;

  PublicType() = default;
  PublicType(PublicTypeKind value) : kind(value) {}
  PublicType(PublicTypeKind value, std::uint32_t index)
      : kind(value), binding_index(index) {}
  static PublicType integer(std::uint32_t width, bool is_signed) {
    PublicType result(PublicTypeKind::Integer);
    result.scalar_width = width;
    result.integer_signed = is_signed;
    return result;
  }
  static PublicType floating(std::uint32_t width) {
    PublicType result(PublicTypeKind::Float);
    result.scalar_width = width;
    return result;
  }
  PublicType(PublicEntityReferenceArtifact entity,
             std::vector<PublicType> type_arguments = {})
      : kind(PublicTypeKind::Nominal), nominal_entity(std::move(entity)),
        arguments(std::move(type_arguments)) {}
  PublicType(PublicType pointee, PublicReferenceMutability mutability,
             PublicReferenceProvenance provenance = {})
      : kind(PublicTypeKind::Reference), arguments{std::move(pointee)},
        reference_mutability(mutability), reference_provenance(provenance) {}
  PublicType(PublicType element, std::uint32_t bound)
      : kind(PublicTypeKind::Array), arguments{std::move(element)},
        array_bound(bound) {}
  static PublicType tuple(std::vector<PublicType> elements) {
    PublicType result(PublicTypeKind::Tuple);
    result.arguments = std::move(elements);
    return result;
  }
  static PublicType cUnion(std::vector<PublicType> members) {
    auto result = tuple(std::move(members));
    result.abi_union = true;
    return result;
  }
  static PublicType slice(PublicType element, bool mutable_view = false) {
    PublicType result(PublicTypeKind::Slice);
    result.arguments.push_back(std::move(element));
    result.slice_mutable = mutable_view;
    return result;
  }
  static PublicType projection(PublicType source,
                               PublicTypeProjectionKind projection,
                               std::uint32_t index = 0) {
    PublicType result(PublicTypeKind::TypeProjection);
    result.arguments.push_back(std::move(source));
    result.projection_kind = projection;
    result.projection_index = index;
    return result;
  }
  static PublicType associated(PublicType subject,
                               PublicEntityReferenceArtifact interface_entity,
                               std::uint32_t binding_index) {
    PublicType result =
        projection(std::move(subject), PublicTypeProjectionKind::Associated,
                   binding_index);
    result.nominal_entity = std::move(interface_entity);
    return result;
  }
  static PublicType rawPointer(PublicType pointee, bool pointee_const) {
    PublicType result(PublicTypeKind::RawPointer);
    result.arguments.push_back(std::move(pointee));
    result.pointer_const = pointee_const;
    return result;
  }
  static PublicType cFunctionPointer(
      std::vector<PublicType> parameters, PublicType result_type,
      bool is_variadic = false, CallableOwnershipSummary contract = {},
      std::uint32_t context_parameter = core::AnyId::InvalidIndex,
      ForeignCallingConvention convention = ForeignCallingConvention::C) {
    PublicType result(PublicTypeKind::CFunctionPointer);
    result.arguments = std::move(parameters);
    result.arguments.push_back(std::move(result_type));
    result.callable_variadic = is_variadic;
    result.callable_context_parameter = context_parameter;
    result.foreign_calling_convention = convention;
    contract.canonicalize();
    result.callable_contract = std::move(contract);
    return result;
  }
  static PublicType function(std::vector<PublicType> parameters,
                             PublicType result_type) {
    PublicType result(PublicTypeKind::Function);
    result.arguments = std::move(parameters);
    result.arguments.push_back(std::move(result_type));
    return result;
  }
  static PublicType callbackAdapter(PublicType entry, PublicType context,
                                    PublicType release) {
    PublicType result(PublicTypeKind::CallbackAdapter);
    result.arguments = {std::move(entry), std::move(context),
                        std::move(release)};
    return result;
  }
  static PublicType callbackRegistration(
      PublicType callback, PublicType handle, PublicType register_type,
      PublicType unregister_type, PublicType cancel_type,
      std::uint8_t authority, std::uint32_t entry_parameter,
      std::uint32_t userdata_parameter, std::uint32_t release_parameter,
      std::vector<CallbackRegistrationBinding> bindings = {},
      std::optional<PublicType> cancel_async_type = std::nullopt,
      std::optional<PublicType> wait_type = std::nullopt,
      std::optional<PublicType> poll_type = std::nullopt,
      std::optional<PublicType> arm_type = std::nullopt,
      std::optional<PublicType> detach_type = std::nullopt,
      std::array<std::uint32_t, 4> arm_parameters = {core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex},
      std::array<std::uint32_t, 3> detach_parameters = {
          core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
          core::AnyId::InvalidIndex}) {
    PublicType result(PublicTypeKind::CallbackRegistration);
    result.arguments = {std::move(callback), std::move(handle),
                        std::move(register_type), std::move(unregister_type),
                        std::move(cancel_type)};
    if (cancel_async_type && wait_type) {
      result.arguments.push_back(std::move(*cancel_async_type));
      result.arguments.push_back(std::move(*wait_type));
      if (poll_type)
        result.arguments.push_back(std::move(*poll_type));
      if (arm_type && detach_type) {
        result.arguments.push_back(std::move(*arm_type));
        result.arguments.push_back(std::move(*detach_type));
      }
    }
    result.registration_authority = authority;
    result.registration_entry_parameter = entry_parameter;
    result.registration_userdata_parameter = userdata_parameter;
    result.registration_release_parameter = release_parameter;
    result.registration_bindings = std::move(bindings);
    result.registration_arm_parameters = arm_parameters;
    result.registration_detach_parameters = detach_parameters;
    result.foreign_resource_protocol = makeCallbackRegistrationProtocol(
        authority, entry_parameter, userdata_parameter, release_parameter,
        result.registration_bindings,
        static_cast<std::uint32_t>(result.arguments.size()), arm_parameters,
        detach_parameters);
    return result;
  }
  static PublicType callbackCompletion(
      PublicType callback, PublicType handle, PublicType token,
      PublicType wait_type, std::optional<PublicType> poll_type,
      std::uint8_t authority, std::optional<PublicType> arm_type = std::nullopt,
      std::optional<PublicType> detach_type = std::nullopt,
      std::array<std::uint32_t, 4> arm_parameters = {core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex,
                                                     core::AnyId::InvalidIndex},
      std::array<std::uint32_t, 3> detach_parameters = {
          core::AnyId::InvalidIndex, core::AnyId::InvalidIndex,
          core::AnyId::InvalidIndex}) {
    PublicType result(PublicTypeKind::CallbackCompletion);
    result.arguments = {std::move(callback), std::move(handle),
                        std::move(token), std::move(wait_type)};
    if (poll_type)
      result.arguments.push_back(std::move(*poll_type));
    if (arm_type && detach_type) {
      result.arguments.push_back(std::move(*arm_type));
      result.arguments.push_back(std::move(*detach_type));
    }
    result.registration_authority = authority;
    result.registration_arm_parameters = arm_parameters;
    result.registration_detach_parameters = detach_parameters;
    result.foreign_resource_protocol = makeCallbackCompletionProtocol(
        authority, static_cast<std::uint32_t>(result.arguments.size()),
        arm_parameters, detach_parameters);
    return result;
  }
  static PublicType callbackWake(PublicType completion) {
    PublicType result(PublicTypeKind::CallbackWake);
    result.arguments.push_back(std::move(completion));
    return result;
  }
  static PublicType foreignCompletion(PublicEntityReferenceArtifact resource) {
    PublicType result(PublicTypeKind::ForeignCompletion);
    result.nominal_entity = std::move(resource);
    return result;
  }
  static PublicType foreignWake(PublicEntityReferenceArtifact resource) {
    PublicType result(PublicTypeKind::ForeignWake);
    result.nominal_entity = std::move(resource);
    return result;
  }
  static PublicType
  foreignOperationState(PublicEntityReferenceArtifact operation,
                        ForeignOperationStateKind state) {
    PublicType result(PublicTypeKind::ForeignOperationState);
    result.nominal_entity = std::move(operation);
    result.foreign_operation_state = state;
    return result;
  }

  friend bool operator==(const PublicType &, const PublicType &) = default;
};

[[nodiscard]] inline bool validCallbackAdapterContract(const PublicType &type) {
  if (type.kind != PublicTypeKind::CallbackAdapter ||
      type.arguments.size() != 3)
    return false;
  const auto &entry = type.arguments[0];
  const auto &context = type.arguments[1];
  const auto &release = type.arguments[2];
  if (context.kind != PublicTypeKind::RawPointer || context.pointer_const ||
      context.arguments.size() != 1 ||
      context.arguments.front().kind != PublicTypeKind::Void ||
      entry.kind != PublicTypeKind::CFunctionPointer ||
      entry.callable_variadic || entry.arguments.size() < 2 ||
      entry.callable_context_parameter >= entry.arguments.size() - 1 ||
      entry.arguments[entry.callable_context_parameter] != context)
    return false;

  const auto context_index = entry.callable_context_parameter;
  for (const auto &effect : entry.callable_contract.effects)
    if (effect.region.parameter_index == context_index &&
        effect.kind != CallableEffectKind::Read &&
        effect.kind != CallableEffectKind::Write)
      return false;
  for (const auto &postcondition : entry.callable_contract.postconditions) {
    if (postcondition.region.parameter_index != context_index)
      continue;
    bool derived_from_write = false;
    for (const auto &effect : entry.callable_contract.effects)
      derived_from_write |= effect.kind == CallableEffectKind::Write &&
                            effect.region == postcondition.region;
    if (!derived_from_write ||
        postcondition.outcomes !=
            (CallableOutcomePreserve | CallableOutcomeInitialize))
      return false;
  }
  for (const auto &source : entry.callable_contract.return_provenance) {
    if (source.region.parameter_index == context_index)
      return false;
    for (const auto &clause : source.condition.clauses)
      for (const auto &atom : clause.atoms)
        if (atom.parameter_index == context_index)
          return false;
  }

  const OwnershipRegion root{.parameter_index = 0};
  return release.kind == PublicTypeKind::CFunctionPointer &&
         !release.callable_variadic && release.arguments.size() == 2 &&
         release.callable_context_parameter == 0 &&
         release.arguments[0] == context &&
         release.arguments[1].kind == PublicTypeKind::Void &&
         release.callable_contract.effects ==
             std::vector<CallableRegionEffect>{
                 {CallableEffectKind::Take, root}} &&
         release.callable_contract.postconditions ==
             std::vector<CallableRegionPostcondition>{
                 {root, CallableOutcomeInvalidate}} &&
         release.callable_contract.returns_owned &&
         release.callable_contract.return_provenance.empty();
}

[[nodiscard]] bool validCallbackRegistrationContract(const PublicType &type);
[[nodiscard]] bool validCallbackCompletionContract(const PublicType &type);
[[nodiscard]] bool validCallbackWakeContract(const PublicType &type);

enum class ForeignAbiValueKind : std::uint8_t {
  Void,
  Bool,
  SignedInteger,
  UnsignedInteger,
  Float,
  RawPointer,
  Reference,
  Aggregate,
  FunctionPointer,
  Count,
};

struct ForeignAbiValue {
  ForeignAbiValueKind kind = ForeignAbiValueKind::Count;
  std::uint32_t width = 0;
  bool pointee_const = false;

  friend bool operator==(const ForeignAbiValue &,
                         const ForeignAbiValue &) = default;
};

namespace interop {

enum class ForeignOperationKind : std::uint8_t {
  Value,
  Resource,
  Memory,
  Callback,
  Async,
  Count,
};
enum class ForeignPortKind : std::uint8_t {
  Input,
  Output,
  Callback,
  Result,
  Count,
};
enum class ForeignPortEffect : std::uint8_t {
  Borrow,
  Mutate,
  Consume,
  Create,
  View,
  ReverseTarget,
  Count,
};
enum class ForeignPayloadKind : std::uint8_t { Void, Exact, Tuple, Count };
enum class ForeignCompletionFamily : std::uint8_t {
  None,
  Callback,
  Async,
  Count,
};
enum class ForeignCompletionProjectionKind : std::uint8_t {
  None,
  ScalarToSubscription,
  ScalarToCompletion,
  ScalarToWake,
  CompletionToWake,
  Count,
};
enum class ForeignCompletionInputEffect : std::uint8_t {
  Borrow,
  Consume,
  Transfer,
  Count,
};

struct ForeignCapability {
  enum class ArgumentKind : std::uint8_t {
    Lane,
    Literal,
    OperationReference,
    AdapterReference,
    Count,
  };
  struct Argument {
    ArgumentKind kind = ArgumentKind::Count;
    std::uint32_t lane = core::AnyId::InvalidIndex;
    std::string literal;
    PublicEntityReferenceArtifact entity;

    friend bool operator==(const Argument &, const Argument &) = default;
    friend auto operator<=>(const Argument &lhs, const Argument &rhs) {
      return std::tie(lhs.kind, lhs.lane, lhs.literal, lhs.entity.kind,
                      lhs.entity.canonical_package, lhs.entity.canonical_module,
                      lhs.entity.canonical_name) <=>
             std::tie(rhs.kind, rhs.lane, rhs.literal, rhs.entity.kind,
                      rhs.entity.canonical_package, rhs.entity.canonical_module,
                      rhs.entity.canonical_name);
    }
  };
  std::string path;
  std::vector<std::uint32_t> lanes;
  std::vector<std::string> literals;
  std::vector<Argument> arguments;

  friend bool operator==(const ForeignCapability &,
                         const ForeignCapability &) = default;
  // Keep capability ordering explicit instead of defaulting a three-way
  // comparison over vectors.  Clang 18/libstdc++ rejects the latter because
  // the nested Argument vector is not modeled as three-way-comparable even
  // though Argument has a stable canonical ordering.  The explicit ordering
  // preserves the canonical path/lane/literal/argument sequence and remains
  // independent of process-local state.
  friend std::strong_ordering operator<=>(const ForeignCapability &lhs,
                                           const ForeignCapability &rhs) {
    if (lhs.path < rhs.path)
      return std::strong_ordering::less;
    if (rhs.path < lhs.path)
      return std::strong_ordering::greater;
    const auto compare_lanes = [&]() {
      const auto count = std::min(lhs.lanes.size(), rhs.lanes.size());
      for (std::size_t index = 0; index < count; ++index) {
        if (lhs.lanes[index] < rhs.lanes[index])
          return std::strong_ordering::less;
        if (rhs.lanes[index] < lhs.lanes[index])
          return std::strong_ordering::greater;
      }
      if (lhs.lanes.size() < rhs.lanes.size())
        return std::strong_ordering::less;
      if (rhs.lanes.size() < lhs.lanes.size())
        return std::strong_ordering::greater;
      return std::strong_ordering::equal;
    };
    if (const auto result = compare_lanes();
        result != std::strong_ordering::equal)
      return result;
    const auto compare_literals = [&]() {
      const auto count = std::min(lhs.literals.size(), rhs.literals.size());
      for (std::size_t index = 0; index < count; ++index) {
        if (lhs.literals[index] < rhs.literals[index])
          return std::strong_ordering::less;
        if (rhs.literals[index] < lhs.literals[index])
          return std::strong_ordering::greater;
      }
      if (lhs.literals.size() < rhs.literals.size())
        return std::strong_ordering::less;
      if (rhs.literals.size() < lhs.literals.size())
        return std::strong_ordering::greater;
      return std::strong_ordering::equal;
    };
    if (const auto result = compare_literals();
        result != std::strong_ordering::equal)
      return result;
    const auto count = std::min(lhs.arguments.size(), rhs.arguments.size());
    for (std::size_t index = 0; index < count; ++index) {
      if (const auto result = lhs.arguments[index] <=> rhs.arguments[index];
          result != std::strong_ordering::equal)
        return result;
    }
    if (lhs.arguments.size() < rhs.arguments.size())
      return std::strong_ordering::less;
    if (rhs.arguments.size() < lhs.arguments.size())
      return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }
};

struct ForeignLogicalPort {
  ForeignPortKind kind = ForeignPortKind::Count;
  ForeignPortEffect effect = ForeignPortEffect::Count;
  std::string path;
  std::vector<std::uint32_t> lanes;
  bool success = false;
  bool failure = false;

  friend bool operator==(const ForeignLogicalPort &,
                         const ForeignLogicalPort &) = default;
  friend auto operator<=>(const ForeignLogicalPort &,
                          const ForeignLogicalPort &) = default;
};

enum class ForeignProtocolIdentityKind : std::uint8_t {
  Action,
  Event,
  Count,
};

struct ForeignProtocolIdentity {
  ForeignProtocolIdentityKind kind = ForeignProtocolIdentityKind::Count;
  std::string canonical_package;
  std::string canonical_module;
  std::string canonical_resource;
  std::string canonical_name;
  std::string owner_callable;
  StableFingerprint fingerprint;

  friend bool operator==(const ForeignProtocolIdentity &,
                         const ForeignProtocolIdentity &) = default;
  friend auto operator<=>(const ForeignProtocolIdentity &lhs,
                          const ForeignProtocolIdentity &rhs) {
    return std::tuple(lhs.kind, lhs.canonical_package, lhs.canonical_module,
                      lhs.canonical_resource, lhs.canonical_name,
                      lhs.owner_callable, lhs.fingerprint.hex()) <=>
           std::tuple(rhs.kind, rhs.canonical_package, rhs.canonical_module,
                      rhs.canonical_resource, rhs.canonical_name,
                      rhs.owner_callable, rhs.fingerprint.hex());
  }
};

enum class ForeignProtocolEdgeKind : std::uint8_t {
  Obliges,
  Discharges,
  Requires,
  Invokes,
  Escapes,
  Transfers,
  Count,
};

struct ForeignProtocolEdge {
  ForeignProtocolEdgeKind kind = ForeignProtocolEdgeKind::Count;
  StableFingerprint source;
  StableFingerprint target;
  std::string predicate;
  std::string owner_callable;

  friend bool operator==(const ForeignProtocolEdge &,
                         const ForeignProtocolEdge &) = default;
  friend auto operator<=>(const ForeignProtocolEdge &lhs,
                          const ForeignProtocolEdge &rhs) {
    return std::tuple(lhs.kind, lhs.source.hex(), lhs.target.hex(),
                      lhs.predicate, lhs.owner_callable) <=>
           std::tuple(rhs.kind, rhs.source.hex(), rhs.target.hex(),
                      rhs.predicate, rhs.owner_callable);
  }
};

struct CompletionFamily {
  PublicEntityReferenceArtifact source;
  PublicEntityReferenceArtifact wait;
  PublicEntityReferenceArtifact poll;
  PublicEntityReferenceArtifact arm;
  PublicEntityReferenceArtifact detach;
  PublicType completion_carrier;
  PublicType registration_callback_adapter;
  PublicType waker_callback_adapter;
  std::uint32_t source_handle_lane = core::AnyId::InvalidIndex;
  std::uint32_t token_result_lane = core::AnyId::InvalidIndex;
  std::vector<std::uint32_t> arm_lane_map;
  std::vector<std::uint32_t> detach_lane_map;
  std::uint8_t authority = 0;
  std::uint32_t readiness_literal = 1;
  ForeignCompletionInputEffect input_effect =
      ForeignCompletionInputEffect::Transfer;
  std::uint32_t abi_epoch = 1;

  friend bool operator==(const CompletionFamily &,
                         const CompletionFamily &) = default;
};

struct ForeignOperationArtifact {
  // Bumped when the CFDL resource-flow model changes. Older payloads are
  // rejected before semantic import rather than interpreted heuristically.
  static constexpr std::uint32_t CurrentCFDLSemanticEpoch = 15;
  enum class OutcomeProjection : std::uint8_t {
    None,
    PosixRead,
    Win32Read,
    Fread,
    Count,
  };
  enum class ArgumentSourceKind : std::uint8_t {
    PublicArgument,
    OutcomeStorage,
    NullPointer,
    Count,
  };
  struct ArgumentSource {
    ArgumentSourceKind kind = ArgumentSourceKind::Count;
    std::uint32_t index = core::AnyId::InvalidIndex;

    friend bool operator==(const ArgumentSource &,
                           const ArgumentSource &) = default;
  };
  enum class ErrorExtractor : std::uint8_t {
    None,
    ReturnedCode,
    Errno,
    Win32LastError,
    Count,
  };
  enum class ErrorPredicate : std::uint8_t {
    None,
    IntegerSet,
    Null,
    InvalidSentinel,
    Count,
  };
  enum class ErrorSuccessPayload : std::uint8_t { None, Void, Raw, Count };
  struct ErrorInterval {
    std::uint64_t lower = 0;
    std::uint64_t upper = 0;

    friend bool operator==(const ErrorInterval &,
                           const ErrorInterval &) = default;
    friend auto operator<=>(const ErrorInterval &,
                            const ErrorInterval &) = default;
  };
  ForeignOperationKind kind = ForeignOperationKind::Count;
  std::vector<ForeignCapability> capabilities;
  std::vector<ForeignProtocolIdentity> protocol_identities;
  std::vector<ForeignProtocolEdge> protocol_edges;
  std::vector<ForeignLogicalPort> ports;
  ForeignPayloadKind success_payload = ForeignPayloadKind::Void;
  ForeignPayloadKind failure_payload = ForeignPayloadKind::Void;
  std::vector<std::uint32_t> success_lanes;
  std::vector<std::uint32_t> failure_lanes;
  // Resource-call outcome contract. Output lanes publish initialization
  // authority independently; an optional status lane is interpreted using the
  // explicit success literal. No C symbol guessing is allowed.
  std::uint32_t status_lane = core::AnyId::InvalidIndex;
  std::int64_t status_success_literal = 0;
  bool out_initialized_on_failure = false;
  std::vector<std::uint32_t> out_lanes;
  ErrorExtractor error_extractor = ErrorExtractor::None;
  ErrorPredicate error_predicate = ErrorPredicate::None;
  ErrorSuccessPayload error_success_payload = ErrorSuccessPayload::None;
  std::vector<ErrorInterval> error_intervals;
  std::uint32_t error_predicate_width = 0;
  bool error_predicate_signed = false;
  bool error_predicate_inverted = false;
  OutcomeProjection outcome_projection = OutcomeProjection::None;
  std::uint32_t outcome_buffer_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_capacity_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_count_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_context_lane = core::AnyId::InvalidIndex;
  std::uint32_t outcome_size_lane = core::AnyId::InvalidIndex;
  std::string outcome_eof_symbol;
  std::string outcome_ferror_symbol;
  std::optional<PublicType> outcome_element_type;
  std::optional<PublicType> outcome_count_type;
  std::vector<ArgumentSource> argument_sources;
  std::uint8_t callback_authority = 0;
  ForeignCompletionFamily completion_family = ForeignCompletionFamily::None;
  ForeignCompletionProjectionKind completion_projection =
      ForeignCompletionProjectionKind::None;
  ForeignCompletionInputEffect completion_input_effect =
      ForeignCompletionInputEffect::Borrow;
  std::uint32_t completion_carrier_lane = core::AnyId::InvalidIndex;
  std::uint32_t completion_result_lane = core::AnyId::InvalidIndex;
  std::uint32_t readiness_success_literal = 1;
  std::vector<std::uint32_t> wake_callback_lanes;
  // Canonical event identities make a completion family a protocol set rather
  // than a single callback spelling. Cancel and wake are projections of that
  // same set; quiescence is closed only after a discharge edge is published.
  std::vector<StableFingerprint> completion_events;
  std::vector<StableFingerprint> cancel_events;
  std::vector<StableFingerprint> wake_events;
  bool requires_quiescence = false;
  bool discharges_quiescence = false;
  std::optional<PublicType> callback_adapter_layout;
  std::optional<PublicType> waker_adapter_layout;
  std::optional<CompletionFamily> completion_descriptor;
  std::uint32_t abi_epoch = 1;
  std::uint32_t cfdl_semantic_epoch = CurrentCFDLSemanticEpoch;
  StableFingerprint fingerprint;

  void canonicalize();
  [[nodiscard]] bool verify(std::string &error) const;

  friend bool operator==(const ForeignOperationArtifact &,
                         const ForeignOperationArtifact &) = default;
};

struct ArtifactReference {
  static constexpr std::uint32_t CurrentSchemaEpoch = 10;
  std::uint32_t schema_epoch = CurrentSchemaEpoch;
  std::string canonical_package;
  std::string canonical_module;
  std::string canonical_name;
  StableFingerprint fingerprint;

  [[nodiscard]] bool verify(std::string &error) const;

  friend bool operator==(const ArtifactReference &lhs,
                         const ArtifactReference &rhs) {
    return lhs.schema_epoch == rhs.schema_epoch &&
           lhs.canonical_package == rhs.canonical_package &&
           lhs.canonical_module == rhs.canonical_module &&
           lhs.canonical_name == rhs.canonical_name &&
           lhs.fingerprint == rhs.fingerprint;
  }
};

struct ArtifactBundleRecord {
  ArtifactReference reference;
  ForeignOperationArtifact artifact;

  friend bool operator==(const ArtifactBundleRecord &,
                         const ArtifactBundleRecord &) = default;
};

struct ArtifactBundle {
  static constexpr std::string_view Magic = "CHNXIOP13";
  static constexpr std::uint32_t CurrentFormatVersion = 12;

  std::uint32_t format_version = CurrentFormatVersion;
  std::vector<ArtifactBundleRecord> records;

  void canonicalize();
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode(std::string &error) const;
  [[nodiscard]] static std::optional<ArtifactBundle>
  decode(std::string_view bytes, std::string &error);
};

class ArtifactRegistry {
public:
  ArtifactRegistry() = default;

  [[nodiscard]] ArtifactReference publish(std::string_view canonical_package,
                                          std::string_view canonical_module,
                                          std::string_view canonical_name,
                                          ForeignOperationArtifact artifact,
                                          std::string &error);
  [[nodiscard]] bool registerArtifact(const ArtifactReference &reference,
                                      ForeignOperationArtifact artifact,
                                      std::string &error);
  [[nodiscard]] bool registerBundle(const ArtifactBundle &bundle,
                                    std::string &error);
  [[nodiscard]] ArtifactBundle exportBundle(std::string &error) const;
  [[nodiscard]] const ForeignOperationArtifact *
  resolve(const ArtifactReference &reference) const;
  [[nodiscard]] const ForeignProtocolIdentity *findProtocolIdentity(
      ForeignProtocolIdentityKind kind, std::string_view canonical_package,
      std::string_view canonical_module, std::string_view canonical_resource,
      std::string_view canonical_name, std::string_view owner_callable,
      std::string &error) const;
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::size_t size() const {
    return artifacts_.size();
  }

private:
  [[nodiscard]] static std::string key(const ArtifactReference &reference);

  core::ValueStore<InteropArtifactId, ForeignOperationArtifact> artifacts_;
  std::unordered_map<std::string, InteropArtifactId> keys_;
};

} // namespace interop

struct ForeignAbiSignature {
  enum class UnwindPolicy : std::uint8_t {
    NoUnwind,
    Count,
  };

  ForeignAbiValue result;
  std::vector<ForeignAbiValue> parameters;
  bool is_variadic = false;
  ForeignCallingConvention calling_convention = ForeignCallingConvention::C;
  UnwindPolicy unwind_policy = UnwindPolicy::NoUnwind;

  [[nodiscard]] bool verify(std::string &error) const;
  friend bool operator==(const ForeignAbiSignature &,
                         const ForeignAbiSignature &) = default;
};

[[nodiscard]] StableFingerprint
foreignAbiSignatureFingerprint(const ForeignAbiSignature &signature);

enum class CallableSemanticDomain : std::uint8_t {
  Ordinary,
  Lifecycle,
  ValueRepresentation,
  ObjectProjection,
  ObjectShell,
  NominalConstruction,
  Count,
};

enum class CallableSemanticRole : std::uint8_t {
  None,
  Copy,
  Drop,
  Pack,
  Init,
  ProjectionLoad,
  ProjectionStore,
  ProjectionTake,
  ProjectionInit,
  ProjectionBorrow,
  ProjectionBorrowMut,
  ObjectInit,
  ObjectCopyInit,
  ObjectMoveInit,
  ObjectDrop,
  Constructor,
  Count,
};

enum CallableSemanticCapability : std::uint16_t {
  CallableCapabilityNone = 0,
  CallableCapabilityCopy = 1U << 0U,
  CallableCapabilityDrop = 1U << 1U,
  CallableCapabilityPack = 1U << 2U,
  CallableCapabilityInit = 1U << 3U,
  CallableCapabilityProjectionLoad = 1U << 4U,
  CallableCapabilityProjectionStore = 1U << 5U,
  CallableCapabilityProjectionTake = 1U << 6U,
  CallableCapabilityProjectionInit = 1U << 7U,
  CallableCapabilityProjectionBorrow = 1U << 8U,
  CallableCapabilityProjectionBorrowMut = 1U << 9U,
  CallableCapabilityObjectInit = 1U << 10U,
  CallableCapabilityObjectCopyInit = 1U << 11U,
  CallableCapabilityObjectMoveInit = 1U << 12U,
  CallableCapabilityObjectDrop = 1U << 13U,
};

struct CallableSemanticContract {
  CallableSemanticDomain domain = CallableSemanticDomain::Ordinary;
  CallableSemanticRole role = CallableSemanticRole::None;
  std::uint16_t capability = CallableCapabilityNone;
  // Count means that this is an ordinary callable with no semantic owner.
  PublicType owner;
  std::uint32_t projector_field = core::AnyId::InvalidIndex;
  bool whole_carrier = false;
  std::vector<std::uint32_t> carrier_path;
  bool has_bit_range = false;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;

  [[nodiscard]] bool verify(std::uint32_t generic_parameter_count,
                            std::string &error) const;

  friend bool operator==(const CallableSemanticContract &,
                         const CallableSemanticContract &) = default;
};

enum class PublicObjectProjectionKind : std::uint8_t {
  StableAddress,
  Computed,
  BitPacked,
  Count,
};

struct PublicNominalFieldArtifact {
  std::string name;
  PublicType type;
  std::vector<std::string> storage_path;
  PublicObjectProjectionKind projection_kind =
      PublicObjectProjectionKind::StableAddress;
  std::string projector_name;
  std::vector<std::string> projection_region_path;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;
  bool is_public = false;

  friend bool operator==(const PublicNominalFieldArtifact &,
                         const PublicNominalFieldArtifact &) = default;
};

enum class PublicEnumPayloadShape : std::uint8_t {
  Unit,
  Tuple,
  Struct,
  Count,
};

struct PublicEnumVariantArtifact {
  std::string name;
  std::vector<PublicNominalFieldArtifact> fields;
  PublicEnumPayloadShape shape = PublicEnumPayloadShape::Unit;
  std::int64_t discriminant = 0;

  friend bool operator==(const PublicEnumVariantArtifact &,
                         const PublicEnumVariantArtifact &) = default;
};

struct PublicForeignResourceOperationArtifact {
  std::string name;
  ForeignResourceRoleKind role = ForeignResourceRoleKind::Count;
  PublicEntityReferenceArtifact target;
  std::uint32_t resource_parameter = core::AnyId::InvalidIndex;
  std::uint32_t completion_parameter = core::AnyId::InvalidIndex;

  friend bool
  operator==(const PublicForeignResourceOperationArtifact &,
             const PublicForeignResourceOperationArtifact &) = default;
};

enum class NominalRepresentationPolicy : std::uint8_t {
  Opaque,
  C,
  Count,
};

enum class NominalKind : std::uint8_t {
  Struct,
  Union,
  Enum,
  ForeignHandle,
  ForeignResource,
  Count,
};

struct PublicNominalTypeArtifact {
  PublicEntityReferenceArtifact entity;
  bool is_exported = true;
  std::uint32_t generic_parameter_count = 0;
  std::vector<PublicNominalFieldArtifact> fields;
  std::vector<PublicEnumVariantArtifact> variants;
  bool is_value_enum = false;
  std::optional<PublicType> value_repr_pattern;
  std::optional<PublicType> object_repr_pattern;
  NominalRepresentationPolicy representation_policy =
      NominalRepresentationPolicy::Opaque;
  StableFingerprint definition_fingerprint;
  NominalKind kind = NominalKind::Struct;
  std::optional<PublicType> foreign_representation;
  ForeignResourceInvalidState foreign_invalid_state =
      ForeignResourceInvalidState::Count;
  std::int64_t foreign_invalid_integer = 0;
  std::optional<PublicType> foreign_handle_type;
  std::optional<PublicType> foreign_completion_handle_type;
  std::optional<PublicType> foreign_callback_type;
  std::optional<PublicType> foreign_waker_type;
  ForeignResourceProtocol foreign_resource_protocol;
  std::vector<PublicForeignResourceOperationArtifact>
      foreign_resource_operations;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<PublicNominalTypeArtifact>
  decode(std::string_view bytes, std::string &error);

  friend bool operator==(const PublicNominalTypeArtifact &,
                         const PublicNominalTypeArtifact &) = default;
};

// Runtime ABI v1 reasons for source-level failures which cannot be recovered.
enum class UnrecoverableFailureReason : std::uint32_t {
  Assertion = 1,
  ReachedUnreachable = 2,
};

constexpr bool isValidUnrecoverableFailureReason(std::int64_t value) {
  return value == static_cast<std::uint32_t>(
                      UnrecoverableFailureReason::Assertion) ||
         value == static_cast<std::uint32_t>(
                      UnrecoverableFailureReason::ReachedUnreachable);
}

enum class GenericTemplateOpcode : std::uint8_t {
  Parameter,
  IntegerLiteral,
  FloatLiteral,
  NameRef,
  BuiltinUnary,
  BuiltinBinary,
  Add,
  NumericConvert,
  CheckedNumericCast,
  Equal,
  BindName,
  Return,
  If,
  Call,
  AggregateInit,
  UnionInit,
  EnumInit,
  EnumTag,
  EnumPayloadAccess,
  StructFieldAccess,
  UnionFieldAccess,
  BorrowLocal,
  BorrowPlace,
  CarrierView,
  Dereference,
  Move,
  Copy,
  Assign,
  While,
  Switch,
  SwitchArm,
  VoidValue,
  FunctionValue,
  IndirectCall,
  IfArm,
  For,
  ForClause,
  DoWhile,
  Break,
  Continue,
  BoolLiteral,
  Yield,
  Defer,
  // Package state and concrete components persist these numeric values.
  // Append new opcodes here; do not reorder the existing entries.
  MaterializeTemporary,
  EndFullExpression,
  ExtendTemporary,
  Assert,
  UnrecoverableFailure,
  InterfaceCall,
  Slice,
  TupleLiteral,
  StaticIndex,
  StringLiteral,
  ArrayLiteral,
  NullPointer,
  TypeQuery,
  CompilerIntrinsicCall,
  BoundMethod,
  Closure,
  MemberAccess,
  Index,
  ScopedBlock,
  CharLiteral,
  Count,
};

static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::Defer) == 42);
static_assert(static_cast<std::uint8_t>(
                  GenericTemplateOpcode::MaterializeTemporary) == 43);
static_assert(
    static_cast<std::uint8_t>(GenericTemplateOpcode::ExtendTemporary) == 45);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::Assert) == 46);
static_assert(static_cast<std::uint8_t>(
                  GenericTemplateOpcode::UnrecoverableFailure) == 47);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::InterfaceCall) ==
              48);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::Slice) == 49);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::TupleLiteral) ==
              50);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::StaticIndex) ==
              51);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::StringLiteral) ==
              52);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::ArrayLiteral) ==
              53);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::NullPointer) ==
              54);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::TypeQuery) ==
              55);
static_assert(static_cast<std::uint8_t>(
                  GenericTemplateOpcode::CompilerIntrinsicCall) == 56);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::BoundMethod) ==
              57);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::Closure) == 58);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::MemberAccess) ==
              59);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::Index) == 60);
static_assert(static_cast<std::uint8_t>(GenericTemplateOpcode::ScopedBlock) ==
              61);

struct GenericTemplateInstArtifact {
  GenericTemplateOpcode opcode = GenericTemplateOpcode::Count;
  PublicType type;
  std::uint32_t arg0 = core::AnyId::InvalidIndex;
  std::uint32_t arg1 = core::AnyId::InvalidIndex;

  friend bool operator==(const GenericTemplateInstArtifact &,
                         const GenericTemplateInstArtifact &) = default;
};

enum class GenericTypeQueryKind : std::uint8_t {
  TypeSame,
  TypeIs,
  TypeHas,
  ArrayExtent,
  TupleArity,
  Count,
};

struct GenericTypeQueryArtifact {
  GenericTypeQueryKind kind = GenericTypeQueryKind::Count;
  PublicType source;
  PublicType other;
  std::string property;

  friend bool operator==(const GenericTypeQueryArtifact &,
                         const GenericTypeQueryArtifact &) = default;
};

struct GenericEvaluationRegionArtifact {
  std::uint32_t entry_block = core::AnyId::InvalidIndex;
  std::vector<GenericTemplateInstArtifact> instructions;
  std::vector<std::vector<std::uint32_t>> blocks;
  // Non-owning operand lists indexed by the consuming instruction. Keeping
  // these parallel to `instructions` prevents a second block identity from
  // accidentally owning or cloning executable instructions.
  std::vector<std::vector<std::uint32_t>> instruction_value_blocks;
  std::vector<PublicType> results;

  friend bool operator==(const GenericEvaluationRegionArtifact &,
                         const GenericEvaluationRegionArtifact &) = default;
};

struct GenericTemplateArtifact {
  std::uint32_t generic_parameter_count = 0;
  std::uint32_t parameter_count = 0;
  std::vector<PublicType> local_types;
  std::vector<std::uint32_t> local_flags;
  std::vector<std::int64_t> integers;
  std::vector<std::string> strings;
  std::vector<GenericTypeQueryArtifact> type_queries;
  std::vector<PublicEntityReferenceArtifact> callees;
  std::vector<std::vector<PublicType>> callee_type_arguments;
  GenericEvaluationRegionArtifact declaration;
  GenericEvaluationRegionArtifact definition;

  [[nodiscard]] bool verify(std::string &error) const;
  friend bool operator==(const GenericTemplateArtifact &,
                         const GenericTemplateArtifact &) = default;
};

enum class PublicFunctionExecutionKind : std::uint8_t {
  Immediate,
  Async,
  Count,
};

struct PublicCoroutineConstructorABI {
  std::uint32_t epoch = 0;
  bool eager_start = false;
  bool left_to_right_exactly_once = false;
  bool supports_root = false;
  bool supports_child = false;

  friend bool operator==(const PublicCoroutineConstructorABI &,
                         const PublicCoroutineConstructorABI &) = default;
};

enum class PublicNominalConstructorResultKind : std::uint8_t {
  None,
  DirectSelf,
  FallibleSelf,
  Count,
};

struct PublicNominalConstructorABI {
  std::uint32_t epoch = 0;
  PublicNominalConstructorResultKind result_kind =
      PublicNominalConstructorResultKind::None;

  friend bool operator==(const PublicNominalConstructorABI &,
                         const PublicNominalConstructorABI &) = default;
};

enum class PublicValueKind : std::uint8_t {
  Constant,
  Static,
  Count,
};

enum class PublicConstantValueKind : std::uint8_t {
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
  Count,
};

struct PublicConstantValue {
  PublicConstantValueKind kind = PublicConstantValueKind::Integer;
  PublicType type;
  std::uint64_t payload = 0;
  std::string string_payload;
  std::vector<PublicConstantValue> elements;
  bool target_dependent = false;

  friend bool operator==(const PublicConstantValue &,
                         const PublicConstantValue &) = default;
};

struct PublicFunctionArtifact {
  std::string name;
  std::optional<PublicEntityReferenceArtifact> member_owner;
  enum class MemberKind : std::uint8_t {
    None,
    Instance,
    Associated,
    Count,
  } member_kind = MemberKind::None;
  std::string canonical_package;
  std::string canonical_module;
  std::string canonical_name;
  std::uint32_t generic_parameter_count = 0;
  std::vector<PublicType> parameters;
  std::vector<std::string> parameter_names;
  std::vector<std::optional<PublicConstantValue>> default_arguments;
  PublicType return_type;
  std::optional<PublicType> error_type;
  PublicFunctionExecutionKind execution_kind =
      PublicFunctionExecutionKind::Immediate;
  PublicCoroutineConstructorABI coroutine_constructor;
  PublicNominalConstructorABI nominal_constructor;
  CallableSemanticContract semantic_contract;
  CompilerIntrinsicRole intrinsic_role = CompilerIntrinsicRole::None;
  CallableOwnershipSummary ownership_summary;
  PublicCallableDeclarationKind declaration_kind =
      PublicCallableDeclarationKind::Definition;
  bool is_unsafe = false;
  bool is_const = false;
  std::string foreign_abi;
  std::string external_symbol;
  std::optional<ForeignAbiSignature> foreign_signature;
  std::optional<interop::ArtifactReference> interop_artifact;
  std::optional<GenericTemplateArtifact> generic_template;
  std::vector<struct PublicInterfaceConstraintArtifact> constraints;
  StableFingerprint entity_fingerprint;

  friend bool operator==(const PublicFunctionArtifact &,
                         const PublicFunctionArtifact &) = default;
};

struct PublicInterfaceConstraintArtifact {
  PublicType subject;
  PublicEntityReferenceArtifact interface_entity;
  std::vector<PublicType> arguments;

  friend bool operator==(const PublicInterfaceConstraintArtifact &,
                         const PublicInterfaceConstraintArtifact &) = default;
};

enum class PublicInterfaceRequirementKind : std::uint8_t {
  Function,
  AssociatedAlias,
  Count,
};

struct PublicInterfaceRequirementArtifact {
  PublicInterfaceRequirementKind kind = PublicInterfaceRequirementKind::Count;
  std::string name;
  PublicEntityReferenceArtifact function;
  PublicType associated_type;
  std::uint32_t binding_index = core::AnyId::InvalidIndex;
  bool has_default = false;

  friend bool operator==(const PublicInterfaceRequirementArtifact &,
                         const PublicInterfaceRequirementArtifact &) = default;
};

struct PublicInterfaceDeclarationArtifact {
  PublicEntityReferenceArtifact entity;
  std::uint32_t generic_parameter_count = 0;
  std::uint32_t explicit_parameter_count = 0;
  std::vector<PublicInterfaceConstraintArtifact> constraints;
  std::vector<PublicInterfaceRequirementArtifact> requirements;

  friend bool operator==(const PublicInterfaceDeclarationArtifact &,
                         const PublicInterfaceDeclarationArtifact &) = default;
};

struct PublicTypeAliasArtifact {
  PublicEntityReferenceArtifact entity;
  std::uint32_t generic_parameter_count = 0;
  PublicType target;
  std::vector<PublicInterfaceConstraintArtifact> constraints;

  friend bool operator==(const PublicTypeAliasArtifact &,
                         const PublicTypeAliasArtifact &) = default;
};

struct PublicInterfaceWitnessEntryArtifact {
  std::uint32_t requirement = core::AnyId::InvalidIndex;
  PublicEntityReferenceArtifact function;
  PublicType associated_type;

  friend bool operator==(const PublicInterfaceWitnessEntryArtifact &,
                         const PublicInterfaceWitnessEntryArtifact &) = default;
};

struct PublicInterfaceWitnessArtifact {
  PublicEntityReferenceArtifact interface_entity;
  std::uint32_t generic_parameter_count = 0;
  PublicType self_type;
  std::vector<PublicType> interface_arguments;
  std::vector<PublicInterfaceConstraintArtifact> constraints;
  std::vector<PublicInterfaceWitnessEntryArtifact> entries;
  StableFingerprint fingerprint;

  friend bool operator==(const PublicInterfaceWitnessArtifact &,
                         const PublicInterfaceWitnessArtifact &) = default;
};

struct PublicValueArtifact {
  PublicValueKind kind = PublicValueKind::Constant;
  std::string name;
  std::string canonical_package;
  std::string canonical_module;
  std::string canonical_name;
  PublicType type;
  PublicConstantValue value;
  StableFingerprint entity_fingerprint;

  friend bool operator==(const PublicValueArtifact &,
                         const PublicValueArtifact &) = default;
};

class PublicInterfaceArtifact {
public:
  PublicInterfaceArtifact() = default;
  PublicInterfaceArtifact(
      std::string package_name, std::string module_name,
      StableFingerprint fingerprint,
      std::vector<PublicFunctionArtifact> functions,
      std::vector<PublicNominalTypeArtifact> nominal_types = {},
      std::vector<PublicValueArtifact> values = {},
      std::vector<PublicInterfaceDeclarationArtifact> interfaces = {},
      std::vector<PublicTypeAliasArtifact> type_aliases = {},
      std::vector<PublicInterfaceWitnessArtifact> interface_witnesses = {});

  [[nodiscard]] std::string_view packageName() const {
    return package_name_;
  }

  [[nodiscard]] std::string_view moduleName() const {
    return module_name_;
  }
  [[nodiscard]] const StableFingerprint &fingerprint() const {
    return fingerprint_;
  }
  [[nodiscard]] std::span<const PublicFunctionArtifact> functions() const {
    return functions_;
  }
  [[nodiscard]] const PublicFunctionArtifact *
  findFunction(std::string_view name) const;
  [[nodiscard]] std::vector<const PublicFunctionArtifact *>
  findFunctions(std::string_view name) const;
  [[nodiscard]] const PublicFunctionArtifact *
  findMemberFunction(const PublicEntityReferenceArtifact &owner,
                     std::string_view name) const;
  [[nodiscard]] std::vector<const PublicFunctionArtifact *>
  findMemberFunctions(const PublicEntityReferenceArtifact &owner,
                      std::string_view name) const;
  [[nodiscard]] std::span<const PublicNominalTypeArtifact>
  nominalTypes() const {
    return nominal_types_;
  }
  [[nodiscard]] const PublicNominalTypeArtifact *
  findNominalType(std::string_view name) const;
  [[nodiscard]] std::span<const PublicValueArtifact> values() const {
    return values_;
  }
  [[nodiscard]] const PublicValueArtifact *
  findValue(std::string_view name) const;
  [[nodiscard]] std::span<const PublicInterfaceDeclarationArtifact>
  interfaceDeclarations() const {
    return interfaces_;
  }
  [[nodiscard]] const PublicInterfaceDeclarationArtifact *
  findInterface(std::string_view name) const;
  [[nodiscard]] std::span<const PublicTypeAliasArtifact> typeAliases() const {
    return type_aliases_;
  }
  [[nodiscard]] const PublicTypeAliasArtifact *
  findTypeAlias(std::string_view name) const;
  [[nodiscard]] std::span<const PublicInterfaceWitnessArtifact>
  interfaceWitnesses() const {
    return interface_witnesses_;
  }
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string print() const;

private:
  friend class internal::PublicInterfaceVerifyService;
  friend class internal::PublicInterfaceArtifactVerificationService;
  [[nodiscard]] bool verifyBody(std::string &error) const;
  std::string package_name_;
  std::string module_name_;
  StableFingerprint fingerprint_;
  std::vector<PublicFunctionArtifact> functions_;
  std::vector<PublicNominalTypeArtifact> nominal_types_;
  std::vector<PublicValueArtifact> values_;
  std::vector<PublicInterfaceDeclarationArtifact> interfaces_;
  std::vector<PublicTypeAliasArtifact> type_aliases_;
  std::vector<PublicInterfaceWitnessArtifact> interface_witnesses_;
};

struct PublicFunctionBindingSpec {
  IdentifierId name;
  std::optional<PublicEntityReferenceArtifact> member_owner;
  PublicFunctionArtifact::MemberKind member_kind =
      PublicFunctionArtifact::MemberKind::None;
  std::uint32_t generic_parameter_count = 0;
  std::vector<PublicType> parameters;
  std::vector<std::string> parameter_names;
  std::vector<std::optional<PublicConstantValue>> default_arguments;
  PublicType return_type;
  std::optional<PublicType> error_type;
  PublicFunctionExecutionKind execution_kind =
      PublicFunctionExecutionKind::Immediate;
  PublicCoroutineConstructorABI coroutine_constructor;
  PublicNominalConstructorABI nominal_constructor;
  CallableSemanticContract semantic_contract;
  CompilerIntrinsicRole intrinsic_role = CompilerIntrinsicRole::None;
  CallableOwnershipSummary ownership_summary;
  PublicCallableDeclarationKind declaration_kind =
      PublicCallableDeclarationKind::Definition;
  bool is_unsafe = false;
  bool is_const = false;
  std::string foreign_abi;
  std::string external_symbol;
  std::optional<ForeignAbiSignature> foreign_signature;
  std::optional<interop::ArtifactReference> interop_artifact;
  GenericId generic;
  std::optional<GenericTemplateArtifact> generic_template;
  std::vector<PublicInterfaceConstraintArtifact> constraints;
  PublicEntityId canonical_entity;
  IdentifierId canonical_name;
};

// Compilation-local native roots which are addressable through published
// interface witnesses. This is intentionally not serialized into artifacts.
struct NativeDefinitionExportClosure {
  std::vector<std::uint32_t> functions;

  void canonicalize() {
    std::ranges::sort(functions,
                      [](const auto lhs, const auto rhs) { return lhs < rhs; });
    functions.erase(std::unique(functions.begin(), functions.end()),
                    functions.end());
  }
};

struct PublicFunctionBinding {
  IdentifierId name;
  std::optional<PublicEntityReferenceArtifact> member_owner;
  PublicFunctionArtifact::MemberKind member_kind =
      PublicFunctionArtifact::MemberKind::None;
  std::uint32_t generic_parameter_count = 0;
  PublicTypeBlockId parameters;
  PublicType return_type;
  PublicEntityId canonical_entity;
};

struct PublicNominalTypeBinding {
  IdentifierId name;
  std::uint32_t generic_parameter_count = 0;
  PublicEntityId canonical_entity;
};

struct PublicEntity {
  PublicEntityKind kind = PublicEntityKind::Function;
  IdentifierId package_name;
  IdentifierId module_name;
  IdentifierId name;
  std::optional<PublicEntityReferenceArtifact> member_owner;
  PublicFunctionArtifact::MemberKind member_kind =
      PublicFunctionArtifact::MemberKind::None;
  std::uint32_t generic_parameter_count = 0;
  std::vector<PublicType> parameters;
  std::vector<std::string> parameter_names;
  std::vector<std::optional<PublicConstantValue>> default_arguments;
  PublicType return_type;
  std::optional<PublicType> error_type;
  PublicFunctionExecutionKind execution_kind =
      PublicFunctionExecutionKind::Immediate;
  PublicCoroutineConstructorABI coroutine_constructor;
  PublicNominalConstructorABI nominal_constructor;
  CallableSemanticContract semantic_contract;
  CompilerIntrinsicRole intrinsic_role = CompilerIntrinsicRole::None;
  CallableOwnershipSummary ownership_summary;
  PublicCallableDeclarationKind declaration_kind =
      PublicCallableDeclarationKind::Definition;
  bool is_unsafe = false;
  bool is_const = false;
  IdentifierId foreign_abi;
  IdentifierId external_symbol;
  std::optional<ForeignAbiSignature> foreign_signature;
  std::optional<interop::ArtifactReference> interop_artifact;
  GenericId generic;
  std::optional<GenericTemplateArtifact> generic_template;
  std::vector<PublicInterfaceConstraintArtifact> constraints;
  StableFingerprint fingerprint;
  bool nominal_is_exported = true;
  std::vector<PublicNominalFieldArtifact> nominal_fields;
  std::vector<PublicEnumVariantArtifact> nominal_variants;
  bool nominal_is_value_enum = false;
  std::optional<PublicType> nominal_value_repr_pattern;
  std::optional<PublicType> nominal_object_repr_pattern;
  NominalRepresentationPolicy nominal_representation_policy =
      NominalRepresentationPolicy::Opaque;
  NominalKind nominal_kind = NominalKind::Struct;
  std::optional<PublicType> nominal_foreign_representation;
  ForeignResourceInvalidState nominal_foreign_invalid_state =
      ForeignResourceInvalidState::Count;
  std::int64_t nominal_foreign_invalid_integer = 0;
  std::optional<PublicType> nominal_foreign_handle_type;
  std::optional<PublicType> nominal_foreign_completion_handle_type;
  std::optional<PublicType> nominal_foreign_callback_type;
  std::optional<PublicType> nominal_foreign_waker_type;
  ForeignResourceProtocol nominal_foreign_resource_protocol;
  std::vector<PublicForeignResourceOperationArtifact>
      nominal_foreign_resource_operations;
  std::optional<PublicInterfaceDeclarationArtifact> interface_declaration;
  std::optional<PublicTypeAliasArtifact> type_alias;
};

class PublicInterface {
public:
  [[nodiscard]] PublicBindingId findFunction(IdentifierId name) const;
  [[nodiscard]] std::span<const PublicBindingId>
  findFunctions(IdentifierId name) const;
  [[nodiscard]] PublicBindingId
  findMemberFunction(const PublicEntityReferenceArtifact &owner,
                     IdentifierId name) const;
  [[nodiscard]] std::span<const PublicBindingId>
  findMemberFunctions(const PublicEntityReferenceArtifact &owner,
                      IdentifierId name) const;
  [[nodiscard]] PublicBindingId findNominalType(IdentifierId name) const;
  [[nodiscard]] const PublicFunctionBinding &function(PublicBindingId id) const;
  [[nodiscard]] const PublicNominalTypeBinding &
  nominalType(PublicBindingId id) const;
  [[nodiscard]] std::span<const PublicType>
  parameterTypes(PublicTypeBlockId id) const;
  [[nodiscard]] CheckIRId checkIRId() const {
    return check_ir_id_;
  }
  [[nodiscard]] IdentifierId packageName() const {
    return package_name_;
  }
  [[nodiscard]] PublicInterfaceId interfaceId() const {
    return interface_id_;
  }
  [[nodiscard]] IdentifierId moduleName() const {
    return module_name_;
  }
  [[nodiscard]] const StableFingerprint &fingerprint() const {
    return fingerprint_;
  }
  [[nodiscard]] std::size_t bindingCount() const {
    return functions_.size();
  }
  [[nodiscard]] std::size_t nominalTypeBindingCount() const {
    return nominal_types_.size();
  }
  [[nodiscard]] std::span<const PublicNominalTypeArtifact>
  nominalArtifacts() const {
    return nominal_artifacts_;
  }
  [[nodiscard]] std::span<const PublicValueArtifact> valueArtifacts() const {
    return value_artifacts_;
  }
  [[nodiscard]] const PublicValueArtifact *findValue(IdentifierId name) const;
  [[nodiscard]] const PublicInterfaceDeclarationArtifact *
  findInterface(IdentifierId name) const;
  [[nodiscard]] const PublicTypeAliasArtifact *
  findTypeAlias(IdentifierId name) const;
  [[nodiscard]] std::span<const PublicInterfaceDeclarationArtifact>
  interfaceArtifacts() const {
    return interface_artifacts_;
  }
  [[nodiscard]] std::span<const PublicTypeAliasArtifact>
  typeAliasArtifacts() const {
    return type_alias_artifacts_;
  }
  [[nodiscard]] std::span<const PublicInterfaceWitnessArtifact>
  interfaceWitnessArtifacts() const {
    return interface_witness_artifacts_;
  }
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string print() const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  friend class PublicInterfaceRegistry;
  friend class internal::PublicInterfaceVerifyService;
  friend class internal::PublicInterfaceRegistryConstructionService;
  PublicInterface(core::Arena &arena, SharedValueStores &values,
                  CheckIRId check_ir_id, PublicInterfaceId interface_id,
                  IdentifierId package_name, IdentifierId module_name);

  [[nodiscard]] PublicBindingId
  addFunction(const PublicFunctionBindingSpec &function,
              PublicEntityId canonical_entity);
  [[nodiscard]] PublicBindingId
  addNominalType(const PublicNominalTypeArtifact &nominal,
                 PublicEntityId canonical_entity);
  void setFingerprint(StableFingerprint fingerprint) {
    fingerprint_ = fingerprint;
  }

  SharedValueStores *values_;
  CheckIRId check_ir_id_;
  PublicInterfaceId interface_id_;
  IdentifierId package_name_;
  IdentifierId module_name_;
  StableFingerprint fingerprint_;
  core::ValueStore<PublicBindingId, PublicFunctionBinding> functions_;
  std::vector<std::vector<PublicType>> parameter_blocks_;
  std::unordered_map<std::uint32_t, std::vector<PublicBindingId>>
      function_names_;
  std::unordered_map<std::string, std::vector<PublicBindingId>>
      member_function_names_;
  core::ValueStore<PublicBindingId, PublicNominalTypeBinding> nominal_types_;
  std::unordered_map<std::uint32_t, PublicBindingId> nominal_type_names_;
  std::vector<PublicNominalTypeArtifact> nominal_artifacts_;
  std::vector<PublicValueArtifact> value_artifacts_;
  std::vector<PublicInterfaceDeclarationArtifact> interface_artifacts_;
  std::vector<PublicTypeAliasArtifact> type_alias_artifacts_;
  std::vector<PublicInterfaceWitnessArtifact> interface_witness_artifacts_;
};

class PublicInterfaceRegistry {
public:
  explicit PublicInterfaceRegistry(SharedValueStores &values);
  ~PublicInterfaceRegistry();
  [[nodiscard]] IdentifierId internIdentifier(std::string_view value);

  [[nodiscard]] PublicInterfaceId registerInterface(
      CheckIRId check_ir_id, IdentifierId package_name,
      IdentifierId module_name,
      std::span<const PublicFunctionBindingSpec> functions, std::string &error,
      std::span<const PublicNominalTypeArtifact> nominal_types = {},
      std::span<const PublicValueArtifact> values = {},
      std::span<const PublicInterfaceDeclarationArtifact> interfaces = {},
      std::span<const PublicTypeAliasArtifact> type_aliases = {},
      std::span<const PublicInterfaceWitnessArtifact> interface_witnesses = {});
  [[nodiscard]] PublicInterfaceId
  registerArtifact(CheckIRId check_ir_id,
                   const PublicInterfaceArtifact &artifact, std::string &error);
  [[nodiscard]] PublicInterfaceId
  registerExternalArtifact(const PublicInterfaceArtifact &artifact,
                           std::string &error);
  [[nodiscard]] bool registerArtifactClosure(
      std::span<const PublicInterfaceArtifact *const> artifacts,
      std::string &error);
  [[nodiscard]] PublicInterfaceArtifact buildArtifact(PublicInterfaceId id,
                                                      std::string &error) const;
  [[nodiscard]] const PublicInterface *tryGet(PublicInterfaceId id) const;
  [[nodiscard]] const PublicEntity *tryGetEntity(PublicEntityId id) const;
  [[nodiscard]] PublicEntityId
  findEntity(std::string_view canonical_package,
             std::string_view canonical_module, std::string_view canonical_name,
             PublicEntityKind kind = PublicEntityKind::Function,
             StableFingerprint expected_fingerprint = {}) const;
  [[nodiscard]] PublicInterfaceId findByModule(IdentifierId package_name,
                                               IdentifierId module_name) const;
  [[nodiscard]] PublicInterfaceId findByCheckIR(CheckIRId check_ir_id) const;
  [[nodiscard]] bool verifyOwnershipSummaryTypes(
      std::span<const PublicType> parameters, const PublicType &return_type,
      const CallableOwnershipSummary &summary, std::string &error) const;
  [[nodiscard]] std::size_t interfaceCount() const {
    return interfaces_.size();
  }
  [[nodiscard]] std::size_t entityCount() const {
    return entities_.size();
  }
  [[nodiscard]] bool verify(std::string &error) const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  friend class internal::PublicInterfaceRegistryService;
  friend class internal::PublicInterfaceRegistryConstructionService;
  friend class internal::PublicInterfaceArtifactBuildService;
  SharedValueStores *values_;
  core::Arena arena_;
  std::vector<std::unique_ptr<PublicInterface>> interfaces_;
  core::ValueStore<PublicEntityId, PublicEntity> entities_;
  std::unordered_map<std::string, PublicInterfaceId> modules_;
  std::unordered_map<std::uint32_t, PublicInterfaceId> check_irs_;
  std::unordered_map<std::string, PublicEntityId> entity_keys_;
};

[[nodiscard]] PublicInterfaceId registerPublicInterface(
    const SemIR &sem_ir, PublicInterfaceRegistry &registry,
    interop::ArtifactRegistry &interop_registry, IdentifierId package_name,
    std::string &error,
    NativeDefinitionExportClosure *native_exports = nullptr);
[[nodiscard]] std::string publicTypeName(PublicType type);
[[nodiscard]] StableFingerprint publicTypeFingerprint(const PublicType &type);

} // namespace chtholly::compiler
