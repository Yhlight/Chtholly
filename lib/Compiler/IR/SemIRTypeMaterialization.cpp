#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/PublicInterface.h"

#include <array>
#include <cassert>
#include <limits>
#include <ranges>
#include <vector>

namespace chtholly::compiler {
namespace {
constexpr std::uint32_t ProjectionIndexMask = 0x7fffffffU;
constexpr std::uint32_t ProjectionKindShift = 31U;
} // namespace

TypeId SemIR::materializeType(CanonicalTypeId canonical_id) {
  const auto &canonical = values_->generics().type(canonical_id);
  switch (canonical.kind) {
  case CanonicalTypeKind::Void:
    return void_type_;
  case CanonicalTypeKind::Never:
    return never_type_;
  case CanonicalTypeKind::Bool:
    return bool_type_;
  case CanonicalTypeKind::Char:
    return char_type_;
  case CanonicalTypeKind::Integer:
    return addType({SemTypeKind::Integer, canonical.arg0, canonical.arg1,
                    canonical_id.index});
  case CanonicalTypeKind::Float:
    return addType(
        {SemTypeKind::Float, canonical.arg0, 0U, canonical_id.index});
  case CanonicalTypeKind::String:
    return string_type_;
  case CanonicalTypeKind::Array: {
    const auto element = materializeType(CanonicalTypeId(canonical.arg0));
    return addType({SemTypeKind::Array, element.index, canonical.arg1,
                    canonical_id.index});
  }
  case CanonicalTypeKind::Tuple: {
    std::vector<TypeId> elements;
    elements.reserve(canonical.elements.size());
    for (const auto element : canonical.elements)
      elements.push_back(materializeType(element));
    return canonical.abi_union ? addCUnionType(elements)
                               : addTupleType(elements);
  }
  case CanonicalTypeKind::Slice:
    return canonical.elements.size() == 1
               ? addSliceType(materializeType(canonical.elements.front()),
                              canonical.arg0 != 0)
               : TypeId::invalid();
  case CanonicalTypeKind::Function: {
    if (canonical.elements.empty())
      return TypeId::invalid();
    std::vector<TypeId> parameters;
    parameters.reserve(canonical.elements.size() - 1);
    for (std::size_t index = 0; index + 1 < canonical.elements.size(); ++index)
      parameters.push_back(materializeType(canonical.elements[index]));
    const auto block = addTypeBlock(parameters);
    const auto result = materializeType(canonical.elements.back());
    return addType(
        {SemTypeKind::Function, block.index, result.index, canonical_id.index});
  }
  case CanonicalTypeKind::AsyncFunction: {
    if (canonical.arg0 >= canonical.elements.size() ||
        canonical.elements.size() - canonical.arg0 < 1 ||
        canonical.elements.size() - canonical.arg0 > 2)
      return TypeId::invalid();
    std::vector<TypeId> parameters;
    parameters.reserve(canonical.arg0);
    for (std::size_t index = 0; index < canonical.arg0; ++index)
      parameters.push_back(materializeType(canonical.elements[index]));
    std::vector<TypeId> outcomes;
    for (std::size_t index = canonical.arg0; index < canonical.elements.size();
         ++index)
      outcomes.push_back(materializeType(canonical.elements[index]));
    return addType({SemTypeKind::AsyncFunction, addTypeBlock(parameters).index,
                    addTypeBlock(outcomes).index, canonical_id.index});
  }
  case CanonicalTypeKind::CFunctionPointer:
  case CanonicalTypeKind::CVariadicFunctionPointer: {
    if (canonical.elements.empty())
      return TypeId::invalid();
    std::vector<TypeId> parameters;
    for (std::size_t index = 0; index + 1 < canonical.elements.size(); ++index)
      parameters.push_back(materializeType(canonical.elements[index]));
    return addCFunctionPointerType(
        parameters, materializeType(canonical.elements.back()),
        canonical.kind == CanonicalTypeKind::CVariadicFunctionPointer,
        canonical.callable_contract, canonical.callable_context_parameter,
        canonical.foreign_calling_convention);
  }
  case CanonicalTypeKind::CallbackAdapter: {
    if (canonical.elements.size() != 3)
      return TypeId::invalid();
    return addCallbackAdapterType(materializeType(canonical.elements[0]),
                                  materializeType(canonical.elements[1]),
                                  materializeType(canonical.elements[2]));
  }
  case CanonicalTypeKind::CallbackCompletion: {
    if (canonical.elements.size() != 4 && canonical.elements.size() != 5 &&
        canonical.elements.size() != 7)
      return TypeId::invalid();
    return addCallbackCompletionType(
        materializeType(canonical.elements[0]),
        materializeType(canonical.elements[1]),
        materializeType(canonical.elements[2]),
        materializeType(canonical.elements[3]),
        canonical.elements.size() == 5 ? materializeType(canonical.elements[4])
        : canonical.elements.size() == 7
            ? materializeType(canonical.elements[4])
            : TypeId::invalid(),
        static_cast<CallbackReleaseAuthority>(canonical.registration_authority),
        canonical.elements.size() == 7 ? materializeType(canonical.elements[5])
                                       : TypeId::invalid(),
        canonical.elements.size() == 7 ? materializeType(canonical.elements[6])
                                       : TypeId::invalid(),
        canonical.registration_arm_parameters,
        canonical.registration_detach_parameters);
  }
  case CanonicalTypeKind::CallbackWake: {
    if (canonical.elements.size() != 1)
      return TypeId::invalid();
    return addCallbackWakeType(materializeType(canonical.elements.front()));
  }
  case CanonicalTypeKind::CallbackRegistration: {
    if (canonical.elements.size() != 5 && canonical.elements.size() != 7 &&
        canonical.elements.size() != 8 && canonical.elements.size() != 10)
      return TypeId::invalid();
    return addCallbackRegistrationType(
        materializeType(canonical.elements[0]),
        materializeType(canonical.elements[1]),
        materializeType(canonical.elements[2]),
        materializeType(canonical.elements[3]),
        materializeType(canonical.elements[4]),
        static_cast<CallbackReleaseAuthority>(canonical.registration_authority),
        canonical.registration_entry_parameter,
        canonical.registration_userdata_parameter,
        canonical.registration_release_parameter,
        canonical.registration_bindings,
        canonical.elements.size() >= 7 ? materializeType(canonical.elements[5])
                                       : TypeId::invalid(),
        canonical.elements.size() >= 7 ? materializeType(canonical.elements[6])
                                       : TypeId::invalid(),
        canonical.elements.size() >= 8 ? materializeType(canonical.elements[7])
                                       : TypeId::invalid(),
        canonical.elements.size() == 10 ? materializeType(canonical.elements[8])
                                        : TypeId::invalid(),
        canonical.elements.size() == 10 ? materializeType(canonical.elements[9])
                                        : TypeId::invalid(),
        canonical.registration_arm_parameters,
        canonical.registration_detach_parameters);
  }
  case CanonicalTypeKind::ForeignCompletion: {
    for (std::uint32_t index = 0; index < nominal_types_.size(); ++index) {
      const auto candidate = addNominalType(NominalTypeId(index));
      const auto &candidate_canonical =
          values_->generics().type(canonicalType(candidate));
      if (candidate_canonical.nominal_key == canonical.nominal_key)
        return addForeignCompletionType(NominalTypeId(index));
    }
    return TypeId::invalid();
  }
  case CanonicalTypeKind::ForeignWake: {
    for (std::uint32_t index = 0; index < nominal_types_.size(); ++index) {
      const auto candidate = addNominalType(NominalTypeId(index));
      const auto &candidate_canonical =
          values_->generics().type(canonicalType(candidate));
      if (candidate_canonical.nominal_key == canonical.nominal_key)
        return addForeignWakeType(NominalTypeId(index));
    }
    return TypeId::invalid();
  }
  case CanonicalTypeKind::TypeParameter:
    return addType({SemTypeKind::TypeParameter, canonical.arg0, canonical.arg1,
                    canonical_id.index});
  case CanonicalTypeKind::Nominal: {
    NominalTypeId nominal;
    for (std::uint32_t index = 0; index < nominal_types_.size(); ++index) {
      const auto &candidate = nominalType(NominalTypeId(index));
      std::string key;
      if (const auto *entity =
              imports_.tryGetEntity(candidate.canonical_entity))
        key = std::string(identifier(entity->package_name)) + "/" +
              std::string(identifier(entity->module_name)) +
              "::" + std::string(identifier(entity->name));
      else
        key = std::string(identifier(module_name_)) +
              "::" + std::string(identifier(name(candidate.name).text));
      if (key == canonical.nominal_key) {
        nominal = NominalTypeId(index);
        break;
      }
    }
    if (!nominal.hasValue())
      return TypeId::invalid();
    std::vector<TypeId> arguments;
    arguments.reserve(canonical.elements.size());
    for (const auto argument : canonical.elements)
      arguments.push_back(materializeType(argument));
    return addType({SemTypeKind::Nominal, nominal.index,
                    addTypeBlock(arguments).index, canonical_id.index});
  }
  case CanonicalTypeKind::Reference:
    if (canonical.elements.size() != 1)
      return TypeId::invalid();
    return addType({SemTypeKind::Reference,
                    materializeType(canonical.elements.front()).index,
                    canonical.arg0, canonical_id.index});
  case CanonicalTypeKind::RawPointer:
    if (canonical.elements.size() != 1)
      return TypeId::invalid();
    return addType({SemTypeKind::RawPointer,
                    materializeType(canonical.elements.front()).index,
                    canonical.arg1, canonical_id.index});
  case CanonicalTypeKind::TypeProjection:
    if (canonical.arg1 > ProjectionIndexMask ||
        canonical.projection_kind >= CanonicalTypeProjectionKind::Count)
      return TypeId::invalid();
    return addType({SemTypeKind::TypeProjection,
                    materializeType(CanonicalTypeId(canonical.arg0)).index,
                    (static_cast<std::uint32_t>(canonical.projection_kind)
                     << ProjectionKindShift) |
                        canonical.arg1,
                    canonical_id.index});
  case CanonicalTypeKind::CoroutineExecutor:
    return coroutine_executor_type_;
  case CanonicalTypeKind::CoroutineScope:
    return coroutine_scope_type_;
  case CanonicalTypeKind::CoroutineTaskCompletion:
    return coroutine_task_completion_type_;
  case CanonicalTypeKind::CoroutineTaskCompletionSet:
    return canonical.elements.size() == 1
               ? addCompletionSetType(materializeType(canonical.elements[0]),
                                      canonical.arg0)
               : TypeId::invalid();
  case CanonicalTypeKind::CoroutineTaskSelection:
    return canonical.elements.size() == 1
               ? addCompletionSelectionType(
                     materializeType(canonical.elements[0]), canonical.arg0)
               : TypeId::invalid();
  case CanonicalTypeKind::CoroutineTask:
  case CanonicalTypeKind::CoroutineTaskOutcome: {
    if (canonical.elements.empty() || canonical.elements.size() > 2)
      return TypeId::invalid();
    const auto success = materializeType(canonical.elements[0]);
    const auto error =
        canonical.elements.size() == 2
            ? std::optional<TypeId>(materializeType(canonical.elements[1]))
            : std::nullopt;
    return canonical.kind == CanonicalTypeKind::CoroutineTask
               ? addCoroutineTaskType(success, error)
               : addCoroutineTaskOutcomeType(success, error);
  }
  case CanonicalTypeKind::CoroutineChecked:
    return canonical.elements.size() == 1
               ? addCoroutineCheckedType(
                     materializeType(canonical.elements.front()))
               : TypeId::invalid();
  case CanonicalTypeKind::Count:
    return TypeId::invalid();
  }
  return TypeId::invalid();
}

TypeId SemIR::materializePublicType(const PublicType &type_value,
                                    GenericId generic, NodeId location,
                                    std::string &error) {
  switch (type_value.kind) {
  case PublicTypeKind::Void:
    return voidType();
  case PublicTypeKind::Never:
    return neverType();
  case PublicTypeKind::Bool:
    return boolType();
  case PublicTypeKind::Integer:
    return addIntegerType(type_value.scalar_width, type_value.integer_signed);
  case PublicTypeKind::Float:
    return addFloatType(type_value.scalar_width);
  case PublicTypeKind::String:
    return stringType();
  case PublicTypeKind::Array: {
    if (type_value.arguments.size() != 1 || type_value.array_bound == 0) {
      error = "array artifact type has no canonical element or bound";
      return TypeId::invalid();
    }
    const auto element = materializePublicType(type_value.arguments.front(),
                                               generic, location, error);
    if (!element.hasValue())
      return TypeId::invalid();
    return addType({SemTypeKind::Array, element.index, type_value.array_bound,
                    core::AnyId::InvalidIndex});
  }
  case PublicTypeKind::Tuple: {
    std::vector<TypeId> elements;
    elements.reserve(type_value.arguments.size());
    for (const auto &argument : type_value.arguments) {
      const auto element =
          materializePublicType(argument, generic, location, error);
      if (!element.hasValue())
        return TypeId::invalid();
      elements.push_back(element);
    }
    return type_value.abi_union ? addCUnionType(elements)
                                : addTupleType(elements);
  }
  case PublicTypeKind::Slice: {
    if (type_value.arguments.size() != 1) {
      error = "slice artifact type has invalid element count";
      return TypeId::invalid();
    }
    const auto element = materializePublicType(type_value.arguments.front(),
                                               generic, location, error);
    return element.hasValue() ? addSliceType(element, type_value.slice_mutable)
                              : TypeId::invalid();
  }
  case PublicTypeKind::RawPointer: {
    if (type_value.arguments.size() != 1) {
      error = "raw pointer artifact type has no canonical pointee";
      return TypeId::invalid();
    }
    const auto pointee = materializePublicType(type_value.arguments.front(),
                                               generic, location, error);
    return pointee.hasValue()
               ? addRawPointerType(pointee, type_value.pointer_const)
               : TypeId::invalid();
  }
  case PublicTypeKind::CFunctionPointer: {
    if (type_value.arguments.empty()) {
      error = "C function-pointer artifact type has no result type";
      return TypeId::invalid();
    }
    std::vector<TypeId> parameters;
    for (std::size_t index = 0; index + 1 < type_value.arguments.size();
         ++index) {
      const auto parameter = materializePublicType(type_value.arguments[index],
                                                   generic, location, error);
      if (!parameter.hasValue())
        return TypeId::invalid();
      parameters.push_back(parameter);
    }
    const auto result = materializePublicType(type_value.arguments.back(),
                                              generic, location, error);
    return result.hasValue()
               ? addCFunctionPointerType(parameters, result,
                                         type_value.callable_variadic,
                                         type_value.callable_contract,
                                         type_value.callable_context_parameter,
                                         type_value.foreign_calling_convention)
               : TypeId::invalid();
  }
  case PublicTypeKind::Function: {
    if (type_value.arguments.empty()) {
      error = "function artifact type has no result type";
      return TypeId::invalid();
    }
    std::vector<TypeId> parameters;
    for (std::size_t index = 0; index + 1 < type_value.arguments.size();
         ++index) {
      const auto parameter = materializePublicType(type_value.arguments[index],
                                                   generic, location, error);
      if (!parameter.hasValue())
        return TypeId::invalid();
      parameters.push_back(parameter);
    }
    const auto result = materializePublicType(type_value.arguments.back(),
                                              generic, location, error);
    return result.hasValue() ? addFunctionType(parameters, result)
                             : TypeId::invalid();
  }
  case PublicTypeKind::CallbackAdapter: {
    if (type_value.arguments.size() != 3) {
      error = "callback adapter artifact type has invalid fields";
      return TypeId::invalid();
    }
    const auto entry = materializePublicType(type_value.arguments[0], generic,
                                             location, error);
    const auto context = materializePublicType(type_value.arguments[1], generic,
                                               location, error);
    const auto release = materializePublicType(type_value.arguments[2], generic,
                                               location, error);
    return entry.hasValue() && context.hasValue() && release.hasValue()
               ? addCallbackAdapterType(entry, context, release)
               : TypeId::invalid();
  }
  case PublicTypeKind::CallbackCompletion: {
    if (type_value.arguments.size() != 4 && type_value.arguments.size() != 5 &&
        type_value.arguments.size() != 7) {
      error = "callback completion artifact type has invalid fields";
      return TypeId::invalid();
    }
    std::vector<TypeId> fields(type_value.arguments.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fields[index] = materializePublicType(type_value.arguments[index],
                                            generic, location, error);
      if (!fields[index].hasValue())
        return TypeId::invalid();
    }
    return addCallbackCompletionType(
        fields[0], fields[1], fields[2], fields[3],
        fields.size() >= 5 ? fields[4] : TypeId::invalid(),
        static_cast<CallbackReleaseAuthority>(
            type_value.registration_authority),
        fields.size() == 7 ? fields[5] : TypeId::invalid(),
        fields.size() == 7 ? fields[6] : TypeId::invalid(),
        type_value.registration_arm_parameters,
        type_value.registration_detach_parameters);
  }
  case PublicTypeKind::CallbackWake: {
    if (type_value.arguments.size() != 1) {
      error = "callback wake artifact type has invalid fields";
      return TypeId::invalid();
    }
    const auto completion = materializePublicType(type_value.arguments.front(),
                                                  generic, location, error);
    return completion.hasValue() ? addCallbackWakeType(completion)
                                 : TypeId::invalid();
  }
  case PublicTypeKind::ForeignOperationState: {
    const auto owner_id = imports_.registry().findEntity(
        type_value.nominal_entity.canonical_package,
        type_value.nominal_entity.canonical_module,
        type_value.nominal_entity.canonical_name,
        PublicEntityKind::ForeignOperation,
        type_value.nominal_entity.expected_fingerprint);
    const auto *owner = imports_.registry().tryGetEntity(owner_id);
    if (!owner || owner->kind != PublicEntityKind::ForeignOperation ||
        owner->fingerprint != type_value.nominal_entity.expected_fingerprint ||
        !owner->interop_artifact) {
      error = "operation-owned state has a stale operation owner";
      return TypeId::invalid();
    }
    const auto find_operation =
        [&](const PublicEntityReferenceArtifact &ref) -> const PublicEntity * {
      const auto id = imports_.registry().findEntity(
          ref.canonical_package, ref.canonical_module, ref.canonical_name,
          PublicEntityKind::ForeignOperation, ref.expected_fingerprint);
      const auto *entity = imports_.registry().tryGetEntity(id);
      return entity && entity->fingerprint == ref.expected_fingerprint
                 ? entity
                 : nullptr;
    };
    const auto physical_type = [&](PublicType value) {
      if (value.kind != PublicTypeKind::Nominal)
        return value;
      const auto nominal_id = imports_.registry().findEntity(
          value.nominal_entity.canonical_package,
          value.nominal_entity.canonical_module,
          value.nominal_entity.canonical_name, PublicEntityKind::NominalType,
          value.nominal_entity.expected_fingerprint);
      const auto *nominal = imports_.registry().tryGetEntity(nominal_id);
      return nominal && nominal->nominal_kind == NominalKind::ForeignHandle &&
                     nominal->nominal_foreign_representation
                 ? *nominal->nominal_foreign_representation
                 : value;
    };
    const auto resolve_operation = [&](const PublicEntity *entity)
        -> const interop::ForeignOperationArtifact * {
      return entity && entity->interop_artifact
                 ? imports_.interopRegistry().resolve(*entity->interop_artifact)
                 : nullptr;
    };
    const auto owner_operation = resolve_operation(owner);
    if (!owner_operation) {
      error = "operation-owned state has no resolved interop artifact";
      return TypeId::invalid();
    }
    const auto callable_type = [&](const PublicEntity *entity) {
      if (!entity)
        return PublicType{};
      std::vector<PublicType> parameters;
      parameters.reserve(entity->parameters.size());
      for (const auto &parameter : entity->parameters)
        parameters.push_back(physical_type(parameter));
      CallableOwnershipSummary contract;
      const auto has_capability = [&](std::string_view path) {
        const auto *operation = resolve_operation(entity);
        return operation &&
               std::ranges::any_of(operation->capabilities,
                                   [&](const auto &capability) {
                                     return capability.path == path;
                                   });
      };
      const auto consume = [&](std::uint32_t parameter) {
        const OwnershipRegion root{.parameter_index = parameter};
        contract.effects.push_back({CallableEffectKind::Take, root});
        contract.postconditions.push_back({root, CallableOutcomeInvalidate});
      };
      if (has_capability("async.action.wait"))
        consume(0);
      else if (has_capability("async.action.detach")) {
        consume(0);
        if (owner_operation->completion_descriptor &&
            owner_operation->completion_descriptor->authority == 0 &&
            parameters.size() >= 2)
          consume(1);
      }
      contract.canonicalize();
      return PublicType::cFunctionPointer(std::move(parameters),
                                          physical_type(entity->return_type),
                                          false, std::move(contract));
    };
    PublicType storage;
    const auto state = type_value.foreign_operation_state;
    if (state == ForeignOperationStateKind::Subscription) {
      if (!owner_operation->callback_adapter_layout) {
        error = "operation-owned subscription has no callback adapter layout";
        return TypeId::invalid();
      }
      auto fields = owner_operation->callback_adapter_layout->arguments;
      if (fields.size() != 3) {
        error = "operation-owned subscription has an invalid callback adapter";
        return TypeId::invalid();
      }
      fields.push_back(physical_type(owner->return_type));
      storage = PublicType::tuple(std::move(fields));
    } else {
      if (!owner_operation->completion_descriptor) {
        error = "operation-owned completion has no closed family";
        return TypeId::invalid();
      }
      const auto &family = owner_operation->completion_descriptor.value();
      const auto source = find_operation(family.source);
      const auto wait = find_operation(family.wait);
      const auto poll = find_operation(family.poll);
      const auto arm = find_operation(family.arm);
      const auto detach = find_operation(family.detach);
      if (!source || !wait || !poll || !arm || !detach)
        return TypeId::invalid();
      if (family.arm_lane_map.size() != 4 ||
          family.detach_lane_map.size() != 3) {
        error = "operation-owned completion has invalid arm/detach lane maps";
        return TypeId::invalid();
      }
      std::array<std::uint32_t, 4> arm_parameters;
      std::ranges::copy(family.arm_lane_map, arm_parameters.begin());
      std::array<std::uint32_t, 3> detach_parameters;
      std::ranges::copy(family.detach_lane_map, detach_parameters.begin());
      const auto completion = PublicType::callbackCompletion(
          family.registration_callback_adapter,
          physical_type(source->return_type),
          physical_type(family.completion_carrier), callable_type(wait),
          callable_type(poll), family.authority, callable_type(arm),
          callable_type(detach), arm_parameters, detach_parameters);
      storage =
          state == ForeignOperationStateKind::Completion
              ? completion
              : PublicType::callbackWake(PublicType::foreignOperationState(
                    type_value.nominal_entity,
                    ForeignOperationStateKind::Completion));
    }
    const auto materialized =
        materializePublicType(storage, generic, location, error);
    return markForeignOperationState(materialized, type_value.nominal_entity,
                                     state);
  }
  case PublicTypeKind::ForeignCompletion:
  case PublicTypeKind::ForeignWake: {
    PublicType owner(type_value.nominal_entity);
    const auto owner_type =
        materializePublicType(owner, generic, location, error);
    if (!owner_type.hasValue() || type(owner_type).kind != SemTypeKind::Nominal)
      return TypeId::invalid();
    const auto owner_id = NominalTypeId(type(owner_type).arg0);
    const auto &nominal = nominalType(owner_id);
    if (nominal.kind != NominalKind::ForeignResource ||
        (type_value.kind == PublicTypeKind::ForeignCompletion &&
         !nominal.foreign_completion_handle_type.hasValue()) ||
        (type_value.kind == PublicTypeKind::ForeignWake &&
         !nominal.foreign_wake_storage_type.hasValue())) {
      error = "foreign associated type has an incomplete resource owner";
      return TypeId::invalid();
    }
    return type_value.kind == PublicTypeKind::ForeignCompletion
               ? addForeignCompletionType(owner_id)
               : addForeignWakeType(owner_id);
  }
  case PublicTypeKind::CallbackRegistration: {
    if (type_value.arguments.size() != 5 && type_value.arguments.size() != 7 &&
        type_value.arguments.size() != 8 && type_value.arguments.size() != 10) {
      error = "callback registration artifact type has invalid fields";
      return TypeId::invalid();
    }
    std::vector<TypeId> fields(type_value.arguments.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fields[index] = materializePublicType(type_value.arguments[index],
                                            generic, location, error);
      if (!fields[index].hasValue())
        return TypeId::invalid();
    }
    return addCallbackRegistrationType(
        fields[0], fields[1], fields[2], fields[3], fields[4],
        static_cast<CallbackReleaseAuthority>(
            type_value.registration_authority),
        type_value.registration_entry_parameter,
        type_value.registration_userdata_parameter,
        type_value.registration_release_parameter,
        type_value.registration_bindings,
        fields.size() >= 7 ? fields[5] : TypeId::invalid(),
        fields.size() >= 7 ? fields[6] : TypeId::invalid(),
        fields.size() >= 8 ? fields[7] : TypeId::invalid(),
        fields.size() == 10 ? fields[8] : TypeId::invalid(),
        fields.size() == 10 ? fields[9] : TypeId::invalid(),
        type_value.registration_arm_parameters,
        type_value.registration_detach_parameters);
  }
  case PublicTypeKind::TypeParameter:
    if (generic.hasValue())
      return addTypeParameter(generic, type_value.binding_index);
    error = "concrete artifact contains a dependent type";
    return TypeId::invalid();
  case PublicTypeKind::TypeProjection: {
    if (!generic.hasValue() || type_value.arguments.size() != 1 ||
        type_value.projection_kind >= PublicTypeProjectionKind::Count ||
        type_value.projection_index > ProjectionIndexMask) {
      error = "artifact contains an invalid type projection";
      return TypeId::invalid();
    }
    const auto source = materializePublicType(type_value.arguments.front(),
                                              generic, location, error);
    if (!source.hasValue())
      return TypeId::invalid();
    if (type_value.projection_kind == PublicTypeProjectionKind::Associated) {
      const auto entity_id = imports_.registry().findEntity(
          type_value.nominal_entity.canonical_package,
          type_value.nominal_entity.canonical_module,
          type_value.nominal_entity.canonical_name, PublicEntityKind::Interface,
          type_value.nominal_entity.expected_fingerprint);
      const auto *entity = imports_.registry().tryGetEntity(entity_id);
      if (!entity || entity->kind != PublicEntityKind::Interface ||
          entity->fingerprint !=
              type_value.nominal_entity.expected_fingerprint ||
          !entity->interface_declaration || !entity->generic.hasValue() ||
          type_value.projection_index >= entity->generic_parameter_count ||
          std::ranges::none_of(
              entity->interface_declaration->requirements,
              [&](const PublicInterfaceRequirementArtifact &requirement) {
                return requirement.kind ==
                           PublicInterfaceRequirementKind::AssociatedAlias &&
                       requirement.binding_index == type_value.projection_index;
              })) {
        error = "associated projection has a stale interface binding";
        return TypeId::invalid();
      }
      return addTypeParameter(entity->generic, type_value.projection_index);
    }
    return addType({SemTypeKind::TypeProjection, source.index,
                    (static_cast<std::uint32_t>(type_value.projection_kind)
                     << ProjectionKindShift) |
                        type_value.projection_index});
  }
  case PublicTypeKind::Reference: {
    if (type_value.arguments.size() != 1) {
      error = "reference artifact type has no canonical pointee";
      return TypeId::invalid();
    }
    const auto pointee = materializePublicType(type_value.arguments.front(),
                                               generic, location, error);
    if (!pointee.hasValue())
      return TypeId::invalid();
    return addReferenceType(pointee,
                            type_value.reference_mutability ==
                                    PublicReferenceMutability::Mutable
                                ? SemReferenceMutability::Mutable
                                : SemReferenceMutability::ReadOnly,
                            type_value.reference_provenance.kind ==
                                    PublicReferenceProvenanceKind::Parameter
                                ? SemReferenceProvenanceKind::Parameter
                                : SemReferenceProvenanceKind::Erased,
                            type_value.reference_provenance.index);
  }
  case PublicTypeKind::Nominal: {
    auto entity_id = imports_.registry().findEntity(
        type_value.nominal_entity.canonical_package,
        type_value.nominal_entity.canonical_module,
        type_value.nominal_entity.canonical_name,
        PublicEntityKind::NominalType);
    const auto *entity = imports_.registry().tryGetEntity(entity_id);
    NominalTypeId nominal_id;
    const auto local_interface = imports_.registry().tryGet(
        imports_.registry().findByCheckIR(check_ir_id_));
    const auto local_artifact_identity =
        local_interface &&
        type_value.nominal_entity.canonical_package ==
            identifier(local_interface->packageName()) &&
        type_value.nominal_entity.canonical_module == identifier(module_name_);
    if ((!entity || entity->kind != PublicEntityKind::NominalType ||
         entity->fingerprint !=
             type_value.nominal_entity.expected_fingerprint) &&
        local_artifact_identity) {
      for (std::uint32_t index = 0; index < nominal_types_.size(); ++index) {
        const auto candidate = NominalTypeId(index);
        if (!nominalType(candidate).canonical_entity.hasValue() &&
            identifier(name(nominalType(candidate).name).text) ==
                type_value.nominal_entity.canonical_name) {
          nominal_id = candidate;
          break;
        }
      }
    }
    if ((!entity || entity->kind != PublicEntityKind::NominalType ||
         entity->fingerprint !=
             type_value.nominal_entity.expected_fingerprint) &&
        !nominal_id.hasValue()) {
      error = "nominal artifact type has a stale canonical entity";
      return TypeId::invalid();
    }
    for (std::uint32_t index = 0; index < nominal_types_.size(); ++index) {
      const auto candidate_id = NominalTypeId(index);
      const auto &candidate = nominal_types_.get(candidate_id);
      if ((entity && candidate.canonical_entity == entity_id) ||
          (entity && !candidate.canonical_entity.hasValue() &&
           entity->module_name == module_name_ &&
           entity->name == name(candidate.name).text)) {
        nominal_id = NominalTypeId(index);
        break;
      }
    }
    if (!nominal_id.hasValue() && entity) {
      nominal_id = addNominalTypeDecl(
          {addName(entity->name), entity->generic, {}, location, 0, entity_id});
      auto nominal = nominalType(nominal_id);
      nominal.kind = entity->nominal_kind;
      for (const auto &field : entity->nominal_fields) {
        const auto field_type =
            materializePublicType(field.type, entity->generic, location, error);
        if (!field_type.hasValue())
          return TypeId::invalid();
        nominal.fields.push_back(
            {addName(values_->internIdentifier(field.name)), field_type,
             location, field.is_public ? 1U : 0U,
             [&] {
               std::vector<NameId> path;
               for (const auto &component : field.storage_path)
                 path.push_back(addName(values_->internIdentifier(component)));
               return path;
             }(),
             field.projection_kind,
             field.projector_name.empty()
                 ? NameId::invalid()
                 : addName(values_->internIdentifier(field.projector_name)),
             [&] {
               std::vector<NameId> path;
               for (const auto &component : field.projection_region_path)
                 path.push_back(addName(values_->internIdentifier(component)));
               return path;
             }(),
             field.bit_begin, field.bit_end});
      }
      if (entity->nominal_object_repr_pattern) {
        nominal.object_repr_pattern =
            materializePublicType(*entity->nominal_object_repr_pattern,
                                  entity->generic, location, error);
        if (!nominal.object_repr_pattern.hasValue())
          return TypeId::invalid();
      }
      if (entity->nominal_value_repr_pattern) {
        nominal.value_repr_pattern =
            materializePublicType(*entity->nominal_value_repr_pattern,
                                  entity->generic, location, error);
        if (!nominal.value_repr_pattern.hasValue())
          return TypeId::invalid();
      }
      setNominalType(nominal_id, std::move(nominal));
    }
    if (nominal_id.hasValue() && entity &&
        entity->nominal_kind == NominalKind::Enum &&
        nominalType(nominal_id).variants.empty() &&
        !entity->nominal_variants.empty()) {
      auto nominal = nominalType(nominal_id);
      nominal.kind = entity->nominal_kind;
      nominal.is_value_enum = entity->nominal_is_value_enum;
      for (const auto &source_variant : entity->nominal_variants) {
        SemEnumVariant variant;
        variant.name = addName(values_->internIdentifier(source_variant.name));
        variant.declaration = location;
        variant.shape = source_variant.shape == PublicEnumPayloadShape::Unit
                            ? SemEnumPayloadShape::Unit
                        : source_variant.shape == PublicEnumPayloadShape::Tuple
                            ? SemEnumPayloadShape::Tuple
                            : SemEnumPayloadShape::Struct;
        variant.discriminant = source_variant.discriminant;
        for (const auto &source_field : source_variant.fields) {
          const auto field_type = materializePublicType(
              source_field.type, entity->generic, location, error);
          if (!field_type.hasValue())
            return TypeId::invalid();
          variant.fields.push_back(
              {addName(values_->internIdentifier(source_field.name)),
               field_type, location, source_field.is_public ? 1U : 0U});
        }
        nominal.variants.push_back(std::move(variant));
      }
      setNominalType(nominal_id, std::move(nominal));
    }
    if (nominal_id.hasValue() && entity &&
        entity->nominal_kind == NominalKind::ForeignHandle) {
      auto nominal = nominalType(nominal_id);
      nominal.kind = NominalKind::ForeignHandle;
      if (entity->nominal_foreign_representation &&
          !nominal.foreign_representation.hasValue()) {
        const auto representation =
            materializePublicType(*entity->nominal_foreign_representation,
                                  entity->generic, location, error);
        if (!representation.hasValue())
          return TypeId::invalid();
        nominal.foreign_representation = representation;
      }
      nominal.foreign_invalid_state = entity->nominal_foreign_invalid_state;
      nominal.foreign_invalid_integer = entity->nominal_foreign_invalid_integer;
      nominal.completion_state = SemNominalCompletionState::Complete;
      setNominalType(nominal_id, std::move(nominal));
    }
    std::vector<TypeId> arguments;
    arguments.reserve(type_value.arguments.size());
    for (const auto &argument : type_value.arguments) {
      const auto mapped =
          materializePublicType(argument, generic, location, error);
      if (!mapped.hasValue())
        return TypeId::invalid();
      arguments.push_back(mapped);
    }
    return addNominalType(nominal_id, arguments);
  }
  case PublicTypeKind::Count:
    error = "artifact contains an invalid public type";
    return TypeId::invalid();
  }
  error = "artifact contains an unknown public type";
  return TypeId::invalid();
}



} // namespace chtholly::compiler
