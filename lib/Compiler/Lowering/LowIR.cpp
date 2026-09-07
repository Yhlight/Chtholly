#include "chtholly/Compiler/LowIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <deque>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler {
namespace {
constexpr auto Names = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1) #Name,
#include "chtholly/Compiler/LowIRKind.def"
});
constexpr auto Args = std::to_array<std::array<LowArgKind, 2>>({
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1)                               \
  std::array{LowArgKind::Arg0, LowArgKind::Arg1},
#include "chtholly/Compiler/LowIRKind.def"
});
static_assert(Names.size() == static_cast<std::size_t>(LowInstKind::Count));
static_assert(Args.size() == static_cast<std::size_t>(LowInstKind::Count));

bool isTerminator(LowInstKind kind) {
  return kind == LowInstKind::Branch || kind == LowInstKind::BranchIf ||
         kind == LowInstKind::Return || kind == LowInstKind::ReturnInPlace ||
         kind == LowInstKind::Unreachable ||
         kind == LowInstKind::FatalFailure ||
         kind == LowInstKind::CoroutineRuntimeFault ||
         kind == LowInstKind::CoroutineReturnSuccess ||
         kind == LowInstKind::CoroutineReturnError ||
         kind == LowInstKind::CoroutineReturnCancelled ||
         kind == LowInstKind::CoroutineCleanupEnd;
}

ForeignAbiTargetKind classifyForeignTarget(std::string_view triple) {
  std::string value(triple);
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  const auto windows = value.find("windows") != std::string::npos;
  if (value.starts_with("x86_64") || value.starts_with("amd64"))
    return windows ? ForeignAbiTargetKind::WindowsX64
                   : ForeignAbiTargetKind::SysVAMD64;
  if (value.starts_with("aarch64") && !windows &&
      (value.find("linux") != std::string::npos ||
       value.find("gnu") != std::string::npos))
    return ForeignAbiTargetKind::AAPCS64;
  return ForeignAbiTargetKind::Unsupported;
}

const ForeignAbiSignature *foreignSignature(const SemIR &sem_ir,
                                            FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir.functionDeclaration(reference.local_function);
    return declaration.foreign_signature ? &*declaration.foreign_signature
                                         : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->foreign_signature ? &*entity->foreign_signature
                                             : nullptr;
}

const interop::ForeignOperationArtifact *
foreignOperation(const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir.functionDeclaration(reference.local_function);
    return declaration.interop_artifact
               ? sem_ir.importIRs().interopRegistry().resolve(
                     *declaration.interop_artifact)
               : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->interop_artifact
             ? sem_ir.importIRs().interopRegistry().resolve(
                   *entity->interop_artifact)
             : nullptr;
}

bool isForeign(const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue())
    return sem_ir.functionDeclaration(reference.local_function).kind ==
           SemCallableDeclarationKind::Foreign;
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity &&
         entity->declaration_kind == PublicCallableDeclarationKind::Foreign;
}

const CanonicalForeignResourceProtocol *verifiedForeignResourceProtocol(
    const SemIR &sem_ir, TypeId type, bool completion_projection,
    ForeignResourceProtocolId &protocol_id, std::string &error) {
  protocol_id = sem_ir.foreignResourceProtocolId(type);
  const auto &protocol = sem_ir.foreignResourceProtocol(type);
  const auto fields = sem_ir.typeBlock(TypeBlockId(sem_ir.type(type).arg0));
  if (protocol.facts.completion_projection != completion_projection ||
      protocol.types.size() != fields.size()) {
    error = "foreign resource protocol disagrees with its semantic type";
    return nullptr;
  }
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (protocol.types[index] != sem_ir.canonicalType(fields[index])) {
      error = "foreign resource protocol has stale canonical type bindings";
      return nullptr;
    }
  }
  if (!protocol.facts.verify(static_cast<std::uint32_t>(protocol.types.size()),
                             error))
    return nullptr;
  return &protocol;
}
} // namespace

LowIR::LowIR(core::Arena &arena, const SemIR &sem_ir,
             std::string normalized_target_triple,
             std::span<const NominalTypeLayoutArtifact> nominal_layouts,
             std::span<const LowNominalLayoutBinding> nominal_layout_bindings)
    : sem_ir_(&sem_ir),
      normalized_target_triple_(std::move(normalized_target_triple)),
      inst_blocks_(arena), block_lists_(arena), slot_blocks_(arena),
      value_blocks_(arena), place_projection_blocks_(arena) {
  nominal_layouts_required_ =
      !nominal_layouts.empty() || !nominal_layout_bindings.empty();
  type_representations_.reserve(sem_ir.typeCount());
  const auto conversion_target = [&](TypeId type,
                                     SemCanonicalFunctionRole role) {
    if (sem_ir.type(type).kind != SemTypeKind::Nominal)
      return FunctionRefId::invalid();
    const auto nominal = NominalTypeId(sem_ir.type(type).arg0);
    const auto exact_signature = [&](FunctionRefId id) {
      const auto &function_type =
          sem_ir.type(sem_ir.functionRef(id).local_type);
      if (function_type.kind != SemTypeKind::Function)
        return false;
      const auto parameters = sem_ir.typeBlock(TypeBlockId(function_type.arg0));
      const auto owner_ref = [&](TypeId parameter,
                                 SemReferenceMutability mutability) {
        return sem_ir.type(parameter).kind == SemTypeKind::Reference &&
               sem_ir.referencePointee(parameter) == type &&
               sem_ir.referenceMutability(parameter) == mutability;
      };
      const auto carrier = sem_ir.valueRepresentationType(type);
      return role == SemCanonicalFunctionRole::Pack
                 ? parameters.size() == 1 &&
                       owner_ref(parameters[0],
                                 SemReferenceMutability::ReadOnly) &&
                       TypeId(function_type.arg1) == carrier
                 : parameters.size() == 2 &&
                       owner_ref(parameters[0],
                                 SemReferenceMutability::Mutable) &&
                       parameters[1] == carrier &&
                       TypeId(function_type.arg1) == sem_ir.voidType();
    };
    for (std::uint32_t index = 0; index < sem_ir.functionRefCount(); ++index) {
      const auto id = FunctionRefId(index);
      const auto &reference = sem_ir.functionRef(id);
      if (!reference.local_function.hasValue())
        continue;
      const auto &contract =
          sem_ir.functionSemanticContract(reference.local_function);
      if (contract.owner == nominal && contract.role == role &&
          contract.domain == CallableSemanticDomain::ValueRepresentation &&
          exact_signature(id))
        return id;
    }
    const auto *witness = sem_ir.nominalSemanticWitness(type);
    const auto *target = witness && role == SemCanonicalFunctionRole::Pack &&
                                 witness->pack_target
                             ? &*witness->pack_target
                         : witness && role == SemCanonicalFunctionRole::Init &&
                                 witness->init_target
                             ? &*witness->init_target
                             : nullptr;
    if (!target)
      return FunctionRefId::invalid();
    for (std::uint32_t index = 0; index < sem_ir.functionRefCount(); ++index) {
      const auto id = FunctionRefId(index);
      const auto &reference = sem_ir.functionRef(id);
      if (!reference.public_entity.hasValue())
        continue;
      const auto *entity =
          sem_ir.importIRs().tryGetEntity(reference.public_entity);
      if (entity &&
          sem_ir.identifier(entity->package_name) ==
              target->canonical_package &&
          sem_ir.identifier(entity->module_name) == target->canonical_module &&
          sem_ir.identifier(entity->name) == target->canonical_name &&
          entity->fingerprint == target->expected_fingerprint)
        return id;
    }
    return FunctionRefId::invalid();
  };
  const auto canonical_target =
      [&](const std::optional<PublicEntityReferenceArtifact> &target,
          TypeId owner_type) {
        if (!target)
          return FunctionRefId::invalid();
        FunctionRefId fallback;
        for (std::uint32_t ref_index = 0; ref_index < sem_ir.functionRefCount();
             ++ref_index) {
          const auto id = FunctionRefId(ref_index);
          const auto &reference = sem_ir.functionRef(id);
          if (!reference.public_entity.hasValue())
            continue;
          const auto *entity =
              sem_ir.importIRs().tryGetEntity(reference.public_entity);
          if (entity &&
              sem_ir.identifier(entity->package_name) ==
                  target->canonical_package &&
              sem_ir.identifier(entity->module_name) ==
                  target->canonical_module &&
              sem_ir.identifier(entity->name) == target->canonical_name &&
              entity->fingerprint == target->expected_fingerprint) {
            fallback = id;
            if (!owner_type.hasValue())
              return id;
            const auto &function_type = sem_ir.type(reference.local_type);
            const auto parameters =
                function_type.kind == SemTypeKind::Function
                    ? sem_ir.typeBlock(TypeBlockId(function_type.arg0))
                    : std::span<const TypeId>{};
            if (!parameters.empty() &&
                sem_ir.type(parameters.front()).kind ==
                    SemTypeKind::Reference &&
                sem_ir.referencePointee(parameters.front()) == owner_type &&
                (!reference.local_function.hasValue() ||
                 (sem_ir.function(reference.local_function).flags &
                  SemFunctionTemplate) == 0))
              return id;
          }
        }
        return owner_type.hasValue() ? FunctionRefId::invalid() : fallback;
      };
  const auto local_target = [&](FunctionId function, TypeId owner_type) {
    if (!function.hasValue())
      return FunctionRefId::invalid();
    const auto matches_owner = [&](FunctionId candidate) {
      const auto &value = sem_ir.function(candidate);
      const auto &value_contract = sem_ir.functionSemanticContract(candidate);
      const auto &template_contract = sem_ir.functionSemanticContract(function);
      if (value_contract.owner != template_contract.owner ||
          value_contract.role != template_contract.role ||
          value_contract.domain != template_contract.domain ||
          value_contract.capability != template_contract.capability ||
          value_contract.projector_field != template_contract.projector_field)
        return false;
      const auto &function_type = sem_ir.type(value.type);
      if (function_type.kind != SemTypeKind::Function)
        return false;
      const auto parameters = sem_ir.typeBlock(TypeBlockId(function_type.arg0));
      return !parameters.empty() &&
             sem_ir.type(parameters.front()).kind == SemTypeKind::Reference &&
             sem_ir.referencePointee(parameters.front()) == owner_type;
    };
    for (std::uint32_t ref_index = 0; ref_index < sem_ir.functionRefCount();
         ++ref_index) {
      const auto id = FunctionRefId(ref_index);
      const auto candidate = sem_ir.functionRef(id).local_function;
      if (candidate.hasValue() && matches_owner(candidate) &&
          (sem_ir.function(candidate).flags & SemFunctionTemplate) == 0)
        return id;
    }
    return FunctionRefId::invalid();
  };
  for (std::uint32_t index = 0; index < sem_ir.typeCount(); ++index) {
    const auto type = TypeId(index);
    auto object_type = type;
    for (std::size_t depth = 0; depth < sem_ir.typeCount(); ++depth) {
      const auto next = sem_ir.objectRepresentationType(object_type);
      if (next == object_type)
        break;
      object_type = next;
    }
    std::vector<LowObjectFieldProjection> projections;
    std::vector<TypeId> object_fields;
    if (sem_ir.type(object_type).kind == SemTypeKind::Nominal) {
      const auto physical_count =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(object_type).arg0))
              .fields.size();
      object_fields.reserve(physical_count);
      const auto &physical_nominal =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(object_type).arg0));
      for (std::uint32_t field = 0; field < physical_count; ++field) {
        const auto concrete = sem_ir.nominalFieldType(object_type, field);
        object_fields.push_back(concrete.hasValue()
                                    ? concrete
                                    : physical_nominal.fields[field].type);
      }
    }
    if (sem_ir.type(object_type).kind == SemTypeKind::CallbackAdapter) {
      const auto fields =
          sem_ir.typeBlock(TypeBlockId(sem_ir.type(object_type).arg0));
      object_fields.assign(fields.begin(), fields.end());
    }
    if (sem_ir.type(object_type).kind == SemTypeKind::CallbackRegistration) {
      const auto fields =
          sem_ir.typeBlock(TypeBlockId(sem_ir.type(object_type).arg0));
      object_fields.assign(fields.begin(), fields.end());
    }
    if (sem_ir.type(object_type).kind == SemTypeKind::CallbackCompletion) {
      const auto fields =
          sem_ir.typeBlock(TypeBlockId(sem_ir.type(object_type).arg0));
      object_fields.assign(fields.begin(), fields.end());
    }
    if (sem_ir.type(object_type).kind == SemTypeKind::CallbackWake) {
      const auto fields =
          sem_ir.typeBlock(TypeBlockId(sem_ir.type(object_type).arg0));
      if (fields.size() == 1)
        object_fields = {fields[0], sem_ir.boolType()};
    }
    if (sem_ir.type(object_type).kind == SemTypeKind::Tuple) {
      const auto fields =
          sem_ir.typeBlock(TypeBlockId(sem_ir.type(object_type).arg0));
      object_fields.assign(fields.begin(), fields.end());
    }
    if (sem_ir.type(type).kind == SemTypeKind::Nominal) {
      const auto field_count =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(type).arg0))
              .fields.size();
      projections.reserve(field_count);
      const auto *witness = sem_ir.nominalSemanticWitness(type);
      const auto &semantic_nominal =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(type).arg0));
      for (std::uint32_t field = 0; field < field_count; ++field) {
        const auto custom = sem_ir.objectFieldProjection(type, field);
        LowObjectFieldProjection projection;
        if (witness && field < witness->object_field_projections.size()) {
          const auto &artifact = witness->object_field_projections[field];
          projection.kind = artifact.kind;
          projection.region_indices = artifact.region_indices;
          projection.bit_begin = artifact.bit_begin;
          projection.bit_end = artifact.bit_end;
          projection.capabilities = artifact.capabilities;
          projection.load_target = canonical_target(artifact.load_target, type);
          projection.store_target =
              canonical_target(artifact.store_target, type);
          projection.take_target = canonical_target(artifact.take_target, type);
          projection.init_target = canonical_target(artifact.init_target, type);
          projection.borrow_target =
              canonical_target(artifact.borrow_target, type);
          projection.borrow_mut_target =
              canonical_target(artifact.borrow_mut_target, type);
          if (artifact.kind == ObjectFieldProjectionKind::Computed &&
              field < semantic_nominal.fields.size()) {
            const auto projector =
                std::ranges::find(semantic_nominal.object_projectors,
                                  semantic_nominal.fields[field].projector_name,
                                  &SemObjectProjector::name);
            if (projector != semantic_nominal.object_projectors.end()) {
              projection.load_target =
                  local_target(projector->load_function, type);
              projection.store_target =
                  local_target(projector->store_function, type);
              projection.take_target =
                  local_target(projector->take_function, type);
              projection.init_target =
                  local_target(projector->init_function, type);
              projection.borrow_target =
                  local_target(projector->borrow_function, type);
              projection.borrow_mut_target =
                  local_target(projector->borrow_mut_function, type);
            }
          }
        } else {
          projection.kind = ObjectFieldProjectionKind::StableAddress;
          projection.capabilities = ProjectionLoad | ProjectionStore |
                                    ProjectionTake | ProjectionInit |
                                    ProjectionBorrow | ProjectionBorrowMut;
        }
        if (custom.empty() &&
            projection.kind == ObjectFieldProjectionKind::StableAddress &&
            semantic_nominal.kind == NominalKind::Struct)
          projection.physical_steps.push_back({type, field});
        else
          projection.physical_steps.assign(custom.begin(), custom.end());
        projections.push_back(std::move(projection));
      }
    }
    if (sem_ir.type(type).kind == SemTypeKind::Tuple) {
      const auto fields = sem_ir.typeBlock(TypeBlockId(sem_ir.type(type).arg0));
      projections.reserve(fields.size());
      for (const auto field : fields) {
        (void)field;
        LowObjectFieldProjection projection;
        projection.kind = ObjectFieldProjectionKind::StableAddress;
        projection.capabilities = ProjectionLoad | ProjectionStore |
                                  ProjectionTake | ProjectionInit |
                                  ProjectionBorrow | ProjectionBorrowMut;
        projections.push_back(std::move(projection));
      }
    }
    const auto *type_witness = sem_ir.nominalSemanticWitness(type);
    FunctionRefId copy_target = canonical_target(
        type_witness ? type_witness->copy_target : std::nullopt, type);
    FunctionRefId destroy_target = canonical_target(
        type_witness ? type_witness->destroy_target : std::nullopt, type);
    FunctionRefId object_init_target = canonical_target(
        type_witness ? type_witness->object_init_target : std::nullopt, type);
    FunctionRefId object_copy_init_target = canonical_target(
        type_witness ? type_witness->object_copy_init_target : std::nullopt,
        type);
    FunctionRefId object_move_init_target = canonical_target(
        type_witness ? type_witness->object_move_init_target : std::nullopt,
        type);
    FunctionRefId object_drop_target = canonical_target(
        type_witness ? type_witness->object_drop_target : std::nullopt, type);
    if (sem_ir.type(type).kind == SemTypeKind::Nominal) {
      const auto &owner =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(type).arg0));
      if (owner.object_init_function.hasValue()) {
        object_init_target = local_target(owner.object_init_function, type);
        object_copy_init_target =
            local_target(owner.object_copy_init_function, type);
        object_move_init_target =
            local_target(owner.object_move_init_function, type);
        object_drop_target = local_target(owner.object_drop_function, type);
      }
      if (owner.lifecycle_copy_function.hasValue())
        copy_target = local_target(owner.lifecycle_copy_function, type);
      if (owner.lifecycle_drop_function.hasValue())
        destroy_target = local_target(owner.lifecycle_drop_function, type);
    }
    type_representations_.push_back(
        {sem_ir.typeRepresentation(type), object_type,
         sem_ir.valueRepresentationType(type),
         conversion_target(type, SemCanonicalFunctionRole::Pack),
         conversion_target(type, SemCanonicalFunctionRole::Init), copy_target,
         destroy_target, object_init_target, object_copy_init_target,
         object_move_init_target, object_drop_target, std::move(object_fields),
         std::move(projections)});
  }
  enum_layouts_.resize(sem_ir.typeCount());
  nominal_layouts_.resize(sem_ir.typeCount());
  std::unordered_set<std::uint32_t> bound_nominals;
  const auto pointer_width =
      normalized_target_triple_.starts_with("x86_64") ||
              normalized_target_triple_.starts_with("amd64") ||
              normalized_target_triple_.starts_with("aarch64")
          ? 64U
      : normalized_target_triple_.starts_with("i386") ||
              normalized_target_triple_.starts_with("i486") ||
              normalized_target_triple_.starts_with("i586") ||
              normalized_target_triple_.starts_with("i686") ||
              normalized_target_triple_.starts_with("arm")
          ? 32U
          : 0U;
  const TargetLayoutConfig target_config{normalized_target_triple_,
                                         pointer_width, NominalLayoutAbiEpoch};
  for (const auto binding : nominal_layout_bindings) {
    if (!binding.type.hasValue() || binding.type.index >= sem_ir.typeCount() ||
        binding.layout_index >= nominal_layouts.size()) {
      nominal_layout_error_ = "nominal layout binding is out of range";
      break;
    }
    const auto &semantic_type = sem_ir.type(binding.type);
    const auto &layout = nominal_layouts[binding.layout_index];
    std::string layout_error;
    if (semantic_type.kind != SemTypeKind::Nominal ||
        !layout.verify(layout_error)) {
      nominal_layout_error_ = layout_error.empty()
                                  ? "nominal layout binding has invalid facts"
                                  : std::move(layout_error);
      break;
    }
    if (!target_config.verify(layout_error) ||
        layout.target_fingerprint != target_config.fingerprint()) {
      nominal_layout_error_ = "nominal layout binding has a stale target ABI";
      break;
    }
    if (binding.expected_type_specific_fingerprint.hasValue() &&
        layout.type_specific_fingerprint !=
            binding.expected_type_specific_fingerprint) {
      nominal_layout_error_ =
          "nominal layout binding has a stale specific fingerprint";
      break;
    }
    if (nominal_layouts_required_ &&
        !binding.expected_type_specific_fingerprint.hasValue()) {
      nominal_layout_error_ =
          "nominal layout binding has no specific fingerprint";
      break;
    }
    const auto &nominal = sem_ir.nominalType(NominalTypeId(semantic_type.arg0));
    if (layout.kind != nominal.kind) {
      nominal_layout_error_ =
          "nominal layout binding disagrees with semantic kind";
      break;
    }
    if (!bound_nominals.insert(binding.type.index).second) {
      nominal_layout_error_ = "nominal type has duplicate layout bindings";
      break;
    }
    LowNominalLayout frozen_nominal{
        layout.kind, layout.size,     layout.alignment,
        {},          layout.tag_size, layout.payload_offset,
        {}};
    frozen_nominal.fields.reserve(layout.fields.size());
    for (const auto &field : layout.fields)
      frozen_nominal.fields.push_back(
          {field.offset, field.size, field.alignment});
    frozen_nominal.variants.reserve(layout.variants.size());
    for (const auto &variant : layout.variants) {
      LowNominalVariantLayout frozen_variant{
          variant.size, variant.alignment, {}};
      frozen_variant.fields.reserve(variant.fields.size());
      for (const auto &field : variant.fields)
        frozen_variant.fields.push_back(
            {field.offset, field.size, field.alignment});
      frozen_nominal.variants.push_back(std::move(frozen_variant));
    }
    if (nominal.kind == NominalKind::Enum &&
        frozen_nominal.variants.size() != nominal.variants.size()) {
      nominal_layout_error_ =
          "enum layout binding disagrees with semantic variants";
      break;
    }
    if (nominal.kind != NominalKind::Enum &&
        frozen_nominal.fields.size() != nominal.fields.size()) {
      nominal_layout_error_ =
          "nominal layout binding disagrees with semantic fields";
      break;
    }
    nominal_layouts_[binding.type.index] = frozen_nominal;
    if (nominal.kind != NominalKind::Enum)
      continue;
    LowEnumLayout frozen{layout.tag_size,
                         layout.payload_offset,
                         layout.size,
                         layout.alignment,
                         {}};
    frozen.variants.reserve(layout.variants.size());
    for (std::size_t variant = 0; variant < layout.variants.size(); ++variant) {
      if (layout.variants[variant].fields.size() !=
          nominal.variants[variant].fields.size()) {
        nominal_layout_error_ =
            "enum layout binding has an invalid payload arity";
        break;
      }
      LowEnumVariantLayout frozen_variant{layout.variants[variant].size,
                                          layout.variants[variant].alignment,
                                          {}};
      for (const auto &field : layout.variants[variant].fields)
        frozen_variant.field_offsets.push_back(field.offset);
      frozen.variants.push_back(std::move(frozen_variant));
    }
    if (!nominal_layout_error_.empty())
      break;
    enum_layouts_[binding.type.index] = std::move(frozen);
  }
  if (nominal_layout_error_.empty() && nominal_layouts_required_) {
    for (std::uint32_t index = 0; index < sem_ir.typeCount(); ++index) {
      const auto type = TypeId(index);
      if (sem_ir.type(type).kind == SemTypeKind::Nominal &&
          sem_ir.nominalSemanticWitness(type) &&
          !bound_nominals.contains(index)) {
        nominal_layout_error_ =
            "concrete nominal type has no frozen layout binding";
        break;
      }
    }
  }
  buildForeignAbiLayouts();
}

LowInstId LowIR::addRawInst(LowInst inst, InstId origin) {
  const auto id = insts_.add(inst);
  const auto origin_id = origins_.add(origin);
  assert(id == origin_id);
  return id;
}

CompletionProviderPlan
LowIR::completionProviderFor(TypeId aggregate_type) const {
  const auto completion = sem_ir_->completionSetElementType(aggregate_type);
  CompletionProviderPlan provider{
      .kind = completion == sem_ir_->coroutineTaskCompletionType()
                  ? CompletionProviderKind::Task
                  : CompletionProviderKind::Operation,
      .completion_type = completion};
  if (provider.kind == CompletionProviderKind::Operation) {
    // Operation-backed completion is reconstructed from the carrier and its
    // callback/wake facts.  Missing or ambiguous facts are rejected here so
    // LLVM never falls back to a surface-level resource spelling.
    if (!completion.hasValue() ||
        sem_ir_->type(completion).arg0 == core::AnyId::InvalidIndex) {
      provider.kind = CompletionProviderKind::Count;
      return provider;
    }
    provider.resource_owner = NominalTypeId(sem_ir_->type(completion).arg0);
    const auto &resource = sem_ir_->nominalType(provider.resource_owner);
    if (!resource.foreign_completion_storage_type.hasValue()) {
      provider.kind = CompletionProviderKind::Count;
      return provider;
    }
    provider.protocol = sem_ir_->foreignResourceProtocolId(
        resource.foreign_completion_storage_type);
    provider.wake_plan =
        callbackWakePlanFor(resource.foreign_completion_storage_type);
    // Prefer the verifier-owned operation completion family when one was
    // materialized for this carrier. The protocol fields above remain only
    // as a compatibility projection for the current LLVM consumer.
    for (std::uint32_t index = 0;
         index < foreign_operation_completion_plans_.size(); ++index) {
      const auto id = ForeignOperationCompletionPlanId(index);
      const auto &operation_completion =
          foreign_operation_completion_plans_.get(id);
      if (operation_completion.completion_carrier == completion &&
          operation_completion.wake_plan.hasValue()) {
        provider.operation_completion = id;
        break;
      }
    }
    if (!provider.protocol.hasValue() || !provider.wake_plan.hasValue())
      provider.kind = CompletionProviderKind::Count;
  }
  return provider;
}
SlotId LowIR::addSlot(LowSlot slot) {
  return slots_.add(slot);
}
LowPlaceId LowIR::addPlace(LowPlace place) {
  return places_.add(place);
}
LowPlaceProjectionBlockId LowIR::addPlaceProjectionBlock(
    std::span<const LowPlaceProjection> projections) {
  return place_projection_blocks_.add(projections);
}
TargetPairId LowIR::addTargets(TargetPair targets) {
  return targets_.add(targets);
}
LowValueBlockId LowIR::addValueBlock(std::span<const LowInstId> values) {
  return value_blocks_.add(values);
}
LowBlockId LowIR::addBlock(std::span<const LowInstId> instructions) {
  const auto storage = inst_blocks_.add(instructions);
  const auto id = blocks_.add(storage);
  assert(id.index == storage.index);
  return id;
}
LowBlockListId LowIR::addBlockList(std::span<const LowBlockId> blocks) {
  return block_lists_.add(blocks);
}
SlotBlockId LowIR::addSlotBlock(std::span<const SlotId> slots) {
  return slot_blocks_.add(slots);
}
LowFunctionId LowIR::addFunction(LowFunction function) {
  return functions_.add(function);
}

bool LowIR::containsArg(LowArgKind kind, std::uint32_t raw) const {
  if (kind == LowArgKind::None)
    return raw == core::AnyId::InvalidIndex;
  if (raw == core::AnyId::InvalidIndex)
    return false;
  switch (kind) {
  case LowArgKind::None:
    return false;
  case LowArgKind::Value:
    return raw < insts_.size();
  case LowArgKind::Slot:
    return raw < slots_.size();
  case LowArgKind::Place:
    return raw < places_.size();
  case LowArgKind::Block:
    return raw < blocks_.size();
  case LowArgKind::Targets:
    return raw < targets_.size();
  case LowArgKind::Integer:
    return raw < sem_ir_->integerCount();
  case LowArgKind::String:
    return raw < sem_ir_->stringCount();
  case LowArgKind::Constant:
    return raw < sem_ir_->constantEntityCount();
  case LowArgKind::ValueBlock:
    return raw < value_blocks_.size();
  case LowArgKind::FunctionRef:
    return raw < sem_ir_->functionRefCount();
  case LowArgKind::ForeignAbiLayout:
    return raw < foreign_abi_layouts_.size();
  case LowArgKind::ForeignAbiCallLayout:
    return raw < foreign_abi_call_layouts_.size();
  case LowArgKind::ForeignCallOutcomePlan:
    return raw < foreign_call_outcome_plans_.size();
  case LowArgKind::ForeignAbiThunkPlan:
    return raw < foreign_abi_thunk_plans_.size();
  case LowArgKind::CallbackAdapterPlan:
    return raw < callback_adapter_plans_.size();
  case LowArgKind::CallbackRegistrationPlan:
    return raw < callback_registration_plans_.size();
  case LowArgKind::CallbackCompletionPlan:
    return raw < callback_completion_plans_.size();
  case LowArgKind::CallbackReadinessPlan:
    return raw < callback_readiness_plans_.size();
  case LowArgKind::CallbackWakePlan:
    return raw < callback_wake_plans_.size();
  case LowArgKind::ForeignOperationCompletionPlan:
    return raw < foreign_operation_completion_plans_.size();
  case LowArgKind::CoroutineTaskCreatePlan:
    return raw < coroutine_task_create_plans_.size();
  case LowArgKind::CoroutineTaskCompletionArmPlan:
    return raw < coroutine_task_completion_arm_plans_.size();
  case LowArgKind::CoroutineTaskCompletionSetPlan:
    return raw < coroutine_task_completion_set_plans_.size();
  case LowArgKind::CoroutineTaskCompletionCombinePlan:
    return raw < coroutine_task_completion_combine_plans_.size();
  case LowArgKind::ConstructPlan:
    return raw < construct_plans_.size();
  case LowArgKind::Parameter:
  case LowArgKind::Field:
    return true;
  }
  return false;
}

} // namespace chtholly::compiler
