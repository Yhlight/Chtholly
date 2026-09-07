#include "SemanticContext.h"

namespace chtholly::compiler::semantics_internal {

[[nodiscard]] std::optional<SemanticContext::IteratorProtocolResolution>
SemanticContext::resolveIteratorProtocol(TypeId iterator_type,
                                         NodeId location) {
  materializeImportedInterfaceWitnesses(location);
  InterfaceId iterator_interface;
  for (std::uint32_t index = 0; index < sem_ir_.interfaceCount(); ++index) {
    const auto candidate = InterfaceId(index);
    const auto &interface_value = sem_ir_.interface(candidate);
    if (!interface_value.name.hasValue() ||
        sem_ir_.identifier(sem_ir_.name(interface_value.name).text) !=
            "Iterator" ||
        !interface_value.canonical_entity.hasValue())
      continue;
    const auto *entity = sem_ir_.importIRs().tryGetEntity(
        interface_value.canonical_entity);
    if (!entity || values_.identifier(entity->package_name) != "std" ||
        values_.identifier(entity->module_name) != "std::iter")
      continue;
    iterator_interface = candidate;
    break;
  }
  if (!iterator_interface.hasValue())
    return std::nullopt;

  const auto witness_result =
      lookupInterfaceWitness(iterator_type, iterator_interface, {}, location);
  if (!witness_result.hasValue())
    return std::nullopt;
  const auto &interface_value = sem_ir_.interface(iterator_interface);
  const auto &witness = sem_ir_.interfaceWitness(witness_result.witness);
  std::uint32_t item_requirement = core::AnyId::InvalidIndex;
  std::uint32_t next_requirement = core::AnyId::InvalidIndex;
  for (std::uint32_t index = 0; index < interface_value.requirements.size();
       ++index) {
    const auto &requirement = interface_value.requirements[index];
    const auto name =
        sem_ir_.identifier(sem_ir_.name(requirement.name).text);
    if (requirement.kind == SemInterfaceRequirementKind::AssociatedAlias &&
        name == "Item")
      item_requirement = index;
    if (requirement.kind == SemInterfaceRequirementKind::Function &&
        name == "next")
      next_requirement = index;
  }
  if (item_requirement == core::AnyId::InvalidIndex ||
      next_requirement == core::AnyId::InvalidIndex ||
      item_requirement >= witness.entries.size() ||
      next_requirement >= witness.entries.size() ||
      !witness.entries[item_requirement].associated_type.hasValue() ||
      !witness.entries[next_requirement].function.hasValue())
    return std::nullopt;

  const auto item_type = witness.entries[item_requirement].associated_type;
  auto next = witness.entries[next_requirement].function;
  if (sem_ir_.functionRef(next).generic.hasValue()) {
    std::vector<CanonicalTypeId> arguments;
    if (sem_ir_.type(iterator_type).kind == SemTypeKind::Nominal)
      for (const auto argument : sem_ir_.typeBlock(
               TypeBlockId(sem_ir_.type(iterator_type).arg1)))
        arguments.push_back(sem_ir_.canonicalType(argument));
    next = specializeFunction(next, {}, location, false, arguments);
    if (!next.hasValue())
      return std::nullopt;
  }
  const auto function_type = sem_ir_.functionRef(next).local_type;
  if (sem_ir_.type(function_type).kind != SemTypeKind::Function)
    return std::nullopt;
  const auto parameters =
      sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(function_type).arg0));
  const auto step_type = TypeId(sem_ir_.type(function_type).arg1);
  if (parameters.size() != 1 ||
      sem_ir_.canonicalType(parameters.front()) !=
          sem_ir_.canonicalType(iterator_type) ||
      sem_ir_.type(step_type).kind != SemTypeKind::Nominal)
    return std::nullopt;

  const auto &step =
      sem_ir_.nominalType(NominalTypeId(sem_ir_.type(step_type).arg0));
  const auto step_name =
      step.name.hasValue()
          ? sem_ir_.identifier(sem_ir_.name(step.name).text)
          : std::string_view{};
  if (step_name != "IterationStep" || step.kind != NominalKind::Enum ||
      step.variants.size() != 2)
    return std::nullopt;
  if (!step.canonical_entity.hasValue())
    return std::nullopt;
  const auto *step_entity =
      sem_ir_.importIRs().tryGetEntity(step.canonical_entity);
  if (!step_entity || values_.identifier(step_entity->package_name) != "std" ||
      values_.identifier(step_entity->module_name) != "std::iter" ||
      values_.identifier(step_entity->name) != "IterationStep")
    return std::nullopt;

  std::uint32_t item_variant = core::AnyId::InvalidIndex;
  std::uint32_t done_variant = core::AnyId::InvalidIndex;
  for (std::uint32_t index = 0; index < step.variants.size(); ++index) {
    const auto name = sem_ir_.identifier(sem_ir_.name(step.variants[index].name).text);
    if (name == "Item")
      item_variant = index;
    else if (name == "Done")
      done_variant = index;
  }
  if (item_variant == core::AnyId::InvalidIndex ||
      done_variant == core::AnyId::InvalidIndex ||
      step.variants[item_variant].fields.size() != 2 ||
      !step.variants[done_variant].fields.empty() ||
      sem_ir_.canonicalType(
          sem_ir_.enumPayloadFieldType(step_type, item_variant, 0)) !=
          sem_ir_.canonicalType(item_type) ||
      sem_ir_.canonicalType(
          sem_ir_.enumPayloadFieldType(step_type, item_variant, 1)) !=
          sem_ir_.canonicalType(iterator_type))
    return std::nullopt;

  return IteratorProtocolResolution{
      witness_result.witness,
      next,
      item_type,
      step_type,
      item_variant,
      done_variant,
      sem_ir_.functionIntrinsicRole(next)};
}

} // namespace chtholly::compiler::semantics_internal
