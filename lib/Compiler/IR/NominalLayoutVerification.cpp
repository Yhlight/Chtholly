#include "NominalLayoutVerificationInternal.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {
namespace {
bool checkedAlign(std::uint64_t value, std::uint64_t alignment,
                  std::uint64_t &result) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0)
    return false;
  const auto mask = alignment - 1;
  if (value > std::numeric_limits<std::uint64_t>::max() - mask)
    return false;
  result = (value + mask) & ~mask;
  return true;
}
} // namespace

bool NominalLayoutVerificationService::verify(
    NominalLayoutVerificationState &state, std::string &error) {
  const auto &layout = state.layout;
  error.clear();
  if (!layout.type_specific_fingerprint.hasValue() ||
      !layout.target_fingerprint.hasValue() ||
      layout.request_fingerprint != state.request_fingerprint(layout) ||
      layout.result_fingerprint != state.result_fingerprint(layout) ||
      layout.alignment == 0 || (layout.alignment & (layout.alignment - 1)) != 0 ||
      layout.kind >= NominalKind::Count) {
    error = "nominal layout artifact has invalid metadata";
    return false;
  }
  if (layout.kind == NominalKind::Enum) {
    if (!layout.fields.empty() || layout.variants.empty() || layout.tag_size != 4 ||
        layout.payload_offset < layout.tag_size ||
        layout.payload_offset % layout.alignment != 0 ||
        layout.size % layout.alignment != 0 || layout.size < layout.payload_offset) {
      error = "enum layout artifact has invalid tag or payload metadata";
      return false;
    }
    std::unordered_set<std::string> variant_names;
    std::uint64_t max_payload_size = 0;
    std::uint64_t max_payload_alignment = 1;
    for (const auto &variant : layout.variants) {
      if (variant.name.empty() || !variant_names.insert(variant.name).second ||
          variant.alignment == 0 || (variant.alignment & (variant.alignment - 1)) != 0 ||
          variant.size % variant.alignment != 0) {
        error = "enum layout artifact has an invalid variant";
        return false;
      }
      std::unordered_set<std::string> payload_names;
      for (const auto &field : variant.fields) {
        if (field.name.empty() || !payload_names.insert(field.name).second ||
            !verifyPublicType(field.type, 0, true, error) ||
            field.kind != ObjectFieldProjectionKind::StableAddress ||
            field.storage_type || field.alignment == 0 ||
            (field.alignment & (field.alignment - 1)) != 0 ||
            field.offset % field.alignment != 0 || field.offset > variant.size ||
            field.size > variant.size - field.offset || field.bit_begin != 0 ||
            field.bit_end != 0) {
          error = "enum layout artifact has an invalid payload field";
          return false;
        }
      }
      max_payload_size = std::max(max_payload_size, variant.size);
      max_payload_alignment = std::max(max_payload_alignment, variant.alignment);
    }
    std::uint64_t expected_offset = 0, expected_size = 0;
    const auto expected_alignment = std::max<std::uint64_t>(4, max_payload_alignment);
    if (!checkedAlign(4, expected_alignment, expected_offset) ||
        !checkedAlign(expected_offset + max_payload_size, expected_alignment,
                      expected_size) ||
        layout.payload_offset != expected_offset ||
        layout.alignment != expected_alignment || layout.size != expected_size) {
      error = "enum layout artifact has inconsistent aggregate size";
      return false;
    }
    return true;
  }
  if (layout.tag_size != 0 || layout.payload_offset != 0 || !layout.variants.empty()) {
    error = "non-enum layout artifact contains enum metadata";
    return false;
  }
  std::uint64_t expected_alignment = 1;
  std::uint64_t union_member_size = 0;
  std::unordered_set<std::string> names;
  for (const auto &field : layout.fields) {
    if (field.name.empty() || !names.insert(field.name).second ||
        !verifyPublicType(field.type, 0, false, error) ||
        field.kind >= ObjectFieldProjectionKind::Count) {
      error = "nominal layout artifact has an invalid field layout";
      return false;
    }
    if (field.kind == ObjectFieldProjectionKind::Computed) {
      if (field.storage_type || field.offset != 0 || field.size != 0 ||
          field.alignment != 1 || field.bit_begin != 0 || field.bit_end != 0) {
        error = "opaque logical field fabricates a physical layout";
        return false;
      }
      continue;
    }
    if (layout.kind == NominalKind::Union &&
        (field.kind != ObjectFieldProjectionKind::StableAddress || field.offset != 0)) {
      error = "nominal union layout requires stable fields at offset zero";
      return false;
    }
    if (field.alignment == 0 || (field.alignment & (field.alignment - 1)) != 0 ||
        field.offset % field.alignment != 0 || field.offset > layout.size ||
        field.size > layout.size - field.offset) {
      error = "nominal layout artifact has an invalid physical field layout";
      return false;
    }
    if (field.kind == ObjectFieldProjectionKind::StableAddress) {
      if (field.storage_type || field.bit_begin != 0 || field.bit_end != 0) {
        error = "stable field layout has bit-packed storage metadata";
        return false;
      }
    } else if (!field.storage_type ||
               !verifyPublicType(*field.storage_type, 0, false, error) ||
               field.bit_begin >= field.bit_end || field.bit_end > field.size * 8U) {
      error = "bit-packed field layout has invalid carrier metadata";
      return false;
    }
    expected_alignment = std::max(expected_alignment, field.alignment);
    union_member_size = std::max(union_member_size, field.size);
  }
  for (std::size_t lhs = 0; lhs < layout.fields.size(); ++lhs)
    for (std::size_t rhs = lhs + 1; rhs < layout.fields.size(); ++rhs) {
      const auto &left = layout.fields[lhs];
      const auto &right = layout.fields[rhs];
      if (left.kind == ObjectFieldProjectionKind::Computed ||
          right.kind == ObjectFieldProjectionKind::Computed)
        continue;
      const auto byte_overlap = left.size != 0 && right.size != 0 &&
                                left.offset < right.offset + right.size &&
                                right.offset < left.offset + left.size;
      if (!byte_overlap)
        continue;
      if (layout.kind == NominalKind::Union && left.offset == 0 && right.offset == 0)
        continue;
      const auto shared_bit_word =
          left.kind == ObjectFieldProjectionKind::BitPacked &&
          right.kind == ObjectFieldProjectionKind::BitPacked &&
          left.offset == right.offset && left.size == right.size &&
          left.storage_type == right.storage_type;
      if (!shared_bit_word ||
          (left.bit_begin < right.bit_end && right.bit_begin < left.bit_end)) {
        error = "nominal layout artifact has overlapping logical fields";
        return false;
      }
    }
  if (layout.kind == NominalKind::Union) {
    std::uint64_t expected_size = 0;
    if (layout.fields.empty() ||
        !checkedAlign(union_member_size, expected_alignment, expected_size) ||
        layout.alignment != expected_alignment || layout.size != expected_size) {
      error = "nominal union layout has invalid size or alignment";
      return false;
    }
  } else if (layout.alignment < expected_alignment || layout.size % layout.alignment != 0) {
    error = "nominal layout artifact has invalid tail padding";
    return false;
  }
  return true;
}

} // namespace chtholly::compiler::internal
