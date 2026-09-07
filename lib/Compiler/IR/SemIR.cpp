#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"
#include "chtholly/Compiler/CarrierView.h"

#include <array>
#include <cassert>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler {

std::size_t
ConstantValueHash::operator()(const ConstantValue &value) const noexcept {
  std::size_t result = static_cast<std::size_t>(value.kind);
  const auto mix = [&](std::size_t part) {
    result ^= part + 0x9e3779b97f4a7c15ULL + (result << 6U) + (result >> 2U);
  };
  mix(value.type.index);
  mix(static_cast<std::size_t>(value.payload));
  mix(value.elements.index);
  mix(value.target_dependent ? 1U : 0U);
  return result;
}
namespace {

constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
constexpr std::uint32_t UnionFieldIndexMask = ~UnionFieldUnsafeBit;
constexpr std::uint32_t ProjectionIndexMask = 0x7fffffffU;
constexpr std::uint32_t ProjectionKindShift = 31U;

std::size_t combineHash(std::size_t hash, std::uint32_t value) {
  hash ^= static_cast<std::size_t>(value) + 0x9e3779b9U + (hash << 6U) +
          (hash >> 2U);
  return hash;
}

constexpr std::uint32_t MutableReferenceBit = 1U;
constexpr std::uint32_t ParameterProvenanceBit = 1U << 1U;
constexpr std::uint32_t ProvenanceIndexShift = 2U;

std::uint32_t encodeReferenceFacts(SemReferenceMutability mutability,
                                   SemReferenceProvenanceKind provenance_kind,
                                   std::uint32_t provenance_index) {
  auto result =
      mutability == SemReferenceMutability::Mutable ? MutableReferenceBit : 0U;
  if (provenance_kind == SemReferenceProvenanceKind::Parameter) {
    assert(provenance_index <=
           (std::numeric_limits<std::uint32_t>::max() >> ProvenanceIndexShift));
    result |=
        ParameterProvenanceBit | (provenance_index << ProvenanceIndexShift);
  }
  return result;
}

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

std::size_t SemTypeHash::operator()(const SemType &type) const noexcept {
  auto hash = static_cast<std::size_t>(type.kind);
  hash = combineHash(hash, type.arg0);
  hash = combineHash(hash, type.arg1);
  return combineHash(hash, type.reserved);
}

std::size_t SemNameHash::operator()(const SemName &name) const noexcept {
  return static_cast<std::size_t>(name.text.index);
}

SemIR::SemIR(core::Arena &arena, SharedValueStores &values,
             CheckIRId check_ir_id, IdentifierId module_name,
             const PublicInterfaceRegistry &public_interfaces,
             const interop::ArtifactRegistry &interop_registry,
             std::span<const ImportIR> imports,
             LanguageVersion language_version)
    : values_(&values), check_ir_id_(check_ir_id), module_name_(module_name),
      language_version_(language_version),
      imports_(check_ir_id, public_interfaces, interop_registry, imports),
      constant_blocks_(arena), inst_blocks_(arena), type_blocks_(arena),
      local_blocks_(arena) {
  void_type_ = addType({SemTypeKind::Void});
  bool_type_ = addType({SemTypeKind::Bool});
  char_type_ = addType({SemTypeKind::Char});
  i32_type_ = addIntegerType(32, true);
  f64_type_ = addFloatType(64);
  string_type_ = addType({SemTypeKind::String});
  never_type_ = addType({SemTypeKind::Never});
  coroutine_executor_type_ = addType({SemTypeKind::CoroutineExecutor});
  coroutine_scope_type_ = addType({SemTypeKind::CoroutineScope});
  coroutine_task_completion_type_ =
      addType({SemTypeKind::CoroutineTaskCompletion});
}

CompilerIntrinsicRole SemIR::functionIntrinsicRole(FunctionRefId id) const {
  if (!id.hasValue() || id.index >= functionRefCount())
    return CompilerIntrinsicRole::None;
  const auto &reference = functionRef(id);
  if (reference.local_function.hasValue()) {
    const auto local_role = function(reference.local_function).intrinsic_role;
    if (local_role != CompilerIntrinsicRole::None)
      return local_role;
  }
  const auto *entity = importIRs().tryGetEntity(reference.public_entity);
  return entity ? entity->intrinsic_role : CompilerIntrinsicRole::None;
}

TypeId SemIR::addType(SemType type) {
  if (type.reserved == core::AnyId::InvalidIndex ||
      (type.reserved == 0 && type.kind != SemTypeKind::Void)) {
    CanonicalType canonical;
    switch (type.kind) {
    case SemTypeKind::Void:
      canonical.kind = CanonicalTypeKind::Void;
      break;
    case SemTypeKind::Never:
      canonical.kind = CanonicalTypeKind::Never;
      break;
    case SemTypeKind::Bool:
      canonical.kind = CanonicalTypeKind::Bool;
      break;
    case SemTypeKind::Char:
      canonical.kind = CanonicalTypeKind::Char;
      break;
    case SemTypeKind::Integer:
      canonical.kind = CanonicalTypeKind::Integer;
      canonical.arg0 = type.arg0;
      canonical.arg1 = type.arg1;
      break;
    case SemTypeKind::Float:
      canonical.kind = CanonicalTypeKind::Float;
      canonical.arg0 = type.arg0;
      break;
    case SemTypeKind::String:
      canonical.kind = CanonicalTypeKind::String;
      break;
    case SemTypeKind::Array:
      canonical.kind = CanonicalTypeKind::Array;
      canonical.arg0 = canonicalType(TypeId(type.arg0)).index;
      canonical.arg1 = type.arg1;
      break;
    case SemTypeKind::Tuple:
      canonical.kind = CanonicalTypeKind::Tuple;
      for (const auto element : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(element));
      if (type.reserved != core::AnyId::InvalidIndex)
        canonical.abi_union =
            values_->generics().type(CanonicalTypeId(type.reserved)).abi_union;
      break;
    case SemTypeKind::Slice:
      canonical.kind = CanonicalTypeKind::Slice;
      canonical.arg0 = type.arg1;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      break;
    case SemTypeKind::Function:
      canonical.kind = CanonicalTypeKind::Function;
      for (const auto parameter : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(parameter));
      canonical.elements.push_back(canonicalType(TypeId(type.arg1)));
      break;
    case SemTypeKind::AsyncFunction: {
      canonical.kind = CanonicalTypeKind::AsyncFunction;
      const auto parameters = typeBlock(TypeBlockId(type.arg0));
      canonical.arg0 = static_cast<std::uint32_t>(parameters.size());
      for (const auto parameter : parameters)
        canonical.elements.push_back(canonicalType(parameter));
      for (const auto outcome : typeBlock(TypeBlockId(type.arg1)))
        canonical.elements.push_back(canonicalType(outcome));
      break;
    }
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
      canonical.kind = type.kind == SemTypeKind::CFunctionPointer
                           ? CanonicalTypeKind::CFunctionPointer
                           : CanonicalTypeKind::CVariadicFunctionPointer;
      for (const auto parameter : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(parameter));
      canonical.elements.push_back(canonicalType(TypeId(type.arg1)));
      if (type.reserved != core::AnyId::InvalidIndex)
        canonical.foreign_calling_convention =
            values_->generics()
                .type(CanonicalTypeId(type.reserved))
                .foreign_calling_convention;
      break;
    case SemTypeKind::CallbackAdapter:
      canonical.kind = CanonicalTypeKind::CallbackAdapter;
      for (const auto element : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(element));
      break;
    case SemTypeKind::CallbackCompletion:
      canonical.kind = CanonicalTypeKind::CallbackCompletion;
      for (const auto element : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(element));
      canonical.registration_authority = type.arg1;
      if (type.reserved != core::AnyId::InvalidIndex) {
        const auto &existing =
            values_->generics().type(CanonicalTypeId(type.reserved));
        canonical.registration_arm_parameters =
            existing.registration_arm_parameters;
        canonical.registration_detach_parameters =
            existing.registration_detach_parameters;
      }
      break;
    case SemTypeKind::CallbackWake:
      canonical.kind = CanonicalTypeKind::CallbackWake;
      for (const auto element : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(element));
      break;
    case SemTypeKind::ForeignCompletion:
      canonical.kind = CanonicalTypeKind::ForeignCompletion;
      canonical.nominal_key =
          values_->generics()
              .type(canonicalType(addNominalType(NominalTypeId(type.arg0))))
              .nominal_key;
      break;
    case SemTypeKind::ForeignWake:
      canonical.kind = CanonicalTypeKind::ForeignWake;
      canonical.nominal_key =
          values_->generics()
              .type(canonicalType(addNominalType(NominalTypeId(type.arg0))))
              .nominal_key;
      break;
    case SemTypeKind::CallbackRegistration:
      canonical.kind = CanonicalTypeKind::CallbackRegistration;
      for (const auto element : typeBlock(TypeBlockId(type.arg0)))
        canonical.elements.push_back(canonicalType(element));
      canonical.registration_authority = type.arg1;
      if (type.reserved != core::AnyId::InvalidIndex) {
        const auto &existing =
            values_->generics().type(CanonicalTypeId(type.reserved));
        canonical.registration_entry_parameter =
            existing.registration_entry_parameter;
        canonical.registration_userdata_parameter =
            existing.registration_userdata_parameter;
        canonical.registration_release_parameter =
            existing.registration_release_parameter;
        canonical.registration_bindings = existing.registration_bindings;
        canonical.registration_arm_parameters =
            existing.registration_arm_parameters;
        canonical.registration_detach_parameters =
            existing.registration_detach_parameters;
      }
      break;
    case SemTypeKind::CoroutineExecutor:
      canonical.kind = CanonicalTypeKind::CoroutineExecutor;
      break;
    case SemTypeKind::CoroutineScope:
      canonical.kind = CanonicalTypeKind::CoroutineScope;
      break;
    case SemTypeKind::CoroutineTask:
    case SemTypeKind::CoroutineTaskOutcome:
      canonical.kind = type.kind == SemTypeKind::CoroutineTask
                           ? CanonicalTypeKind::CoroutineTask
                           : CanonicalTypeKind::CoroutineTaskOutcome;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      if (type.arg1 != core::AnyId::InvalidIndex)
        canonical.elements.push_back(canonicalType(TypeId(type.arg1)));
      break;
    case SemTypeKind::CoroutineTaskCompletion:
      canonical.kind = CanonicalTypeKind::CoroutineTaskCompletion;
      break;
    case SemTypeKind::CoroutineTaskCompletionSet:
      canonical.kind = CanonicalTypeKind::CoroutineTaskCompletionSet;
      canonical.arg0 = type.arg1;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      break;
    case SemTypeKind::CoroutineTaskSelection:
      canonical.kind = CanonicalTypeKind::CoroutineTaskSelection;
      canonical.arg0 = type.arg1;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      break;
    case SemTypeKind::CoroutineChecked:
      canonical.kind = CanonicalTypeKind::CoroutineChecked;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      break;
    case SemTypeKind::TypeParameter:
      canonical.kind = CanonicalTypeKind::TypeParameter;
      canonical.arg0 = type.arg0;
      canonical.arg1 = type.arg1;
      break;
    case SemTypeKind::Nominal: {
      canonical.kind = CanonicalTypeKind::Nominal;
      const auto &nominal = nominalType(NominalTypeId(type.arg0));
      if (const auto *entity = imports_.tryGetEntity(nominal.canonical_entity))
        canonical.nominal_key = std::string(identifier(entity->package_name)) +
                                "/" +
                                std::string(identifier(entity->module_name)) +
                                "::" + std::string(identifier(entity->name));
      else
        canonical.nominal_key =
            std::string(identifier(module_name_)) +
            "::" + std::string(identifier(name(nominal.name).text));
      for (const auto argument : typeBlock(TypeBlockId(type.arg1)))
        canonical.elements.push_back(canonicalType(argument));
      break;
    }
    case SemTypeKind::Reference:
      canonical.kind = CanonicalTypeKind::Reference;
      canonical.arg0 = type.arg1;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      break;
    case SemTypeKind::RawPointer:
      canonical.kind = CanonicalTypeKind::RawPointer;
      canonical.arg1 = type.arg1;
      canonical.elements.push_back(canonicalType(TypeId(type.arg0)));
      break;
    case SemTypeKind::TypeProjection:
      canonical.kind = CanonicalTypeKind::TypeProjection;
      canonical.arg0 = canonicalType(TypeId(type.arg0)).index;
      canonical.arg1 = type.arg1 & ProjectionIndexMask;
      canonical.projection_kind = static_cast<CanonicalTypeProjectionKind>(
          type.arg1 >> ProjectionKindShift);
      break;
    case SemTypeKind::Invalid:
    case SemTypeKind::Count:
      break;
    }
    type.reserved = values_->generics().internType(std::move(canonical)).index;
  }
  return types_.add(type);
}

bool SemIR::bindNominalSemanticWitness(TypeId id,
                                       NominalSemanticWitnessArtifact witness,
                                       std::string &error) {
  error.clear();
  if (!id.hasValue() || id.index >= types_.size() ||
      type(id).kind != SemTypeKind::Nominal || !witness.verify(error)) {
    if (error.empty())
      error =
          "nominal semantic witness can only bind to a concrete nominal type";
    return false;
  }
  if (const auto found = nominal_semantic_witnesses_.find(id.index);
      found != nominal_semantic_witnesses_.end()) {
    if (found->second != witness) {
      error = "nominal type already has a different nominal semantic witness";
      return false;
    }
    return true;
  }
  if (witness.representation.value_repr == ValueReprKind::Custom) {
    auto carrier = materializePublicType(
        *witness.value_repr_carrier, GenericId::invalid(),
        nominalType(NominalTypeId(type(id).arg0)).declaration, error);
    if (!carrier.hasValue())
      return false;
    const auto carrier_kind = type(carrier).kind;
    if (carrier_kind == SemTypeKind::Invalid ||
        carrier_kind == SemTypeKind::Void ||
        carrier_kind == SemTypeKind::Function ||
        carrier_kind == SemTypeKind::TypeParameter ||
        carrier_kind == SemTypeKind::Count) {
      error = "custom value representation does not have a concrete carrier";
      return false;
    }
    value_repr_carriers_.emplace(id.index, carrier);
  }
  if (witness.representation.object_repr == ObjectReprKind::Custom) {
    auto carrier = materializePublicType(
        *witness.object_repr_carrier, GenericId::invalid(),
        nominalType(NominalTypeId(type(id).arg0)).declaration, error);
    if (!carrier.hasValue() || type(carrier).kind != SemTypeKind::Nominal) {
      if (error.empty())
        error = "custom object representation has no concrete nominal carrier";
      return false;
    }
    object_repr_carriers_.emplace(id.index, carrier);
    const auto nominal_field_type = [&](TypeId owner,
                                        std::uint32_t field_index) {
      const auto &owner_type = type(owner);
      const auto &nominal = nominalType(NominalTypeId(owner_type.arg0));
      auto field_type = nominal.fields[field_index].type;
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
          if (value.kind == CanonicalTypeKind::Array && result.elements.empty())
            result.arg0 = self(self, CanonicalTypeId(value.arg0)).index;
          return values_->generics().internType(std::move(result));
        }
        return source;
      };
      return materializeType(substitute(substitute, canonicalType(field_type)));
    };
    std::vector<std::vector<SemObjectProjectionStep>> projections;
    projections.reserve(witness.object_field_projections.size());
    for (const auto &artifact_projection : witness.object_field_projections) {
      std::vector<SemObjectProjectionStep> projection;
      auto current = carrier;
      for (const auto field_index : artifact_projection.field_indices) {
        const auto &current_type = type(current);
        if (current_type.kind != SemTypeKind::Nominal ||
            field_index >=
                nominalType(NominalTypeId(current_type.arg0)).fields.size()) {
          error = "custom object projection has an invalid local field index";
          return false;
        }
        if (const auto nested = object_field_projections_.find(current.index);
            nested != object_field_projections_.end()) {
          if (field_index >= nested->second.size()) {
            error =
                "custom object projection has an incomplete carrier witness";
            return false;
          }
          projection.insert(projection.end(),
                            nested->second[field_index].begin(),
                            nested->second[field_index].end());
        } else {
          projection.push_back({current, field_index});
        }
        current = nominal_field_type(current, field_index);
      }
      projections.push_back(std::move(projection));
    }
    object_field_projections_.emplace(id.index, std::move(projections));
  }
  nominal_semantic_witnesses_.emplace(id.index, std::move(witness));
  return true;
}

NominalTypeId SemIR::addNominalTypeDecl(SemNominalType nominal) {
  return nominal_types_.add(std::move(nominal));
}

void SemIR::setNominalType(NominalTypeId id, SemNominalType nominal) {
  nominal_types_.get(id) = std::move(nominal);
}

NameId SemIR::addName(IdentifierId text) {
  return names_.add({text});
}

IntegerId SemIR::addInteger(std::int64_t value) const {
  return values_->internInteger(value);
}

StableFingerprint SemIR::specificDependencyFingerprint() const {
  std::vector<std::string> fingerprints;
  for (std::uint32_t index = 0; index < functions_.size(); ++index) {
    const auto &value = functions_.get(FunctionId(index));
    if (!value.specific.hasValue())
      continue;
    const auto &specific = values_->generics().specific(value.specific);
    if (!specific.fingerprint_key.empty())
      fingerprints.push_back(specific.fingerprint_key);
  }
  std::ranges::sort(fingerprints);
  fingerprints.erase(std::unique(fingerprints.begin(), fingerprints.end()),
                     fingerprints.end());
  std::string canonical = "chtholly.next.module-specifics.v1\n";
  for (const auto &fingerprint : fingerprints)
    canonical += fingerprint + "\n";
  return StableFingerprint::fromCanonicalBytes(canonical);
}

void SemIR::recordSpecializationLookup(bool hit) {
  ++specialization_cache_stats_.lookups;
  if (hit)
    ++specialization_cache_stats_.hits;
  else
    ++specialization_cache_stats_.misses;
}

void SemIR::recordSpecializationSemanticRejection() {
  if (specialization_cache_stats_.hits != 0)
    --specialization_cache_stats_.hits;
  ++specialization_cache_stats_.misses;
  ++specialization_cache_stats_.semantic_rejections;
}

void SemIR::recordSpecializationComponent(
    ConcreteSpecializationComponentArtifact artifact, bool rebuilt) {
  const auto component_fingerprint = artifact.fingerprint();
  if (std::ranges::any_of(specialization_components_, [&](const auto &stored) {
        return stored.fingerprint() == component_fingerprint;
      }))
    return;
  for (const auto &node : artifact.nodes())
    specialization_references_.push_back(
        {node.request_fingerprint, component_fingerprint});
  specialization_components_.push_back(std::move(artifact));
  if (rebuilt)
    ++specialization_cache_stats_.rebuilt_components;
}

LocalId SemIR::addLocal(SemLocal local) {
  return locals_.add(local);
}

ConstantEntityId SemIR::addConstantEntity(SemConstant constant) {
  return constants_.add(std::move(constant));
}

FunctionId SemIR::addFunction(SemFunction function) {
  const auto id = functions_.add(function);
  coroutine_constructor_entities_.push_back(PublicEntityId::invalid());
  SemCallableSemanticContract contract;
  contract.role = static_cast<CallableSemanticRole>(function.semantic_role);
  contract.domain = semanticDomain(contract.role);
  contract.capability = semanticCapability(contract.role);
  if (contract.domain != CallableSemanticDomain::Ordinary)
    contract.owner = function.semantic_owner;
  contract.whole_carrier =
      contract.domain == CallableSemanticDomain::ValueRepresentation ||
      contract.domain == CallableSemanticDomain::ObjectShell;
  if (contract.domain == CallableSemanticDomain::ObjectProjection &&
      function.semantic_owner.hasValue()) {
    const auto &owner = nominalType(function.semantic_owner);
    for (std::uint32_t field = 0; field < owner.fields.size(); ++field) {
      if (owner.fields[field].projector_name != function.semantic_projector)
        continue;
      contract.projector_field = field;
      contract.has_bit_range = owner.fields[field].bit_begin != 0 ||
                               owner.fields[field].bit_end != 0;
      contract.bit_begin = owner.fields[field].bit_begin;
      contract.bit_end = owner.fields[field].bit_end;
      auto current = owner.object_repr_pattern;
      for (const auto component : owner.fields[field].projection_region_path) {
        if (!current.hasValue() || type(current).kind != SemTypeKind::Nominal)
          break;
        const auto &carrier = nominalType(NominalTypeId(type(current).arg0));
        const auto found = std::ranges::find_if(
            carrier.fields, [&](const SemNominalField &candidate) {
              return candidate.name == component;
            });
        if (found == carrier.fields.end())
          break;
        const auto index = static_cast<std::uint32_t>(
            std::distance(carrier.fields.begin(), found));
        contract.carrier_path.push_back(index);
        current = found->type;
      }
      contract.whole_carrier = contract.carrier_path.empty();
      break;
    }
  }
  function_semantic_contracts_.push_back(std::move(contract));
  function_ownership_summaries_.emplace_back();
  expected_function_ownership_summaries_.emplace_back();
  function_declarations_.emplace_back();
  function_constraints_.emplace_back();
  return id;
}

FunctionRefId SemIR::addFunctionRef(SemFunctionRef function_ref) {
  const auto id = function_refs_.add(function_ref);
  function_ref_concrete_arguments_.emplace_back();
  return id;
}

bool SemIR::isConcreteReverseTarget(FunctionRefId target) const {
  if (!target.hasValue() || target.index >= functionRefCount())
    return false;
  const auto &reference = functionRef(target);
  if (reference.generic.hasValue())
    return false;
  if (reference.local_function.hasValue()) {
    const auto &declaration = functionDeclaration(reference.local_function);
    return declaration.kind == SemCallableDeclarationKind::Definition &&
           !declaration.is_unsafe &&
           functionSemanticContract(reference.local_function).domain ==
               CallableSemanticDomain::Ordinary;
  }
  const auto *entity = imports_.tryGetEntity(reference.public_entity);
  return entity && entity->kind == PublicEntityKind::Function &&
         entity->generic_parameter_count == 0 &&
         entity->declaration_kind ==
             PublicCallableDeclarationKind::Definition &&
         !entity->is_unsafe &&
         entity->semantic_contract.domain == CallableSemanticDomain::Ordinary &&
         (entity->member_kind != PublicFunctionArtifact::MemberKind::Instance ||
          !reverseTargetWitnesses(target).empty());
}

std::vector<StableFingerprint>
SemIR::reverseTargetWitnesses(FunctionRefId target) const {
  std::vector<StableFingerprint> result;
  if (!target.hasValue() || target.index >= functionRefCount())
    return result;
  const auto &reference = functionRef(target);
  for (std::uint32_t index = 0; index < interfaceWitnessCount(); ++index) {
    const auto &witness = interfaceWitness(InterfaceWitnessId(index));
    if (witness.state != SemInterfaceWitnessState::Complete ||
        witness.generic.hasValue())
      continue;
    if (std::ranges::any_of(witness.entries, [&](const auto &entry) {
          return entry.function == target;
        }))
      result.push_back(witness.fingerprint);
  }
  if (reference.public_entity.hasValue()) {
    const auto *entity = imports_.tryGetEntity(reference.public_entity);
    if (entity) {
      const auto matches = [&](const PublicEntityReferenceArtifact &candidate) {
        return candidate.kind == PublicEntityKind::Function &&
               candidate.canonical_package ==
                   identifier(entity->package_name) &&
               candidate.canonical_module == identifier(entity->module_name) &&
               candidate.canonical_name == identifier(entity->name) &&
               candidate.expected_fingerprint == entity->fingerprint;
      };
      for (std::uint32_t index = 0; index < imports_.size(); ++index) {
        const auto *interface = imports_.tryGetInterface(ImportIRId(index));
        if (!interface)
          continue;
        for (const auto &witness : interface->interfaceWitnessArtifacts()) {
          if (witness.generic_parameter_count != 0)
            continue;
          if (std::ranges::any_of(witness.entries, [&](const auto &entry) {
                return matches(entry.function);
              }))
            result.push_back(witness.fingerprint);
        }
      }
    }
  }
  std::ranges::sort(result, {}, [](const StableFingerprint &fingerprint) {
    return fingerprint.hex();
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

FunctionRefId SemIR::addCanonicalExternalFunctionRef(
    PublicEntityId entity, std::string &error,
    std::span<const PublicType> concrete_arguments) {
  error.clear();
  const auto *function = imports_.tryGetEntity(entity);
  if (!function || function->kind != PublicEntityKind::Function ||
      function->generic_parameter_count != concrete_arguments.size()) {
    error = "canonical semantic target has invalid concrete arguments";
    return FunctionRefId::invalid();
  }
  const auto substitute = [&](const auto &self,
                              const PublicType &source) -> PublicType {
    if (source.kind == PublicTypeKind::TypeParameter)
      return source.binding_index < concrete_arguments.size()
                 ? concrete_arguments[source.binding_index]
                 : PublicType{};
    auto result = source;
    result.arguments.clear();
    for (const auto &argument : source.arguments)
      result.arguments.push_back(self(self, argument));
    return result;
  };
  SpecificId specific;
  if (function->generic.hasValue()) {
    std::vector<CanonicalConstantId> constants;
    constants.reserve(concrete_arguments.size());
    for (const auto &argument : concrete_arguments) {
      const auto local = materializePublicType(argument, GenericId::invalid(),
                                               NodeId::invalid(), error);
      if (!local.hasValue())
        return FunctionRefId::invalid();
      constants.push_back(values_->generics().internTypeConstant(
          canonicalType(local), ConstantDependence::Concrete));
    }
    specific =
        values_->generics().getOrAddSpecific(function->generic, constants);
  }
  for (std::uint32_t index = 0; index < function_refs_.size(); ++index) {
    const auto id = FunctionRefId(index);
    const auto &existing = functionRef(id);
    // A local imported evaluator may share the public entity and SpecificId.
    // It is not a canonical external reference and must never satisfy this
    // lookup: intrinsic lowering needs the artifact-owned callable identity.
    if (!existing.local_function.hasValue() &&
        !existing.import_ir_inst.hasValue() &&
        existing.public_entity == entity && existing.specific == specific) {
      setFunctionRefConcreteArguments(
          id, std::vector<PublicType>(concrete_arguments.begin(),
                                      concrete_arguments.end()));
      return id;
    }
  }
  std::vector<TypeId> parameters;
  parameters.reserve(function->parameters.size());
  for (const auto &parameter : function->parameters) {
    const auto local =
        materializePublicType(substitute(substitute, parameter),
                              GenericId::invalid(), NodeId::invalid(), error);
    if (!local.hasValue())
      return FunctionRefId::invalid();
    parameters.push_back(local);
  }
  const auto result =
      materializePublicType(substitute(substitute, function->return_type),
                            GenericId::invalid(), NodeId::invalid(), error);
  if (!result.hasValue())
    return FunctionRefId::invalid();
  std::optional<TypeId> error_type;
  if (function->error_type) {
    error_type =
        materializePublicType(substitute(substitute, *function->error_type),
                              GenericId::invalid(), NodeId::invalid(), error);
    if (!error_type->hasValue())
      return FunctionRefId::invalid();
  }
  const auto type =
      function->execution_kind == PublicFunctionExecutionKind::Async
          ? addAsyncFunctionType(parameters, result, error_type)
          : addType({SemTypeKind::Function, addTypeBlock(parameters).index,
                     result.index, 0});
  const auto reference =
      addFunctionRef({FunctionId::invalid(), ImportIRInstId::invalid(), type,
                      entity, function->generic, specific});
  setFunctionRefConcreteArguments(
      reference, std::vector<PublicType>(concrete_arguments.begin(),
                                         concrete_arguments.end()));
  return reference;
}

std::optional<CanonicalIntrinsicResolution> SemIR::resolveCanonicalIntrinsic(
    PublicEntityId entity, std::span<const PublicType> concrete_arguments,
    const CanonicalIntrinsicShapeSpec &shape, std::string &error) {
  error.clear();
  const auto reference =
      addCanonicalExternalFunctionRef(entity, error, concrete_arguments);
  if (!reference.hasValue())
    return std::nullopt;

  const auto &function_ref = functionRef(reference);
  if (function_ref.local_function.hasValue() ||
      function_ref.import_ir_inst.hasValue() ||
      !function_ref.public_entity.hasValue()) {
    error = "canonical intrinsic resolution produced a non-external target";
    return std::nullopt;
  }
  const auto &function_type_value = type(function_ref.local_type);
  const bool is_async = function_type_value.kind == SemTypeKind::AsyncFunction;
  if (function_type_value.kind != SemTypeKind::Function && !is_async) {
    error = "canonical intrinsic resolution produced an invalid function type";
    return std::nullopt;
  }
  if (shape.is_async && *shape.is_async != is_async) {
    error = "canonical intrinsic execution kind does not match its shape";
    return std::nullopt;
  }

  const auto parameters = typeBlock(TypeBlockId(function_type_value.arg0));
  if (parameters.size() != shape.parameter_types.size()) {
    error = "canonical intrinsic parameter count does not match its shape";
    return std::nullopt;
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (!shape.parameter_types[index].hasValue() ||
        canonicalType(parameters[index]) !=
            canonicalType(shape.parameter_types[index])) {
      error = "canonical intrinsic parameter type does not match its shape";
      return std::nullopt;
    }
  }

  const auto result = is_async ? asyncSuccessType(function_ref.local_type)
                               : TypeId(function_type_value.arg1);
  if (!shape.return_type.hasValue() ||
      canonicalType(result) != canonicalType(shape.return_type)) {
    error = "canonical intrinsic return type does not match its shape";
    return std::nullopt;
  }
  const auto async_error = asyncErrorType(function_ref.local_type);
  if (shape.has_error_type != async_error.has_value() ||
      (shape.has_error_type &&
       (!shape.error_type.hasValue() ||
        canonicalType(*async_error) != canonicalType(shape.error_type)))) {
    error = "canonical intrinsic error type does not match its shape";
    return std::nullopt;
  }

  CanonicalIntrinsicResolution result_value;
  result_value.reference = reference;
  result_value.function_type = function_ref.local_type;
  result_value.parameter_types.assign(parameters.begin(), parameters.end());
  result_value.return_type = result;
  result_value.has_error_type = async_error.has_value();
  if (async_error)
    result_value.error_type = *async_error;
  return result_value;
}

std::optional<FunctionRefId>
SemIR::resolveCanonicalIntrinsic(FunctionRefId reference, std::string &error) {
  error.clear();
  if (!reference.hasValue() || reference.index >= functionRefCount()) {
    error = "canonical intrinsic reference is invalid";
    return std::nullopt;
  }
  if (functionIntrinsicRole(reference) == CompilerIntrinsicRole::None)
    return reference;
  const auto &source = functionRef(reference);
  if (!source.public_entity.hasValue())
    return reference;
  const auto *entity = imports_.tryGetEntity(source.public_entity);
  if (!entity || entity->kind != PublicEntityKind::Function) {
    error = "canonical intrinsic reference has no imported function entity";
    return std::nullopt;
  }
  const auto concrete_arguments = functionRefConcreteArguments(reference);
  // A private local nominal cannot be encoded as a PublicType. Its local
  // specific remains valid for compiler-owned intrinsic execution and is
  // still checked by the ordinary SemIR intrinsic verifier.
  if (entity->generic_parameter_count != 0 && concrete_arguments.empty())
    return reference;

  const auto &function_type = type(source.local_type);
  if (function_type.kind != SemTypeKind::Function &&
      function_type.kind != SemTypeKind::AsyncFunction) {
    error = "canonical intrinsic reference has an invalid function type";
    return std::nullopt;
  }
  const bool is_async = function_type.kind == SemTypeKind::AsyncFunction;
  const auto parameters = typeBlock(TypeBlockId(function_type.arg0));
  const auto result = is_async ? asyncSuccessType(source.local_type)
                               : TypeId(function_type.arg1);
  const auto async_error =
      is_async ? asyncErrorType(source.local_type) : std::optional<TypeId>{};
  CanonicalIntrinsicShapeSpec shape{parameters, result,
                                    async_error.value_or(TypeId::invalid()),
                                    async_error.has_value(), is_async};
  const auto resolved = resolveCanonicalIntrinsic(
      source.public_entity, concrete_arguments, shape, error);
  return resolved ? std::optional<FunctionRefId>(resolved->reference)
                  : std::nullopt;
}

bool SemIR::bindCoroutineConstructorEntity(FunctionId scaffold,
                                           PublicEntityId entity,
                                           std::string &error) {
  error.clear();
  if (!scaffold.hasValue() || scaffold.index >= functions_.size() ||
      !entity.hasValue()) {
    error = "coroutine constructor binding has an invalid identity";
    return false;
  }
  const auto &local = function(scaffold);
  const auto *external = imports_.tryGetEntity(entity);
  if ((local.flags & (SemFunctionCoroutineScaffold | SemFunctionAsync)) !=
          (SemFunctionCoroutineScaffold | SemFunctionAsync) ||
      (local.flags & (SemFunctionTemplate | SemFunctionSpecific)) != 0 ||
      type(local.type).kind != SemTypeKind::AsyncFunction || !external ||
      external->kind != PublicEntityKind::Function ||
      external->execution_kind != PublicFunctionExecutionKind::Async ||
      external->coroutine_constructor !=
          PublicCoroutineConstructorABI{1, true, true, true, true}) {
    error = "coroutine constructor binding disagrees with its async entity";
    return false;
  }
  const auto local_parameters = typeBlock(TypeBlockId(type(local.type).arg0));
  if (local_parameters.size() != external->parameters.size()) {
    error = "coroutine constructor binding has an incompatible signature";
    return false;
  }
  // Reuse canonical external materialization to compare the complete ordered
  // signature, including success and error outcomes.
  const auto reference = addCanonicalExternalFunctionRef(entity, error);
  if (!reference.hasValue() ||
      functionRef(reference).local_type != local.type) {
    if (error.empty())
      error = "coroutine constructor binding has an incompatible signature";
    return false;
  }
  auto &bound = coroutine_constructor_entities_[scaffold.index];
  if (bound.hasValue() && bound != entity) {
    error = "coroutine scaffold has conflicting constructor identities";
    return false;
  }
  bound = entity;
  return true;
}

void SemIR::addLocalOccurrence(NodeId location, SemSymbolOccurrenceKind kind,
                               LocalId local) {
  symbol_occurrences_.push_back(
      {location, kind, SemSymbolTargetKind::Local, local.index});
}

void SemIR::addFunctionOccurrence(NodeId location, SemSymbolOccurrenceKind kind,
                                  FunctionRefId function) {
  symbol_occurrences_.push_back(
      {location, kind, SemSymbolTargetKind::Function, function.index});
}

void SemIR::addConstantOccurrence(NodeId location, SemSymbolOccurrenceKind kind,
                                  ConstantEntityId constant) {
  symbol_occurrences_.push_back(
      {location, kind, SemSymbolTargetKind::Constant, constant.index});
}

void SemIR::setFunction(FunctionId id, SemFunction function) {
  functions_.get(id) = function;
}

InstId SemIR::addRawInst(SemInst inst, NodeId location) {
  const auto id = insts_.add(inst);
  const auto location_id = locations_.add(location);
  assert(location_id == id);
  return id;
}

InstBlockId SemIR::addInstBlock(std::span<const InstId> insts, bool canonical) {
  return canonical ? inst_blocks_.addCanonical(insts) : inst_blocks_.add(insts);
}

TypeBlockId SemIR::addTypeBlock(std::span<const TypeId> types) {
  return type_blocks_.addCanonical(types);
}

std::uint32_t SemIR::addTypeQuery(SemTypeQueryArtifact query) {
  type_queries_.push_back(std::move(query));
  return static_cast<std::uint32_t>(type_queries_.size() - 1);
}

const SemTypeQueryArtifact &SemIR::typeQuery(std::uint32_t index) const {
  return type_queries_.at(index);
}

LocalBlockId SemIR::addLocalBlock(std::span<const LocalId> locals) {
  return local_blocks_.add(locals);
}

bool SemIR::containsArg(SemArgKind kind, std::uint32_t raw) const {
  if (kind == SemArgKind::None)
    return raw == core::AnyId::InvalidIndex;
  if (raw == core::AnyId::InvalidIndex)
    return false;
  switch (kind) {
  case SemArgKind::None:
    return false;
  case SemArgKind::Inst:
    return raw < insts_.size();
  case SemArgKind::Type:
    return raw < types_.size();
  case SemArgKind::Name:
    return raw < names_.size();
  case SemArgKind::Function:
    return raw < functions_.size();
  case SemArgKind::FunctionRef:
    return raw < function_refs_.size();
  case SemArgKind::Local:
    return raw < locals_.size();
  case SemArgKind::Integer:
    return raw < values_->integerCount();
  case SemArgKind::String:
    return raw < values_->stringLiteralCount();
  case SemArgKind::Block:
    return raw < inst_blocks_.size();
  case SemArgKind::Constant:
    return raw < constants_.size();
  }
  return false;
}

std::string SemIR::print() const {
  std::ostringstream out;
  out << "types:\n";
  for (std::size_t index = 0; index < types_.size(); ++index) {
    const auto &value = type(TypeId(static_cast<std::uint32_t>(index)));
    out << "  type" << index << " = " << semTypeKindName(value.kind);
    if (value.kind == SemTypeKind::Array)
      out << " type" << value.arg0 << '[' << value.arg1 << ']';
    if (value.kind == SemTypeKind::Function)
      out << " params" << value.arg0 << " -> type" << value.arg1;
    if (value.kind == SemTypeKind::AsyncFunction) {
      const auto outcomes = typeBlock(TypeBlockId(value.arg1));
      out << " async-params" << value.arg0 << " -> type"
          << outcomes.front().index;
      if (outcomes.size() == 2)
        out << " error type" << outcomes.back().index;
    }
    if (value.kind == SemTypeKind::Nominal)
      out << " nominal" << value.arg0 << " args" << value.arg1;
    if (value.kind == SemTypeKind::Reference)
      out << " type" << value.arg0
          << (referenceMutability(TypeId(static_cast<std::uint32_t>(index))) ==
                      SemReferenceMutability::Mutable
                  ? "&"
                  : " const&");
    out << '\n';
  }
  out << "functions:\n";
  for (std::size_t index = 0; index < functions_.size(); ++index) {
    const auto &value = function(FunctionId(static_cast<std::uint32_t>(index)));
    out << "  fn" << index << ' ' << identifier(name(value.name).text)
        << " : type" << value.type.index << " body=block" << value.body.index
        << ((value.flags & SemFunctionPublic) != 0 ? " public" : "")
        << ((value.flags & SemFunctionTemplate) != 0 ? " template" : "")
        << ((value.flags & SemFunctionSpecific) != 0 ? " specific" : "")
        << ((value.flags & SemFunctionCoroutineScaffold) != 0
                ? " coroutine-scaffold"
                : "")
        << ((value.flags & SemFunctionAsync) != 0 ? " async" : "")
        << ((value.flags & SemFunctionCoroutineExecutionEntry) != 0
                ? " coroutine-entry"
                : "")
        << (value.generic.hasValue()
                ? " generic=" + std::to_string(value.generic.index)
                : std::string{})
        << (value.specific.hasValue()
                ? " specific-id=" + std::to_string(value.specific.index)
                : std::string{})
        << '\n';
  }
  out << "function_refs:\n";
  for (std::size_t index = 0; index < function_refs_.size(); ++index) {
    const auto &value =
        functionRef(FunctionRefId(static_cast<std::uint32_t>(index)));
    out << "  ref" << index << " = ";
    if (value.local_function.hasValue())
      out << "local.fn" << value.local_function.index;
    else
      out << "import" << value.import_ir_inst.index;
    out << " : type" << value.local_type.index << '\n';
  }
  out << "instructions:\n";
  for (std::size_t index = 0; index < insts_.size(); ++index) {
    const auto &value = inst(InstId(static_cast<std::uint32_t>(index)));
    out << "  inst" << index << " = " << semInstKindName(value.kind)
        << " type=type" << value.type << " arg0=" << value.arg0
        << " arg1=" << value.arg1 << '\n';
  }
  out << "blocks:\n";
  for (std::size_t index = 0; index < inst_blocks_.size(); ++index) {
    out << "  block" << index << " = [";
    bool first = true;
    for (const auto id :
         instBlock(InstBlockId(static_cast<std::uint32_t>(index)))) {
      if (!first)
        out << ", ";
      first = false;
      out << "inst" << id.index;
    }
    out << "]\n";
  }
  out << "top = block" << top_block_.index << '\n';
  return out.str();
}

void SemIR::collectMetrics(core::CompilerMetrics &metrics,
                           std::string_view label) const {
  types_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "types"));
  names_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "names"));
  locals_.collectMetrics(metrics,
                         core::CompilerMetrics::childLabel(label, "locals"));
  functions_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "functions"));
  function_refs_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "function_refs"));
  imports_.collectMetrics(metrics,
                          core::CompilerMetrics::childLabel(label, "imports"));
  insts_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "insts"));
  locations_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "locations"));
  inst_blocks_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "inst_blocks"));
  type_blocks_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "type_blocks"));
  local_blocks_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "local_blocks"));
}

} // namespace chtholly::compiler
