#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/CallableOwnership.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler {

namespace {

constexpr std::uint32_t ProjectionIndexMask = 0x7fffffffU;
constexpr std::uint32_t ProjectionKindShift = 31U;
constexpr std::uint32_t MutableReferenceBit = 1U;
constexpr std::uint32_t ParameterProvenanceBit = 1U << 1U;
constexpr std::uint32_t ProvenanceIndexShift = 2U;

std::uint32_t encodeReferenceFacts(SemReferenceMutability mutability,
                                   SemReferenceProvenanceKind provenance_kind,
                                   std::uint32_t provenance_index) {
  auto result = mutability == SemReferenceMutability::Mutable
                    ? MutableReferenceBit
                    : 0U;
  if (provenance_kind == SemReferenceProvenanceKind::Parameter) {
    assert(provenance_index <=
           (std::numeric_limits<std::uint32_t>::max() >> ProvenanceIndexShift));
    result |= ParameterProvenanceBit |
              (provenance_index << ProvenanceIndexShift);
  }
  return result;
}

} // namespace

bool SemIR::isCallAbiType(TypeId id) const {
  switch (type(id).kind) {
  case SemTypeKind::Void:
  case SemTypeKind::Never:
  case SemTypeKind::Bool:
  case SemTypeKind::Char:
  case SemTypeKind::Integer:
  case SemTypeKind::Float:
  case SemTypeKind::String:
  case SemTypeKind::Array:
  case SemTypeKind::Function:
  case SemTypeKind::TypeParameter:
  case SemTypeKind::Nominal:
  case SemTypeKind::Reference:
  case SemTypeKind::RawPointer:
  case SemTypeKind::Tuple:
  case SemTypeKind::Slice:
  case SemTypeKind::CFunctionPointer:
  case SemTypeKind::CVariadicFunctionPointer:
  case SemTypeKind::CallbackAdapter:
  case SemTypeKind::CallbackRegistration:
  case SemTypeKind::CallbackCompletion:
  case SemTypeKind::CallbackWake:
  case SemTypeKind::ForeignCompletion:
  case SemTypeKind::ForeignWake:
    return true;
  default:
    return false;
  }
}

std::optional<CanonicalResultShape>
SemIR::canonicalResultShape(TypeId type_id) const {
  const auto &semantic_type = type(type_id);
  if (semantic_type.kind != SemTypeKind::Nominal)
    return std::nullopt;
  const auto &nominal = nominalType(NominalTypeId(semantic_type.arg0));
  const auto *entity = imports_.tryGetEntity(nominal.canonical_entity);
  if (!entity || nominal.kind != NominalKind::Enum ||
      identifier(entity->package_name) != "std" ||
      identifier(entity->module_name) != "std::result" ||
      identifier(entity->name) != "Result")
    return std::nullopt;
  const auto arguments = typeBlock(TypeBlockId(semantic_type.arg1));
  if (arguments.size() != 2 || nominal.variants.size() != 2)
    return std::nullopt;
  CanonicalResultShape result{arguments[0], arguments[1]};
  for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
    const auto &variant = nominal.variants[index];
    if (variant.shape != SemEnumPayloadShape::Tuple ||
        variant.fields.size() != 1)
      return std::nullopt;
    const auto variant_name = identifier(name(variant.name).text);
    if (variant_name == "Ok")
      result.ok_variant = index;
    else if (variant_name == "Err")
      result.err_variant = index;
  }
  if (result.ok_variant == core::AnyId::InvalidIndex ||
      result.err_variant == core::AnyId::InvalidIndex ||
      enumPayloadFieldType(type_id, result.ok_variant, 0) != result.success ||
      enumPayloadFieldType(type_id, result.err_variant, 0) != result.error)
    return std::nullopt;
  return result;
}

std::optional<CanonicalReadOutcomeShape>
SemIR::canonicalReadOutcomeShape(TypeId type_id) const {
  const auto &semantic_type = type(type_id);
  if (semantic_type.kind != SemTypeKind::Nominal)
    return std::nullopt;
  const auto &nominal = nominalType(NominalTypeId(semantic_type.arg0));
  const auto *entity = imports_.tryGetEntity(nominal.canonical_entity);
  if (!entity || nominal.kind != NominalKind::Enum ||
      identifier(entity->package_name) != "std" ||
      identifier(entity->module_name) != "std::io" ||
      identifier(entity->name) != "ReadOutcome")
    return std::nullopt;
  const auto arguments = typeBlock(TypeBlockId(semantic_type.arg1));
  if (arguments.size() != 1 || nominal.variants.size() != 2)
    return std::nullopt;
  CanonicalReadOutcomeShape result{arguments[0]};
  for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
    const auto &variant = nominal.variants[index];
    const auto variant_name = identifier(name(variant.name).text);
    if (variant_name == "Data" && variant.shape == SemEnumPayloadShape::Tuple &&
        variant.fields.size() == 1)
      result.data_variant = index;
    else if (variant_name == "Eof" &&
             variant.shape == SemEnumPayloadShape::Unit &&
             variant.fields.empty())
      result.eof_variant = index;
  }
  if (result.data_variant == core::AnyId::InvalidIndex ||
      result.eof_variant == core::AnyId::InvalidIndex ||
      enumPayloadFieldType(type_id, result.data_variant, 0) != result.data)
    return std::nullopt;
  return result;
}

TypeId SemIR::canonicalResultOutcomeType(TypeId type_id) const {
  const auto shape = canonicalResultShape(type_id);
  if (!shape)
    return TypeId::invalid();
  const auto &result = type(type_id);
  for (std::uint32_t index = 0; index < typeCount(); ++index) {
    const auto candidate = TypeId(index);
    const auto &candidate_type = type(candidate);
    if (candidate_type.kind != SemTypeKind::Nominal ||
        candidate_type.arg0 != result.arg0)
      continue;
    const auto arguments = typeBlock(TypeBlockId(candidate_type.arg1));
    if (arguments.size() == 2 && arguments[0] == void_type_ &&
        arguments[1] == shape->error)
      return candidate;
  }
  return TypeId::invalid();
}

bool SemIR::isGenericArgumentType(CanonicalTypeId id) const {
  switch (values_->generics().type(id).kind) {
  case CanonicalTypeKind::Never:
  case CanonicalTypeKind::Bool:
  case CanonicalTypeKind::Char:
  case CanonicalTypeKind::Integer:
  case CanonicalTypeKind::Float:
  case CanonicalTypeKind::String:
  case CanonicalTypeKind::Array:
  case CanonicalTypeKind::Function:
  case CanonicalTypeKind::TypeParameter:
  case CanonicalTypeKind::TypeProjection:
  case CanonicalTypeKind::Nominal:
  case CanonicalTypeKind::Reference:
  case CanonicalTypeKind::RawPointer:
  case CanonicalTypeKind::CFunctionPointer:
  case CanonicalTypeKind::CVariadicFunctionPointer:
  case CanonicalTypeKind::CallbackAdapter:
  case CanonicalTypeKind::CallbackRegistration:
  case CanonicalTypeKind::CallbackCompletion:
  case CanonicalTypeKind::CallbackWake:
  case CanonicalTypeKind::ForeignCompletion:
  case CanonicalTypeKind::ForeignWake:
  case CanonicalTypeKind::Tuple:
  case CanonicalTypeKind::Slice:
    return true;
  default:
    return false;
  }
}

bool SemIR::isCompletionAggregationProvider(TypeId id) const {
  if (id == coroutine_task_completion_type_)
    return true;
  if (!id.hasValue() || type(id).kind != SemTypeKind::ForeignCompletion)
    return false;
  const auto &resource = nominalType(NominalTypeId(type(id).arg0));
  if (resource.completion_state != SemNominalCompletionState::Complete ||
      !resource.foreign_completion_storage_type.hasValue())
    return false;
  const auto storage = resource.foreign_completion_storage_type;
  if (type(storage).kind != SemTypeKind::CallbackCompletion ||
      typeBlock(TypeBlockId(type(storage).arg0)).size() != 7)
    return false;
  const auto &protocol = foreignResourceProtocol(storage).facts;
  return protocol.findRole(ForeignResourceRoleKind::InspectReady) &&
         protocol.findRole(ForeignResourceRoleKind::ArmOneShot) &&
         protocol.findRole(ForeignResourceRoleKind::DetachCompletion) &&
         !protocol.wake_cleanup_path.empty();
}

bool SemIR::matchesPublicType(TypeId local, const PublicType &external) const {
  const auto kind = type(local).kind;
  if ((kind == SemTypeKind::Void && external.kind == PublicTypeKind::Void) ||
      (kind == SemTypeKind::Never && external.kind == PublicTypeKind::Never) ||
      (kind == SemTypeKind::Bool && external.kind == PublicTypeKind::Bool) ||
      (kind == SemTypeKind::Char && external.kind == PublicTypeKind::Char) ||
      (kind == SemTypeKind::Integer &&
       external.kind == PublicTypeKind::Integer &&
       type(local).arg0 == external.scalar_width &&
       (type(local).arg1 != 0) == external.integer_signed) ||
      (kind == SemTypeKind::Float && external.kind == PublicTypeKind::Float &&
       type(local).arg0 == external.scalar_width) ||
      (kind == SemTypeKind::String &&
       external.kind == PublicTypeKind::String) ||
      (kind == SemTypeKind::TypeParameter &&
       external.kind == PublicTypeKind::TypeParameter &&
       type(local).arg1 == external.binding_index))
    return true;
  if (kind == SemTypeKind::Reference &&
      external.kind == PublicTypeKind::Reference) {
    return external.arguments.size() == 1 &&
           (referenceMutability(local) == SemReferenceMutability::Mutable) ==
               (external.reference_mutability ==
                PublicReferenceMutability::Mutable) &&
           (referenceProvenanceKind(local) ==
            SemReferenceProvenanceKind::Parameter) ==
               (external.reference_provenance.kind ==
                PublicReferenceProvenanceKind::Parameter) &&
           (referenceProvenanceKind(local) !=
                SemReferenceProvenanceKind::Parameter ||
            referenceProvenanceIndex(local) ==
                external.reference_provenance.index) &&
           matchesPublicType(referencePointee(local),
                             external.arguments.front());
  }
  if (kind == SemTypeKind::RawPointer &&
      external.kind == PublicTypeKind::RawPointer) {
    return external.arguments.size() == 1 &&
           rawPointerPointeeConst(local) == external.pointer_const &&
           matchesPublicType(rawPointerPointee(local),
                             external.arguments.front());
  }
  if (kind == SemTypeKind::Tuple && external.kind == PublicTypeKind::Tuple) {
    const auto elements = typeBlock(TypeBlockId(type(local).arg0));
    if (elements.size() != external.arguments.size())
      return false;
    for (std::size_t index = 0; index < elements.size(); ++index)
      if (!matchesPublicType(elements[index], external.arguments[index]))
        return false;
    return true;
  }
  if (kind == SemTypeKind::Slice && external.kind == PublicTypeKind::Slice) {
    return external.arguments.size() == 1 &&
           sliceMutable(local) == external.slice_mutable &&
           matchesPublicType(sliceElementType(local),
                             external.arguments.front());
  }
  if (kind == SemTypeKind::TypeProjection &&
      external.kind == PublicTypeKind::TypeProjection) {
    return external.arguments.size() == 1 &&
           static_cast<std::uint32_t>(external.projection_kind) ==
               (type(local).arg1 >> ProjectionKindShift) &&
           external.projection_index ==
               (type(local).arg1 & ProjectionIndexMask) &&
           matchesPublicType(TypeId(type(local).arg0),
                             external.arguments.front());
  }
  if (kind == SemTypeKind::TypeParameter &&
      external.kind == PublicTypeKind::TypeProjection &&
      external.projection_kind == PublicTypeProjectionKind::Associated &&
      external.arguments.size() == 1) {
    const auto entity_id = imports_.registry().findEntity(
        external.nominal_entity.canonical_package,
        external.nominal_entity.canonical_module,
        external.nominal_entity.canonical_name, PublicEntityKind::Interface,
        external.nominal_entity.expected_fingerprint);
    const auto *entity = imports_.registry().tryGetEntity(entity_id);
    return entity && entity->kind == PublicEntityKind::Interface &&
           entity->fingerprint ==
               external.nominal_entity.expected_fingerprint &&
           entity->generic.hasValue() &&
           type(local).arg0 == entity->generic.index &&
           type(local).arg1 == external.projection_index;
  }
  if (kind == SemTypeKind::Function &&
      external.kind == PublicTypeKind::Function) {
    const auto parameters = typeBlock(TypeBlockId(type(local).arg0));
    if (external.arguments.size() != parameters.size() + 1)
      return false;
    for (std::size_t index = 0; index < parameters.size(); ++index)
      if (!matchesPublicType(parameters[index], external.arguments[index]))
        return false;
    return matchesPublicType(TypeId(type(local).arg1),
                             external.arguments.back());
  }
  if (kind == SemTypeKind::Tuple && external.kind == PublicTypeKind::Tuple &&
      isCUnionType(local) != external.abi_union)
    return false;
  if ((kind == SemTypeKind::CFunctionPointer ||
       kind == SemTypeKind::CVariadicFunctionPointer) &&
      external.kind == PublicTypeKind::CFunctionPointer) {
    const auto parameters = typeBlock(TypeBlockId(type(local).arg0));
    if (external.arguments.size() != parameters.size() + 1 ||
        external.callable_variadic !=
            (kind == SemTypeKind::CVariadicFunctionPointer) ||
        callbackContextParameter(local) !=
            external.callable_context_parameter ||
        cFunctionCallingConvention(local) !=
            external.foreign_calling_convention ||
        callbackContract(local) != external.callable_contract)
      return false;
    for (std::size_t index = 0; index < parameters.size(); ++index)
      if (!matchesPublicType(parameters[index], external.arguments[index]))
        return false;
    return matchesPublicType(TypeId(type(local).arg1),
                             external.arguments.back());
  }
  if (kind == SemTypeKind::CallbackAdapter &&
      external.kind == PublicTypeKind::CallbackAdapter) {
    const auto fields = typeBlock(TypeBlockId(type(local).arg0));
    if (fields.size() != 3 || external.arguments.size() != 3)
      return false;
    for (std::size_t index = 0; index < fields.size(); ++index)
      if (!matchesPublicType(fields[index], external.arguments[index]))
        return false;
    return true;
  }
  if (kind == SemTypeKind::CallbackRegistration &&
      external.kind == PublicTypeKind::CallbackRegistration) {
    const auto fields = typeBlock(TypeBlockId(type(local).arg0));
    if ((fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
         fields.size() != 10) ||
        fields.size() != external.arguments.size() ||
        static_cast<std::uint8_t>(callbackRegistrationAuthority(local)) !=
            external.registration_authority)
      return false;
    const auto marked = callbackRegistrationParameters(local);
    if (marked[0] != external.registration_entry_parameter ||
        marked[1] != external.registration_userdata_parameter ||
        marked[2] != external.registration_release_parameter ||
        callbackArmParameters(local) != external.registration_arm_parameters ||
        callbackDetachParameters(local) !=
            external.registration_detach_parameters ||
        !std::ranges::equal(callbackRegistrationBindings(local),
                            external.registration_bindings))
      return false;
    for (std::size_t index = 0; index < fields.size(); ++index)
      if (!matchesPublicType(fields[index], external.arguments[index]))
        return false;
    return true;
  }
  if (kind == SemTypeKind::CallbackCompletion &&
      external.kind == PublicTypeKind::CallbackCompletion) {
    const auto fields = typeBlock(TypeBlockId(type(local).arg0));
    if ((fields.size() != 4 && fields.size() != 5 && fields.size() != 7) ||
        fields.size() != external.arguments.size() ||
        static_cast<std::uint8_t>(callbackCompletionAuthority(local)) !=
            external.registration_authority ||
        callbackArmParameters(local) != external.registration_arm_parameters ||
        callbackDetachParameters(local) !=
            external.registration_detach_parameters)
      return false;
    for (std::size_t index = 0; index < fields.size(); ++index)
      if (!matchesPublicType(fields[index], external.arguments[index]))
        return false;
    return true;
  }
  if (kind == SemTypeKind::CallbackWake &&
      external.kind == PublicTypeKind::CallbackWake) {
    const auto fields = typeBlock(TypeBlockId(type(local).arg0));
    return fields.size() == 1 && external.arguments.size() == 1 &&
           matchesPublicType(fields[0], external.arguments.front());
  }
  if ((kind == SemTypeKind::ForeignCompletion &&
       external.kind == PublicTypeKind::ForeignCompletion) ||
      (kind == SemTypeKind::ForeignWake &&
       external.kind == PublicTypeKind::ForeignWake)) {
    const auto &owner = nominalType(NominalTypeId(type(local).arg0));
    const auto *entity = imports_.tryGetEntity(owner.canonical_entity);
    return entity && entity->kind == PublicEntityKind::NominalType &&
           entity->fingerprint ==
               external.nominal_entity.expected_fingerprint &&
           identifier(entity->package_name) ==
               external.nominal_entity.canonical_package &&
           identifier(entity->module_name) ==
               external.nominal_entity.canonical_module &&
           identifier(entity->name) == external.nominal_entity.canonical_name;
  }
  if (kind == SemTypeKind::Array && external.kind == PublicTypeKind::Array) {
    return external.arguments.size() == 1 &&
           type(local).arg1 == external.array_bound &&
           matchesPublicType(TypeId(type(local).arg0),
                             external.arguments.front());
  }
  if (kind != SemTypeKind::Nominal || external.kind != PublicTypeKind::Nominal)
    return false;
  const auto &local_type = type(local);
  const auto &nominal = nominalType(NominalTypeId(local_type.arg0));
  const auto *entity = imports_.tryGetEntity(nominal.canonical_entity);
  if (!entity || entity->kind != PublicEntityKind::NominalType ||
      entity->fingerprint != external.nominal_entity.expected_fingerprint ||
      identifier(entity->package_name) !=
          external.nominal_entity.canonical_package ||
      identifier(entity->module_name) !=
          external.nominal_entity.canonical_module ||
      identifier(entity->name) != external.nominal_entity.canonical_name)
    return false;
  const auto arguments = typeBlock(TypeBlockId(local_type.arg1));
  if (arguments.size() != external.arguments.size())
    return false;
  for (std::size_t index = 0; index < arguments.size(); ++index)
    if (!matchesPublicType(arguments[index], external.arguments[index]))
      return false;
  return true;
}

TypeId SemIR::addNominalType(NominalTypeId nominal,
                             std::span<const TypeId> arguments) {
  return addType({SemTypeKind::Nominal, nominal.index,
                  addTypeBlock(arguments).index, core::AnyId::InvalidIndex});
}

TypeId SemIR::addTupleType(std::span<const TypeId> elements) {
  return addType({SemTypeKind::Tuple, addTypeBlock(elements).index});
}

TypeId SemIR::addCUnionType(std::span<const TypeId> members) {
  CanonicalType canonical;
  canonical.kind = CanonicalTypeKind::Tuple;
  canonical.abi_union = true;
  for (const auto member : members)
    canonical.elements.push_back(canonicalType(member));
  const auto canonical_id =
      values_->generics().internType(std::move(canonical));
  return addType({SemTypeKind::Tuple, addTypeBlock(members).index,
                  core::AnyId::InvalidIndex, canonical_id.index});
}

bool SemIR::isCUnionType(TypeId type_id) const {
  const auto &value = type(type_id);
  return value.kind == SemTypeKind::Tuple &&
         value.reserved != core::AnyId::InvalidIndex &&
         values_->generics().type(CanonicalTypeId(value.reserved)).abi_union;
}

TypeId SemIR::addSliceType(TypeId element, bool mutable_view) {
  return addType({SemTypeKind::Slice, element.index, mutable_view ? 1U : 0U});
}

TypeId SemIR::addReferenceType(TypeId pointee,
                               SemReferenceMutability mutability,
                               SemReferenceProvenanceKind provenance_kind,
                               std::uint32_t provenance_index) {
  return addType(
      {SemTypeKind::Reference, pointee.index,
       encodeReferenceFacts(mutability, provenance_kind, provenance_index),
       core::AnyId::InvalidIndex});
}

TypeId SemIR::referencePointee(TypeId reference) const {
  const auto &value = type(reference);
  assert(value.kind == SemTypeKind::Reference);
  return TypeId(value.arg0);
}

std::uint32_t SemIR::tupleArity(TypeId tuple) const {
  assert(type(tuple).kind == SemTypeKind::Tuple);
  return static_cast<std::uint32_t>(
      typeBlock(TypeBlockId(type(tuple).arg0)).size());
}

TypeId SemIR::tupleElementType(TypeId tuple, std::uint32_t index) const {
  assert(type(tuple).kind == SemTypeKind::Tuple);
  const auto elements = typeBlock(TypeBlockId(type(tuple).arg0));
  assert(index < elements.size());
  return elements[index];
}

TypeId SemIR::sliceElementType(TypeId slice) const {
  assert(type(slice).kind == SemTypeKind::Slice);
  return TypeId(type(slice).arg0);
}

bool SemIR::sliceMutable(TypeId slice) const {
  assert(type(slice).kind == SemTypeKind::Slice);
  return type(slice).arg1 != 0;
}

SemReferenceMutability SemIR::referenceMutability(TypeId reference) const {
  return (type(reference).arg1 & MutableReferenceBit) != 0
             ? SemReferenceMutability::Mutable
             : SemReferenceMutability::ReadOnly;
}

SemReferenceProvenanceKind
SemIR::referenceProvenanceKind(TypeId reference) const {
  return (type(reference).arg1 & ParameterProvenanceBit) != 0
             ? SemReferenceProvenanceKind::Parameter
             : SemReferenceProvenanceKind::Erased;
}

std::uint32_t SemIR::referenceProvenanceIndex(TypeId reference) const {
  return referenceProvenanceKind(reference) ==
                 SemReferenceProvenanceKind::Parameter
             ? type(reference).arg1 >> ProvenanceIndexShift
             : core::AnyId::InvalidIndex;
}

TypeId SemIR::rawPointerPointee(TypeId pointer) const {
  const auto &value = type(pointer);
  assert(value.kind == SemTypeKind::RawPointer);
  return TypeId(value.arg0);
}

SemLoanCarrierCapability SemIR::loanCarrierCapability(TypeId root) const {
  constexpr std::size_t MaxVisitedTypes = 4096;
  std::vector<TypeId> worklist{root};
  std::unordered_set<std::uint32_t> visited;
  auto capability = SemLoanCarrierCapability::None;
  const auto observe = [&](SemReferenceMutability mutability) {
    capability = mutability == SemReferenceMutability::Mutable
                     ? SemLoanCarrierCapability::Mutable
                     : std::max(capability, SemLoanCarrierCapability::Shared);
  };

  while (!worklist.empty()) {
    const auto current = worklist.back();
    worklist.pop_back();
    if (!current.hasValue() || current.index >= typeCount() ||
        !visited.insert(current.index).second)
      continue;
    if (visited.size() > MaxVisitedTypes)
      return SemLoanCarrierCapability::Mutable;

    const auto &value = type(current);
    if (value.kind == SemTypeKind::Reference) {
      observe(referenceMutability(current));
      if (capability == SemLoanCarrierCapability::Mutable)
        return capability;
      continue;
    }
    if (value.kind == SemTypeKind::Slice) {
      observe(sliceMutable(current) ? SemReferenceMutability::Mutable
                                    : SemReferenceMutability::ReadOnly);
      if (capability == SemLoanCarrierCapability::Mutable)
        return capability;
      continue;
    }
    if (value.kind == SemTypeKind::Array) {
      worklist.push_back(TypeId(value.arg0));
      continue;
    }
    if (value.kind == SemTypeKind::Tuple) {
      for (const auto element : typeBlock(TypeBlockId(value.arg0)))
        worklist.push_back(element);
      continue;
    }
    if (value.kind != SemTypeKind::Nominal)
      continue;

    const auto &nominal = nominalType(NominalTypeId(value.arg0));
    for (std::uint32_t index = 0; index < nominal.fields.size(); ++index) {
      const auto field = nominalFieldType(current, index);
      worklist.push_back(field.hasValue() ? field : nominal.fields[index].type);
    }
    for (std::uint32_t variant = 0; variant < nominal.variants.size();
         ++variant)
      for (std::uint32_t field = 0;
           field < nominal.variants[variant].fields.size(); ++field) {
        const auto payload = enumPayloadFieldType(current, variant, field);
        worklist.push_back(payload.hasValue()
                               ? payload
                               : nominal.variants[variant].fields[field].type);
      }
  }
  return capability;
}

std::vector<std::vector<CallableReturnSource::CarrierStep>>
SemIR::loanCarrierPaths(TypeId root) const {
  std::vector<std::vector<CallableReturnSource::CarrierStep>> paths;
  std::vector<CallableReturnSource::CarrierStep> path;
  std::unordered_set<std::uint32_t> visiting;
  const auto collect = [&](const auto &self, TypeId type_id) -> void {
    const auto kind = type(type_id).kind;
    if (kind == SemTypeKind::Reference || kind == SemTypeKind::Slice) {
      paths.push_back(path);
      return;
    }
    if (kind != SemTypeKind::Nominal || !visiting.insert(type_id.index).second)
      return;
    const auto &nominal = nominalType(NominalTypeId(type(type_id).arg0));
    if (nominal.kind == NominalKind::Enum) {
      for (std::uint32_t variant = 0; variant < nominal.variants.size();
           ++variant) {
        path.push_back(
            {CallableReturnSource::CarrierStepKind::EnumVariant, variant});
        for (std::uint32_t field = 0;
             field < nominal.variants[variant].fields.size(); ++field) {
          path.push_back({CallableReturnSource::CarrierStepKind::Field, field});
          self(self, enumPayloadFieldType(type_id, variant, field));
          path.pop_back();
        }
        path.pop_back();
      }
    } else {
      for (std::uint32_t field = 0; field < nominal.fields.size(); ++field) {
        path.push_back({CallableReturnSource::CarrierStepKind::Field, field});
        self(self, nominalFieldType(type_id, field));
        path.pop_back();
      }
    }
    visiting.erase(type_id.index);
  };
  collect(collect, root);
  return paths;
}

bool SemIR::rawPointerPointeeConst(TypeId pointer) const {
  const auto &value = type(pointer);
  assert(value.kind == SemTypeKind::RawPointer);
  return value.arg1 != 0;
}

TypeRepresentationFacts SemIR::typeRepresentation(TypeId id) const {
  if (const auto *witness = nominalSemanticWitness(id))
    return witness->representation;
  switch (type(id).kind) {
  case SemTypeKind::Void:
  case SemTypeKind::Never:
    return {};
  case SemTypeKind::Bool:
  case SemTypeKind::Char:
  case SemTypeKind::Integer:
  case SemTypeKind::Float:
  case SemTypeKind::Function:
  case SemTypeKind::RawPointer:
  case SemTypeKind::CoroutineExecutor:
  case SemTypeKind::CoroutineScope:
  case SemTypeKind::CFunctionPointer:
  case SemTypeKind::CVariadicFunctionPointer:
    return {ValueReprKind::Copy,     InitReprKind::ByCopy,
            OwnershipReprKind::None, CopyReprKind::Trivial,
            MoveReprKind::Trivial,   DestroyReprKind::None,
            ObjectReprKind::Identity};
  case SemTypeKind::CoroutineTask:
  case SemTypeKind::CoroutineTaskCompletion:
  case SemTypeKind::CoroutineTaskCompletionSet:
  case SemTypeKind::CoroutineTaskSelection:
    return {ValueReprKind::Copy,      InitReprKind::ByCopy,
            OwnershipReprKind::Owned, CopyReprKind::Unavailable,
            MoveReprKind::Trivial,    DestroyReprKind::Custom,
            ObjectReprKind::Identity};
  case SemTypeKind::CoroutineTaskOutcome:
    return {ValueReprKind::Copy,     InitReprKind::ByCopy,
            OwnershipReprKind::None, CopyReprKind::Trivial,
            MoveReprKind::Trivial,   DestroyReprKind::None,
            ObjectReprKind::Identity};
  case SemTypeKind::CoroutineChecked:
    return {ValueReprKind::Copy,       InitReprKind::ByCopy,
            OwnershipReprKind::Owned,  CopyReprKind::Unavailable,
            MoveReprKind::Unavailable, DestroyReprKind::Custom,
            ObjectReprKind::Identity};
  case SemTypeKind::CallbackAdapter:
    return {ValueReprKind::Copy,      InitReprKind::ByCopy,
            OwnershipReprKind::Owned, CopyReprKind::Unavailable,
            MoveReprKind::Trivial,    DestroyReprKind::Trivial,
            ObjectReprKind::Identity};
  case SemTypeKind::CallbackRegistration:
  case SemTypeKind::CallbackCompletion:
  case SemTypeKind::CallbackWake:
  case SemTypeKind::ForeignCompletion:
  case SemTypeKind::ForeignWake:
    return {ValueReprKind::Copy,      InitReprKind::ByCopy,
            OwnershipReprKind::Owned, CopyReprKind::Unavailable,
            MoveReprKind::Trivial,    DestroyReprKind::Custom,
            ObjectReprKind::Identity};
  case SemTypeKind::Reference:
    return {ValueReprKind::Copy,         InitReprKind::ByCopy,
            OwnershipReprKind::Borrowed, CopyReprKind::Trivial,
            MoveReprKind::Trivial,       DestroyReprKind::None,
            ObjectReprKind::Identity};
  case SemTypeKind::String:
    return {ValueReprKind::Copy,      InitReprKind::ByCopy,
            OwnershipReprKind::Owned, CopyReprKind::Trivial,
            MoveReprKind::Trivial,    DestroyReprKind::Trivial,
            ObjectReprKind::Identity};
  case SemTypeKind::Array:
  case SemTypeKind::Tuple: {
    const auto element_requires_in_place = [&](TypeId element) {
      return typeRepresentation(element).init_repr == InitReprKind::InPlace;
    };
    const auto has_borrowed_element = [&](TypeId element) {
      return typeRepresentation(element).ownership ==
             OwnershipReprKind::Borrowed;
    };
    const auto elements = type(id).kind == SemTypeKind::Tuple
                              ? typeBlock(TypeBlockId(type(id).arg0))
                              : std::span<const TypeId>{};
    const auto in_place =
        type(id).kind == SemTypeKind::Array
            ? element_requires_in_place(TypeId(type(id).arg0))
            : std::ranges::any_of(elements, element_requires_in_place);
    const auto borrowed =
        type(id).kind == SemTypeKind::Array
            ? has_borrowed_element(TypeId(type(id).arg0))
            : std::ranges::any_of(elements, has_borrowed_element);
    return {in_place ? ValueReprKind::Pointer : ValueReprKind::Copy,
            in_place ? InitReprKind::InPlace : InitReprKind::ByCopy,
            borrowed ? OwnershipReprKind::Borrowed : OwnershipReprKind::Owned,
            CopyReprKind::Trivial,
            MoveReprKind::Trivial,
            borrowed ? DestroyReprKind::None : DestroyReprKind::Trivial,
            ObjectReprKind::Identity};
  }
  case SemTypeKind::Slice:
    return {ValueReprKind::Copy,         InitReprKind::ByCopy,
            OwnershipReprKind::Borrowed, CopyReprKind::Trivial,
            MoveReprKind::Trivial,       DestroyReprKind::None,
            ObjectReprKind::Identity};
  case SemTypeKind::Nominal: {
    const auto &nominal = nominalType(NominalTypeId(type(id).arg0));
    if (nominal.kind == NominalKind::ForeignHandle &&
        !nominal.foreign_representation.hasValue())
      return {ValueReprKind::None,       InitReprKind::None,
              OwnershipReprKind::None,   CopyReprKind::Unavailable,
              MoveReprKind::Unavailable, DestroyReprKind::None,
              ObjectReprKind::None};
    if (nominal.kind == NominalKind::ForeignHandle &&
        (type(nominal.foreign_representation).kind == SemTypeKind::Tuple ||
         type(nominal.foreign_representation).kind == SemTypeKind::Array))
      return {ValueReprKind::Pointer,  InitReprKind::InPlace,
              OwnershipReprKind::None, CopyReprKind::Trivial,
              MoveReprKind::Trivial,   DestroyReprKind::None,
              ObjectReprKind::Identity};
    if (nominal.kind == NominalKind::ForeignHandle)
      return {ValueReprKind::Copy,     InitReprKind::ByCopy,
              OwnershipReprKind::None, CopyReprKind::Trivial,
              MoveReprKind::Trivial,   DestroyReprKind::None,
              ObjectReprKind::Identity};
    if (nominal.kind == NominalKind::ForeignResource)
      return {ValueReprKind::Copy,      InitReprKind::ByCopy,
              OwnershipReprKind::Owned, CopyReprKind::Unavailable,
              MoveReprKind::Trivial,    DestroyReprKind::Custom,
              ObjectReprKind::Identity};
    const auto copy =
        static_cast<SemLifecycleCopyPolicy>(nominal.lifecycle_copy);
    const auto move =
        static_cast<SemLifecycleMovePolicy>(nominal.lifecycle_move);
    const auto drop =
        static_cast<SemLifecycleDropPolicy>(nominal.lifecycle_drop);
    return {ValueReprKind::Pointer,
            InitReprKind::InPlace,
            OwnershipReprKind::Owned,
            copy == SemLifecycleCopyPolicy::Delete   ? CopyReprKind::Unavailable
            : copy == SemLifecycleCopyPolicy::Custom ? CopyReprKind::Custom
                                                     : CopyReprKind::Trivial,
            move == SemLifecycleMovePolicy::Delete ? MoveReprKind::Unavailable
                                                   : MoveReprKind::Trivial,
            drop == SemLifecycleDropPolicy::Custom ? DestroyReprKind::Custom
                                                   : DestroyReprKind::Trivial,
            ObjectReprKind::NominalAggregate};
  }
  case SemTypeKind::TypeParameter:
  case SemTypeKind::TypeProjection:
    return {ValueReprKind::Dependent,     InitReprKind::Dependent,
            OwnershipReprKind::Dependent, CopyReprKind::Dependent,
            MoveReprKind::Dependent,      DestroyReprKind::Dependent,
            ObjectReprKind::Dependent};
  case SemTypeKind::AsyncFunction:
  case SemTypeKind::Invalid:
  case SemTypeKind::Count:
    return {};
  }
  return {};
}

const NominalSemanticWitnessArtifact *
SemIR::nominalSemanticWitness(TypeId id) const {
  const auto found = nominal_semantic_witnesses_.find(id.index);
  return found == nominal_semantic_witnesses_.end() ? nullptr : &found->second;
}

TypeId SemIR::valueRepresentationType(TypeId id) const {
  if (type(id).kind == SemTypeKind::Nominal) {
    const auto &nominal = nominalType(NominalTypeId(type(id).arg0));
    if (nominal.kind == NominalKind::ForeignResource &&
        nominal.foreign_registration_storage_type.hasValue())
      return valueRepresentationType(nominal.foreign_registration_storage_type);
  }
  if (type(id).kind == SemTypeKind::ForeignCompletion) {
    const auto &owner = nominalType(NominalTypeId(type(id).arg0));
    if (owner.foreign_completion_storage_type.hasValue())
      return valueRepresentationType(owner.foreign_completion_storage_type);
    return foreignRepresentationType(owner.foreign_completion_handle_type);
  }
  if (type(id).kind == SemTypeKind::ForeignWake) {
    const auto &owner = nominalType(NominalTypeId(type(id).arg0));
    return owner.foreign_wake_storage_type.hasValue()
               ? valueRepresentationType(owner.foreign_wake_storage_type)
               : TypeId::invalid();
  }
  if (const auto foreign = foreignRepresentationType(id); foreign.hasValue())
    return foreign;
  if (const auto found = value_repr_carriers_.find(id.index);
      found != value_repr_carriers_.end())
    return found->second;
  return id;
}

TypeId SemIR::objectRepresentationType(TypeId id) const {
  if (type(id).kind == SemTypeKind::Nominal) {
    const auto &nominal = nominalType(NominalTypeId(type(id).arg0));
    if (nominal.kind == NominalKind::ForeignResource &&
        nominal.foreign_registration_storage_type.hasValue())
      return objectRepresentationType(
          nominal.foreign_registration_storage_type);
  }
  if (type(id).kind == SemTypeKind::ForeignCompletion) {
    const auto &owner = nominalType(NominalTypeId(type(id).arg0));
    if (owner.foreign_completion_storage_type.hasValue())
      return objectRepresentationType(owner.foreign_completion_storage_type);
    return foreignRepresentationType(owner.foreign_completion_handle_type);
  }
  if (type(id).kind == SemTypeKind::ForeignWake) {
    const auto &owner = nominalType(NominalTypeId(type(id).arg0));
    return owner.foreign_wake_storage_type.hasValue()
               ? objectRepresentationType(owner.foreign_wake_storage_type)
               : TypeId::invalid();
  }
  if (const auto foreign = foreignRepresentationType(id); foreign.hasValue())
    return foreign;
  if (const auto found = object_repr_carriers_.find(id.index);
      found != object_repr_carriers_.end())
    return found->second;
  return id;
}

TypeId SemIR::foreignRepresentationType(TypeId id) const {
  if (!id.hasValue() || type(id).kind != SemTypeKind::Nominal)
    return TypeId::invalid();
  const auto &nominal = nominalType(NominalTypeId(type(id).arg0));
  if (nominal.kind == NominalKind::ForeignHandle)
    return nominal.foreign_representation;
  if (nominal.kind == NominalKind::ForeignResource &&
      nominal.foreign_handle_type.hasValue())
    return foreignRepresentationType(nominal.foreign_handle_type);
  return TypeId::invalid();
}

TypeId SemIR::nominalFieldType(TypeId owner, std::uint32_t field_index) const {
  const auto &owner_type = type(owner);
  if (owner_type.kind != SemTypeKind::Nominal)
    return TypeId::invalid();
  const auto &nominal = nominalType(NominalTypeId(owner_type.arg0));
  if (field_index >= nominal.fields.size())
    return TypeId::invalid();
  return substituteNominalMemberType(owner, nominal.fields[field_index].type);
}

TypeId SemIR::enumPayloadFieldType(TypeId owner, std::uint32_t variant_index,
                                   std::uint32_t field_index) const {
  const auto &owner_type = type(owner);
  if (owner_type.kind != SemTypeKind::Nominal)
    return TypeId::invalid();
  const auto &nominal = nominalType(NominalTypeId(owner_type.arg0));
  if (nominal.kind != NominalKind::Enum ||
      variant_index >= nominal.variants.size() ||
      field_index >= nominal.variants[variant_index].fields.size())
    return TypeId::invalid();
  return substituteNominalMemberType(
      owner, nominal.variants[variant_index].fields[field_index].type);
}

TypeId SemIR::substituteNominalMemberType(TypeId owner,
                                          TypeId field_type) const {
  const auto &owner_type = type(owner);
  const auto &nominal = nominalType(NominalTypeId(owner_type.arg0));
  if (!nominal.generic.hasValue())
    return field_type;
  std::vector<CanonicalTypeId> arguments;
  for (const auto argument : typeBlock(TypeBlockId(owner_type.arg1)))
    arguments.push_back(canonicalType(argument));
  const auto substitute = [&](const auto &self,
                              CanonicalTypeId source) -> CanonicalTypeId {
    const auto value = values_->generics().type(source);
    if (value.kind == CanonicalTypeKind::TypeParameter &&
        value.arg0 == nominal.generic.index)
      return value.arg1 < arguments.size() ? arguments[value.arg1]
                                           : CanonicalTypeId::invalid();
    if (value.kind == CanonicalTypeKind::Array ||
        value.kind == CanonicalTypeKind::Tuple ||
        value.kind == CanonicalTypeKind::Slice ||
        value.kind == CanonicalTypeKind::Function ||
        value.kind == CanonicalTypeKind::AsyncFunction ||
        value.kind == CanonicalTypeKind::Nominal ||
        value.kind == CanonicalTypeKind::Reference ||
        value.kind == CanonicalTypeKind::RawPointer ||
        value.kind == CanonicalTypeKind::CFunctionPointer ||
        value.kind == CanonicalTypeKind::CVariadicFunctionPointer ||
        (value.kind == CanonicalTypeKind::CallbackAdapter ||
         value.kind == CanonicalTypeKind::CallbackRegistration ||
         value.kind == CanonicalTypeKind::CallbackCompletion ||
         value.kind == CanonicalTypeKind::CallbackWake)) {
      CanonicalType result = value;
      result.elements.clear();
      for (const auto element : value.elements)
        result.elements.push_back(self(self, element));
      if (value.kind == CanonicalTypeKind::Array)
        result.arg0 = self(self, CanonicalTypeId(value.arg0)).index;
      return values_->generics().internType(std::move(result));
    }
    return source;
  };
  const auto canonical = substitute(substitute, canonicalType(field_type));
  if (!canonical.hasValue())
    return TypeId::invalid();
  // Concrete nominal members are requested lazily by verification and
  // analysis. Materialization only interns canonical semantic types and is
  // therefore safe to populate through this logically-const query.
  return const_cast<SemIR *>(this)->materializeType(canonical);
}

std::span<const SemObjectProjectionStep>
SemIR::objectFieldProjection(TypeId id, std::uint32_t field_index) const {
  const auto found = object_field_projections_.find(id.index);
  if (found == object_field_projections_.end() ||
      field_index >= found->second.size())
    return {};
  return found->second[field_index];
}



} // namespace chtholly::compiler
