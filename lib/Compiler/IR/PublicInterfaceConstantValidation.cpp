#include "PublicInterfaceServices.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace chtholly::compiler::internal {
namespace {

std::optional<PublicType> substituteConstantType(
    const PublicType &type, std::span<const PublicType> arguments) {
  if (type.kind == PublicTypeKind::TypeParameter) {
    if (type.binding_index >= arguments.size())
      return std::nullopt;
    return arguments[type.binding_index];
  }
  PublicType result = type;
  for (auto &argument : result.arguments) {
    auto substituted = substituteConstantType(argument, arguments);
    if (!substituted)
      return std::nullopt;
    argument = std::move(*substituted);
  }
  return result;
}

} // namespace

bool PublicInterfaceConstantValidationService::validValue(
    const PublicConstantValue &value, std::uint32_t depth) {
  if (depth >= 128 || value.kind >= PublicConstantValueKind::Count ||
      !PublicInterfaceTypeValidationService::validPublicType(value.type, 0,
                                                              true) ||
      value.elements.size() > std::numeric_limits<std::uint32_t>::max())
    return false;
  const auto scalar = value.kind == PublicConstantValueKind::Integer ||
                      value.kind == PublicConstantValueKind::Float ||
                      value.kind == PublicConstantValueKind::Bool ||
                      value.kind == PublicConstantValueKind::String ||
                      value.kind == PublicConstantValueKind::Null ||
                      value.kind == PublicConstantValueKind::ForeignEnum;
  if (scalar && !value.elements.empty())
    return false;
  if (value.kind != PublicConstantValueKind::String &&
      !value.string_payload.empty())
    return false;
  const auto elements_match = [&](const PublicType &type) {
    return std::ranges::all_of(value.elements, [&](const auto &element) {
      return element.type == type;
    });
  };
  switch (value.kind) {
  case PublicConstantValueKind::Integer:
    if (value.type.kind != PublicTypeKind::Integer &&
        value.type.kind != PublicTypeKind::Char)
      return false;
    break;
  case PublicConstantValueKind::Float:
    if (value.type.kind != PublicTypeKind::Float)
      return false;
    break;
  case PublicConstantValueKind::Bool:
    if (value.type.kind != PublicTypeKind::Bool || value.payload > 1)
      return false;
    break;
  case PublicConstantValueKind::String:
    if (value.type.kind != PublicTypeKind::String || value.payload != 0)
      return false;
    break;
  case PublicConstantValueKind::Null:
    if (value.type.kind != PublicTypeKind::RawPointer || value.payload != 0)
      return false;
    break;
  case PublicConstantValueKind::Array:
    if (value.type.kind != PublicTypeKind::Array ||
        value.type.arguments.size() != 1 || value.payload != 0 ||
        value.elements.size() != value.type.array_bound ||
        !elements_match(value.type.arguments.front()))
      return false;
    break;
  case PublicConstantValueKind::Tuple:
    if (value.type.kind != PublicTypeKind::Tuple || value.payload != 0 ||
        value.elements.size() != value.type.arguments.size())
      return false;
    for (std::size_t index = 0; index < value.elements.size(); ++index)
      if (value.elements[index].type != value.type.arguments[index])
        return false;
    break;
  case PublicConstantValueKind::Aggregate:
    if (value.type.kind != PublicTypeKind::Nominal || value.payload != 0)
      return false;
    break;
  case PublicConstantValueKind::Union:
    if (value.type.kind != PublicTypeKind::Nominal ||
        value.elements.size() != 1)
      return false;
    break;
  case PublicConstantValueKind::Enum:
  case PublicConstantValueKind::ForeignEnum:
    if (value.type.kind != PublicTypeKind::Nominal)
      return false;
    break;
  case PublicConstantValueKind::Count:
    return false;
  }
  return std::ranges::all_of(value.elements, [&](const auto &element) {
    return validValue(element, depth + 1);
  });
}

bool PublicInterfaceConstantValidationService::validShape(
    const PublicConstantValue &value,
    std::span<const PublicNominalTypeArtifact> nominal_types) {
  if (!validValue(value))
    return false;
  if (value.kind != PublicConstantValueKind::Aggregate &&
      value.kind != PublicConstantValueKind::Union &&
      value.kind != PublicConstantValueKind::Enum)
    return true;
  const auto nominal = std::ranges::find_if(
      nominal_types, [&](const PublicNominalTypeArtifact &candidate) {
        return candidate.entity == value.type.nominal_entity;
      });
  if (nominal == nominal_types.end() ||
      value.type.arguments.size() != nominal->generic_parameter_count)
    return false;
  const auto fields_match =
      [&](std::span<const PublicNominalFieldArtifact> fields) {
        if (value.elements.size() != fields.size())
          return false;
        for (std::size_t index = 0; index < fields.size(); ++index) {
          const auto field_type =
              substituteConstantType(fields[index].type, value.type.arguments);
          if (!field_type || value.elements[index].type != *field_type)
            return false;
        }
        return true;
      };
  if (value.kind == PublicConstantValueKind::Aggregate) {
    if (nominal->kind != NominalKind::Struct || !fields_match(nominal->fields))
      return false;
  } else if (value.kind == PublicConstantValueKind::Union) {
    if (nominal->kind != NominalKind::Union ||
        value.payload >= nominal->fields.size())
      return false;
    const auto field_type = substituteConstantType(
        nominal->fields[static_cast<std::size_t>(value.payload)].type,
        value.type.arguments);
    if (!field_type || value.elements.size() != 1 ||
        value.elements.front().type != *field_type)
      return false;
  } else {
    if (nominal->kind != NominalKind::Enum ||
        value.payload >= nominal->variants.size() ||
        !fields_match(
            nominal->variants[static_cast<std::size_t>(value.payload)].fields))
      return false;
  }
  return std::ranges::all_of(value.elements, [&](const auto &element) {
    return validShape(element, nominal_types);
  });
}

} // namespace chtholly::compiler::internal
