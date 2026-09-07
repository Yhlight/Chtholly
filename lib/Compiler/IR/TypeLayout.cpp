#include "chtholly/Compiler/TypeLayout.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace chtholly::compiler {
namespace {

bool checkedAlign(std::uint64_t value, std::uint64_t alignment,
                  std::uint64_t &result) {
  if (alignment == 0 || (alignment & (alignment - 1U)) != 0)
    return false;
  const auto remainder = value % alignment;
  const auto padding = remainder == 0 ? 0 : alignment - remainder;
  if (value > std::numeric_limits<std::uint64_t>::max() - padding)
    return false;
  result = value + padding;
  return true;
}

class Query {
public:
  Query(const SemIR &sem_ir, const TargetLayoutConfig &target,
        std::string &error)
      : sem_ir_(sem_ir), target_(target), error_(error) {}

  std::optional<SemanticTypeLayout> run(TypeId type) {
    if (!target_.verify(error_))
      return std::nullopt;
    return layout(type);
  }

private:
  std::optional<SemanticTypeLayout> layout(TypeId type) {
    if (!type.hasValue() || type.index >= sem_ir_.typeCount()) {
      error_ = "layout query has an invalid semantic type";
      return std::nullopt;
    }
    const auto value = sem_ir_.type(type);
    const auto pointer = static_cast<std::uint64_t>(target_.pointer_width / 8U);
    switch (value.kind) {
    case SemTypeKind::Bool:
      return SemanticTypeLayout{1, 1};
    case SemTypeKind::Char:
      return SemanticTypeLayout{4, 4};
    case SemTypeKind::Integer:
    case SemTypeKind::Float: {
      const auto bytes = static_cast<std::uint64_t>(value.arg0 / 8U);
      if (bytes == 0 || (bytes & (bytes - 1U)) != 0) {
        error_ = "scalar type has no target object layout";
        return std::nullopt;
      }
      return SemanticTypeLayout{bytes, bytes};
    }
    case SemTypeKind::String:
      return SemanticTypeLayout{pointer + 8U,
                                std::max(pointer, std::uint64_t{8})};
    case SemTypeKind::RawPointer:
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
      return SemanticTypeLayout{pointer, pointer};
    case SemTypeKind::CallbackAdapter:
      return SemanticTypeLayout{pointer * 3U, pointer};
    case SemTypeKind::Array: {
      const auto element = layout(TypeId(value.arg0));
      if (!element || value.arg1 == 0 ||
          element->size >
              std::numeric_limits<std::uint64_t>::max() / value.arg1) {
        if (error_.empty())
          error_ = "array layout is incomplete or overflows";
        return std::nullopt;
      }
      return SemanticTypeLayout{element->size * value.arg1, element->alignment};
    }
    case SemTypeKind::Slice:
      return SemanticTypeLayout{pointer + pointer,
                                std::max(pointer, std::uint64_t{8})};
    case SemTypeKind::Tuple: {
      std::uint64_t cursor = 0;
      std::uint64_t alignment = 1;
      for (const auto element :
           sem_ir_.typeBlock(TypeBlockId(value.arg0))) {
        const auto child = layout(element);
        if (!child)
          return std::nullopt;
        std::uint64_t offset = 0;
        alignment = std::max(alignment, child->alignment);
        if (!checkedAlign(cursor, child->alignment, offset) ||
            child->size > std::numeric_limits<std::uint64_t>::max() - offset ||
            !checkedAlign(offset + child->size, alignment, cursor)) {
          error_ = "tuple layout overflows the target address space";
          return std::nullopt;
        }
      }
      std::uint64_t size = 0;
      if (!checkedAlign(cursor, alignment, size)) {
        error_ = "tuple layout tail padding overflows the target address space";
        return std::nullopt;
      }
      return SemanticTypeLayout{size, alignment};
    }
    case SemTypeKind::Nominal:
      return nominal(type, NominalTypeId(value.arg0));
    case SemTypeKind::Reference:
      error_ = "references do not expose an owned object layout";
      return std::nullopt;
    case SemTypeKind::TypeParameter:
      error_ = "dependent type layout is not concrete";
      return std::nullopt;
    default:
      error_ = "type has no queryable object representation";
      return std::nullopt;
    }
  }

  std::optional<SemanticTypeLayout> nominal(TypeId type, NominalTypeId id) {
    if (!active_.insert(id.index).second) {
      error_ = "nominal layout contains a by-value cycle";
      return std::nullopt;
    }
    const auto &nominal = sem_ir_.nominalType(id);
    if (nominal.completion_state != SemNominalCompletionState::Complete) {
      error_ = "nominal type is incomplete";
      active_.erase(id.index);
      return std::nullopt;
    }
    if (nominal.canonical_entity.hasValue() &&
        nominal.representation_policy == NominalRepresentationPolicy::Opaque) {
      error_ = "imported opaque type does not publish its layout";
      active_.erase(id.index);
      return std::nullopt;
    }
    const auto object = sem_ir_.objectRepresentationType(type);
    if (object.hasValue() && object != type) {
      const auto result = layout(object);
      active_.erase(id.index);
      return result;
    }

    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    if (nominal.kind == NominalKind::Enum) {
      std::uint64_t max_payload = 0;
      std::uint64_t payload_alignment = 1;
      for (const auto &variant : nominal.variants) {
        const auto payload = aggregate(variant.fields, false);
        if (!payload) {
          active_.erase(id.index);
          return std::nullopt;
        }
        max_payload = std::max(max_payload, payload->size);
        payload_alignment = std::max(payload_alignment, payload->alignment);
      }
      alignment = std::max<std::uint64_t>(4, payload_alignment);
      std::uint64_t payload_offset = 0;
      if (!checkedAlign(4, alignment, payload_offset) ||
          max_payload >
              std::numeric_limits<std::uint64_t>::max() - payload_offset ||
          !checkedAlign(payload_offset + max_payload, alignment, size)) {
        error_ = "enum layout overflows the target address space";
        active_.erase(id.index);
        return std::nullopt;
      }
    } else {
      const auto result =
          aggregate(nominal.fields, nominal.kind == NominalKind::Union);
      if (!result) {
        active_.erase(id.index);
        return std::nullopt;
      }
      size = result->size;
      alignment = result->alignment;
    }
    active_.erase(id.index);
    return SemanticTypeLayout{size, alignment};
  }

  std::optional<SemanticTypeLayout>
  aggregate(std::span<const SemNominalField> fields, bool is_union) {
    if (is_union && fields.empty()) {
      error_ = "empty union has no object layout";
      return std::nullopt;
    }
    std::uint64_t cursor = 0;
    std::uint64_t alignment = 1;
    for (const auto &field : fields) {
      if (field.projection_kind == PublicObjectProjectionKind::Computed)
        continue;
      auto field_type = field.type;
      const auto child = layout(field_type);
      if (!child)
        return std::nullopt;
      alignment = std::max(alignment, child->alignment);
      if (is_union) {
        cursor = std::max(cursor, child->size);
        continue;
      }
      std::uint64_t offset = 0;
      if (!checkedAlign(cursor, child->alignment, offset) ||
          child->size > std::numeric_limits<std::uint64_t>::max() - offset) {
        error_ = "aggregate layout overflows the target address space";
        return std::nullopt;
      }
      cursor = offset + child->size;
    }
    std::uint64_t size = 0;
    if (!checkedAlign(cursor, alignment, size)) {
      error_ = "aggregate tail padding overflows the target address space";
      return std::nullopt;
    }
    return SemanticTypeLayout{size, alignment};
  }

  const SemIR &sem_ir_;
  const TargetLayoutConfig &target_;
  std::string &error_;
  std::unordered_set<std::uint32_t> active_;
};

} // namespace

std::optional<SemanticTypeLayout>
querySemanticTypeLayout(const SemIR &sem_ir, TypeId type,
                        const TargetLayoutConfig &target, std::string &error) {
  error.clear();
  return Query(sem_ir, target, error).run(type);
}

} // namespace chtholly::compiler
