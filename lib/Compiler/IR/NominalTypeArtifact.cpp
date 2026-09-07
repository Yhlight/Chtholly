#include "chtholly/Compiler/NominalTypeArtifact.h"

#include "ArtifactDecodeInternal.h"
#include "NominalLayoutVerificationInternal.h"
#include "NominalSemanticWitnessVerificationInternal.h"
#include "NominalTypeSpecificVerificationInternal.h"
#include "NominalTypeArtifactReaderInternal.h"
#include "NominalTypeArtifactVerificationInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <unordered_set>

namespace chtholly::compiler {
namespace {

using Reader = internal::NominalArtifactReader;

#include "NominalTypeArtifactSupport.inc"
} // namespace

#include "NominalTypeArtifactDefinition.inc"

#include "NominalSemanticWitnessArtifactMethods.inc"
#include "NominalTypeSpecificArtifactMethods.inc"
#include "NominalTypeLayoutArtifactMethods.inc"
std::optional<NominalTypeSpecificArtifact>
buildNominalTypeSpecific(const TypeSpecificBuildInput &input,
                         std::string &error) {
  error.clear();
  if (!input.definition || !input.definition->verify(error) ||
      input.arguments.size() != input.definition->generic_parameter_count) {
    if (error.empty())
      error = "nominal type specific has the wrong number of arguments";
    return std::nullopt;
  }
  NominalTypeSpecificArtifact result;
  result.template_entity = input.definition->entity;
  result.arguments.assign(input.arguments.begin(), input.arguments.end());
  result.semantic_options_fingerprint = input.semantic_options_fingerprint;
  result.representation_policy = input.definition->representation_policy;
  result.kind = input.definition->kind;
  result.is_value_enum = input.definition->is_value_enum;
  if (!result.semantic_options_fingerprint.hasValue()) {
    error = "nominal type specific requires semantic option identity";
    return std::nullopt;
  }
  for (const auto &argument : result.arguments)
    if (!verifyPublicType(argument, 0, true, error))
      return std::nullopt;
  for (const auto &field : input.definition->fields) {
    auto type = substituteType(field.type, input.arguments, error);
    if (!type)
      return std::nullopt;
    result.fields.push_back({field.name, std::move(*type), field.storage_path,
                             field.projection_kind, field.projector_name,
                             field.projection_region_path, field.bit_begin,
                             field.bit_end, field.is_public});
    if (result.fields.back().type.kind == PublicTypeKind::Nominal) {
      if (!input.child_specific_fingerprint) {
        error = "nominal type specific requires a child type query";
        return std::nullopt;
      }
      auto child =
          input.child_specific_fingerprint(result.fields.back().type, error);
      if (!child || !child->hasValue())
        return std::nullopt;
      result.child_specific_fingerprints.push_back(*child);
    }
  }
  for (const auto &source_variant : input.definition->variants) {
    PublicEnumVariantArtifact variant;
    variant.name = source_variant.name;
    variant.shape = source_variant.shape;
    variant.discriminant = source_variant.discriminant;
    for (const auto &field : source_variant.fields) {
      auto type = substituteType(field.type, input.arguments, error);
      if (!type)
        return std::nullopt;
      variant.fields.push_back({field.name,
                                std::move(*type),
                                {},
                                PublicObjectProjectionKind::StableAddress,
                                {},
                                {},
                                0,
                                0,
                                field.is_public});
      if (variant.fields.back().type.kind == PublicTypeKind::Nominal) {
        if (!input.child_specific_fingerprint) {
          error = "enum specific requires a child type query";
          return std::nullopt;
        }
        auto child =
            input.child_specific_fingerprint(variant.fields.back().type, error);
        if (!child || !child->hasValue())
          return std::nullopt;
        result.child_specific_fingerprints.push_back(*child);
      }
    }
    result.variants.push_back(std::move(variant));
  }
  if (input.definition->object_repr_pattern) {
    result.object_repr_carrier = substituteType(
        *input.definition->object_repr_pattern, input.arguments, error);
    if (!result.object_repr_carrier ||
        result.object_repr_carrier->kind != PublicTypeKind::Nominal) {
      if (error.empty())
        error =
            "custom object representation requires a concrete nominal carrier";
      return std::nullopt;
    }
  }
  result.request_fingerprint =
      specificRequestFingerprint(result.template_entity, result.arguments,
                                 input.semantic_options_fingerprint);
  result.structural_fingerprint = structuralSpecificFingerprint(
      result.template_entity, result.arguments,
      result.semantic_options_fingerprint, result.fields, result.variants,
      result.representation_policy, result.kind, result.is_value_enum,
      result.object_repr_carrier, result.child_specific_fingerprints);
  result.nominal_semantic_witness.nominal_template = result.template_entity;
  result.nominal_semantic_witness.arguments = result.arguments;
  result.nominal_semantic_witness.semantic_options_fingerprint =
      result.semantic_options_fingerprint;
  result.nominal_semantic_witness.structural_specific_fingerprint =
      result.structural_fingerprint;
  result.nominal_semantic_witness.kind = result.kind;
  result.nominal_semantic_witness.representation = {
      ValueReprKind::Pointer,          InitReprKind::InPlace,
      OwnershipReprKind::Owned,        CopyReprKind::Trivial,
      MoveReprKind::Trivial,           DestroyReprKind::Trivial,
      ObjectReprKind::NominalAggregate};
  if (result.object_repr_carrier) {
    result.nominal_semantic_witness.representation.object_repr =
        ObjectReprKind::Custom;
    result.nominal_semantic_witness.object_repr_carrier =
        result.object_repr_carrier;
  }
  if (input.definition->value_repr_pattern) {
    auto carrier = substituteType(*input.definition->value_repr_pattern,
                                  input.arguments, error);
    if (!carrier)
      return std::nullopt;
    if (carrier->kind == PublicTypeKind::Void ||
        carrier->kind == PublicTypeKind::Never ||
        carrier->kind == PublicTypeKind::TypeParameter ||
        carrier->kind == PublicTypeKind::Count) {
      error = "custom value representation requires a concrete carrier";
      return std::nullopt;
    }
    result.nominal_semantic_witness.representation.value_repr =
        ValueReprKind::Custom;
    result.nominal_semantic_witness.representation.init_repr =
        InitReprKind::ByConversion;
    result.nominal_semantic_witness.value_repr_carrier = std::move(*carrier);
  }
  result.nominal_semantic_witness.transitive_specific_fingerprints =
      result.child_specific_fingerprints;
  std::ranges::sort(
      result.nominal_semantic_witness.transitive_specific_fingerprints, {},
      [](const StableFingerprint &fingerprint) { return fingerprint.hex(); });
  result.nominal_semantic_witness.transitive_specific_fingerprints.erase(
      std::ranges::unique(
          result.nominal_semantic_witness.transitive_specific_fingerprints)
          .begin(),
      result.nominal_semantic_witness.transitive_specific_fingerprints.end());
  result.nominal_semantic_witness.request_fingerprint =
      semanticWitnessRequestFingerprint(result.nominal_semantic_witness);
  result.nominal_semantic_witness.result_fingerprint =
      semanticWitnessResultFingerprint(result.nominal_semantic_witness);
  result.result_fingerprint = specificResultFingerprint(result);
  return result;
}

std::optional<NominalSemanticWitnessArtifact>
buildNominalSemanticWitnessArtifact(
    const NominalSemanticWitnessBuildInput &input, std::string &error) {
  error.clear();
  if (!input.specific || !input.specific->verify(error)) {
    if (error.empty())
      error = "nominal semantic witness requires a verified nominal specific";
    return std::nullopt;
  }
  if (input.representation.value_repr == ValueReprKind::Custom &&
      (!input.pack_target || !input.init_target)) {
    error = "custom value representation requires pack and init targets";
    return std::nullopt;
  }
  if (input.representation.object_repr == ObjectReprKind::Custom &&
      (!input.object_repr_carrier || input.object_field_projections.size() !=
                                         input.specific->fields.size())) {
    error = "custom object representation requires one projection per logical "
            "field";
    return std::nullopt;
  }
  NominalSemanticWitnessArtifact result;
  result.nominal_template = input.specific->template_entity;
  result.arguments = input.specific->arguments;
  result.semantic_options_fingerprint =
      input.specific->semantic_options_fingerprint;
  result.structural_specific_fingerprint =
      input.specific->structural_fingerprint;
  result.kind = input.specific->kind;
  result.representation = input.representation;
  result.concurrency = input.concurrency;
  result.copy_target = input.copy_target;
  result.destroy_target = input.destroy_target;
  result.pack_target = input.pack_target;
  result.init_target = input.init_target;
  result.value_repr_carrier = input.value_repr_carrier;
  result.object_repr_carrier = input.object_repr_carrier;
  result.object_field_projections = input.object_field_projections;
  result.object_init_target = input.object_init_target;
  result.object_copy_init_target = input.object_copy_init_target;
  result.object_move_init_target = input.object_move_init_target;
  result.object_drop_target = input.object_drop_target;
  result.copy_body = input.copy_body;
  result.drop_body = input.drop_body;
  result.transitive_specific_fingerprints =
      input.transitive_specific_fingerprints.empty()
          ? input.specific->child_specific_fingerprints
          : input.transitive_specific_fingerprints;
  std::ranges::sort(
      result.transitive_specific_fingerprints, {},
      [](const StableFingerprint &fingerprint) { return fingerprint.hex(); });
  result.transitive_specific_fingerprints.erase(
      std::ranges::unique(result.transitive_specific_fingerprints).begin(),
      result.transitive_specific_fingerprints.end());
  result.request_fingerprint = semanticWitnessRequestFingerprint(result);
  result.result_fingerprint = semanticWitnessResultFingerprint(result);
  if (!result.verify(error))
    return std::nullopt;
  return result;
}

bool bindNominalSemanticWitness(NominalTypeSpecificArtifact &specific,
                                NominalSemanticWitnessArtifact witness,
                                std::string &error) {
  if (!witness.verify(error) ||
      witness.nominal_template != specific.template_entity ||
      witness.arguments != specific.arguments ||
      witness.semantic_options_fingerprint !=
          specific.semantic_options_fingerprint ||
      witness.structural_specific_fingerprint !=
          specific.structural_fingerprint ||
      witness.kind != specific.kind) {
    if (error.empty())
      error = "nominal semantic witness does not bind to the nominal specific";
    return false;
  }
  specific.nominal_semantic_witness = std::move(witness);
  specific.result_fingerprint = specificResultFingerprint(specific);
  return specific.verify(error);
}

std::optional<NominalTypeLayoutArtifact>
buildNominalTypeLayout(const NominalTypeSpecificArtifact &specific,
                       const TargetLayoutConfig &target,
                       const TypeLayoutQuery &query_child_layout,
                       std::string &error) {
  error.clear();
  if (!specific.verify(error) || !target.verify(error))
    return std::nullopt;
  NominalTypeLayoutArtifact result;
  result.type_specific_fingerprint = specific.result_fingerprint;
  result.target_fingerprint = target.fingerprint();
  result.kind = specific.kind;
  result.alignment = 1;
  std::uint64_t cursor = 0;
  const auto concrete_layout = [&](const auto &self, const PublicType &type)
      -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    switch (type.kind) {
    case PublicTypeKind::Void:
      return std::pair<std::uint64_t, std::uint64_t>{0, 1};
    case PublicTypeKind::Bool:
      return std::pair<std::uint64_t, std::uint64_t>{1, 1};
    case PublicTypeKind::Char:
      return std::pair<std::uint64_t, std::uint64_t>{4, 4};
    case PublicTypeKind::Integer:
    case PublicTypeKind::Float: {
      const auto bytes = type.scalar_width / 8U;
      return std::pair<std::uint64_t, std::uint64_t>{bytes, bytes};
    }
    case PublicTypeKind::Reference:
    case PublicTypeKind::RawPointer:
    case PublicTypeKind::Function:
    case PublicTypeKind::CFunctionPointer:
      return std::pair<std::uint64_t, std::uint64_t>{target.pointer_width / 8U,
                                                     target.pointer_width / 8U};
    case PublicTypeKind::CallbackAdapter: {
      const auto pointer_bytes = target.pointer_width / 8U;
      return std::pair<std::uint64_t, std::uint64_t>{pointer_bytes * 3U,
                                                     pointer_bytes};
    }
    case PublicTypeKind::CallbackRegistration: {
      const auto pointer_bytes = target.pointer_width / 8U;
      const auto pointer_count = type.arguments.size() + 2U;
      return std::pair<std::uint64_t, std::uint64_t>{
          pointer_bytes * pointer_count, pointer_bytes};
    }
    case PublicTypeKind::CallbackCompletion: {
      const auto pointer_bytes = target.pointer_width / 8U;
      return std::pair<std::uint64_t, std::uint64_t>{
          pointer_bytes * (type.arguments.size() + 2U), pointer_bytes};
    }
    case PublicTypeKind::CallbackWake: {
      if (type.arguments.size() != 1)
        return std::nullopt;
      const auto completion = self(self, type.arguments.front());
      if (!completion)
        return std::nullopt;
      const auto size = ((completion->first + 1U + completion->second - 1U) /
                         completion->second) *
                        completion->second;
      return std::pair<std::uint64_t, std::uint64_t>{size, completion->second};
    }
    case PublicTypeKind::String: {
      const auto pointer_bytes = target.pointer_width / 8U;
      return std::pair<std::uint64_t, std::uint64_t>{
          pointer_bytes + 8U, std::max<std::uint64_t>(pointer_bytes, 8U)};
    }
    case PublicTypeKind::Array: {
      if (type.arguments.size() != 1 || type.array_bound == 0) {
        error = "array field has no concrete element layout";
        return std::nullopt;
      }
      const auto element = self(self, type.arguments.front());
      if (!element ||
          element->first >
              std::numeric_limits<std::uint64_t>::max() / type.array_bound) {
        error = "array field layout overflows the target address space";
        return std::nullopt;
      }
      return std::pair{element->first * type.array_bound, element->second};
    }
    case PublicTypeKind::Tuple: {
      std::uint64_t tuple_cursor = 0;
      std::uint64_t tuple_alignment = 1;
      for (const auto &element_type : type.arguments) {
        const auto element = self(self, element_type);
        std::uint64_t offset = 0;
        if (!element || !checkedAlign(tuple_cursor, element->second, offset) ||
            element->first >
                std::numeric_limits<std::uint64_t>::max() - offset) {
          error = "tuple field layout overflows the target address space";
          return std::nullopt;
        }
        tuple_cursor = offset + element->first;
        tuple_alignment = std::max(tuple_alignment, element->second);
      }
      std::uint64_t tuple_size = 0;
      if (!checkedAlign(tuple_cursor, tuple_alignment, tuple_size)) {
        error = "tuple field layout overflows the target address space";
        return std::nullopt;
      }
      return std::pair{tuple_size, tuple_alignment};
    }
    case PublicTypeKind::Slice: {
      const auto pointer_bytes = target.pointer_width / 8U;
      return std::pair<std::uint64_t, std::uint64_t>{pointer_bytes * 2U,
                                                     pointer_bytes};
    }
    case PublicTypeKind::Nominal:
      if (!query_child_layout) {
        error = "nominal layout requires a child layout query";
        return std::nullopt;
      }
      if (const auto layout = query_child_layout(type, error))
        return std::pair{layout->size, layout->alignment};
      return std::nullopt;
    default:
      error = "nominal field has no concrete object layout";
      return std::nullopt;
    }
  };
  if (specific.kind == NominalKind::Enum) {
    result.tag_size = 4;
    std::uint64_t max_payload_size = 0;
    std::uint64_t max_payload_alignment = 1;
    for (const auto &source_variant : specific.variants) {
      EnumVariantLayoutArtifact variant;
      variant.name = source_variant.name;
      std::uint64_t payload_cursor = 0;
      for (const auto &source_field : source_variant.fields) {
        const auto field_layout =
            concrete_layout(concrete_layout, source_field.type);
        std::uint64_t offset = 0;
        if (!field_layout ||
            !checkedAlign(payload_cursor, field_layout->second, offset) ||
            field_layout->first >
                std::numeric_limits<std::uint64_t>::max() - offset) {
          if (error.empty())
            error = "enum payload layout overflows the target address space";
          return std::nullopt;
        }
        variant.fields.push_back({source_field.name, source_field.type,
                                  ObjectFieldProjectionKind::StableAddress,
                                  std::nullopt, offset, field_layout->first,
                                  field_layout->second, 0, 0});
        payload_cursor = offset + field_layout->first;
        variant.alignment = std::max(variant.alignment, field_layout->second);
      }
      if (!checkedAlign(payload_cursor, variant.alignment, variant.size)) {
        error = "enum payload tail padding overflows";
        return std::nullopt;
      }
      max_payload_size = std::max(max_payload_size, variant.size);
      max_payload_alignment =
          std::max(max_payload_alignment, variant.alignment);
      result.variants.push_back(std::move(variant));
    }
    result.alignment = std::max<std::uint64_t>(4, max_payload_alignment);
    if (!checkedAlign(result.tag_size, result.alignment,
                      result.payload_offset) ||
        result.payload_offset >
            std::numeric_limits<std::uint64_t>::max() - max_payload_size ||
        !checkedAlign(result.payload_offset + max_payload_size,
                      result.alignment, result.size)) {
      error = "enum aggregate layout overflows the target address space";
      return std::nullopt;
    }
    result.request_fingerprint = layoutRequestFingerprint(result);
    result.result_fingerprint = layoutResultFingerprint(result);
    return result;
  }
  if (specific.object_repr_carrier) {
    if (!query_child_layout) {
      error = "custom object layout requires a carrier layout query";
      return std::nullopt;
    }
    const auto carrier_layout =
        query_child_layout(*specific.object_repr_carrier, error);
    if (!carrier_layout)
      return std::nullopt;
    result.size = carrier_layout->size;
    result.alignment = carrier_layout->alignment;
    if (specific.nominal_semantic_witness.object_field_projections.size() !=
        specific.fields.size()) {
      error = "custom object layout has incomplete field projections";
      return std::nullopt;
    }
    for (std::size_t logical_index = 0; logical_index < specific.fields.size();
         ++logical_index) {
      const auto &logical_field = specific.fields[logical_index];
      const auto &projection = specific.nominal_semantic_witness
                                   .object_field_projections[logical_index];
      if (projection.kind == ObjectFieldProjectionKind::Computed) {
        result.fields.push_back({logical_field.name, logical_field.type,
                                 ObjectFieldProjectionKind::Computed,
                                 std::nullopt, 0, 0, 1, 0, 0});
        continue;
      }
      auto current = *specific.object_repr_carrier;
      std::uint64_t offset = 0;
      for (const auto field_index : projection.field_indices) {
        if (current.kind != PublicTypeKind::Nominal) {
          error = "custom object layout projection crosses a non-nominal type";
          return std::nullopt;
        }
        const auto current_layout = query_child_layout(current, error);
        if (!current_layout || field_index >= current_layout->fields.size() ||
            current_layout->fields[field_index].kind !=
                ObjectFieldProjectionKind::StableAddress ||
            current_layout->fields[field_index].offset >
                std::numeric_limits<std::uint64_t>::max() - offset) {
          if (error.empty())
            error = "custom object layout projection is out of range";
          return std::nullopt;
        }
        offset += current_layout->fields[field_index].offset;
        current = current_layout->fields[field_index].type;
      }
      const auto storage_layout = concrete_layout(concrete_layout, current);
      if (!storage_layout)
        return std::nullopt;
      if (projection.kind == ObjectFieldProjectionKind::StableAddress) {
        if (current != logical_field.type) {
          error = "stable object projection changes the logical field type";
          return std::nullopt;
        }
        result.fields.push_back({logical_field.name, logical_field.type,
                                 ObjectFieldProjectionKind::StableAddress,
                                 std::nullopt, offset, storage_layout->first,
                                 storage_layout->second, 0, 0});
      } else {
        result.fields.push_back({logical_field.name, logical_field.type,
                                 ObjectFieldProjectionKind::BitPacked, current,
                                 offset, storage_layout->first,
                                 storage_layout->second, projection.bit_begin,
                                 projection.bit_end});
      }
    }
    result.request_fingerprint = layoutRequestFingerprint(result);
    result.result_fingerprint = layoutResultFingerprint(result);
    return result;
  }
  for (const auto &field : specific.fields) {
    const auto field_layout = concrete_layout(concrete_layout, field.type);
    if (!field_layout)
      return std::nullopt;
    const auto layout = *field_layout;
    std::uint64_t offset = 0;
    if (layout.second == 0 ||
        (specific.kind == NominalKind::Struct &&
         !checkedAlign(cursor, layout.second, offset)) ||
        layout.first > std::numeric_limits<std::uint64_t>::max() - offset) {
      error = "nominal layout overflows the target address space";
      return std::nullopt;
    }
    result.fields.push_back(
        {field.name, field.type, ObjectFieldProjectionKind::StableAddress,
         std::nullopt, offset, layout.first, layout.second, 0, 0});
    cursor = specific.kind == NominalKind::Union
                 ? std::max(cursor, layout.first)
                 : offset + layout.first;
    result.alignment = std::max(result.alignment, layout.second);
  }
  if (!checkedAlign(cursor, result.alignment, result.size)) {
    error = "nominal layout tail padding overflows";
    return std::nullopt;
  }
  result.request_fingerprint = layoutRequestFingerprint(result);
  result.result_fingerprint = layoutResultFingerprint(result);
  return result;
}

} // namespace chtholly::compiler
