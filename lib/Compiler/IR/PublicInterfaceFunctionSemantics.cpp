#include "PublicInterfaceServices.h"

#include "PublicInterfaceEncodingInternal.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler::internal {

bool validCoroutineConstructorABI(
    PublicFunctionExecutionKind execution_kind,
    const PublicCoroutineConstructorABI &constructor) {
  return execution_kind == PublicFunctionExecutionKind::Async
             ? constructor ==
                   PublicCoroutineConstructorABI{1, true, true, true, true}
             : constructor == PublicCoroutineConstructorABI{};
}

bool validNominalConstructorABI(
    const CallableSemanticContract &contract, const PublicType &return_type,
    const PublicNominalConstructorABI &constructor) {
  if (contract.role != CallableSemanticRole::Constructor)
    return constructor == PublicNominalConstructorABI{};
  const auto fallible =
      return_type.kind == PublicTypeKind::Nominal &&
      return_type.nominal_entity.canonical_package == "std" &&
      return_type.nominal_entity.canonical_module == "std::result" &&
      return_type.nominal_entity.canonical_name == "Result" &&
      return_type.arguments.size() == 2 &&
      return_type.arguments[0] == contract.owner;
  return constructor.epoch == 1 &&
         constructor.result_kind ==
             (fallible ? PublicNominalConstructorResultKind::FallibleSelf
                       : PublicNominalConstructorResultKind::DirectSelf);
}

bool validReferenceProvenance(const PublicType &type,
                              std::size_t parameter_count,
                              bool allow_parameter) {
  if (type.kind == PublicTypeKind::Reference &&
      type.reference_provenance.kind ==
          PublicReferenceProvenanceKind::Parameter &&
      (!allow_parameter || type.reference_provenance.index >= parameter_count))
    return false;
  return std::ranges::all_of(type.arguments, [&](const PublicType &argument) {
    return validReferenceProvenance(argument, parameter_count, allow_parameter);
  });
}

CallableOwnershipSummary
normalizePublicOwnershipSummary(CallableOwnershipSummary summary,
                                std::span<const PublicType> parameters) {
  const auto normalize_region = [&](OwnershipRegion &region) {
    if (region.parameter_index >= parameters.size() || region.path.empty() ||
        parameters[region.parameter_index].kind != PublicTypeKind::Reference ||
        region.path.front().kind != OwnershipRegionStepKind::Dereference ||
        region.path.front().index != 0)
      return;
    region.path.erase(region.path.begin());
  };
  for (auto &effect : summary.effects)
    normalize_region(effect.region);
  for (auto &postcondition : summary.postconditions)
    normalize_region(postcondition.region);
  for (auto &source : summary.return_provenance)
    normalize_region(source.region);
  PublicInterfaceCanonicalizeService::callableOwnership(summary);
  return summary;
}

bool memberSignatureMatchesOwner(
    const std::optional<PublicEntityReferenceArtifact> &owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::span<const PublicType> parameters) {
  if (!owner)
    return member_kind == PublicFunctionArtifact::MemberKind::None;
  if (member_kind == PublicFunctionArtifact::MemberKind::Associated)
    return true;
  if (member_kind != PublicFunctionArtifact::MemberKind::Instance)
    return false;
  if (parameters.empty())
    return false;
  const auto *receiver = &parameters.front();
  if (receiver->kind == PublicTypeKind::Reference) {
    if (receiver->arguments.size() != 1)
      return false;
    receiver = &receiver->arguments.front();
  }
  return receiver->kind == PublicTypeKind::Nominal &&
         receiver->nominal_entity == *owner;
}

std::optional<ForeignAbiValue>
classifyForeignType(const PublicType &type, bool result,
                    const ForeignNominalResolver &resolve_nominal) {
  ForeignAbiValue value;
  switch (type.kind) {
  case PublicTypeKind::Void:
    if (!result)
      return std::nullopt;
    value.kind = ForeignAbiValueKind::Void;
    break;
  case PublicTypeKind::Bool:
    value.kind = ForeignAbiValueKind::Bool;
    value.width = 1;
    break;
  case PublicTypeKind::Char:
    value.kind = ForeignAbiValueKind::UnsignedInteger;
    value.width = 32;
    break;
  case PublicTypeKind::Integer:
    value.kind = type.integer_signed ? ForeignAbiValueKind::SignedInteger
                                     : ForeignAbiValueKind::UnsignedInteger;
    value.width = type.scalar_width;
    break;
  case PublicTypeKind::Float:
    value.kind = ForeignAbiValueKind::Float;
    value.width = type.scalar_width;
    break;
  case PublicTypeKind::Reference:
    value.kind = ForeignAbiValueKind::Reference;
    value.pointee_const =
        type.reference_mutability == PublicReferenceMutability::ReadOnly;
    break;
  case PublicTypeKind::RawPointer:
    value.kind = ForeignAbiValueKind::RawPointer;
    value.pointee_const = type.pointer_const;
    break;
  case PublicTypeKind::CFunctionPointer:
    value.kind = ForeignAbiValueKind::FunctionPointer;
    break;
  case PublicTypeKind::Nominal:
    if (resolve_nominal) {
      const auto representation = resolve_nominal(type);
      if (representation)
        return classifyForeignType(*representation, result, resolve_nominal);
    }
    value.kind = ForeignAbiValueKind::Aggregate;
    break;
  case PublicTypeKind::Array:
  case PublicTypeKind::Tuple:
    value.kind = ForeignAbiValueKind::Aggregate;
    break;
  case PublicTypeKind::TypeParameter:
  case PublicTypeKind::Never:
  case PublicTypeKind::String:
  case PublicTypeKind::Function:
  case PublicTypeKind::CallbackAdapter:
  case PublicTypeKind::CallbackRegistration:
  case PublicTypeKind::CallbackCompletion:
  case PublicTypeKind::CallbackWake:
  case PublicTypeKind::ForeignCompletion:
  case PublicTypeKind::ForeignWake:
  case PublicTypeKind::Slice:
  case PublicTypeKind::TypeProjection:
  case PublicTypeKind::ForeignOperationState:
  case PublicTypeKind::Count:
    return std::nullopt;
  }
  return value;
}

bool foreignSignatureMatches(std::span<const PublicType> parameters,
                             const PublicType &return_type,
                             const std::optional<ForeignAbiSignature> &stored,
                             const ForeignNominalResolver &resolve_nominal,
                             bool allows_hidden_parameters) {
  if (!stored ||
      (!allows_hidden_parameters &&
       stored->parameters.size() != parameters.size()) ||
      (allows_hidden_parameters &&
       stored->parameters.size() < parameters.size()))
    return false;
  const auto result = classifyForeignType(return_type, true, resolve_nominal);
  if (!result || *result != stored->result)
    return false;
  if (allows_hidden_parameters &&
      stored->parameters.size() != parameters.size()) {
    std::string error;
    return stored->verify(error);
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto parameter =
        classifyForeignType(parameters[index], false, resolve_nominal);
    if (!parameter || *parameter != stored->parameters[index])
      return false;
  }
  std::string error;
  return stored->verify(error);
}

bool ownershipMatchesSignature(std::span<const PublicType> parameters,
                               const PublicType &return_type,
                               const CallableOwnershipSummary &summary) {
  const auto contains_borrowed_view = [&](const auto &self,
                                          const PublicType &type) -> bool {
    if (type.kind == PublicTypeKind::Reference ||
        type.kind == PublicTypeKind::Slice)
      return true;
    if (type.kind != PublicTypeKind::Array &&
        type.kind != PublicTypeKind::Tuple)
      return false;
    return std::ranges::any_of(type.arguments, [&](const auto &element) {
      return self(self, element);
    });
  };
  const auto borrowed_return =
      contains_borrowed_view(contains_borrowed_view, return_type);
  if (summary.returns_owned && borrowed_return)
    return false;
  if (summary.returns_owned)
    return true;
  return std::ranges::all_of(
      summary.return_provenance, [&](const CallableReturnSource &source) {
        if (source.region.parameter_index >= parameters.size())
          return false;
        const auto &parameter = parameters[source.region.parameter_index];
        const auto return_mutable =
            return_type.kind == PublicTypeKind::Reference
                ? return_type.reference_mutability ==
                      PublicReferenceMutability::Mutable
                : return_type.kind == PublicTypeKind::Slice &&
                      return_type.slice_mutable;
        const auto source_mutable =
            parameter.kind == PublicTypeKind::Reference
                ? parameter.reference_mutability ==
                      PublicReferenceMutability::Mutable
                : parameter.kind == PublicTypeKind::Slice &&
                      parameter.slice_mutable;
        return (!return_mutable || source_mutable ||
                (parameter.kind != PublicTypeKind::Reference &&
                 parameter.kind != PublicTypeKind::Slice)) &&
               std::ranges::all_of(
                   source.condition.clauses, [&](const auto &clause) {
                     return std::ranges::all_of(
                         clause.atoms, [&](const auto &atom) {
                           return atom.parameter_index < parameters.size() &&
                                  parameters[atom.parameter_index].kind ==
                                      (atom.variant == core::AnyId::InvalidIndex ? PublicTypeKind::Bool : PublicTypeKind::Nominal);
                         });
                   });
      });
}

bool semanticContractMatchesSignature(
    std::span<const PublicType> parameters, const PublicType &return_type,
    const CallableSemanticContract &contract) {
  if (contract.domain == CallableSemanticDomain::Ordinary)
    return true;
  const auto owner_ref = [&](std::size_t index,
                             PublicReferenceMutability mutability) {
    return index < parameters.size() &&
           parameters[index].kind == PublicTypeKind::Reference &&
           parameters[index].arguments.size() == 1 &&
           parameters[index].reference_mutability == mutability &&
           parameters[index].arguments.front() == contract.owner;
  };
  using Role = CallableSemanticRole;
  switch (contract.role) {
  case Role::Copy:
  case Role::ObjectCopyInit:
    return parameters.size() == 2 &&
           owner_ref(0, PublicReferenceMutability::Mutable) &&
           owner_ref(1, PublicReferenceMutability::ReadOnly) &&
           return_type.kind == PublicTypeKind::Void;
  case Role::ObjectMoveInit:
    return parameters.size() == 2 &&
           owner_ref(0, PublicReferenceMutability::Mutable) &&
           owner_ref(1, PublicReferenceMutability::Mutable) &&
           return_type.kind == PublicTypeKind::Void;
  case Role::Drop:
  case Role::ObjectInit:
  case Role::ObjectDrop:
    return parameters.size() == 1 &&
           owner_ref(0, PublicReferenceMutability::Mutable) &&
           return_type.kind == PublicTypeKind::Void;
  case Role::Pack:
    return parameters.size() == 1 &&
           owner_ref(0, PublicReferenceMutability::ReadOnly) &&
           return_type.kind != PublicTypeKind::Void;
  case Role::Init:
  case Role::ProjectionStore:
  case Role::ProjectionInit:
    return parameters.size() == 2 &&
           owner_ref(0, PublicReferenceMutability::Mutable) &&
           return_type.kind == PublicTypeKind::Void;
  case Role::ProjectionLoad:
    return parameters.size() == 1 &&
           owner_ref(0, PublicReferenceMutability::ReadOnly) &&
           return_type.kind != PublicTypeKind::Void &&
           return_type.kind != PublicTypeKind::Reference;
  case Role::ProjectionTake:
    return parameters.size() == 1 &&
           owner_ref(0, PublicReferenceMutability::Mutable) &&
           return_type.kind != PublicTypeKind::Void &&
           return_type.kind != PublicTypeKind::Reference;
  case Role::ProjectionBorrow:
  case Role::ProjectionBorrowMut: {
    const auto mutability = contract.role == Role::ProjectionBorrowMut
                                ? PublicReferenceMutability::Mutable
                                : PublicReferenceMutability::ReadOnly;
    return parameters.size() == 1 && owner_ref(0, mutability) &&
           return_type.kind == PublicTypeKind::Reference &&
           return_type.reference_mutability == mutability;
  }
  case Role::Constructor:
    if (contract.owner.kind != PublicTypeKind::Nominal)
      return false;
    if (return_type == contract.owner)
      return true;
    return return_type.kind == PublicTypeKind::Nominal &&
           return_type.nominal_entity.canonical_package == "std" &&
           return_type.nominal_entity.canonical_module == "std::result" &&
           return_type.nominal_entity.canonical_name == "Result" &&
           return_type.arguments.size() == 2 &&
           return_type.arguments[0] == contract.owner;
  case Role::None:
  case Role::Count:
    return false;
  }
  return false;
}

bool semanticContractMatchesEffects(const CallableSemanticContract &contract,
                                    const CallableOwnershipSummary &summary) {
  if (contract.domain != CallableSemanticDomain::ObjectProjection)
    return true;
  const auto matches = [&](const OwnershipRegion &region) {
    return region.parameter_index == 0 && region.path.size() == 1 &&
           region.path.front().kind == OwnershipRegionStepKind::Field &&
           region.path.front().index == contract.projector_field &&
           region.has_bit_range == contract.has_bit_range &&
           (!region.has_bit_range || (region.bit_begin == contract.bit_begin &&
                                      region.bit_end == contract.bit_end));
  };
  return std::ranges::all_of(summary.effects,
                             [&](const auto &effect) {
                               // By-value operands describe accesses to the
                               // helper's local copy; only the owner-reference
                               // root is representation authority.
                               return effect.region.parameter_index != 0 ||
                                      matches(effect.region);
                             }) &&
         std::ranges::all_of(summary.postconditions,
                             [&](const auto &postcondition) {
                               return matches(postcondition.region);
                             }) &&
         std::ranges::all_of(
             summary.return_provenance,
             [&](const auto &source) { return matches(source.region); });
}



} // namespace chtholly::compiler::internal
