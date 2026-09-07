#include "PublicInterfaceConstructionInternal.h"

#include <functional>
#include <utility>

namespace chtholly::compiler::internal {

bool PublicInterfaceDeclarationConstructionService::build(
    const SemIR &sem_ir, PublicInterfaceRegistry &registry,
    IdentifierId package_name, std::string &error,
    NativeDefinitionExportClosure *native_exports,
    PublicInterfaceTypeConstructionContext &types,
    const std::unordered_map<std::uint32_t, std::size_t> &local_function_specs,
    PublicInterfaceDeclarationConstructionCallbacks callbacks,
    std::vector<PublicFunctionBindingSpec> &functions,
    std::vector<PublicValueArtifact> &public_values,
    std::vector<PublicInterfaceDeclarationArtifact> &public_interfaces,
    std::vector<PublicTypeAliasArtifact> &public_aliases,
    std::vector<PublicInterfaceWitnessArtifact> &public_witnesses) {
  std::function<std::optional<PublicType>(TypeId)> map_type = [&](TypeId type) {
    return types.mapType(type);
  };
  std::function<std::optional<PublicConstantValue>(ConstantId)> map_constant =
      [&](ConstantId id) { return types.mapConstant(id); };
  auto &mapped_generic = types.mappedGeneric();
  auto &mapped_constraints = types.mappedConstraints();
  const auto &final_function_reference = callbacks.function_reference;
  std::vector<std::optional<PublicInterfaceDeclarationArtifact>>
      interface_artifacts(sem_ir.interfaceCount());
  std::vector<std::uint8_t> interface_states(sem_ir.interfaceCount());
  std::function<bool(InterfaceId)> build_interface;
  std::function<std::optional<PublicEntityReferenceArtifact>(InterfaceId)>
      map_interface_reference;
  std::function<std::optional<std::vector<PublicInterfaceConstraintArtifact>>(
      std::span<const SemInterfaceConstraint>)>
      map_constraints;
  map_interface_reference =
      [&](InterfaceId id) -> std::optional<PublicEntityReferenceArtifact> {
    const auto &source = sem_ir.interface(id);
    if (source.canonical_entity.hasValue()) {
      const auto *entity = registry.tryGetEntity(source.canonical_entity);
      if (!entity || entity->kind != PublicEntityKind::Interface)
        return std::nullopt;
      return PublicEntityReferenceArtifact{
          PublicEntityKind::Interface,
          std::string(sem_ir.identifier(entity->package_name)),
          std::string(sem_ir.identifier(entity->module_name)),
          std::string(sem_ir.identifier(entity->name)), entity->fingerprint};
    }
    if ((source.flags & SemInterfacePublic) == 0 || !build_interface(id))
      return std::nullopt;
    return interface_artifacts[id.index]->entity;
  };
  map_constraints = [&](std::span<const SemInterfaceConstraint> constraints)
      -> std::optional<std::vector<PublicInterfaceConstraintArtifact>> {
    std::vector<PublicInterfaceConstraintArtifact> result;
    result.reserve(constraints.size());
    for (const auto &source : constraints) {
      auto subject = map_type(source.subject);
      auto interface_entity = map_interface_reference(source.interface_id);
      if (!subject || !interface_entity)
        return std::nullopt;
      PublicInterfaceConstraintArtifact constraint{
          .subject = std::move(*subject),
          .interface_entity = std::move(*interface_entity)};
      for (const auto argument : sem_ir.typeBlock(source.arguments)) {
        auto mapped = map_type(argument);
        if (!mapped)
          return std::nullopt;
        constraint.arguments.push_back(std::move(*mapped));
      }
      result.push_back(std::move(constraint));
    }
    return result;
  };
  build_interface = [&](InterfaceId id) {
    if (interface_states[id.index] == 2)
      return true;
    if (interface_states[id.index] == 1) {
      error = "public interface constraints contain an identity cycle";
      return false;
    }
    interface_states[id.index] = 1;
    const auto &source = sem_ir.interface(id);
    PublicTypeMappingScope mapping_scope(mapped_generic, mapped_constraints,
                                         source.generic, source.constraints);
    PublicInterfaceDeclarationArtifact artifact;
    artifact.entity = {
        PublicEntityKind::Interface,
        std::string(sem_ir.identifier(package_name)),
        std::string(sem_ir.identifier(sem_ir.moduleName())),
        std::string(sem_ir.identifier(sem_ir.name(source.name).text)),
        {}};
    artifact.generic_parameter_count =
        sem_ir.genericValues().generic(source.generic).binding_count;
    artifact.explicit_parameter_count = source.explicit_parameter_count;
    const auto constraints = map_constraints(source.constraints);
    if (!constraints)
      return false;
    artifact.constraints = *constraints;
    for (const auto &requirement : source.requirements) {
      PublicInterfaceRequirementArtifact target;
      target.kind = requirement.kind == SemInterfaceRequirementKind::Function
                        ? PublicInterfaceRequirementKind::Function
                        : PublicInterfaceRequirementKind::AssociatedAlias;
      target.name =
          std::string(sem_ir.identifier(sem_ir.name(requirement.name).text));
      target.binding_index = requirement.binding_index;
      target.has_default = requirement.has_default;
      if (requirement.kind == SemInterfaceRequirementKind::Function) {
        const auto &reference = sem_ir.functionRef(requirement.function);
        if (reference.local_function.hasValue() &&
            !sem_ir.functionConstraints(reference.local_function).empty()) {
          error = "public interface requirement constraints form a recursive "
                  "definition identity";
          return false;
        }
        const auto function = final_function_reference(requirement.function);
        if (!function)
          return false;
        target.function = *function;
      } else if (requirement.has_default) {
        auto associated_type = map_type(requirement.type);
        if (!associated_type)
          return false;
        target.associated_type = std::move(*associated_type);
      }
      artifact.requirements.push_back(std::move(target));
    }
    artifact.entity.expected_fingerprint =
        callbacks.interface_fingerprint(artifact);
    interface_artifacts[id.index] = std::move(artifact);
    interface_states[id.index] = 2;
    return true;
  };
  for (std::uint32_t index = 0; index < sem_ir.interfaceCount(); ++index) {
    const auto id = InterfaceId(index);
    if ((sem_ir.interface(id).flags & SemInterfacePublic) != 0 &&
        (!build_interface(id) || !interface_artifacts[index]))
      return false;
  }
  for (auto &artifact : interface_artifacts)
    if (artifact)
      public_interfaces.push_back(*artifact);

  for (const auto &[function_index, spec_index] : local_function_specs) {
    const auto function_id = FunctionId(function_index);
    const auto &function = sem_ir.function(function_id);
    PublicTypeMappingScope mapping_scope(
        mapped_generic, mapped_constraints, function.generic,
        sem_ir.functionConstraints(function_id));
    const auto constraints =
        map_constraints(sem_ir.functionConstraints(function_id));
    if (!constraints)
      return false;
    functions[spec_index].constraints = *constraints;
  }

  for (std::uint32_t index = 0; index < sem_ir.typeAliasCount(); ++index) {
    const auto &source = sem_ir.typeAlias(TypeAliasId(index));
    if ((source.flags & SemTypeAliasPublic) == 0)
      continue;
    auto target = map_type(source.target);
    auto constraints = map_constraints(source.constraints);
    if (!target || !constraints)
      return false;
    PublicTypeAliasArtifact artifact{
        .entity = {PublicEntityKind::TypeAlias,
                   std::string(sem_ir.identifier(package_name)),
                   std::string(sem_ir.identifier(sem_ir.moduleName())),
                   std::string(
                       sem_ir.identifier(sem_ir.name(source.name).text)),
                   {}},
        .generic_parameter_count =
            source.generic.hasValue()
                ? sem_ir.genericValues().generic(source.generic).binding_count
                : 0U,
        .target = std::move(*target),
        .constraints = std::move(*constraints)};
    artifact.entity.expected_fingerprint =
        callbacks.alias_fingerprint(artifact);
    public_aliases.push_back(std::move(artifact));
  }

  for (std::uint32_t index = 0; index < sem_ir.interfaceWitnessCount();
       ++index) {
    const auto &source = sem_ir.interfaceWitness(InterfaceWitnessId(index));
    if (source.state != SemInterfaceWitnessState::Complete)
      continue;
    PublicTypeMappingScope mapping_scope(mapped_generic, mapped_constraints,
                                         source.generic, source.constraints);
    const auto &self_sem_type = sem_ir.type(source.self_type);
    if (self_sem_type.kind == SemTypeKind::Nominal &&
        (sem_ir.nominalType(NominalTypeId(self_sem_type.arg0)).flags &
         (SemNominalTypePublic | SemNominalTypeArtifactDependency)) == 0)
      continue;
    auto interface_entity = map_interface_reference(source.interface_id);
    auto self_type = map_type(source.self_type);
    if (!interface_entity || !self_type)
      continue;
    PublicInterfaceWitnessArtifact artifact{
        .interface_entity = std::move(*interface_entity),
        .generic_parameter_count =
            source.generic.hasValue()
                ? sem_ir.genericValues().generic(source.generic).binding_count
                : 0U,
        .self_type = std::move(*self_type)};
    const auto witness_constraints = map_constraints(source.constraints);
    if (!witness_constraints)
      return false;
    artifact.constraints = *witness_constraints;
    bool valid = true;
    std::vector<std::uint32_t> witness_native_exports;
    for (const auto argument : sem_ir.typeBlock(source.interface_arguments)) {
      auto mapped = map_type(argument);
      if (!mapped) {
        valid = false;
        break;
      }
      artifact.interface_arguments.push_back(std::move(*mapped));
    }
    for (const auto &entry : source.entries) {
      PublicInterfaceWitnessEntryArtifact target{.requirement =
                                                     entry.requirement};
      if (entry.function.hasValue()) {
        const auto &reference = sem_ir.functionRef(entry.function);
        if (reference.local_function.hasValue()) {
          const auto function_id = reference.local_function;
          const auto &semantic_function = sem_ir.function(function_id);
          if ((semantic_function.flags &
               (SemFunctionTemplate | SemFunctionEvaluatorArtifact)) == 0 &&
              sem_ir.functionDeclaration(function_id).kind ==
                  SemCallableDeclarationKind::Definition)
            witness_native_exports.push_back(function_id.index);
        }
        const auto function = final_function_reference(entry.function);
        if (!function) {
          valid = false;
          break;
        }
        target.function = *function;
      } else {
        auto associated_type = map_type(entry.associated_type);
        if (!associated_type) {
          valid = false;
          break;
        }
        target.associated_type = std::move(*associated_type);
      }
      artifact.entries.push_back(std::move(target));
    }
    if (!valid)
      continue;
    if (native_exports)
      native_exports->functions.insert(native_exports->functions.end(),
                                       witness_native_exports.begin(),
                                       witness_native_exports.end());
    artifact.fingerprint = callbacks.witness_fingerprint(artifact);
    public_witnesses.push_back(std::move(artifact));
  }
  for (std::uint32_t index = 0; index < sem_ir.constantEntityCount(); ++index) {
    const auto &source = sem_ir.constantEntity(ConstantEntityId(index));
    if ((source.flags & (SemConstantPublic | SemConstantModule)) !=
            (SemConstantPublic | SemConstantModule) ||
        !source.result.isConcrete())
      continue;
    auto type = map_type(source.type);
    auto value = map_constant(source.result.value);
    if (!type || !value)
      return false;
    const auto name =
        std::string(sem_ir.identifier(sem_ir.name(source.name).text));
    PublicValueArtifact artifact{
        .kind = (source.flags & SemConstantStatic) != 0
                    ? PublicValueKind::Static
                    : PublicValueKind::Constant,
        .name = name,
        .canonical_package = std::string(sem_ir.identifier(package_name)),
        .canonical_module = std::string(sem_ir.identifier(sem_ir.moduleName())),
        .canonical_name = name,
        .type = std::move(*type),
        .value = std::move(*value)};
    artifact.entity_fingerprint = callbacks.value_fingerprint(artifact);
    public_values.push_back(std::move(artifact));
  }
  for (const auto &source : sem_ir.foreignConstants()) {
    auto type = map_type(source.type);
    if (!type)
      return false;
    const auto name =
        std::string(sem_ir.identifier(sem_ir.name(source.name).text));
    PublicValueArtifact artifact{
        .kind = PublicValueKind::Constant,
        .name = name,
        .canonical_package = std::string(sem_ir.identifier(package_name)),
        .canonical_module = std::string(sem_ir.identifier(sem_ir.moduleName())),
        .canonical_name = name,
        .type = *type,
        .value = {
            .kind = source.kind, .type = *type, .payload = source.payload}};
    artifact.entity_fingerprint = callbacks.value_fingerprint(artifact);
    public_values.push_back(std::move(artifact));
  }

  return true;
}

} // namespace chtholly::compiler::internal
