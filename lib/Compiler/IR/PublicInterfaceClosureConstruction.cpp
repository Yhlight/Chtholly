#include "PublicInterfaceConstructionInternal.h"

#include <algorithm>
#include <functional>
#include <ranges>
#include <tuple>
#include <utility>

namespace chtholly::compiler::internal {

PublicInterfaceId
PublicInterfaceClosureConstructionService::finalizeAndRegister(
    const SemIR &sem_ir, PublicInterfaceRegistry &registry,
    IdentifierId package_name, std::string &error,
    NativeDefinitionExportClosure *native_exports,
    std::vector<PublicFunctionBindingSpec> &functions,
    std::vector<PublicNominalTypeArtifact> &public_nominals,
    std::vector<PublicValueArtifact> &public_values,
    std::vector<PublicInterfaceDeclarationArtifact> &public_interfaces,
    std::vector<PublicTypeAliasArtifact> &public_aliases,
    std::vector<PublicInterfaceWitnessArtifact> &public_witnesses) {
  for (std::uint32_t index = 0; index < sem_ir.importIRs().size(); ++index) {
    const auto import_id = ImportIRId(index);
    const auto *import = sem_ir.importIRs().tryGet(import_id);
    const auto *interface = sem_ir.importIRs().tryGetInterface(import_id);
    if (!import || !import->is_export || !interface)
      continue;
    for (std::uint32_t binding_index = 0;
         binding_index < interface->bindingCount(); ++binding_index) {
      const auto &binding = interface->function(PublicBindingId(binding_index));
      PublicFunctionBindingSpec spec{
          .name = binding.name,
          .member_owner = binding.member_owner,
          .member_kind = binding.member_kind,
          .generic_parameter_count = binding.generic_parameter_count,
          .return_type = binding.return_type,
          .canonical_entity = binding.canonical_entity};
      if (const auto *entity =
              registry.tryGetEntity(binding.canonical_entity)) {
        spec.canonical_name = entity->name;
        spec.error_type = entity->error_type;
        spec.execution_kind = entity->execution_kind;
        spec.coroutine_constructor = entity->coroutine_constructor;
        spec.nominal_constructor = entity->nominal_constructor;
        spec.semantic_contract = entity->semantic_contract;
        spec.intrinsic_role = entity->intrinsic_role;
        spec.ownership_summary = entity->ownership_summary;
        spec.parameter_names = entity->parameter_names;
        spec.default_arguments = entity->default_arguments;
        spec.declaration_kind = entity->declaration_kind;
        spec.is_unsafe = entity->is_unsafe;
        spec.is_const = entity->is_const;
        if (entity->foreign_abi.hasValue())
          spec.foreign_abi = sem_ir.identifier(entity->foreign_abi);
        if (entity->external_symbol.hasValue())
          spec.external_symbol = sem_ir.identifier(entity->external_symbol);
        spec.foreign_signature = entity->foreign_signature;
        spec.interop_artifact = entity->interop_artifact;
        spec.generic = entity->generic;
        spec.generic_template = entity->generic_template;
        spec.constraints = entity->constraints;
      }
      const auto parameters = interface->parameterTypes(binding.parameters);
      spec.parameters.assign(parameters.begin(), parameters.end());
      functions.push_back(std::move(spec));
    }
    public_nominals.insert(public_nominals.end(),
                           interface->nominalArtifacts().begin(),
                           interface->nominalArtifacts().end());
    public_values.insert(public_values.end(),
                         interface->valueArtifacts().begin(),
                         interface->valueArtifacts().end());
    public_interfaces.insert(public_interfaces.end(),
                             interface->interfaceArtifacts().begin(),
                             interface->interfaceArtifacts().end());
    public_aliases.insert(public_aliases.end(),
                          interface->typeAliasArtifacts().begin(),
                          interface->typeAliasArtifacts().end());
    public_witnesses.insert(public_witnesses.end(),
                            interface->interfaceWitnessArtifacts().begin(),
                            interface->interfaceWitnessArtifacts().end());
  }
  std::ranges::sort(public_nominals, {}, [](const auto &artifact) {
    return std::tie(artifact.entity.canonical_package,
                    artifact.entity.canonical_module,
                    artifact.entity.canonical_name);
  });
  const auto same_nominal_identity = [](const auto &lhs, const auto &rhs) {
    return lhs.entity.canonical_package == rhs.entity.canonical_package &&
           lhs.entity.canonical_module == rhs.entity.canonical_module &&
           lhs.entity.canonical_name == rhs.entity.canonical_name;
  };
  for (std::size_t index = 1; index < public_nominals.size(); ++index) {
    const auto &previous = public_nominals[index - 1];
    const auto &current = public_nominals[index];
    if (same_nominal_identity(previous, current) && previous != current) {
      error = "forwarded nominal closure has conflicting definitions";
      return PublicInterfaceId::invalid();
    }
  }
  public_nominals.erase(std::unique(public_nominals.begin(),
                                    public_nominals.end(),
                                    same_nominal_identity),
                        public_nominals.end());
  std::ranges::sort(public_values, {}, [](const auto &value) {
    return std::tie(value.canonical_package, value.canonical_module,
                    value.canonical_name);
  });
  const auto same_value_identity = [](const auto &lhs, const auto &rhs) {
    return lhs.canonical_package == rhs.canonical_package &&
           lhs.canonical_module == rhs.canonical_module &&
           lhs.canonical_name == rhs.canonical_name;
  };
  for (std::size_t index = 1; index < public_values.size(); ++index)
    if (same_value_identity(public_values[index - 1], public_values[index]) &&
        public_values[index - 1] != public_values[index]) {
      error = "forwarded value closure has conflicting definitions";
      return PublicInterfaceId::invalid();
    }
  public_values.erase(std::unique(public_values.begin(), public_values.end(),
                                  same_value_identity),
                      public_values.end());
  const auto canonical_entity_less = [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.entity.canonical_package, lhs.entity.canonical_module,
                    lhs.entity.canonical_name) <
           std::tie(rhs.entity.canonical_package, rhs.entity.canonical_module,
                    rhs.entity.canonical_name);
  };
  const auto same_canonical_entity = [](const auto &lhs, const auto &rhs) {
    return lhs.entity.canonical_package == rhs.entity.canonical_package &&
           lhs.entity.canonical_module == rhs.entity.canonical_module &&
           lhs.entity.canonical_name == rhs.entity.canonical_name;
  };
  const auto canonicalize_entities = [&](auto &artifacts,
                                         std::string_view description) {
    std::ranges::sort(artifacts, canonical_entity_less);
    for (std::size_t index = 1; index < artifacts.size(); ++index)
      if (same_canonical_entity(artifacts[index - 1], artifacts[index]) &&
          artifacts[index - 1] != artifacts[index]) {
        error = "forwarded " + std::string(description) +
                " closure has conflicting definitions";
        return false;
      }
    artifacts.erase(
        std::unique(artifacts.begin(), artifacts.end(), same_canonical_entity),
        artifacts.end());
    return true;
  };
  if (!canonicalize_entities(public_interfaces, "interface") ||
      !canonicalize_entities(public_aliases, "type alias"))
    return PublicInterfaceId::invalid();
  const auto canonicalize_interface_reference =
      [&](PublicEntityReferenceArtifact &reference) {
        if (reference.kind != PublicEntityKind::Interface)
          return;
        const auto found = std::ranges::find_if(
            public_interfaces,
            [&](const PublicInterfaceDeclarationArtifact &declaration) {
              return declaration.entity.canonical_package ==
                         reference.canonical_package &&
                     declaration.entity.canonical_module ==
                         reference.canonical_module &&
                     declaration.entity.canonical_name ==
                         reference.canonical_name;
            });
        if (found != public_interfaces.end())
          reference = found->entity;
      };
  std::function<void(PublicType &)> canonicalize_function_type;
  canonicalize_function_type = [&](PublicType &type) {
    if (type.kind == PublicTypeKind::TypeProjection &&
        type.projection_kind == PublicTypeProjectionKind::Associated)
      canonicalize_interface_reference(type.nominal_entity);
    for (auto &argument : type.arguments)
      canonicalize_function_type(argument);
  };
  const auto canonicalize_region =
      [&](GenericEvaluationRegionArtifact &region) {
        for (auto &instruction : region.instructions)
          canonicalize_function_type(instruction.type);
        for (auto &result : region.results)
          canonicalize_function_type(result);
      };
  for (auto &function : functions) {
    for (auto &parameter : function.parameters)
      canonicalize_function_type(parameter);
    canonicalize_function_type(function.return_type);
    if (function.error_type)
      canonicalize_function_type(*function.error_type);
    canonicalize_function_type(function.semantic_contract.owner);
    for (auto &constraint : function.constraints) {
      canonicalize_function_type(constraint.subject);
      canonicalize_interface_reference(constraint.interface_entity);
      for (auto &argument : constraint.arguments)
        canonicalize_function_type(argument);
    }
    for (auto &default_argument : function.default_arguments)
      if (default_argument) {
        std::function<void(PublicConstantValue &)> canonicalize_constant;
        canonicalize_constant = [&](PublicConstantValue &value) {
          canonicalize_function_type(value.type);
          for (auto &element : value.elements)
            canonicalize_constant(element);
        };
        canonicalize_constant(*default_argument);
      }
    if (!function.generic_template)
      continue;
    auto &generic_template = *function.generic_template;
    for (auto &type : generic_template.local_types)
      canonicalize_function_type(type);
    for (auto &query : generic_template.type_queries) {
      canonicalize_function_type(query.source);
      canonicalize_function_type(query.other);
    }
    for (auto &arguments : generic_template.callee_type_arguments)
      for (auto &argument : arguments)
        canonicalize_function_type(argument);
    canonicalize_region(generic_template.declaration);
    canonicalize_region(generic_template.definition);
  }
  std::ranges::sort(public_witnesses, {}, [](const auto &witness) {
    return witness.fingerprint.hex();
  });
  for (std::size_t index = 1; index < public_witnesses.size(); ++index)
    if (public_witnesses[index - 1].fingerprint ==
            public_witnesses[index].fingerprint &&
        public_witnesses[index - 1] != public_witnesses[index]) {
      error = "forwarded interface witness closure has conflicting "
              "definitions";
      return PublicInterfaceId::invalid();
    }
  public_witnesses.erase(
      std::unique(public_witnesses.begin(), public_witnesses.end(),
                  [](const auto &lhs, const auto &rhs) {
                    return lhs.fingerprint == rhs.fingerprint;
                  }),
      public_witnesses.end());
  if (native_exports)
    native_exports->canonicalize();
  return registry.registerInterface(
      sem_ir.checkIRId(), package_name, sem_ir.moduleName(), functions, error,
      public_nominals, public_values, public_interfaces, public_aliases,
      public_witnesses);
}

} // namespace chtholly::compiler::internal
