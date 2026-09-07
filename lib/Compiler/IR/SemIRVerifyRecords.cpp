#include "SemIRVerificationContext.h"

#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool SemIRVerificationContext::verifyConstantRecords(std::string &error) const {
  for (std::uint32_t index = 0; index < sem_ir_.constants_.size(); ++index) {
    const auto &entity = sem_ir_.constantEntity(ConstantEntityId(index));
    const auto concrete = entity.result.state == ConstantEvalState::Concrete;
    const auto imported = (entity.flags & SemConstantImported) != 0;
    if (!entity.name.hasValue() || entity.name.index >= sem_ir_.names_.size() ||
        !entity.type.hasValue() || entity.type.index >= sem_ir_.types_.size() ||
        (!imported &&
         (!entity.initializer.hasValue() ||
          entity.initializer.index >= sem_ir_.inst_blocks_.size() ||
          !entity.value.hasValue() ||
          entity.value.index >= sem_ir_.insts_.size())) ||
        (entity.flags & ~(SemConstantPublic | SemConstantModule |
                          SemConstantStatic | SemConstantImported)) != 0 ||
        ((entity.flags & SemConstantPublic) != 0 &&
         (entity.flags & SemConstantModule) == 0) ||
        ((entity.flags & SemConstantStatic) != 0 &&
         (entity.flags & SemConstantModule) == 0) ||
        (imported && ((entity.flags & SemConstantModule) == 0 ||
                      !entity.public_fingerprint.hasValue() ||
                      !entity.canonical_package.hasValue() ||
                      !entity.canonical_module.hasValue() ||
                      !entity.canonical_name.hasValue())) ||
        (!imported && (entity.public_fingerprint.hasValue() ||
                       entity.canonical_package.hasValue() ||
                       entity.canonical_module.hasValue() ||
                       entity.canonical_name.hasValue())) ||
        (concrete != entity.result.value.hasValue()) ||
        (concrete &&
         (entity.result.value.index >= sem_ir_.constant_values_.size() ||
          sem_ir_.constantValue(entity.result.value).type != entity.type))) {
      error = "constant has an invalid entity record";
      return false;
    }
  }
  for (std::uint32_t index = 0; index < sem_ir_.constant_values_.size();
       ++index) {
    const auto &value = sem_ir_.constantValue(ConstantId(index));
    if (!value.type.hasValue() || value.type.index >= sem_ir_.types_.size() ||
        !value.elements.hasValue() ||
        value.elements.index >= sem_ir_.constant_blocks_.size()) {
      error = "canonical constant has an invalid record";
      return false;
    }
    if ((value.kind == ConstantValueKind::Integer ||
         value.kind == ConstantValueKind::Float ||
         value.kind == ConstantValueKind::Bool ||
         value.kind == ConstantValueKind::Union ||
         value.kind == ConstantValueKind::Enum ||
         value.kind == ConstantValueKind::ForeignEnum) &&
        value.payload >= sem_ir_.values_->integerCount()) {
      error = "canonical constant has an invalid scalar payload";
      return false;
    }
    if (value.kind == ConstantValueKind::String &&
        value.payload >= sem_ir_.values_->stringLiteralCount()) {
      error = "canonical constant has an invalid string payload";
      return false;
    }
    for (const auto element : sem_ir_.constantBlock(value.elements))
      if (!element.hasValue() ||
          element.index >= sem_ir_.constant_values_.size()) {
        error = "canonical constant has an invalid child";
        return false;
      }
  }
  return true;
}

bool SemIRVerificationContext::verifyDeclarationRecords(
    std::string &error) const {
  for (std::uint32_t index = 0; index < sem_ir_.type_aliases_.size(); ++index) {
    const auto &alias = sem_ir_.typeAlias(TypeAliasId(index));
    if (!alias.name.hasValue() || alias.name.index >= sem_ir_.names_.size() ||
        !alias.target.hasValue() ||
        alias.target.index >= sem_ir_.types_.size() ||
        alias.state != SemTypeAliasState::Complete ||
        !alias.fingerprint.hasValue() ||
        (alias.generic.hasValue() &&
         alias.generic.index >= sem_ir_.values_->generics().genericCount())) {
      error = "type alias has an invalid entity record";
      return false;
    }
  }
  for (std::uint32_t index = 0; index < sem_ir_.interfaces_.size(); ++index) {
    const auto &interface_value = sem_ir_.interface(InterfaceId(index));
    if (!interface_value.name.hasValue() ||
        interface_value.name.index >= sem_ir_.names_.size() ||
        !interface_value.generic.hasValue() ||
        interface_value.generic.index >=
            sem_ir_.values_->generics().genericCount() ||
        !interface_value.fingerprint.hasValue()) {
      error = "interface has an invalid entity record";
      return false;
    }
    for (const auto &requirement : interface_value.requirements) {
      if (!requirement.name.hasValue() ||
          requirement.name.index >= sem_ir_.names_.size() ||
          (requirement.kind == SemInterfaceRequirementKind::Function &&
           (!requirement.type.hasValue() ||
            requirement.type.index >= sem_ir_.types_.size() ||
            !requirement.function.hasValue() ||
            requirement.function.index >= sem_ir_.function_refs_.size())) ||
          (requirement.kind == SemInterfaceRequirementKind::AssociatedAlias &&
           requirement.binding_index >= sem_ir_.values_->generics()
                                            .generic(interface_value.generic)
                                            .binding_count)) {
        error = "interface has an invalid requirement";
        return false;
      }
    }
  }
  for (std::uint32_t index = 0; index < sem_ir_.interface_witnesses_.size();
       ++index) {
    const auto &witness = sem_ir_.interfaceWitness(InterfaceWitnessId(index));
    const auto *declaration =
        witness.interface_id.hasValue() &&
                witness.interface_id.index < sem_ir_.interfaces_.size()
            ? &sem_ir_.interface(witness.interface_id)
            : nullptr;
    if (!witness.interface_id.hasValue() ||
        witness.interface_id.index >= sem_ir_.interfaces_.size() ||
        !declaration || !witness.self_type.hasValue() ||
        witness.self_type.index >= sem_ir_.types_.size() ||
        !witness.interface_arguments.hasValue() ||
        witness.interface_arguments.index >= sem_ir_.type_blocks_.size() ||
        witness.state == SemInterfaceWitnessState::Declared ||
        !witness.fingerprint.hasValue() ||
        (witness.generic.hasValue() &&
         (witness.generic.index >= sem_ir_.values_->generics().genericCount() ||
          sem_ir_.values_->generics().generic(witness.generic).binding_count ==
              0)) ||
        sem_ir_.typeBlock(witness.interface_arguments).size() !=
            declaration->explicit_parameter_count ||
        (witness.state == SemInterfaceWitnessState::Complete &&
         witness.entries.size() != declaration->requirements.size())) {
      error = "interface witness has an invalid entity record";
      return false;
    }
    for (const auto &constraint : witness.constraints) {
      if (!constraint.subject.hasValue() ||
          constraint.subject.index >= sem_ir_.types_.size() ||
          !constraint.interface_id.hasValue() ||
          constraint.interface_id.index >= sem_ir_.interfaces_.size() ||
          !constraint.arguments.hasValue() ||
          constraint.arguments.index >= sem_ir_.type_blocks_.size() ||
          sem_ir_.typeBlock(constraint.arguments).size() !=
              sem_ir_.interface(constraint.interface_id)
                  .explicit_parameter_count ||
          (constraint.witness.hasValue() &&
           constraint.witness.index >= sem_ir_.interface_witnesses_.size())) {
        error = "interface witness has an invalid constraint";
        return false;
      }
    }
    std::vector<bool> populated(declaration->requirements.size());
    for (const auto &entry : witness.entries) {
      if (entry.requirement >= declaration->requirements.size() ||
          populated[entry.requirement] ||
          (entry.function.hasValue() &&
           entry.function.index >= sem_ir_.function_refs_.size()) ||
          (entry.associated_type.hasValue() &&
           entry.associated_type.index >= sem_ir_.types_.size()) ||
          (declaration->requirements[entry.requirement].kind ==
               SemInterfaceRequirementKind::Function &&
           (!entry.function.hasValue() || entry.associated_type.hasValue())) ||
          (declaration->requirements[entry.requirement].kind ==
               SemInterfaceRequirementKind::AssociatedAlias &&
           (entry.function.hasValue() || !entry.associated_type.hasValue()))) {
        error = "interface witness has an invalid requirement entry";
        return false;
      }
      populated[entry.requirement] = true;
    }
  }
  return true;
}

} // namespace chtholly::compiler::internal
