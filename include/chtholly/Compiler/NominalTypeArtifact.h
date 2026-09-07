#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

inline constexpr std::uint32_t NominalLayoutAbiEpoch = 9;

enum class ValueReprKind : std::uint8_t {
  None,
  Copy,
  Pointer,
  Custom,
  Dependent,
  Count,
};

enum class InitReprKind : std::uint8_t {
  None,
  ByCopy,
  ByConversion,
  InPlace,
  Dependent,
  Count,
};

enum class ObjectReprKind : std::uint8_t {
  None,
  Identity,
  NominalAggregate,
  Custom,
  Dependent,
  Count,
};

enum class ObjectFieldProjectionKind : std::uint8_t {
  StableAddress,
  Computed,
  BitPacked,
  Count,
};

enum ObjectProjectionCapability : std::uint16_t {
  ProjectionLoad = 1U << 0U,
  ProjectionStore = 1U << 1U,
  ProjectionTake = 1U << 2U,
  ProjectionInit = 1U << 3U,
  ProjectionBorrow = 1U << 4U,
  ProjectionBorrowMut = 1U << 5U,
};

struct ObjectFieldProjectionArtifact {
  ObjectFieldProjectionKind kind = ObjectFieldProjectionKind::StableAddress;
  std::vector<std::uint32_t> field_indices;
  // A canonical path in the selected object carrier. Empty denotes the whole
  // carrier and is therefore conservative for aliasing.
  std::vector<std::uint32_t> region_indices;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;
  std::uint16_t capabilities = 0;
  std::optional<PublicEntityReferenceArtifact> load_target;
  std::optional<PublicEntityReferenceArtifact> store_target;
  std::optional<PublicEntityReferenceArtifact> take_target;
  std::optional<PublicEntityReferenceArtifact> init_target;
  std::optional<PublicEntityReferenceArtifact> borrow_target;
  std::optional<PublicEntityReferenceArtifact> borrow_mut_target;

  friend bool operator==(const ObjectFieldProjectionArtifact &,
                         const ObjectFieldProjectionArtifact &) = default;
};

enum class OwnershipReprKind : std::uint8_t {
  None,
  Borrowed,
  Owned,
  Dependent,
  Count,
};

enum class CopyReprKind : std::uint8_t {
  None,
  Trivial,
  Custom,
  Unavailable,
  Dependent,
  Count,
};

enum class MoveReprKind : std::uint8_t {
  None,
  Trivial,
  Unavailable,
  Dependent,
  Count,
};

enum class DestroyReprKind : std::uint8_t {
  None,
  Trivial,
  Custom,
  Dependent,
  Count,
};

// A lifecycle body is deliberately structural.  It records the field-wise
// operations selected by semantic checking without exposing LLVM layout or
// process-local instruction ids to the artifact store.
enum class LifecycleBodyOpKind : std::uint8_t {
  CopyField,
  DropField,
  Count,
};

struct LifecycleBodyOp {
  LifecycleBodyOpKind kind = LifecycleBodyOpKind::Count;
  std::uint32_t field_index = 0;
  StableFingerprint child_witness;

  friend bool operator==(const LifecycleBodyOp &,
                         const LifecycleBodyOp &) = default;
};

struct TypeRepresentationFacts {
  ValueReprKind value_repr = ValueReprKind::None;
  InitReprKind init_repr = InitReprKind::None;
  OwnershipReprKind ownership = OwnershipReprKind::None;
  CopyReprKind copy = CopyReprKind::None;
  MoveReprKind move = MoveReprKind::None;
  DestroyReprKind destroy = DestroyReprKind::None;
  ObjectReprKind object_repr = ObjectReprKind::None;

  friend bool operator==(const TypeRepresentationFacts &,
                         const TypeRepresentationFacts &) = default;
};

struct TypeConcurrencyFacts {
  bool transferable = false;
  bool shareable = false;

  friend bool operator==(const TypeConcurrencyFacts &,
                         const TypeConcurrencyFacts &) = default;
};

struct NominalSemanticWitnessArtifact {
  PublicEntityReferenceArtifact nominal_template;
  std::vector<PublicType> arguments;
  StableFingerprint semantic_options_fingerprint;
  StableFingerprint structural_specific_fingerprint;
  TypeRepresentationFacts representation;
  TypeConcurrencyFacts concurrency;
  std::optional<PublicEntityReferenceArtifact> copy_target;
  std::optional<PublicEntityReferenceArtifact> destroy_target;
  std::optional<PublicEntityReferenceArtifact> pack_target;
  std::optional<PublicEntityReferenceArtifact> init_target;
  std::optional<PublicType> value_repr_carrier;
  std::optional<PublicType> object_repr_carrier;
  std::vector<ObjectFieldProjectionArtifact> object_field_projections;
  std::optional<PublicEntityReferenceArtifact> object_init_target;
  std::optional<PublicEntityReferenceArtifact> object_copy_init_target;
  std::optional<PublicEntityReferenceArtifact> object_move_init_target;
  std::optional<PublicEntityReferenceArtifact> object_drop_target;
  std::vector<LifecycleBodyOp> copy_body;
  std::vector<LifecycleBodyOp> drop_body;
  std::vector<StableFingerprint> transitive_specific_fingerprints;
  StableFingerprint request_fingerprint;
  StableFingerprint result_fingerprint;
  NominalKind kind = NominalKind::Struct;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<NominalSemanticWitnessArtifact>
  decode(std::string_view bytes, std::string &error);

  friend bool operator==(const NominalSemanticWitnessArtifact &,
                         const NominalSemanticWitnessArtifact &) = default;
};

[[nodiscard]] PublicNominalTypeArtifact buildPublicNominalTypeArtifact(
    std::string canonical_package, std::string canonical_module,
    std::string canonical_name, std::uint32_t generic_parameter_count,
    std::vector<PublicNominalFieldArtifact> fields,
    std::optional<PublicType> value_repr_pattern = std::nullopt,
    std::optional<PublicType> object_repr_pattern = std::nullopt,
    NominalRepresentationPolicy representation_policy =
        NominalRepresentationPolicy::Opaque,
    NominalKind kind = NominalKind::Struct,
    std::vector<PublicEnumVariantArtifact> variants = {},
    bool is_exported = true, bool is_value_enum = false);
void finalizePublicNominalTypeArtifact(PublicNominalTypeArtifact &artifact);

struct NominalTypeSpecificArtifact {
  PublicEntityReferenceArtifact template_entity;
  std::vector<PublicType> arguments;
  StableFingerprint semantic_options_fingerprint;
  std::vector<PublicNominalFieldArtifact> fields;
  std::vector<PublicEnumVariantArtifact> variants;
  bool is_value_enum = false;
  NominalRepresentationPolicy representation_policy =
      NominalRepresentationPolicy::Opaque;
  std::optional<PublicType> object_repr_carrier;
  std::vector<StableFingerprint> child_specific_fingerprints;
  StableFingerprint structural_fingerprint;
  NominalSemanticWitnessArtifact nominal_semantic_witness;
  StableFingerprint request_fingerprint;
  StableFingerprint result_fingerprint;
  NominalKind kind = NominalKind::Struct;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<NominalTypeSpecificArtifact>
  decode(std::string_view bytes, std::string &error);

  friend bool operator==(const NominalTypeSpecificArtifact &,
                         const NominalTypeSpecificArtifact &) = default;
};

struct TargetLayoutConfig {
  std::string normalized_triple;
  std::uint32_t pointer_width = 0;
  std::uint32_t abi_epoch = NominalLayoutAbiEpoch;

  [[nodiscard]] StableFingerprint fingerprint() const;
  [[nodiscard]] bool verify(std::string &error) const;

  friend bool operator==(const TargetLayoutConfig &,
                         const TargetLayoutConfig &) = default;
};

struct NominalFieldLayoutArtifact {
  std::string name;
  PublicType type;
  ObjectFieldProjectionKind kind = ObjectFieldProjectionKind::StableAddress;
  std::optional<PublicType> storage_type;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::uint32_t bit_begin = 0;
  std::uint32_t bit_end = 0;

  friend bool operator==(const NominalFieldLayoutArtifact &,
                         const NominalFieldLayoutArtifact &) = default;
};

struct EnumVariantLayoutArtifact {
  std::string name;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::vector<NominalFieldLayoutArtifact> fields;

  friend bool operator==(const EnumVariantLayoutArtifact &,
                         const EnumVariantLayoutArtifact &) = default;
};

struct NominalTypeLayoutArtifact {
  StableFingerprint type_specific_fingerprint;
  StableFingerprint target_fingerprint;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::vector<NominalFieldLayoutArtifact> fields;
  std::uint32_t tag_size = 0;
  std::uint64_t payload_offset = 0;
  std::vector<EnumVariantLayoutArtifact> variants;
  StableFingerprint request_fingerprint;
  StableFingerprint result_fingerprint;
  NominalKind kind = NominalKind::Struct;

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<NominalTypeLayoutArtifact>
  decode(std::string_view bytes, std::string &error);

  friend bool operator==(const NominalTypeLayoutArtifact &,
                         const NominalTypeLayoutArtifact &) = default;
};

struct TypeSpecificBuildInput {
  const PublicNominalTypeArtifact *definition = nullptr;
  std::span<const PublicType> arguments;
  StableFingerprint semantic_options_fingerprint;
  std::function<std::optional<StableFingerprint>(const PublicType &,
                                                 std::string &)>
      child_specific_fingerprint;
};

struct NominalSemanticWitnessBuildInput {
  const NominalTypeSpecificArtifact *specific = nullptr;
  TypeRepresentationFacts representation;
  TypeConcurrencyFacts concurrency;
  std::optional<PublicEntityReferenceArtifact> copy_target;
  std::optional<PublicEntityReferenceArtifact> destroy_target;
  std::optional<PublicEntityReferenceArtifact> pack_target;
  std::optional<PublicEntityReferenceArtifact> init_target;
  std::optional<PublicType> value_repr_carrier;
  std::optional<PublicType> object_repr_carrier;
  std::vector<ObjectFieldProjectionArtifact> object_field_projections;
  std::optional<PublicEntityReferenceArtifact> object_init_target;
  std::optional<PublicEntityReferenceArtifact> object_copy_init_target;
  std::optional<PublicEntityReferenceArtifact> object_move_init_target;
  std::optional<PublicEntityReferenceArtifact> object_drop_target;
  std::vector<LifecycleBodyOp> copy_body;
  std::vector<LifecycleBodyOp> drop_body;
  std::vector<StableFingerprint> transitive_specific_fingerprints;
};

[[nodiscard]] std::optional<NominalSemanticWitnessArtifact>
buildNominalSemanticWitnessArtifact(
    const NominalSemanticWitnessBuildInput &input, std::string &error);

[[nodiscard]] bool
bindNominalSemanticWitness(NominalTypeSpecificArtifact &specific,
                           NominalSemanticWitnessArtifact witness,
                           std::string &error);

[[nodiscard]] std::optional<NominalTypeSpecificArtifact>
buildNominalTypeSpecific(const TypeSpecificBuildInput &input,
                         std::string &error);

using TypeLayoutQuery = std::function<std::optional<NominalTypeLayoutArtifact>(
    const PublicType &, std::string &)>;

[[nodiscard]] std::optional<NominalTypeLayoutArtifact>
buildNominalTypeLayout(const NominalTypeSpecificArtifact &specific,
                       const TargetLayoutConfig &target,
                       const TypeLayoutQuery &query_child_layout,
                       std::string &error);

[[nodiscard]] std::string canonicalPublicTypeBytes(const PublicType &type);
[[nodiscard]] bool verifyPublicType(const PublicType &type,
                                    std::uint32_t generic_parameter_count,
                                    bool allow_void, std::string &error);

} // namespace chtholly::compiler
