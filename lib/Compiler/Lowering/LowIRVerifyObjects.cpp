#include "LowIRVerificationContext.h"

#include <ranges>

namespace chtholly::compiler::internal {

bool LowIRVerificationContext::verifyObjectInstruction(
    LowInstId id, std::string &error) const {
  const auto &value = low_ir_.inst(id);
  const auto *sem_ir_ = low_ir_.sem_ir_;
  const auto &insts_ = low_ir_.insts_;
  const auto instruction_type = TypeId(value.type);
  const auto value_type = [&](std::uint32_t raw) {
    return TypeId(low_ir_.inst(LowInstId(raw)).type);
  };
  const auto valueBlock = [&](LowValueBlockId value_id) {
    return low_ir_.valueBlock(value_id);
  };
  const auto inst = [&](LowInstId value_id) -> const LowInst & {
    return low_ir_.inst(value_id);
  };
  const auto slot = [&](SlotId value_id) -> const LowSlot & {
    return low_ir_.slot(value_id);
  };
  const auto place = [&](LowPlaceId value_id) -> const LowPlace & {
    return low_ir_.place(value_id);
  };
  const auto typeRepresentation = [&](TypeId value_id) -> const auto & {
    return low_ir_.typeRepresentation(value_id);
  };
  const auto target_contract_matches =
      [&](FunctionRefId target, TypeId owner, CallableSemanticDomain domain,
          CallableSemanticRole role, std::uint32_t projector_field,
          bool whole_carrier, std::span<const std::uint32_t> carrier_path,
          bool has_bit_range, std::uint32_t bit_begin, std::uint32_t bit_end) {
        if (!target.hasValue() || target.index >= sem_ir_->functionRefCount())
          return false;
        const auto &reference = sem_ir_->functionRef(target);
        const auto &function_type = sem_ir_->type(reference.local_type);
        if (function_type.kind != SemTypeKind::Function)
          return false;
        const auto parameters =
            sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
        if (parameters.empty() ||
            sem_ir_->type(parameters.front()).kind != SemTypeKind::Reference ||
            sem_ir_->referencePointee(parameters.front()) != owner)
          return false;
        const auto expected_capability = [](CallableSemanticRole candidate) {
          return candidate == CallableSemanticRole::None
                     ? static_cast<std::uint16_t>(CallableCapabilityNone)
                     : static_cast<std::uint16_t>(
                           1U << (static_cast<unsigned>(candidate) - 1U));
        };
        if (reference.local_function.hasValue()) {
          const auto &contract =
              sem_ir_->functionSemanticContract(reference.local_function);
          return contract.domain == domain && contract.role == role &&
                 contract.capability == expected_capability(role) &&
                 contract.projector_field == projector_field &&
                 contract.whole_carrier == whole_carrier &&
                 std::ranges::equal(contract.carrier_path, carrier_path) &&
                 contract.has_bit_range == has_bit_range &&
                 contract.bit_begin == bit_begin && contract.bit_end == bit_end;
        }
        const auto *entity =
            sem_ir_->importIRs().tryGetEntity(reference.public_entity);
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
  const auto matches_lifecycle_target =
      [&](FunctionRefId target, TypeId type, SemCanonicalFunctionRole role) {
        if (!target.hasValue() ||
            target.index >= sem_ir_->functionRefCount() ||
            sem_ir_->type(type).kind != SemTypeKind::Nominal)
          return false;
        const std::span<const std::uint32_t> no_path;
        const bool intrinsic_vec_drop =
            role == SemCanonicalFunctionRole::Drop &&
            sem_ir_->functionIntrinsicRole(target) == CompilerIntrinsicRole::VecDrop;
        if (!intrinsic_vec_drop &&
            !target_contract_matches(
                target, type, CallableSemanticDomain::Lifecycle, role,
                core::AnyId::InvalidIndex, false, no_path, false, 0, 0))
          return false;
        const auto &reference = sem_ir_->functionRef(target);
        const auto &function_type = sem_ir_->type(reference.local_type);
        if (function_type.kind != SemTypeKind::Function ||
            TypeId(function_type.arg1) != sem_ir_->voidType())
          return false;
        const auto parameters =
            sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
        const auto matches_reference =
            [&](TypeId parameter, SemReferenceMutability mutability) {
              return sem_ir_->type(parameter).kind == SemTypeKind::Reference &&
                     sem_ir_->referencePointee(parameter) == type &&
                     sem_ir_->referenceMutability(parameter) == mutability;
            };
        if ((role == SemCanonicalFunctionRole::Copy &&
             (parameters.size() != 2 ||
              !matches_reference(parameters[0],
                                 SemReferenceMutability::Mutable) ||
              !matches_reference(parameters[1],
                                 SemReferenceMutability::ReadOnly))) ||
            (role == SemCanonicalFunctionRole::Drop &&
             (parameters.size() != 1 ||
              !matches_reference(parameters[0],
                                 SemReferenceMutability::Mutable))))
          return false;
        if (reference.local_function.hasValue()) {
          const auto &contract =
              sem_ir_->functionSemanticContract(reference.local_function);
          return contract.owner == NominalTypeId(sem_ir_->type(type).arg0) &&
                 contract.role == role;
        }
        const auto *witness = sem_ir_->nominalSemanticWitness(type);
        const auto *expected =
            witness && role == SemCanonicalFunctionRole::Copy &&
                    witness->copy_target
                ? &*witness->copy_target
            : witness && role == SemCanonicalFunctionRole::Drop &&
                    witness->destroy_target
                ? &*witness->destroy_target
                : nullptr;
        const auto *entity =
            sem_ir_->importIRs().tryGetEntity(reference.public_entity);
        return expected && entity &&
               sem_ir_->identifier(entity->package_name) ==
                   expected->canonical_package &&
               sem_ir_->identifier(entity->module_name) ==
                   expected->canonical_module &&
               sem_ir_->identifier(entity->name) == expected->canonical_name &&
               entity->fingerprint == expected->expected_fingerprint;
      };
  const auto matches_conversion_target =
      [&](FunctionRefId target, TypeId type,
          SemCanonicalFunctionRole role) {
        if (!target.hasValue() || target.index >= sem_ir_->functionRefCount() ||
            sem_ir_->type(type).kind != SemTypeKind::Nominal)
          return false;
        const std::span<const std::uint32_t> no_path;
        if (!target_contract_matches(
                target, type, CallableSemanticDomain::ValueRepresentation, role,
                core::AnyId::InvalidIndex, true, no_path, false, 0, 0))
          return false;
        const auto &representation = typeRepresentation(type);
        if (representation.facts.value_repr != ValueReprKind::Custom ||
            (role == SemCanonicalFunctionRole::Pack
                 ? representation.pack_target != target
                 : representation.init_target != target))
          return false;
        const auto &function_type =
            sem_ir_->type(sem_ir_->functionRef(target).local_type);
        if (function_type.kind != SemTypeKind::Function)
          return false;
        const auto parameters =
            sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
        const auto owner_ref = [&](TypeId parameter,
                                   SemReferenceMutability mutability) {
          return sem_ir_->type(parameter).kind == SemTypeKind::Reference &&
                 sem_ir_->referencePointee(parameter) == type &&
                 sem_ir_->referenceMutability(parameter) == mutability;
        };
        return role == SemCanonicalFunctionRole::Pack
                   ? parameters.size() == 1 &&
                         owner_ref(parameters[0],
                                   SemReferenceMutability::ReadOnly) &&
                         TypeId(function_type.arg1) == representation.value_type
                   : parameters.size() == 2 &&
                         owner_ref(parameters[0],
                                   SemReferenceMutability::Mutable) &&
                         parameters[1] == representation.value_type &&
                         TypeId(function_type.arg1) == sem_ir_->voidType();
      };
  switch (value.kind) {
    case LowInstKind::StaticLoad: {
      const auto &entity =
          sem_ir_->constantEntity(ConstantEntityId(value.arg0));
      if ((entity.flags & SemConstantStatic) == 0 ||
          !entity.result.isConcrete() || entity.type != instruction_type) {
        error = "static load does not match a concrete static entity";
        return false;
      }
      break;
    }
    case LowInstKind::MakeArray: {
      const auto &array_type = sem_ir_->type(instruction_type);
      const auto elements = valueBlock(LowValueBlockId(value.arg0));
      if (array_type.kind != SemTypeKind::Array ||
          array_type.arg1 != elements.size()) {
        error = "array construction has an invalid type";
        return false;
      }
      for (const auto element : elements) {
        if (element.index >= insts_.size() ||
            inst(element).type != array_type.arg0) {
          error = "array construction has an invalid element";
          return false;
        }
      }
      break;
    }
    case LowInstKind::MakeTuple: {
      const auto &tuple_type = sem_ir_->type(instruction_type);
      const auto elements = valueBlock(LowValueBlockId(value.arg0));
      if (tuple_type.kind != SemTypeKind::Tuple) {
        error = "tuple construction has an invalid type";
        return false;
      }
      const auto element_types =
          sem_ir_->typeBlock(TypeBlockId(tuple_type.arg0));
      if (element_types.size() != elements.size()) {
        error = "tuple construction has an invalid arity";
        return false;
      }
      for (std::size_t element_index = 0; element_index < elements.size();
           ++element_index) {
        if (elements[element_index].index >= insts_.size() ||
            inst(elements[element_index]).type !=
                element_types[element_index].index) {
          error = "tuple construction has an invalid element";
          return false;
        }
      }
      break;
    }
    case LowInstKind::MakeAggregate: {
      const auto &aggregate_type = sem_ir_->type(instruction_type);
      const auto fields = valueBlock(LowValueBlockId(value.arg0));
      if (aggregate_type.kind != SemTypeKind::Nominal ||
          sem_ir_->nominalType(NominalTypeId(aggregate_type.arg0)).kind !=
              NominalKind::Struct ||
          fields.size() !=
              sem_ir_->nominalType(NominalTypeId(aggregate_type.arg0))
                  .fields.size() ||
          std::ranges::any_of(fields, [&](LowInstId field) {
            return field.index >= insts_.size();
          })) {
        error = "aggregate construction has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::MakeUnion: {
      const auto &aggregate_type = sem_ir_->type(instruction_type);
      const auto member = sem_ir_->integer(IntegerId(value.arg1));
      if (aggregate_type.kind != SemTypeKind::Nominal || member < 0) {
        error = "union construction has invalid types";
        return false;
      }
      const auto &nominal =
          sem_ir_->nominalType(NominalTypeId(aggregate_type.arg0));
      if (nominal.kind != NominalKind::Union ||
          static_cast<std::size_t>(member) >= nominal.fields.size() ||
          value_type(value.arg0) !=
              sem_ir_->nominalFieldType(instruction_type,
                                        static_cast<std::uint32_t>(member))) {
        error = "union construction does not match its active member";
        return false;
      }
      break;
    }
    case LowInstKind::MakeEnum: {
      const auto &enum_type = sem_ir_->type(instruction_type);
      const auto variant = sem_ir_->integer(IntegerId(value.arg1));
      if (enum_type.kind != SemTypeKind::Nominal || variant < 0) {
        error = "enum construction has invalid types";
        return false;
      }
      const auto &nominal = sem_ir_->nominalType(NominalTypeId(enum_type.arg0));
      const auto payload = valueBlock(LowValueBlockId(value.arg0));
      if (nominal.kind != NominalKind::Enum ||
          static_cast<std::size_t>(variant) >= nominal.variants.size() ||
          payload.size() != nominal.variants[variant].fields.size()) {
        error = "enum construction does not match its active variant";
        return false;
      }
      for (std::uint32_t field = 0; field < payload.size(); ++field)
        if (const auto expected = sem_ir_->enumPayloadFieldType(
                instruction_type, static_cast<std::uint32_t>(variant), field);
            value_type(payload[field].index) != expected) {
          error = "enum construction has payload type" +
                  std::to_string(value_type(payload[field].index).index) +
                  " but expected type" + std::to_string(expected.index);
          return false;
        }
      break;
    }
    case LowInstKind::MakeObject:
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::Nominal) {
        error = "object shell construction has a non-nominal type";
        return false;
      }
      break;
    case LowInstKind::MakeObjectCopy:
    case LowInstKind::MakeObjectMove: {
      const auto &representation = typeRepresentation(instruction_type);
      const auto target = value.kind == LowInstKind::MakeObjectCopy
                              ? representation.object_copy_init_target
                              : representation.object_move_init_target;
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::Nominal ||
          value_type(value.arg0) != instruction_type || !target.hasValue()) {
        error = "object shell transfer does not match its lifecycle witness";
        return false;
      }
      break;
    }
    case LowInstKind::Load:
      if (instruction_type != slot(SlotId(value.arg0)).type) {
        error = "load type does not match its slot";
        return false;
      }
      break;
    case LowInstKind::LoadPlace:
      if (instruction_type != place(LowPlaceId(value.arg0)).type ||
          (place(LowPlaceId(value.arg0)).flags & LowPlaceAddressable) == 0) {
        error = "place load does not match an addressable logical place";
        return false;
      }
      break;
    case LowInstKind::Borrow:
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::Reference ||
          sem_ir_->referencePointee(instruction_type) !=
              slot(SlotId(value.arg0)).type) {
        error = "borrow does not match its source slot";
        return false;
      }
      break;
    case LowInstKind::BorrowPlace: {
      const auto &source = place(LowPlaceId(value.arg0));
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::Reference ||
          sem_ir_->referencePointee(instruction_type) != source.type ||
          (source.flags & LowPlaceAddressable) == 0) {
        error = "place borrow does not match its source place";
        return false;
      }
      break;
    }
    case LowInstKind::CarrierView: {
      const auto input_type = value_type(value.arg0);
      if (sem_ir_->type(input_type).kind != SemTypeKind::Reference ||
          sem_ir_->type(instruction_type).kind != SemTypeKind::Reference ||
          sem_ir_->referenceMutability(input_type) !=
              sem_ir_->referenceMutability(instruction_type)) {
        error = "carrier view has invalid reference facts";
        return false;
      }
      break;
    }
    case LowInstKind::ObjectAddress:
      if (instruction_type != slot(SlotId(value.arg0)).type) {
        error = "object address does not match its source slot";
        return false;
      }
      break;
    case LowInstKind::PlaceAddress: {
      const auto &target = place(LowPlaceId(value.arg0));
      if (instruction_type != target.type ||
          (target.flags & LowPlaceAddressable) == 0) {
        error = "place address targets a non-addressable logical place";
        return false;
      }
      break;
    }
    case LowInstKind::Dereference: {
      const auto operand_type = value_type(value.arg0);
      const auto operand_kind = sem_ir_->type(operand_type).kind;
      const auto pointee = operand_kind == SemTypeKind::Reference
                               ? sem_ir_->referencePointee(operand_type)
                           : operand_kind == SemTypeKind::RawPointer
                               ? sem_ir_->rawPointerPointee(operand_type)
                               : TypeId::invalid();
      if (!pointee.hasValue() || pointee != instruction_type) {
        error = "dereference has invalid indirection facts";
        return false;
      }
      break;
    }
    case LowInstKind::DereferenceObject: {
      const auto operand_type = value_type(value.arg0);
      const auto operand_kind = sem_ir_->type(operand_type).kind;
      const auto pointee = operand_kind == SemTypeKind::Reference
                               ? sem_ir_->referencePointee(operand_type)
                           : operand_kind == SemTypeKind::RawPointer
                               ? sem_ir_->rawPointerPointee(operand_type)
                               : TypeId::invalid();
      if (!pointee.hasValue() || pointee != instruction_type ||
          (typeRepresentation(instruction_type).facts.value_repr !=
               ValueReprKind::Custom &&
           typeRepresentation(instruction_type).facts.object_repr !=
               ObjectReprKind::Custom)) {
        error = "object dereference has invalid representation types";
        return false;
      }
      break;
    }
    case LowInstKind::PackValue:
      if ((instruction_type != value_type(value.arg0) &&
           (sem_ir_->type(value_type(value.arg0)).kind !=
                SemTypeKind::Reference ||
            sem_ir_->referencePointee(value_type(value.arg0)) !=
                instruction_type)) ||
          !matches_conversion_target(
              typeRepresentation(instruction_type).pack_target,
              instruction_type, SemCanonicalFunctionRole::Pack)) {
        error = "pack operation does not match its representation witness";
        return false;
      }
      break;
    case LowInstKind::UnpackValue:
      if (instruction_type != value_type(value.arg0) ||
          !matches_conversion_target(
              typeRepresentation(instruction_type).init_target,
              instruction_type, SemCanonicalFunctionRole::Init)) {
        error = "unpack operation does not match its representation witness";
        return false;
      }
      break;
    case LowInstKind::Initialize:
    case LowInstKind::Transfer:
      if (instruction_type != sem_ir_->voidType() ||
          slot(SlotId(value.arg0)).type != value_type(value.arg1)) {
        error = "initialization action does not match its slot";
        return false;
      }
      if (value.kind == LowInstKind::Transfer &&
          sem_ir_->typeRepresentation(slot(SlotId(value.arg0)).type)
                  .ownership != OwnershipReprKind::Owned) {
        error = "transfer targets a non-owning slot";
        return false;
      }
      break;
    case LowInstKind::InitializeFromValue:
      if (instruction_type != sem_ir_->voidType() ||
          slot(SlotId(value.arg0)).type != value_type(value.arg1) ||
          typeRepresentation(value_type(value.arg1)).facts.init_repr !=
              InitReprKind::ByConversion) {
        error = "conversion initialization does not match its slot";
        return false;
      }
      break;
    case LowInstKind::MoveOut: {
      const auto &place_value = place(LowPlaceId(value.arg1));
      if (instruction_type != value_type(value.arg0) ||
          instruction_type != place_value.type) {
        error = "move-out action low" + std::to_string(id.index) +
                " has result/source/place types " +
                std::to_string(instruction_type.index) + "/" +
                std::to_string(value_type(value.arg0).index) + "/" +
                std::to_string(place_value.type.index);
        return false;
      }
      break;
    }
    case LowInstKind::CopyValue:
      if (instruction_type != value_type(value.arg0) ||
          sem_ir_->typeRepresentation(instruction_type).copy ==
              CopyReprKind::Unavailable) {
        error = "copy action has an unavailable or mismatched type";
        return false;
      }
      break;
    case LowInstKind::LifecycleCopy: {
      if (instruction_type != value_type(value.arg1) ||
          sem_ir_->typeRepresentation(instruction_type).copy !=
              CopyReprKind::Custom ||
          !matches_lifecycle_target(FunctionRefId(value.arg0), instruction_type,
                                    SemCanonicalFunctionRole::Copy)) {
        error =
            "custom copy action does not match its nominal semantic witness";
        return false;
      }
      break;
    }
    case LowInstKind::InitializePlace: {
      const auto &target = place(LowPlaceId(value.arg0));
      if (instruction_type != sem_ir_->voidType() ||
          target.type != value_type(value.arg1)) {
        error = "place initialization does not match its target";
        return false;
      }
      break;
    }
    case LowInstKind::InitializePlaceFromValue: {
      const auto &target = place(LowPlaceId(value.arg0));
      if (instruction_type != sem_ir_->voidType() ||
          target.type != value_type(value.arg1) ||
          typeRepresentation(target.type).facts.init_repr !=
              InitReprKind::ByConversion) {
        error = "conversion initialization does not match its place";
        return false;
      }
      break;
    }
    case LowInstKind::StringLength:
      if (instruction_type != sem_ir_->i32Type() ||
          value_type(value.arg0) != sem_ir_->stringType()) {
        error = "string length has invalid types";
        return false;
      }
      break;
    case LowInstKind::SliceLength:
      if ((instruction_type != sem_ir_->i32Type() &&
           (sem_ir_->type(instruction_type).kind != SemTypeKind::Integer ||
            sem_ir_->type(instruction_type).arg0 != 64 ||
            sem_ir_->type(instruction_type).arg1 != 0)) ||
          sem_ir_->type(value_type(value.arg0)).kind != SemTypeKind::Slice) {
        error = "slice length has invalid types";
        return false;
      }
      break;
    case LowInstKind::MakeSlice: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      const auto valid_index = [&](TypeId type) {
        return type == sem_ir_->i32Type() ||
               (sem_ir_->type(type).kind == SemTypeKind::Integer &&
                sem_ir_->type(type).arg0 == 64 &&
                sem_ir_->type(type).arg1 == 0);
      };
      if (operands.size() != 3 ||
          sem_ir_->type(instruction_type).kind != SemTypeKind::Slice ||
          sem_ir_->type(value_type(operands[0].index)).kind !=
              SemTypeKind::Reference ||
          !valid_index(value_type(operands[1].index)) ||
          !valid_index(value_type(operands[2].index)) ||
          value_type(operands[1].index) != value_type(operands[2].index)) {
        error = "slice construction has invalid low-level types";
        return false;
      }
      break;
    }
    case LowInstKind::TupleElement: {
      const auto tuple_type = value_type(value.arg0);
      const auto &tuple = sem_ir_->type(tuple_type);
      const auto element_index = sem_ir_->integer(IntegerId(value.arg1));
      if (tuple.kind != SemTypeKind::Tuple || element_index < 0 ||
          static_cast<std::size_t>(element_index) >=
              sem_ir_->typeBlock(TypeBlockId(tuple.arg0)).size() ||
          instruction_type !=
              sem_ir_->tupleElementType(
                  tuple_type, static_cast<std::uint32_t>(element_index))) {
        error = "tuple element projection has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::StructField: {
      const auto &base = sem_ir_->type(value_type(value.arg0));
      const auto field = sem_ir_->integer(IntegerId(value.arg1));
      if (base.kind != SemTypeKind::Nominal || field < 0 ||
          static_cast<std::size_t>(field) >=
              sem_ir_->nominalType(NominalTypeId(base.arg0)).fields.size() ||
          sem_ir_->nominalType(NominalTypeId(base.arg0)).kind !=
              NominalKind::Struct) {
        error = "struct field projection has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::UnionField: {
      const auto owner = value_type(value.arg0);
      const auto &base = sem_ir_->type(owner);
      auto field = sem_ir_->integer(IntegerId(value.arg1));
      if (field == std::numeric_limits<std::int64_t>::min()) {
        error = "union field projection has invalid types";
        return false;
      }
      if (field < 0)
        field = -field - 1;
      if (base.kind != SemTypeKind::Nominal || field < 0) {
        error = "union field projection has invalid types";
        return false;
      }
      const auto &nominal = sem_ir_->nominalType(NominalTypeId(base.arg0));
      if (nominal.kind != NominalKind::Union ||
          static_cast<std::size_t>(field) >= nominal.fields.size() ||
          instruction_type != sem_ir_->nominalFieldType(
                                  owner, static_cast<std::uint32_t>(field))) {
        error = "union field projection does not match its member";
        return false;
      }
      break;
    }
    case LowInstKind::EnumTag: {
      const auto owner = value_type(value.arg0);
      const auto &base = sem_ir_->type(owner);
      if (instruction_type != sem_ir_->i32Type() ||
          base.kind != SemTypeKind::Nominal ||
          sem_ir_->nominalType(NominalTypeId(base.arg0)).kind !=
              NominalKind::Enum) {
        error = "enum tag projection has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::EnumPayload: {
      const auto owner = value_type(value.arg0);
      const auto &base = sem_ir_->type(owner);
      const auto encoded =
          static_cast<std::uint64_t>(sem_ir_->integer(IntegerId(value.arg1)));
      const auto variant = static_cast<std::uint32_t>(encoded >> 32U);
      const auto field = static_cast<std::uint32_t>(encoded);
      if (base.kind != SemTypeKind::Nominal ||
          sem_ir_->nominalType(NominalTypeId(base.arg0)).kind !=
              NominalKind::Enum ||
          instruction_type !=
              sem_ir_->enumPayloadFieldType(owner, variant, field)) {
        error = "enum payload projection has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::ProjectionLoad:
    case LowInstKind::ProjectionTake:
    case LowInstKind::ProjectionBorrow:
    case LowInstKind::ProjectionBorrowMut: {
      const auto owner = value_type(value.arg0);
      if (sem_ir_->type(owner).kind != SemTypeKind::Nominal) {
        error = "object projection has a non-nominal owner";
        return false;
      }
      const auto &representation = typeRepresentation(owner);
      if (value.arg1 >= representation.field_projections.size()) {
        error = "object projection has an out-of-range field";
        return false;
      }
      const auto field_type = sem_ir_->nominalFieldType(owner, value.arg1);
      const auto &projection = representation.field_projections[value.arg1];
      const auto capability =
          value.kind == LowInstKind::ProjectionLoad     ? ProjectionLoad
          : value.kind == LowInstKind::ProjectionTake   ? ProjectionTake
          : value.kind == LowInstKind::ProjectionBorrow ? ProjectionBorrow
                                                        : ProjectionBorrowMut;
      if ((projection.capabilities & capability) == 0) {
        error = "object projection uses an unavailable capability";
        return false;
      }
      if (value.kind == LowInstKind::ProjectionLoad ||
          value.kind == LowInstKind::ProjectionTake) {
        if (instruction_type != field_type) {
          error = "object projection read has the wrong field type";
          return false;
        }
      } else {
        const auto expected_mutability =
            value.kind == LowInstKind::ProjectionBorrowMut
                ? SemReferenceMutability::Mutable
                : SemReferenceMutability::ReadOnly;
        if (sem_ir_->type(instruction_type).kind != SemTypeKind::Reference ||
            sem_ir_->referencePointee(instruction_type) != field_type ||
            sem_ir_->referenceMutability(instruction_type) !=
                expected_mutability) {
          error = "object projection borrow has the wrong reference type";
          return false;
        }
      }
      break;
    }
    case LowInstKind::ProjectionStore:
    case LowInstKind::ProjectionInit: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      if (instruction_type != sem_ir_->voidType() || operands.size() != 2 ||
          operands[0].index >= insts_.size() ||
          operands[1].index >= insts_.size()) {
        error = "object projection write has invalid operands";
        return false;
      }
      const auto owner = TypeId(inst(operands[0]).type);
      if (sem_ir_->type(owner).kind != SemTypeKind::Nominal) {
        error = "object projection write has a non-nominal owner";
        return false;
      }
      const auto &representation = typeRepresentation(owner);
      if (value.arg1 >= representation.field_projections.size()) {
        error = "object projection write has an out-of-range field";
        return false;
      }
      const auto capability = value.kind == LowInstKind::ProjectionStore
                                  ? ProjectionStore
                                  : ProjectionInit;
      if ((representation.field_projections[value.arg1].capabilities &
           capability) == 0 ||
          TypeId(inst(operands[1]).type) !=
              sem_ir_->nominalFieldType(owner, value.arg1)) {
        error = "object projection write does not match its field";
        return false;
      }
      break;
    }
    case LowInstKind::IndexStore: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      if (operands.size() != 2 || instruction_type != sem_ir_->voidType()) {
        error = "indexed store has invalid operands"; return false;
      }
      const auto &array = sem_ir_->type(place(LowPlaceId(value.arg1)).type);
      const auto &index = sem_ir_->type(value_type(operands[0].index));
      if (array.kind != SemTypeKind::Array || array.arg0 != value_type(operands[1].index).index ||
          index.kind != SemTypeKind::Integer || (index.arg0 != 32 && index.arg0 != 64)) {
        error = "indexed store has invalid types"; return false;
      }
      break;
    }
    case LowInstKind::ArrayIndex: {
      const auto base_type = value_type(value.arg0);
      const auto &base = sem_ir_->type(base_type);
      const auto aggregate_type = base.kind == SemTypeKind::Reference
                                      ? sem_ir_->referencePointee(base_type)
                                      : base_type;
      const auto &array_type = sem_ir_->type(aggregate_type);
      const auto &result_type = sem_ir_->type(instruction_type);
      const auto element_type =
          result_type.kind == SemTypeKind::Reference
              ? sem_ir_->referencePointee(instruction_type)
              : instruction_type;
      const auto valid_index = [&](TypeId type) {
        return type == sem_ir_->i32Type() ||
               (sem_ir_->type(type).kind == SemTypeKind::Integer &&
                sem_ir_->type(type).arg0 == 64 &&
                sem_ir_->type(type).arg1 == 0);
      };
      if ((array_type.kind != SemTypeKind::Array &&
           array_type.kind != SemTypeKind::Slice) ||
          (base.kind == SemTypeKind::Reference &&
           array_type.kind != SemTypeKind::Array) ||
          !valid_index(value_type(value.arg1)) ||
          element_type.index != array_type.arg0) {
        error = "aggregate index has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::DynamicIndexBorrow: {
      const auto &source = place(LowPlaceId(value.arg0));
      const auto &array_type = sem_ir_->type(source.type);
      const auto valid_index = [&](TypeId type) {
        return type == sem_ir_->i32Type() ||
               (sem_ir_->type(type).kind == SemTypeKind::Integer &&
                sem_ir_->type(type).arg0 == 64 &&
                sem_ir_->type(type).arg1 == 0);
      };
      if ((source.flags & LowPlaceAddressable) == 0 ||
          array_type.kind != SemTypeKind::Array ||
          !valid_index(value_type(value.arg1)) ||
          sem_ir_->type(instruction_type).kind != SemTypeKind::Reference ||
          sem_ir_->referencePointee(instruction_type).index !=
              array_type.arg0) {
        error = "dynamic index borrow has invalid types";
        return false;
      }
      break;
    }
  default:
    return true;
  }
  return true;
}

} // namespace chtholly::compiler::internal
