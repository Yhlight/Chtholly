#include "SemIRVerificationContext.h"
#include "chtholly/Compiler/CarrierView.h"

#include <ranges>

namespace chtholly::compiler::internal {
namespace {

std::uint16_t semanticCapability(CallableSemanticRole role) {
  if (role == CallableSemanticRole::None || role >= CallableSemanticRole::Count)
    return CallableCapabilityNone;
  return static_cast<std::uint16_t>(1U << (static_cast<unsigned>(role) - 1U));
}

CallableSemanticDomain semanticDomain(CallableSemanticRole role) {
  if (role == CallableSemanticRole::Constructor)
    return CallableSemanticDomain::NominalConstruction;
  if (role == CallableSemanticRole::Copy || role == CallableSemanticRole::Drop)
    return CallableSemanticDomain::Lifecycle;
  if (role == CallableSemanticRole::Pack || role == CallableSemanticRole::Init)
    return CallableSemanticDomain::ValueRepresentation;
  if (role >= CallableSemanticRole::ProjectionLoad &&
      role <= CallableSemanticRole::ProjectionBorrowMut)
    return CallableSemanticDomain::ObjectProjection;
  if (role >= CallableSemanticRole::ObjectInit &&
      role <= CallableSemanticRole::ObjectDrop)
    return CallableSemanticDomain::ObjectShell;
  return CallableSemanticDomain::Ordinary;
}

} // namespace

bool SemIRVerificationContext::verifyFunctions(std::string &error) const {
  bool has_coroutine_task_driver = false;
  for (std::size_t index = 0; index < sem_ir_.functions_.size(); ++index) {
    const auto &declaration = sem_ir_.functionDeclaration(
        FunctionId(static_cast<std::uint32_t>(index)));
    if (declaration.kind >= SemCallableDeclarationKind::Count ||
        (declaration.kind == SemCallableDeclarationKind::Foreign) !=
            declaration.foreign_abi.hasValue() ||
        (declaration.kind != SemCallableDeclarationKind::Foreign &&
         declaration.is_unsafe &&
         sem_ir_.function(FunctionId(static_cast<std::uint32_t>(index)))
                 .intrinsic_role == CompilerIntrinsicRole::None)) {
      error = "function has an invalid declaration contract";
      return false;
    }
    const auto &value =
        sem_ir_.function(FunctionId(static_cast<std::uint32_t>(index)));
    if (value.intrinsic_role >= CompilerIntrinsicRole::Count ||
        (value.intrinsic_role != CompilerIntrinsicRole::None &&
         declaration.kind != SemCallableDeclarationKind::Forward)) {
      error = "function has an invalid compiler intrinsic role";
      return false;
    }
    for (const auto &constraint : sem_ir_.functionConstraints(
             FunctionId(static_cast<std::uint32_t>(index)))) {
      if (!constraint.subject.hasValue() ||
          constraint.subject.index >= sem_ir_.types_.size() ||
          !constraint.interface_id.hasValue() ||
          constraint.interface_id.index >= sem_ir_.interfaces_.size() ||
          !constraint.arguments.hasValue() ||
          constraint.arguments.index >= sem_ir_.type_blocks_.size() ||
          (constraint.witness.hasValue() &&
           constraint.witness.index >= sem_ir_.interface_witnesses_.size())) {
        error = "function has an invalid interface constraint";
        return false;
      }
    }
    if (value.name.index >= sem_ir_.names_.size() ||
        value.type.index >= sem_ir_.types_.size() ||
        value.parameters.index >= sem_ir_.local_blocks_.size() ||
        value.body.index >= sem_ir_.inst_blocks_.size() ||
        (value.flags &
         ~(SemFunctionPublic | SemFunctionTemplate | SemFunctionSpecific |
           SemFunctionCoroutineScaffold | SemFunctionAsync |
           SemFunctionCoroutineExecutionEntry | SemFunctionCoroutineTaskDriver |
           SemFunctionConst | SemFunctionEvaluatorArtifact |
           SemFunctionInterfaceMember)) != 0 ||
        (((value.flags & (SemFunctionTemplate | SemFunctionSpecific)) != 0) !=
         value.generic.hasValue()) ||
        (value.generic.hasValue() != value.specific.hasValue()) ||
        ((value.flags & SemFunctionTemplate) != 0 &&
         (value.flags & SemFunctionSpecific) != 0) ||
        ((value.flags & SemFunctionCoroutineExecutionEntry) != 0 &&
         (value.flags & SemFunctionAsync) == 0) ||
        ((value.flags & SemFunctionCoroutineTaskDriver) != 0 &&
         (value.flags &
          (SemFunctionPublic | SemFunctionTemplate | SemFunctionSpecific |
           SemFunctionCoroutineScaffold | SemFunctionAsync)) != 0)) {
      error = "function has an invalid entity record";
      return false;
    }
    const auto is_const = (value.flags & SemFunctionConst) != 0;
    if (declaration.is_const != is_const ||
        (is_const &&
         ((declaration.kind != SemCallableDeclarationKind::Definition &&
           !(value.intrinsic_role == CompilerIntrinsicRole::WrappingMul &&
             declaration.kind == SemCallableDeclarationKind::Forward)) ||
          declaration.is_unsafe))) {
      error = "constant function has an invalid declaration contract";
      return false;
    }
    const auto function_kind = sem_ir_.type(value.type).kind;
    const auto is_async = (value.flags & SemFunctionAsync) != 0;
    const auto constructor_entity = sem_ir_.coroutineConstructorEntity(
        FunctionId(static_cast<std::uint32_t>(index)));
    if (constructor_entity.hasValue() &&
        (value.flags & SemFunctionCoroutineScaffold) == 0) {
      error = "non-scaffold function has a coroutine constructor binding";
      return false;
    }
    if ((value.flags & SemFunctionCoroutineScaffold) != 0 &&
        !constructor_entity.hasValue()) {
      error = "coroutine scaffold has no canonical constructor entity";
      return false;
    }
    if ((function_kind != SemTypeKind::Function &&
         function_kind != SemTypeKind::AsyncFunction) ||
        (function_kind == SemTypeKind::AsyncFunction) != is_async ||
        (is_async && declaration.kind == SemCallableDeclarationKind::Foreign)) {
      error = "function entity does not have a function type";
      return false;
    }
    const auto &function_type = sem_ir_.type(value.type);
    const auto return_type = is_async ? sem_ir_.asyncSuccessType(value.type)
                                      : TypeId(function_type.arg1);
    const auto is_template = (value.flags & SemFunctionTemplate) != 0;
    const auto semantic_role =
        static_cast<SemCanonicalFunctionRole>(value.semantic_role);
    const auto &semantic_contract = sem_ir_.functionSemanticContract(
        FunctionId(static_cast<std::uint32_t>(index)));
    const auto contract_projection =
        semantic_contract.domain == CallableSemanticDomain::ObjectProjection;
    if (semantic_contract.role != semantic_role ||
        semantic_contract.domain != semanticDomain(semantic_role) ||
        semantic_contract.capability != semanticCapability(semantic_role) ||
        (semantic_contract.domain != CallableSemanticDomain::Ordinary &&
         semantic_contract.owner != value.semantic_owner) ||
        semantic_contract.carrier_path.size() > 256 ||
        (semantic_contract.has_bit_range &&
         (semantic_contract.bit_begin >= semantic_contract.bit_end ||
          semantic_contract.bit_end > 32 || semantic_contract.whole_carrier)) ||
        (!semantic_contract.has_bit_range &&
         (semantic_contract.bit_begin != 0 ||
          semantic_contract.bit_end != 0)) ||
        (semantic_contract.domain == CallableSemanticDomain::Ordinary &&
         (semantic_contract.owner.hasValue() ||
          semantic_contract.projector_field != core::AnyId::InvalidIndex ||
          semantic_contract.whole_carrier ||
          !semantic_contract.carrier_path.empty())) ||
        (semantic_contract.domain != CallableSemanticDomain::Ordinary &&
         !semantic_contract.owner.hasValue()) ||
        (contract_projection !=
         (semantic_contract.projector_field != core::AnyId::InvalidIndex)) ||
        (contract_projection && semantic_contract.whole_carrier !=
                                    semantic_contract.carrier_path.empty())) {
      error = "function has an invalid callable semantic contract";
      return false;
    }
    if (contract_projection) {
      const auto &owner = sem_ir_.nominalType(semantic_contract.owner);
      if (semantic_contract.projector_field >= owner.fields.size() ||
          owner.fields[semantic_contract.projector_field].projector_name !=
              value.semantic_projector) {
        error = "projection helper contract has an invalid owner field";
        return false;
      }
    }
    const auto is_representation_conversion =
        semantic_role == SemCanonicalFunctionRole::Pack ||
        semantic_role == SemCanonicalFunctionRole::Init;
    const auto is_declaration_specific =
        (is_template ||
         declaration.kind == SemCallableDeclarationKind::Forward) &&
        value.generic.hasValue() && value.specific.hasValue() &&
        value.specific !=
            sem_ir_.values_->generics().generic(value.generic).self_specific;
    if (!sem_ir_.isCallAbiType(return_type) &&
        sem_ir_.type(return_type).kind != SemTypeKind::CoroutineTask &&
        !(is_representation_conversion &&
          (sem_ir_.type(return_type).kind == SemTypeKind::String ||
           sem_ir_.type(return_type).kind == SemTypeKind::Array)) &&
        !(is_template &&
          (sem_ir_.type(return_type).kind == SemTypeKind::TypeParameter ||
           sem_ir_.type(return_type).kind == SemTypeKind::TypeProjection))) {
      error = "function return type is outside the supported ABI";
      return false;
    }
    const auto has_body =
        declaration.kind == SemCallableDeclarationKind::Definition &&
        (value.flags & SemFunctionEvaluatorArtifact) == 0;
    const auto parameters = sem_ir_.localBlock(value.parameters);
    const auto parameter_types =
        sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
    if ((value.flags & SemFunctionCoroutineTaskDriver) != 0) {
      if (has_coroutine_task_driver || return_type != sem_ir_.i32_type_ ||
          parameter_types.size() != 1 ||
          parameter_types.front() != sem_ir_.coroutine_scope_type_) {
        error = "coroutine task driver has an invalid host ABI";
        return false;
      }
      has_coroutine_task_driver = true;
    }
    if (has_body && parameters.size() != parameter_types.size()) {
      error = "function entity has inconsistent parameter counts";
      return false;
    }
    const auto &ownership = sem_ir_.functionOwnership(
        FunctionId(static_cast<std::uint32_t>(index)));
    const auto return_loan_capability =
        sem_ir_.loanCarrierCapability(return_type);
    const auto return_kind = sem_ir_.type(return_type).kind;
    const auto dependent_return = return_kind == SemTypeKind::TypeParameter ||
                                  return_kind == SemTypeKind::TypeProjection;
    if (!ownership.verify(static_cast<std::uint32_t>(parameter_types.size()),
                          error)) {
      error = "function `" +
              std::string(sem_ir_.identifier(sem_ir_.name(value.name).text)) +
              "` ownership: " + error;
      return false;
    }
    if (!dependent_return &&
        ((return_loan_capability == SemLoanCarrierCapability::None &&
          (sem_ir_.typeRepresentation(return_type).ownership ==
           OwnershipReprKind::Borrowed) == ownership.returns_owned) ||
         (return_loan_capability == SemLoanCarrierCapability::None) !=
             ownership.return_provenance.empty())) {
      if (error.empty())
        error = "function `" +
                std::string(sem_ir_.identifier(sem_ir_.name(value.name).text)) +
                "` ownership summary disagrees with its return type";
      return false;
    }
    const auto valid_ownership_region = [&](const OwnershipRegion &region) {
      if (region.parameter_index >= parameter_types.size())
        return false;
      auto current = parameter_types[region.parameter_index];
      bool has_expected_bits = false;
      std::uint32_t expected_begin = 0;
      std::uint32_t expected_end = 0;
      if (sem_ir_.type(current).kind == SemTypeKind::Reference)
        current = sem_ir_.referencePointee(current);
      for (const auto &step : region.path) {
        has_expected_bits = false;
        expected_begin = 0;
        expected_end = 0;
        if (step.kind == OwnershipRegionStepKind::Dereference) {
          const auto kind = sem_ir_.type(current).kind;
          if (step.index != 0 || (kind != SemTypeKind::Reference &&
                                  kind != SemTypeKind::RawPointer))
            return false;
          current = kind == SemTypeKind::Reference
                        ? sem_ir_.referencePointee(current)
                        : sem_ir_.rawPointerPointee(current);
        } else if (step.kind == OwnershipRegionStepKind::Field) {
          if (sem_ir_.type(current).kind != SemTypeKind::Nominal)
            return false;
          const auto &nominal =
              sem_ir_.nominalType(NominalTypeId(sem_ir_.type(current).arg0));
          if (step.index >= nominal.fields.size())
            return false;
          const auto &field = nominal.fields[step.index];
          if (field.projection_kind == PublicObjectProjectionKind::BitPacked) {
            has_expected_bits = true;
            expected_begin = field.bit_begin;
            expected_end = field.bit_end;
          }
          current = sem_ir_.nominalFieldType(current, step.index);
        } else if (step.kind == OwnershipRegionStepKind::StaticElement ||
                   step.kind == OwnershipRegionStepKind::AnyElement) {
          const auto kind = sem_ir_.type(current).kind;
          if ((kind != SemTypeKind::Array && kind != SemTypeKind::Tuple &&
               kind != SemTypeKind::Slice) ||
              (kind == SemTypeKind::Tuple &&
               step.kind == OwnershipRegionStepKind::AnyElement) ||
              (kind == SemTypeKind::Slice &&
               (step.kind != OwnershipRegionStepKind::AnyElement ||
                step.index != 0)) ||
              (kind == SemTypeKind::Array &&
               step.kind == OwnershipRegionStepKind::StaticElement &&
               step.index >= sem_ir_.type(current).arg1) ||
              (kind == SemTypeKind::Tuple &&
               step.index >= sem_ir_.tupleArity(current)))
            return false;
          current = kind == SemTypeKind::Array || kind == SemTypeKind::Slice
                        ? TypeId(sem_ir_.type(current).arg0)
                        : sem_ir_.tupleElementType(current, step.index);
        } else {
          return false;
        }
      }
      return region.has_bit_range == has_expected_bits &&
             (!has_expected_bits || (region.bit_begin == expected_begin &&
                                     region.bit_end == expected_end));
    };
    const auto valid_carrier_path = [&](const CallableReturnSource &source) {
      auto current = return_type;
      std::optional<std::uint32_t> variant;
      for (const auto &step : source.carrier_path) {
        if (sem_ir_.type(current).kind == SemTypeKind::TypeParameter ||
            sem_ir_.type(current).kind == SemTypeKind::TypeProjection)
          return false;
        if (sem_ir_.type(current).kind != SemTypeKind::Nominal)
          return false;
        const auto &nominal =
            sem_ir_.nominalType(NominalTypeId(sem_ir_.type(current).arg0));
        if (step.kind == CallableReturnSource::CarrierStepKind::EnumVariant) {
          if (variant || nominal.kind != NominalKind::Enum ||
              step.index >= nominal.variants.size())
            return false;
          variant = step.index;
          continue;
        }
        if (step.kind != CallableReturnSource::CarrierStepKind::Field)
          return false;
        if (variant) {
          if (step.index >= nominal.variants[*variant].fields.size())
            return false;
          current = sem_ir_.enumPayloadFieldType(current, *variant, step.index);
          variant.reset();
        } else {
          if (nominal.kind == NominalKind::Enum ||
              step.index >= nominal.fields.size())
            return false;
          current = sem_ir_.nominalFieldType(current, step.index);
        }
      }
      return !variant &&
             (dependent_return ||
              sem_ir_.type(current).kind == SemTypeKind::Reference ||
              sem_ir_.type(current).kind == SemTypeKind::Slice);
    };
    const auto valid_guard = [&](const CallableConditionAtom &atom) {
      if (atom.parameter_index >= parameter_types.size()) return false;
      const auto &type = sem_ir_.type(parameter_types[atom.parameter_index]);
      if (atom.variant == core::AnyId::InvalidIndex) return type.kind == SemTypeKind::Bool;
      return type.kind == SemTypeKind::Nominal &&
             sem_ir_.nominalType(NominalTypeId(type.arg0)).kind == NominalKind::Enum &&
             atom.variant < sem_ir_.nominalType(NominalTypeId(type.arg0)).variants.size();
    };
    if (std::ranges::any_of(ownership.effects,
                            [&](const auto &effect) {
                              return !valid_ownership_region(effect.region);
                            }) ||
        std::ranges::any_of(ownership.postconditions,
                            [&](const auto &postcondition) {
                              const auto result = return_type;
                              if (postcondition.result_variant != core::AnyId::InvalidIndex &&
                                  (sem_ir_.type(result).kind != SemTypeKind::Nominal ||
                                   sem_ir_.nominalType(NominalTypeId(sem_ir_.type(result).arg0)).kind != NominalKind::Enum ||
                                   postcondition.result_variant >= sem_ir_.nominalType(NominalTypeId(sem_ir_.type(result).arg0)).variants.size()))
                                return true;
                              if (!valid_ownership_region(
                                      postcondition.region))
                                return true;
                              return std::ranges::any_of(
                                  postcondition.condition.clauses,
                                  [&](const auto &clause) {
                                    return std::ranges::any_of(
                                        clause.atoms, [&](const auto &atom) {
                                          return !valid_guard(atom);
                                        });
                                  });
                            }) ||
        std::ranges::any_of(ownership.return_provenance, [&](const auto
                                                                 &source) {
          const auto &region = source.region;
          if (!valid_ownership_region(region) ||
              region.parameter_index >= parameter_types.size() ||
              !valid_carrier_path(source))
            return true;
          const auto source_capability = sem_ir_.loanCarrierCapability(
              parameter_types[region.parameter_index]);
          return source_capability == SemLoanCarrierCapability::None ||
                 (return_loan_capability == SemLoanCarrierCapability::Mutable &&
                  source_capability != SemLoanCarrierCapability::Mutable) ||
                 std::ranges::any_of(source.condition.clauses, [&](const auto &
                                                                       clause) {
                   return std::ranges::any_of(clause.atoms, [&](const auto
                                                                    &atom) {
                     return !valid_guard(atom);
                   });
                 });
        })) {
      error = "function ownership summary has a type-invalid region path";
      return false;
    }
    if (const auto &expected = sem_ir_.expectedFunctionOwnership(
            FunctionId(static_cast<std::uint32_t>(index)));
        expected && *expected != ownership) {
      error = "cached function ownership summary disagrees with its body";
      return false;
    }
    if (sem_ir_.identifier(sem_ir_.name(value.name).text) == "main" &&
        (!parameters.empty() || return_type != sem_ir_.i32_type_)) {
      error = "source main does not match the entry ABI";
      return false;
    }
    const auto body = sem_ir_.instBlock(value.body);
    for (std::size_t parameter = 0; parameter < parameters.size();
         ++parameter) {
      if (((!sem_ir_.isCallAbiType(parameter_types[parameter]) ||
            sem_ir_.type(parameter_types[parameter]).kind ==
                SemTypeKind::Never) &&
           sem_ir_.type(parameter_types[parameter]).kind !=
               SemTypeKind::CoroutineScope &&
           sem_ir_.type(parameter_types[parameter]).kind !=
               SemTypeKind::CoroutineTask &&
           !(is_representation_conversion &&
             (sem_ir_.type(parameter_types[parameter]).kind ==
                  SemTypeKind::String ||
              sem_ir_.type(parameter_types[parameter]).kind ==
                  SemTypeKind::Array)) &&
           !(is_template && (sem_ir_.type(parameter_types[parameter]).kind ==
                                 SemTypeKind::TypeParameter ||
                             sem_ir_.type(parameter_types[parameter]).kind ==
                                 SemTypeKind::TypeProjection))) ||
          parameters[parameter].index >= sem_ir_.locals_.size() ||
          sem_ir_.local(parameters[parameter]).type !=
              parameter_types[parameter]) {
        error = "function entity has an invalid parameter";
        return false;
      }
      if (!is_declaration_specific &&
          (parameter >= body.size() ||
           sem_ir_.inst(body[parameter]).kind != SemInstKind::Parameter ||
           sem_ir_.inst(body[parameter]).arg0 != parameters[parameter].index)) {
        error = "function parameter instructions are not in signature order: " +
                std::string(sem_ir_.identifier(sem_ir_.name(value.name).text));
        return false;
      }
    }
    for (std::size_t position = is_declaration_specific ? body.size()
                                                        : parameters.size();
         position < body.size(); ++position) {
      if (sem_ir_.inst(body[position]).kind == SemInstKind::Parameter) {
        error = "function has a parameter outside its signature prefix";
        return false;
      }
    }
  }
  return true;
}

} // namespace chtholly::compiler::internal
