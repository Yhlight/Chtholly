#include "LLVMInternal.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

std::optional<LLVMDebugFieldInfo>
debugFieldInfo(const SemNominalField &field,
               const LowNominalFieldLayout &layout) {
  constexpr auto bits = std::numeric_limits<std::uint64_t>::max();
  if (field.projection_kind == PublicObjectProjectionKind::Computed)
    return std::nullopt;
  if (field.projection_kind == PublicObjectProjectionKind::BitPacked) {
    if (field.bit_begin >= field.bit_end ||
        layout.size == 0 || layout.alignment == 0 ||
        layout.offset > bits / 8U)
      return std::nullopt;
    return LLVMDebugFieldInfo{
        .bit_field = true,
        .size_bits = static_cast<std::uint64_t>(field.bit_end -
                                                 field.bit_begin),
        .offset_bits = field.bit_begin,
        .storage_offset_bits = layout.offset * 8U};
  }
  if (layout.size == 0 || layout.alignment == 0 ||
      layout.size > bits / 8U || layout.offset > bits / 8U)
    return std::nullopt;
  return LLVMDebugFieldInfo{.bit_field = false,
                            .size_bits = layout.size * 8U,
                            .offset_bits = layout.offset * 8U,
                            .storage_offset_bits = 0};
}

namespace {

constexpr unsigned DebugLine = 1;

std::string typeName(const SemIR &sem_ir, TypeId id) {
  const auto &type = sem_ir.type(id);
  if (type.kind != SemTypeKind::Nominal)
    return "chtholly.type." + std::to_string(id.index);
  const auto &nominal = sem_ir.nominalType(NominalTypeId(type.arg0));
  return std::string(sem_ir.identifier(sem_ir.name(nominal.name).text));
}

std::string fieldName(const SemIR &sem_ir, const SemNominalField &field) {
  return std::string(sem_ir.identifier(sem_ir.name(field.name).text));
}

std::optional<SemanticTypeLayout> objectLayout(TypeId id,
                                                LLVMDebugTypeState &state) {
  std::string error;
  return querySemanticTypeLayout(state.sem_ir, id,
                                 state.sem_ir.targetLayout(), error);
}

llvm::DIType *unspecified(TypeId id, LLVMDebugTypeState &state) {
  return state.builder.createUnspecifiedType(
      "chtholly.type." + std::to_string(id.index));
}

llvm::DINodeArray makeElements(llvm::DIBuilder &builder,
                               std::vector<llvm::Metadata *> &elements) {
  return builder.getOrCreateArray(elements);
}

llvm::DICompositeType *lowerFieldComposite(
    std::string_view name, llvm::dwarf::Tag tag,
    std::span<const SemNominalField> fields,
    std::span<const LowNominalFieldLayout> field_layouts,
    std::uint64_t size, std::uint64_t alignment, LLVMDebugTypeState &state) {
  std::vector<llvm::Metadata *> elements;
  elements.reserve(fields.size());
  std::uint64_t fallback_offset = 0;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto field_type = state.recursive(fields[index].type);
    const auto layout = objectLayout(fields[index].type, state);
    const auto field_size = index < field_layouts.size()
                                ? field_layouts[index].size
                                : layout ? layout->size : 0;
    const auto field_alignment = index < field_layouts.size()
                                     ? field_layouts[index].alignment
                                     : layout ? layout->alignment : 1;
    const auto field_info = debugFieldInfo(
        fields[index], index < field_layouts.size()
                           ? field_layouts[index]
                           : LowNominalFieldLayout{fallback_offset, field_size,
                                                   field_alignment});
    if (!field_info)
      continue;
    if (field_info->bit_field) {
      elements.push_back(state.builder.createBitFieldMemberType(
          nullptr, fieldName(state.sem_ir, fields[index]), state.file,
          DebugLine, field_info->size_bits, field_info->offset_bits,
          field_info->storage_offset_bits, llvm::DINode::FlagZero, field_type));
      continue;
    }
    const auto field_offset = field_info->offset_bits / 8U;
    fallback_offset = field_offset + field_size;
    elements.push_back(state.builder.createMemberType(
        nullptr, fieldName(state.sem_ir, fields[index]), state.file, DebugLine,
        field_info->size_bits, static_cast<std::uint32_t>(field_alignment * 8),
        field_info->offset_bits, llvm::DINode::FlagZero, field_type));
  }
  const auto element_array = makeElements(state.builder, elements);
  if (tag == llvm::dwarf::DW_TAG_union_type)
    return state.builder.createUnionType(
        nullptr, name, state.file, DebugLine, size * 8,
        static_cast<std::uint32_t>(alignment * 8), llvm::DINode::FlagZero,
        element_array);
  return state.builder.createStructType(
      nullptr, name, state.file, DebugLine, size * 8,
      static_cast<std::uint32_t>(alignment * 8), llvm::DINode::FlagZero,
      nullptr, element_array);
}

llvm::DIType *lowerTuple(TypeId id, LLVMDebugTypeState &state) {
  const auto &type = state.sem_ir.type(id);
  const auto elements = state.sem_ir.typeBlock(TypeBlockId(type.arg0));
  const auto layout = objectLayout(id, state);
  if (!layout)
    return unspecified(id, state);

  std::vector<llvm::Metadata *> members;
  members.reserve(elements.size());
  std::uint64_t cursor = 0;
  std::uint64_t alignment = 1;
  for (std::size_t index = 0; index < elements.size(); ++index) {
    const auto child_layout = objectLayout(elements[index], state);
    if (!child_layout)
      return unspecified(id, state);
    alignment = std::max(alignment, child_layout->alignment);
    const auto remainder = cursor % child_layout->alignment;
    const auto offset = remainder == 0
                            ? cursor
                            : cursor + child_layout->alignment - remainder;
    cursor = offset + child_layout->size;
    members.push_back(state.builder.createMemberType(
        nullptr, "[" + std::to_string(index) + "]", state.file, DebugLine,
        child_layout->size * 8,
        static_cast<std::uint32_t>(child_layout->alignment * 8), offset * 8,
        llvm::DINode::FlagZero, state.recursive(elements[index])));
  }
  return state.builder.createStructType(
      nullptr, "tuple", state.file, DebugLine, layout->size * 8,
      static_cast<std::uint32_t>(alignment * 8), llvm::DINode::FlagZero,
      nullptr, makeElements(state.builder, members));
}

llvm::DIType *lowerNominal(TypeId id, LLVMDebugTypeState &state) {
  const auto &type = state.sem_ir.type(id);
  const auto nominal_id = NominalTypeId(type.arg0);
  const auto &nominal = state.sem_ir.nominalType(nominal_id);
  const auto *layout = state.low_ir.nominalLayout(id);
  if (!layout || nominal.completion_state != SemNominalCompletionState::Complete ||
      (nominal.canonical_entity.hasValue() &&
       nominal.representation_policy == NominalRepresentationPolicy::Opaque))
    return unspecified(id, state);

  const auto name = typeName(state.sem_ir, id);
  // Create the identity-bearing composite before descending into fields. The
  // empty element list is filled below; recursive pointer fields therefore
  // resolve to this same metadata node without leaving a temporary forward
  // declaration in the finished module.
  llvm::DICompositeType *placeholder = nullptr;
  if (nominal.kind == NominalKind::Union) {
    placeholder = state.builder.createUnionType(
        nullptr, name, state.file, DebugLine, layout->size * 8,
        static_cast<std::uint32_t>(layout->alignment * 8),
        llvm::DINode::FlagZero, llvm::DINodeArray());
  } else {
    placeholder = state.builder.createStructType(
        nullptr, name, state.file, DebugLine, layout->size * 8,
        static_cast<std::uint32_t>(layout->alignment * 8),
        llvm::DINode::FlagZero, nullptr, llvm::DINodeArray(), 0, nullptr,
        name);
  }
  state.types.emplace(id.index, placeholder);

  std::vector<llvm::Metadata *> members;
  if (nominal.kind == NominalKind::Enum) {
    members.reserve(nominal.variants.size());
    for (std::size_t variant = 0; variant < nominal.variants.size(); ++variant) {
      const auto &source = nominal.variants[variant];
      const auto variant_layout = variant < layout->variants.size()
                                      ? layout->variants[variant]
                                      : LowNominalVariantLayout{};
      llvm::DIType *variant_type = unspecified(id, state);
      if (!source.fields.empty())
        variant_type = lowerFieldComposite(
            state.sem_ir.identifier(state.sem_ir.name(source.name).text),
            llvm::dwarf::DW_TAG_structure_type, source.fields,
            variant_layout.fields, variant_layout.size,
            variant_layout.alignment, state);
      members.push_back(state.builder.createMemberType(
          nullptr, state.sem_ir.identifier(state.sem_ir.name(source.name).text),
          state.file, DebugLine, variant_layout.size * 8,
          static_cast<std::uint32_t>(variant_layout.alignment * 8),
          layout->payload_offset * 8, llvm::DINode::FlagZero, variant_type));
    }
  } else {
    members.reserve(nominal.fields.size());
    for (std::size_t index = 0; index < nominal.fields.size(); ++index) {
      const auto field_layout = index < layout->fields.size()
                                    ? layout->fields[index]
                                    : LowNominalFieldLayout{};
      const auto &field = nominal.fields[index];
      const auto field_info = debugFieldInfo(field, field_layout);
      if (!field_info)
        continue;
      if (field_info->bit_field) {
        members.push_back(state.builder.createBitFieldMemberType(
            nullptr, fieldName(state.sem_ir, field), state.file, DebugLine,
            field_info->size_bits, field_info->offset_bits,
            field_info->storage_offset_bits, llvm::DINode::FlagZero,
            state.recursive(field.type)));
        continue;
      }
      members.push_back(state.builder.createMemberType(
          nullptr, fieldName(state.sem_ir, field), state.file, DebugLine,
          field_info->size_bits,
          static_cast<std::uint32_t>(field_layout.alignment * 8),
          field_info->offset_bits, llvm::DINode::FlagZero,
          state.recursive(field.type)));
    }
  }
  state.builder.replaceArrays(placeholder, makeElements(state.builder, members));
  return placeholder;
}

} // namespace

llvm::DIType *LLVMDebugTypeLoweringService::lower(
    TypeId id, LLVMDebugTypeState &state) {
  if (const auto found = state.types.find(id.index);
      found != state.types.end())
    return found->second;
  const auto &type = state.sem_ir.type(id);
  llvm::DIType *result = nullptr;
  switch (type.kind) {
  case SemTypeKind::Bool:
    result = state.builder.createBasicType("bool", 8,
                                           llvm::dwarf::DW_ATE_boolean);
    break;
  case SemTypeKind::Char:
    result = state.builder.createBasicType("char", 32,
                                           llvm::dwarf::DW_ATE_UTF);
    break;
  case SemTypeKind::Integer:
    result = state.builder.createBasicType(
        (type.arg1 != 0 ? "i" : "u") + std::to_string(type.arg0), type.arg0,
        type.arg1 != 0 ? llvm::dwarf::DW_ATE_signed
                       : llvm::dwarf::DW_ATE_unsigned);
    break;
  case SemTypeKind::Float:
    result = state.builder.createBasicType(
        "f" + std::to_string(type.arg0), type.arg0, llvm::dwarf::DW_ATE_float);
    break;
  case SemTypeKind::Reference:
  case SemTypeKind::RawPointer:
    result = state.builder.createPointerType(
        state.recursive(TypeId(type.arg0)),
        state.module.getDataLayout().getPointerSizeInBits());
    break;
  case SemTypeKind::Array: {
    const auto layout = objectLayout(id, state);
    if (!layout) {
      result = unspecified(id, state);
      break;
    }
    auto *subscript = state.builder.getOrCreateSubrange(
        0, static_cast<std::int64_t>(type.arg1));
    result = state.builder.createArrayType(
        layout->size * 8, static_cast<std::uint32_t>(layout->alignment * 8),
        state.recursive(TypeId(type.arg0)),
        state.builder.getOrCreateArray({subscript}));
    break;
  }
  case SemTypeKind::Tuple:
    result = lowerTuple(id, state);
    break;
  case SemTypeKind::Nominal:
    result = lowerNominal(id, state);
    break;
  default:
    result = unspecified(id, state);
    break;
  }
  state.types.emplace(id.index, result);
  return result;
}

} // namespace chtholly::compiler
