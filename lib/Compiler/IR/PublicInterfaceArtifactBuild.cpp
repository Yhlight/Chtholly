#include "PublicInterfaceServices.h"

#include "chtholly/Compiler/SharedValueStores.h"

namespace chtholly::compiler::internal {

PublicInterfaceArtifact PublicInterfaceArtifactBuildService::build(
    const PublicInterfaceRegistry &registry, PublicInterfaceId id,
    std::string &error) {
  error.clear();
  const auto *interface = registry.tryGet(id);
  if (!interface) {
    error = "cannot build an artifact for a missing public interface";
    return {};
  }
  std::vector<PublicFunctionArtifact> functions;
  functions.reserve(interface->bindingCount());
  for (std::uint32_t index = 0; index < interface->bindingCount(); ++index) {
    const auto &binding = interface->function(PublicBindingId(index));
    const auto *entity = registry.tryGetEntity(binding.canonical_entity);
    if (!entity) {
      error = "cannot build an artifact for a missing public entity";
      return {};
    }
    const auto &values = *registry.values_;
    functions.push_back(
        {.name = std::string(values.identifier(binding.name)),
         .member_owner = binding.member_owner,
         .member_kind = binding.member_kind,
         .canonical_package = std::string(values.identifier(entity->package_name)),
         .canonical_module = std::string(values.identifier(entity->module_name)),
         .canonical_name = std::string(values.identifier(entity->name)),
         .generic_parameter_count = entity->generic_parameter_count,
         .parameters = entity->parameters,
         .parameter_names = entity->parameter_names,
         .default_arguments = entity->default_arguments,
         .return_type = entity->return_type,
         .error_type = entity->error_type,
         .execution_kind = entity->execution_kind,
         .coroutine_constructor = entity->coroutine_constructor,
         .nominal_constructor = entity->nominal_constructor,
         .semantic_contract = entity->semantic_contract,
         .intrinsic_role = entity->intrinsic_role,
         .ownership_summary = entity->ownership_summary,
         .declaration_kind = entity->declaration_kind,
         .is_unsafe = entity->is_unsafe,
         .is_const = entity->is_const,
         .foreign_abi = entity->foreign_abi.hasValue()
                           ? std::string(values.identifier(entity->foreign_abi))
                           : std::string{},
         .external_symbol = entity->external_symbol.hasValue()
                               ? std::string(values.identifier(entity->external_symbol))
                               : std::string{},
         .foreign_signature = entity->foreign_signature,
         .interop_artifact = entity->interop_artifact,
         .generic_template = entity->generic_template,
         .constraints = entity->constraints,
         .entity_fingerprint = entity->fingerprint});
  }
  auto nominal_types = std::vector<PublicNominalTypeArtifact>(
      interface->nominalArtifacts().begin(), interface->nominalArtifacts().end());
  auto values = std::vector<PublicValueArtifact>(interface->valueArtifacts().begin(),
                                                 interface->valueArtifacts().end());
  auto interfaces = std::vector<PublicInterfaceDeclarationArtifact>(
      interface->interfaceArtifacts().begin(), interface->interfaceArtifacts().end());
  auto aliases = std::vector<PublicTypeAliasArtifact>(
      interface->typeAliasArtifacts().begin(), interface->typeAliasArtifacts().end());
  auto witnesses = std::vector<PublicInterfaceWitnessArtifact>(
      interface->interfaceWitnessArtifacts().begin(),
      interface->interfaceWitnessArtifacts().end());
  PublicInterfaceArtifact artifact(
      std::string(registry.values_->identifier(interface->packageName())),
      std::string(registry.values_->identifier(interface->moduleName())),
      interface->fingerprint(), std::move(functions), std::move(nominal_types),
      std::move(values), std::move(interfaces), std::move(aliases),
      std::move(witnesses));
  if (!PublicInterfaceVerifyService::artifact(artifact, error))
    return {};
  return artifact;
}

} // namespace chtholly::compiler::internal
