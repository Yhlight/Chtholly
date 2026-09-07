#include "LowIRVerificationContext.h"

#include <array>
#include <bit>
#include <ranges>
#include <string>
#include <utility>

namespace chtholly::compiler::internal {

bool LowIRVerificationContext::verifyRepresentations(std::string &error) const {
  if (low_ir_.type_representations_.size() != low_ir_.sem_ir_->typeCount()) {
    error = "LowIR has an incomplete semantic representation table";
    return false;
  }
  const auto expected_capability = [](CallableSemanticRole role) {
    return role == CallableSemanticRole::None
               ? static_cast<std::uint16_t>(CallableCapabilityNone)
               : static_cast<std::uint16_t>(
                     1U << (static_cast<unsigned>(role) - 1U));
  };
  const auto target_contract_matches =
      [&](FunctionRefId target, TypeId owner, CallableSemanticDomain domain,
          CallableSemanticRole role, std::uint32_t projector_field,
          bool whole_carrier, std::span<const std::uint32_t> carrier_path,
          bool has_bit_range, std::uint32_t bit_begin, std::uint32_t bit_end) {
        if (!target.hasValue() ||
            target.index >= low_ir_.sem_ir_->functionRefCount())
          return false;
        const auto &reference = low_ir_.sem_ir_->functionRef(target);
        const auto &function_type = low_ir_.sem_ir_->type(reference.local_type);
        if (function_type.kind != SemTypeKind::Function)
          return false;
        const auto parameters =
            low_ir_.sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
        if (parameters.empty() ||
            low_ir_.sem_ir_->type(parameters.front()).kind !=
                SemTypeKind::Reference ||
            low_ir_.sem_ir_->referencePointee(parameters.front()) != owner)
          return false;
        if (reference.local_function.hasValue()) {
          const auto &contract = low_ir_.sem_ir_->functionSemanticContract(
              reference.local_function);
          return contract.domain == domain && contract.role == role &&
                 contract.capability == expected_capability(role) &&
                 contract.projector_field == projector_field &&
                 contract.whole_carrier == whole_carrier &&
                 std::ranges::equal(contract.carrier_path, carrier_path) &&
                 contract.has_bit_range == has_bit_range &&
                 contract.bit_begin == bit_begin && contract.bit_end == bit_end;
        }
        const auto *entity =
            low_ir_.sem_ir_->importIRs().tryGetEntity(reference.public_entity);
        return entity && entity->semantic_contract.domain == domain &&
               entity->semantic_contract.role == role &&
               entity->semantic_contract.capability ==
                   expected_capability(role) &&
               entity->semantic_contract.projector_field == projector_field &&
               entity->semantic_contract.whole_carrier == whole_carrier &&
               std::ranges::equal(entity->semantic_contract.carrier_path,
                                  carrier_path) &&
               entity->semantic_contract.has_bit_range == has_bit_range &&
               entity->semantic_contract.bit_begin == bit_begin &&
               entity->semantic_contract.bit_end == bit_end;
      };
  for (std::uint32_t index = 0; index < low_ir_.type_representations_.size();
       ++index) {
    const auto type = TypeId(index);
    const auto &representation = low_ir_.type_representations_[index];
    const auto valid_target = [&](FunctionRefId target) {
      return !target.hasValue() ||
             target.index < low_ir_.sem_ir_->functionRefCount();
    };
    if (representation.facts !=
            low_ir_.sem_ir_->typeRepresentation(type) ||
        representation.value_type !=
            low_ir_.sem_ir_->valueRepresentationType(type) ||
        representation.object_type.index >= low_ir_.sem_ir_->typeCount() ||
        representation.value_type.index >= low_ir_.sem_ir_->typeCount() ||
        (representation.facts.value_repr == ValueReprKind::Custom &&
         (!representation.pack_target.hasValue() ||
          !representation.init_target.hasValue())) ||
        (representation.facts.value_repr != ValueReprKind::Custom &&
         (representation.pack_target.hasValue() ||
          representation.init_target.hasValue())) ||
        (representation.copy_target.hasValue() &&
         representation.facts.copy != CopyReprKind::Custom) ||
        (representation.destroy_target.hasValue() &&
         representation.facts.destroy != DestroyReprKind::Custom) ||
        ((representation.object_init_target.hasValue() ||
          representation.object_copy_init_target.hasValue() ||
          representation.object_move_init_target.hasValue() ||
          representation.object_drop_target.hasValue()) &&
         !(representation.object_init_target.hasValue() &&
           representation.object_copy_init_target.hasValue() &&
           representation.object_move_init_target.hasValue() &&
           representation.object_drop_target.hasValue())) ||
        !valid_target(representation.pack_target) ||
        !valid_target(representation.init_target) ||
        !valid_target(representation.copy_target) ||
        !valid_target(representation.destroy_target) ||
        !valid_target(representation.object_init_target) ||
        !valid_target(representation.object_copy_init_target) ||
        !valid_target(representation.object_move_init_target) ||
        !valid_target(representation.object_drop_target)) {
      error = "LowIR has a stale semantic representation entry for type" +
              std::to_string(type.index);
      return false;
    }
    const std::span<const std::uint32_t> no_path;
    if ((representation.pack_target.hasValue() &&
         !target_contract_matches(representation.pack_target, type,
                                  CallableSemanticDomain::ValueRepresentation,
                                  CallableSemanticRole::Pack,
                                  core::AnyId::InvalidIndex, true, no_path,
                                  false, 0, 0)) ||
        (representation.init_target.hasValue() &&
         !target_contract_matches(representation.init_target, type,
                                  CallableSemanticDomain::ValueRepresentation,
                                  CallableSemanticRole::Init,
                                  core::AnyId::InvalidIndex, true, no_path,
                                  false, 0, 0)) ||
        (representation.copy_target.hasValue() &&
         !target_contract_matches(
             representation.copy_target, type,
             CallableSemanticDomain::Lifecycle, CallableSemanticRole::Copy,
             core::AnyId::InvalidIndex, false, no_path, false, 0, 0)) ||
        (representation.destroy_target.hasValue() &&
         !target_contract_matches(
             representation.destroy_target, type,
             CallableSemanticDomain::Lifecycle, CallableSemanticRole::Drop,
             core::AnyId::InvalidIndex, false, no_path, false, 0, 0)) ||
        (representation.object_init_target.hasValue() &&
         !target_contract_matches(representation.object_init_target, type,
                                  CallableSemanticDomain::ObjectShell,
                                  CallableSemanticRole::ObjectInit,
                                  core::AnyId::InvalidIndex, true, no_path,
                                  false, 0, 0)) ||
        (representation.object_copy_init_target.hasValue() &&
         !target_contract_matches(representation.object_copy_init_target, type,
                                  CallableSemanticDomain::ObjectShell,
                                  CallableSemanticRole::ObjectCopyInit,
                                  core::AnyId::InvalidIndex, true, no_path,
                                  false, 0, 0)) ||
        (representation.object_move_init_target.hasValue() &&
         !target_contract_matches(representation.object_move_init_target, type,
                                  CallableSemanticDomain::ObjectShell,
                                  CallableSemanticRole::ObjectMoveInit,
                                  core::AnyId::InvalidIndex, true, no_path,
                                  false, 0, 0)) ||
        (representation.object_drop_target.hasValue() &&
         !target_contract_matches(representation.object_drop_target, type,
                                  CallableSemanticDomain::ObjectShell,
                                  CallableSemanticRole::ObjectDrop,
                                  core::AnyId::InvalidIndex, true, no_path,
                                  false, 0, 0))) {
      error = "LowIR representation target has an unauthorized contract";
      return false;
    }
    if (low_ir_.sem_ir_->type(type).kind == SemTypeKind::Nominal &&
        representation.field_projections.size() !=
            low_ir_.sem_ir_
                ->nominalType(NominalTypeId(low_ir_.sem_ir_->type(type).arg0))
                .fields.size()) {
      error = "LowIR has an incomplete object projection table";
      return false;
    }
    if (low_ir_.sem_ir_->type(type).kind == SemTypeKind::Nominal) {
      constexpr auto BaseCapabilities =
          ProjectionLoad | ProjectionStore | ProjectionTake | ProjectionInit;
      constexpr auto AllCapabilities =
          BaseCapabilities | ProjectionBorrow | ProjectionBorrowMut;
      for (std::uint32_t field = 0;
           field < representation.field_projections.size(); ++field) {
        const auto &projection = representation.field_projections[field];
        const std::array targets{
            projection.load_target,   projection.store_target,
            projection.take_target,   projection.init_target,
            projection.borrow_target, projection.borrow_mut_target};
        if (projection.kind >= ObjectFieldProjectionKind::Count ||
            (projection.capabilities & ~AllCapabilities) != 0 ||
            (projection.capabilities & BaseCapabilities) != BaseCapabilities ||
            std::ranges::any_of(targets, [&](FunctionRefId target) {
              return !valid_target(target);
            })) {
          error = "LowIR has an invalid object projection capability record";
          return false;
        }
        const auto target_matches_capability =
            [&](FunctionRefId target, ObjectProjectionCapability capability) {
              return target.hasValue() ==
                     ((projection.capabilities & capability) != 0);
            };
        if (projection.kind == ObjectFieldProjectionKind::Computed) {
          if (!projection.physical_steps.empty() || projection.bit_begin != 0 ||
              projection.bit_end != 0) {
            error = "LowIR computed object projector has physical metadata";
            return false;
          }
          const std::array capability_targets{
              std::pair{projection.load_target, ProjectionLoad},
              std::pair{projection.store_target, ProjectionStore},
              std::pair{projection.take_target, ProjectionTake},
              std::pair{projection.init_target, ProjectionInit},
              std::pair{projection.borrow_target, ProjectionBorrow},
              std::pair{projection.borrow_mut_target, ProjectionBorrowMut}};
          if (const auto invalid = std::ranges::find_if(
                  capability_targets,
                  [&](const auto &entry) {
                    if (!target_matches_capability(entry.first, entry.second))
                      return true;
                    const auto role = static_cast<CallableSemanticRole>(
                        static_cast<unsigned>(
                            CallableSemanticRole::ProjectionLoad) +
                        std::countr_zero(static_cast<unsigned>(entry.second)));
                    return entry.first.hasValue() &&
                           !target_contract_matches(
                               entry.first, type,
                               CallableSemanticDomain::ObjectProjection, role,
                               field, projection.region_indices.empty(),
                               projection.region_indices,
                               projection.bit_begin != 0 ||
                                   projection.bit_end != 0,
                               projection.bit_begin, projection.bit_end);
                  });
              invalid != capability_targets.end()) {
            error = "LowIR computed object projector target disagrees with "
                    "capability " +
                    std::to_string(static_cast<unsigned>(invalid->second));
            return false;
          }
        } else {
          const auto union_projection =
              projection.physical_steps.empty() &&
              low_ir_.sem_ir_->type(representation.object_type).kind ==
                  SemTypeKind::Nominal &&
              low_ir_.sem_ir_
                      ->nominalType(NominalTypeId(
                          low_ir_.sem_ir_->type(representation.object_type)
                              .arg0))
                      .kind == NominalKind::Union;
          if ((!union_projection && projection.physical_steps.empty()) ||
              std::ranges::any_of(targets, &FunctionRefId::hasValue) ||
              (projection.kind == ObjectFieldProjectionKind::StableAddress &&
               (projection.capabilities != AllCapabilities ||
                projection.bit_begin != 0 || projection.bit_end != 0)) ||
              (projection.kind == ObjectFieldProjectionKind::BitPacked &&
               (projection.capabilities != BaseCapabilities ||
                projection.bit_begin >= projection.bit_end ||
                projection.bit_end > 32))) {
            error = "LowIR has an invalid physical object projector";
            return false;
          }
          auto current = union_projection
                             ? low_ir_.sem_ir_->nominalFieldType(type, field)
                             : representation.object_type;
          for (const auto step : projection.physical_steps) {
            if (step.aggregate_type != current ||
                low_ir_.sem_ir_->type(current).kind != SemTypeKind::Nominal ||
                step.field_index >=
                    low_ir_.sem_ir_
                        ->nominalType(
                            NominalTypeId(low_ir_.sem_ir_->type(current).arg0))
                        .fields.size()) {
              error = "LowIR object projector has an invalid physical path";
              return false;
            }
            current =
                low_ir_.sem_ir_->nominalFieldType(current, step.field_index);
          }
          const auto logical = low_ir_.sem_ir_->nominalFieldType(type, field);
          if ((projection.kind == ObjectFieldProjectionKind::StableAddress &&
               current != logical) ||
              (projection.kind == ObjectFieldProjectionKind::BitPacked &&
               (current != low_ir_.sem_ir_->i32Type() ||
                logical != low_ir_.sem_ir_->i32Type()))) {
            error = "LowIR object projector changes its logical field type";
            return false;
          }
        }
      }
    }
    if (low_ir_.sem_ir_->type(representation.object_type).kind ==
            SemTypeKind::Nominal &&
        (representation.object_fields.size() !=
             low_ir_.sem_ir_
                 ->nominalType(NominalTypeId(
                     low_ir_.sem_ir_->type(representation.object_type).arg0))
                 .fields.size() ||
         std::ranges::any_of(representation.object_fields, [&](TypeId field) {
           return !field.hasValue() ||
                  field.index >= low_ir_.sem_ir_->typeCount();
         }))) {
      error = "LowIR has an invalid physical object field table";
      return false;
    }
  }
  return true;
}

} // namespace chtholly::compiler::internal
