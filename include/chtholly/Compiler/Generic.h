#pragma once

#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/CompilationIds.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chtholly::compiler {

enum class CanonicalTypeKind : std::uint8_t {
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

enum class CanonicalTypeProjectionKind : std::uint8_t {
  Element,
  Pointee,
  Count,
};

struct CanonicalType {
  CanonicalTypeKind kind = CanonicalTypeKind::Count;
  std::uint32_t arg0 = core::AnyId::InvalidIndex;
  std::uint32_t arg1 = core::AnyId::InvalidIndex;
  CanonicalTypeProjectionKind projection_kind =
      CanonicalTypeProjectionKind::Count;
  std::vector<CanonicalTypeId> elements;
  bool abi_union = false;
  ForeignCallingConvention foreign_calling_convention =
      ForeignCallingConvention::C;
  std::string nominal_key;
  CallableOwnershipSummary callable_contract;
  std::uint32_t callable_context_parameter = core::AnyId::InvalidIndex;
  std::uint32_t registration_authority = 0;
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
  ForeignResourceProtocolId foreign_resource_protocol;

  friend bool operator==(const CanonicalType &,
                         const CanonicalType &) = default;
};

struct CanonicalForeignResourceProtocol {
  ForeignResourceProtocol facts;
  std::vector<CanonicalTypeId> types;

  friend bool operator==(const CanonicalForeignResourceProtocol &,
                         const CanonicalForeignResourceProtocol &) = default;
};

struct CanonicalForeignResourceProtocolHash {
  std::size_t
  operator()(const CanonicalForeignResourceProtocol &value) const noexcept;
};

struct CanonicalTypeHash {
  std::size_t operator()(const CanonicalType &type) const noexcept;
};

enum class ConstantDependence : std::uint8_t {
  Concrete,
  CheckedSymbolic,
  TemplateSymbolic,
  Runtime,
};

struct CanonicalConstant {
  CanonicalTypeId type;
  ConstantDependence dependence = ConstantDependence::Runtime;

  friend bool operator==(const CanonicalConstant &,
                         const CanonicalConstant &) = default;
};

struct CanonicalConstantHash {
  std::size_t operator()(const CanonicalConstant &value) const noexcept;
};

struct Generic {
  CheckIRId owner;
  IdentifierId module_name;
  IdentifierId name;
  std::uint32_t binding_count = 0;
  SpecificId self_specific;
};

enum class SpecificRegionState : std::uint8_t {
  Unresolved,
  Queued,
  Evaluating,
  Ready,
  Failed,
};

struct SpecificDependency {
  SpecificId target;
  std::uint32_t public_entity = core::AnyId::InvalidIndex;
  std::uint32_t call_ordinal = 0;

  friend bool operator==(const SpecificDependency &,
                         const SpecificDependency &) = default;
};

struct Specific {
  GenericId generic;
  std::vector<CanonicalConstantId> arguments;
  SpecificRegionState declaration_state = SpecificRegionState::Unresolved;
  SpecificRegionState definition_state = SpecificRegionState::Unresolved;
  std::vector<SpecificDependency> dependencies;
  std::vector<StableFingerprint> constraint_witnesses;
  std::string fingerprint_key;
};

class GenericValueStores {
public:
  GenericValueStores();

  [[nodiscard]] CanonicalTypeId internType(CanonicalType type);
  [[nodiscard]] const CanonicalType &type(CanonicalTypeId id) const;
  [[nodiscard]] ForeignResourceProtocolId
  internForeignResourceProtocol(CanonicalForeignResourceProtocol protocol);
  [[nodiscard]] const CanonicalForeignResourceProtocol &
  foreignResourceProtocol(ForeignResourceProtocolId id) const;
  [[nodiscard]] CanonicalConstantId
  internTypeConstant(CanonicalTypeId type, ConstantDependence dependence);
  [[nodiscard]] const CanonicalConstant &constant(CanonicalConstantId id) const;

  [[nodiscard]] GenericId addGeneric(CheckIRId owner, IdentifierId module_name,
                                     IdentifierId name,
                                     std::uint32_t binding_count);
  [[nodiscard]] const Generic &generic(GenericId id) const;
  [[nodiscard]] SpecificId
  getOrAddSpecific(GenericId generic,
                   std::span<const CanonicalConstantId> arguments,
                   std::span<const StableFingerprint> constraint_witnesses = {});
  [[nodiscard]] const Specific &specific(SpecificId id) const;
  [[nodiscard]] Specific &specific(SpecificId id);

  [[nodiscard]] CanonicalTypeId voidType() const {
    return void_type_;
  }
  [[nodiscard]] CanonicalTypeId boolType() const {
    return bool_type_;
  }
  [[nodiscard]] CanonicalTypeId charType() const { return char_type_; }
  [[nodiscard]] CanonicalTypeId i32Type() const {
    return i32_type_;
  }
  [[nodiscard]] CanonicalTypeId integerType(std::uint32_t width,
                                            bool is_signed) {
    return internType({.kind = CanonicalTypeKind::Integer,
                       .arg0 = width,
                       .arg1 = is_signed ? 1U : 0U});
  }
  [[nodiscard]] CanonicalTypeId floatType(std::uint32_t width) {
    return internType({.kind = CanonicalTypeKind::Float, .arg0 = width});
  }
  [[nodiscard]] CanonicalTypeId stringType() const {
    return string_type_;
  }
  [[nodiscard]] CanonicalTypeId neverType() const {
    return never_type_;
  }
  [[nodiscard]] CanonicalTypeId
  tupleType(std::span<const CanonicalTypeId> elements) {
    CanonicalType type;
    type.kind = CanonicalTypeKind::Tuple;
    type.elements.assign(elements.begin(), elements.end());
    return internType(std::move(type));
  }
  [[nodiscard]] CanonicalTypeId sliceType(CanonicalTypeId element,
                                          bool mutable_view = false) {
    CanonicalType type;
    type.kind = CanonicalTypeKind::Slice;
    type.arg0 = mutable_view ? 1U : 0U;
    type.elements.push_back(element);
    return internType(std::move(type));
  }
  [[nodiscard]] std::size_t typeCount() const {
    return types_.size();
  }
  [[nodiscard]] std::size_t foreignResourceProtocolCount() const {
    return foreign_resource_protocols_.size();
  }
  [[nodiscard]] std::size_t genericCount() const {
    return generics_.size();
  }
  [[nodiscard]] std::size_t specificCount() const {
    return specifics_.size();
  }
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  core::CanonicalValueStore<CanonicalTypeId, CanonicalType, CanonicalTypeHash>
      types_;
  core::CanonicalValueStore<CanonicalConstantId, CanonicalConstant,
                            CanonicalConstantHash>
      constants_;
  core::CanonicalValueStore<ForeignResourceProtocolId,
                            CanonicalForeignResourceProtocol,
                            CanonicalForeignResourceProtocolHash>
      foreign_resource_protocols_;
  core::ValueStore<GenericId, Generic> generics_;
  core::ValueStore<SpecificId, Specific> specifics_;
  std::unordered_map<std::string, SpecificId> specific_lookup_;
  CanonicalTypeId void_type_;
  CanonicalTypeId bool_type_;
  CanonicalTypeId char_type_;
  CanonicalTypeId i32_type_;
  CanonicalTypeId string_type_;
  CanonicalTypeId never_type_;
};

[[nodiscard]] std::string_view canonicalTypeKindName(CanonicalTypeKind kind);

} // namespace chtholly::compiler
