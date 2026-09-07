#include "PublicInterfaceServices.h"

#include "PublicInterfaceEncodingInternal.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace chtholly::compiler::internal {

namespace {

void appendInterfaceConstraint(
    std::string &out, const PublicInterfaceConstraintArtifact &constraint) {
  appendType(out, constraint.subject);
  appendEntityReference(out, constraint.interface_entity);
  appendU32(out, static_cast<std::uint32_t>(constraint.arguments.size()));
  for (const auto &argument : constraint.arguments)
    appendType(out, argument);
}

} // namespace

bool publicTypesMayOverlap(const PublicType &lhs, const PublicType &rhs) {
  if (lhs.kind == PublicTypeKind::TypeParameter ||
      rhs.kind == PublicTypeKind::TypeParameter ||
      lhs.kind == PublicTypeKind::TypeProjection ||
      rhs.kind == PublicTypeKind::TypeProjection)
    return true;
  if (lhs.kind != rhs.kind)
    return false;
  if (lhs.kind == PublicTypeKind::Nominal &&
      lhs.nominal_entity != rhs.nominal_entity)
    return false;
  if (lhs.arguments.size() != rhs.arguments.size())
    return false;
  if (lhs.arguments.empty())
    return lhs == rhs;
  for (std::size_t index = 0; index < lhs.arguments.size(); ++index)
    if (!publicTypesMayOverlap(lhs.arguments[index], rhs.arguments[index]))
      return false;
  return true;
}

bool interfaceWitnessesMayOverlap(const PublicInterfaceWitnessArtifact &lhs,
                                  const PublicInterfaceWitnessArtifact &rhs) {
  if (lhs.interface_entity != rhs.interface_entity ||
      lhs.interface_arguments.size() != rhs.interface_arguments.size() ||
      !publicTypesMayOverlap(lhs.self_type, rhs.self_type))
    return false;
  for (std::size_t index = 0; index < lhs.interface_arguments.size(); ++index)
    if (!publicTypesMayOverlap(lhs.interface_arguments[index],
                               rhs.interface_arguments[index]))
      return false;
  return true;
}

StableFingerprint interfaceDeclarationFingerprint(
    const PublicInterfaceDeclarationArtifact &declaration) {
  std::string input;
  appendField(input, "chtholly.next.public-interface-declaration.v1");
  appendField(input, declaration.entity.canonical_package);
  appendField(input, declaration.entity.canonical_module);
  appendField(input, declaration.entity.canonical_name);
  appendU32(input, declaration.generic_parameter_count);
  appendU32(input, declaration.explicit_parameter_count);
  appendU32(input, static_cast<std::uint32_t>(declaration.constraints.size()));
  for (const auto &constraint : declaration.constraints)
    appendInterfaceConstraint(input, constraint);
  appendU32(input, static_cast<std::uint32_t>(declaration.requirements.size()));
  for (const auto &requirement : declaration.requirements) {
    appendU32(input, static_cast<std::uint32_t>(requirement.kind));
    appendField(input, requirement.name);
    if (requirement.kind == PublicInterfaceRequirementKind::Function)
      appendEntityReference(input, requirement.function);
    else
      appendType(input, requirement.associated_type);
    appendU32(input, requirement.binding_index);
    appendU32(input, requirement.has_default ? 1U : 0U);
  }
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint typeAliasFingerprint(const PublicTypeAliasArtifact &alias) {
  std::string input;
  appendField(input, "chtholly.next.public-type-alias.v1");
  appendField(input, alias.entity.canonical_package);
  appendField(input, alias.entity.canonical_module);
  appendField(input, alias.entity.canonical_name);
  appendU32(input, alias.generic_parameter_count);
  appendType(input, alias.target);
  appendU32(input, static_cast<std::uint32_t>(alias.constraints.size()));
  for (const auto &constraint : alias.constraints)
    appendInterfaceConstraint(input, constraint);
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint
interfaceWitnessFingerprint(const PublicInterfaceWitnessArtifact &witness) {
  std::string input;
  appendField(input, "chtholly.next.public-interface-witness.v1");
  appendEntityReference(input, witness.interface_entity);
  appendU32(input, witness.generic_parameter_count);
  appendType(input, witness.self_type);
  appendU32(input,
            static_cast<std::uint32_t>(witness.interface_arguments.size()));
  for (const auto &argument : witness.interface_arguments)
    appendType(input, argument);
  appendU32(input, static_cast<std::uint32_t>(witness.constraints.size()));
  for (const auto &constraint : witness.constraints)
    appendInterfaceConstraint(input, constraint);
  appendU32(input, static_cast<std::uint32_t>(witness.entries.size()));
  for (const auto &entry : witness.entries) {
    appendU32(input, entry.requirement);
    if (entry.function.expected_fingerprint.hasValue())
      appendEntityReference(input, entry.function);
    else
      appendType(input, entry.associated_type);
  }
  return StableFingerprint::fromCanonicalBytes(input);
}

std::vector<std::string>
canonicalParameterNames(std::size_t parameter_count,
                        std::span<const std::string> names) {
  if (!names.empty())
    return {names.begin(), names.end()};
  std::vector<std::string> result;
  result.reserve(parameter_count);
  for (std::size_t index = 0; index < parameter_count; ++index)
    result.push_back("$arg" + std::to_string(index));
  return result;
}

std::vector<std::optional<PublicConstantValue>> canonicalDefaultArguments(
    std::size_t parameter_count,
    std::span<const std::optional<PublicConstantValue>> arguments) {
  if (!arguments.empty())
    return {arguments.begin(), arguments.end()};
  return std::vector<std::optional<PublicConstantValue>>(parameter_count);
}

bool sameSignature(const PublicEntity &entity,
                   const PublicFunctionBindingSpec &function) {
  const auto canonical_name = function.canonical_name.hasValue()
                                  ? function.canonical_name
                                  : function.name;
  return entity.kind == PublicEntityKind::Function &&
         entity.name == canonical_name &&
         entity.member_owner == function.member_owner &&
         entity.member_kind == function.member_kind &&
         entity.generic_parameter_count == function.generic_parameter_count &&
         entity.return_type == function.return_type &&
         entity.error_type == function.error_type &&
         entity.execution_kind == function.execution_kind &&
         entity.coroutine_constructor == function.coroutine_constructor &&
         entity.nominal_constructor == function.nominal_constructor &&
         entity.semantic_contract == function.semantic_contract &&
         entity.intrinsic_role == function.intrinsic_role &&
         entity.parameters == function.parameters &&
         entity.parameter_names ==
             canonicalParameterNames(function.parameters.size(),
                                     function.parameter_names) &&
         entity.default_arguments ==
             canonicalDefaultArguments(function.parameters.size(),
                                       function.default_arguments) &&
         entity.ownership_summary == function.ownership_summary &&
         entity.declaration_kind == function.declaration_kind &&
         entity.is_unsafe == function.is_unsafe &&
         entity.is_const == function.is_const &&
         entity.external_symbol.hasValue() ==
             !function.external_symbol.empty() &&
         entity.foreign_signature == function.foreign_signature &&
         entity.interop_artifact == function.interop_artifact &&
         entity.generic_template == function.generic_template &&
         entity.constraints == function.constraints;
}



} // namespace chtholly::compiler::internal
