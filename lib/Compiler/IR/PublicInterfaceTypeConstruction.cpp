#include "PublicInterfaceConstructionInternal.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <ranges>
#include <unordered_map>
#include <utility>

namespace chtholly::compiler::internal {

PublicTypeMappingScope::PublicTypeMappingScope(
    GenericId &generic_slot,
    std::span<const SemInterfaceConstraint> &constraint_slot, GenericId generic,
    std::span<const SemInterfaceConstraint> constraints)
    : generic_slot_(generic_slot), constraint_slot_(constraint_slot),
      previous_generic_(generic_slot), previous_constraints_(constraint_slot) {
  generic_slot_ = generic;
  constraint_slot_ = constraints;
}

PublicTypeMappingScope::~PublicTypeMappingScope() {
  generic_slot_ = previous_generic_;
  constraint_slot_ = previous_constraints_;
}

struct PublicInterfaceTypeConstructionContext::Impl {
  Impl(const SemIR &semantic_ir, PublicInterfaceRegistry &interface_registry,
       IdentifierId package, std::string &error_output,
       PublicInterfaceTypeConstructionCallbacks callback_set)
      : sem_ir(semantic_ir), registry(interface_registry),
        package_name(package), error(error_output),
        callbacks(std::move(callback_set)),
        nominal_artifacts(semantic_ir.nominalTypeCount()),
        nominal_state(semantic_ir.nominalTypeCount()) {
    build_nominal = [this](NominalTypeId id) {
      if (nominal_state[id.index] == 2)
        return true;
      if (nominal_state[id.index] == 1) {
        error = "public nominal definition has a by-value cycle";
        return false;
      }
      nominal_state[id.index] = 1;
      const auto &nominal = sem_ir.nominalType(id);
      PublicTypeMappingScope mapping_scope(mapped_generic, mapped_constraints,
                                           nominal.generic);
      if (nominal.canonical_entity.hasValue()) {
        const auto *entity = registry.tryGetEntity(nominal.canonical_entity);
        if (!entity || entity->kind != PublicEntityKind::NominalType) {
          error = "imported nominal type has no canonical registry entity";
          return false;
        }
        nominal_artifacts[id.index] = buildPublicNominalTypeArtifact(
            std::string(sem_ir.identifier(entity->package_name)),
            std::string(sem_ir.identifier(entity->module_name)),
            std::string(sem_ir.identifier(entity->name)),
            entity->generic_parameter_count, entity->nominal_fields,
            entity->nominal_value_repr_pattern,
            entity->nominal_object_repr_pattern,
            entity->nominal_representation_policy, entity->nominal_kind,
            entity->nominal_variants, entity->nominal_is_exported,
            entity->nominal_is_value_enum);
        auto &artifact = *nominal_artifacts[id.index];
        artifact.foreign_representation =
            entity->nominal_foreign_representation;
        artifact.foreign_invalid_state = entity->nominal_foreign_invalid_state;
        artifact.foreign_invalid_integer =
            entity->nominal_foreign_invalid_integer;
        artifact.foreign_handle_type = entity->nominal_foreign_handle_type;
        artifact.foreign_completion_handle_type =
            entity->nominal_foreign_completion_handle_type;
        artifact.foreign_callback_type = entity->nominal_foreign_callback_type;
        artifact.foreign_waker_type = entity->nominal_foreign_waker_type;
        artifact.foreign_resource_protocol =
            entity->nominal_foreign_resource_protocol;
        artifact.foreign_resource_operations =
            entity->nominal_foreign_resource_operations;
        finalizePublicNominalTypeArtifact(artifact);
        if (nominal_artifacts[id.index]->definition_fingerprint !=
            entity->fingerprint) {
          error =
              "imported nominal definition disagrees with its canonical entity";
          return false;
        }
        nominal_state[id.index] = 2;
        return true;
      }
      std::vector<PublicNominalFieldArtifact> fields;
      fields.reserve(nominal.fields.size());
      for (const auto &field : nominal.fields) {
        const auto &field_sem_type = sem_ir.type(field.type);
        const auto artifact_type =
            field_sem_type.kind == SemTypeKind::ForeignCompletion ||
                    field_sem_type.kind == SemTypeKind::ForeignWake
                ? sem_ir.objectRepresentationType(field.type)
                : field.type;
        auto type = map_type(artifact_type);
        if (!type)
          return false;
        fields.push_back(
            {std::string(sem_ir.identifier(sem_ir.name(field.name).text)),
             std::move(*type),
             [&] {
               std::vector<std::string> path;
               for (const auto component : field.storage_path)
                 path.push_back(std::string(
                     sem_ir.identifier(sem_ir.name(component).text)));
               return path;
             }(),
             field.projection_kind,
             field.projector_name.hasValue()
                 ? std::string(sem_ir.identifier(
                       sem_ir.name(field.projector_name).text))
                 : std::string{},
             [&] {
               std::vector<std::string> path;
               for (const auto component : field.projection_region_path)
                 path.push_back(std::string(
                     sem_ir.identifier(sem_ir.name(component).text)));
               return path;
             }(),
             field.bit_begin, field.bit_end, (field.flags & 1U) != 0});
      }
      std::vector<PublicEnumVariantArtifact> variants;
      for (const auto &source_variant : nominal.variants) {
        PublicEnumVariantArtifact variant;
        variant.name = std::string(
            sem_ir.identifier(sem_ir.name(source_variant.name).text));
        variant.shape = source_variant.shape == SemEnumPayloadShape::Unit
                            ? PublicEnumPayloadShape::Unit
                        : source_variant.shape == SemEnumPayloadShape::Tuple
                            ? PublicEnumPayloadShape::Tuple
                            : PublicEnumPayloadShape::Struct;
        variant.discriminant = source_variant.discriminant;
        for (const auto &source_field : source_variant.fields) {
          auto type = map_type(source_field.type);
          if (!type)
            return false;
          variant.fields.push_back(
              {std::string(
                   sem_ir.identifier(sem_ir.name(source_field.name).text)),
               std::move(*type),
               {},
               PublicObjectProjectionKind::StableAddress,
               {},
               {},
               0,
               0,
               (nominal.flags & SemNominalTypePublic) != 0});
        }
        variants.push_back(std::move(variant));
      }
      std::optional<PublicType> value_repr_pattern;
      std::optional<PublicType> object_repr_pattern;
      if (nominal.value_repr_pattern.hasValue()) {
        value_repr_pattern = map_type(nominal.value_repr_pattern);
        if (!value_repr_pattern)
          return false;
      }
      if (nominal.object_repr_pattern.hasValue()) {
        object_repr_pattern = map_type(nominal.object_repr_pattern);
        if (!object_repr_pattern)
          return false;
      }
      nominal_artifacts[id.index] = buildPublicNominalTypeArtifact(
          std::string(sem_ir.identifier(package_name)),
          std::string(sem_ir.identifier(sem_ir.moduleName())),
          std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)),
          nominal.generic.hasValue()
              ? sem_ir.genericValues().generic(nominal.generic).binding_count
              : 0U,
          std::move(fields), std::move(value_repr_pattern),
          std::move(object_repr_pattern), nominal.representation_policy,
          nominal.kind, std::move(variants),
          (nominal.flags & SemNominalTypePublic) != 0, nominal.is_value_enum);
      auto &artifact = *nominal_artifacts[id.index];
      if (nominal.kind == NominalKind::ForeignHandle) {
        if (nominal.foreign_representation.hasValue())
          artifact.foreign_representation =
              map_type(nominal.foreign_representation);
        artifact.foreign_invalid_state = nominal.foreign_invalid_state;
        artifact.foreign_invalid_integer = nominal.foreign_invalid_integer;
        if (nominal.foreign_representation.hasValue() &&
            !artifact.foreign_representation)
          return false;
      } else if (nominal.kind == NominalKind::ForeignResource) {
        artifact.foreign_handle_type = map_type(nominal.foreign_handle_type);
        if (nominal.foreign_completion_handle_type.hasValue())
          artifact.foreign_completion_handle_type =
              map_type(nominal.foreign_completion_handle_type);
        if (nominal.foreign_callback_type.hasValue())
          artifact.foreign_callback_type =
              map_type(nominal.foreign_callback_type);
        if (nominal.foreign_waker_type.hasValue())
          artifact.foreign_waker_type = map_type(nominal.foreign_waker_type);
        if (!artifact.foreign_handle_type ||
            (nominal.foreign_completion_handle_type.hasValue() &&
             !artifact.foreign_completion_handle_type) ||
            (nominal.foreign_callback_type.hasValue() &&
             !artifact.foreign_callback_type) ||
            (nominal.foreign_waker_type.hasValue() &&
             !artifact.foreign_waker_type))
          return false;
        artifact.foreign_resource_protocol =
            sem_ir.genericValues()
                .foreignResourceProtocol(nominal.foreign_resource_protocol)
                .facts;
        for (const auto &operation : nominal.foreign_resource_operations) {
          auto target = map_function_ref(operation.target);
          if (!target)
            return false;
          artifact.foreign_resource_operations.push_back(
              {std::string(sem_ir.identifier(sem_ir.name(operation.name).text)),
               operation.role, std::move(*target), operation.resource_parameter,
               operation.completion_parameter});
        }
      }
      finalizePublicNominalTypeArtifact(artifact);
      nominal_state[id.index] = 2;
      return true;
    };
    map_type = [this](TypeId type) -> std::optional<PublicType> {
      if (!type.hasValue() || type.index >= sem_ir.typeCount())
        return std::nullopt;
      const auto &value = sem_ir.type(type);
      if (value.kind == SemTypeKind::Void)
        return PublicType(PublicTypeKind::Void);
      if (value.kind == SemTypeKind::Never)
        return PublicType(PublicTypeKind::Never);
      if (value.kind == SemTypeKind::Bool)
        return PublicType(PublicTypeKind::Bool);
      if (value.kind == SemTypeKind::Char)
        return PublicType(PublicTypeKind::Char);
      if (value.kind == SemTypeKind::Integer)
        return PublicType::integer(value.arg0, value.arg1 != 0);
      if (value.kind == SemTypeKind::Float)
        return PublicType::floating(value.arg0);
      if (value.kind == SemTypeKind::String)
        return PublicType(PublicTypeKind::String);
      if (value.kind == SemTypeKind::Array) {
        auto element = map_type(TypeId(value.arg0));
        if (!element || value.arg1 == 0)
          return std::nullopt;
        return PublicType(std::move(*element), value.arg1);
      }
      if (value.kind == SemTypeKind::Tuple) {
        std::vector<PublicType> elements;
        for (const auto element : sem_ir.typeBlock(TypeBlockId(value.arg0))) {
          auto mapped = map_type(element);
          if (!mapped)
            return std::nullopt;
          elements.push_back(std::move(*mapped));
        }
        return sem_ir.isCUnionType(type)
                   ? PublicType::cUnion(std::move(elements))
                   : PublicType::tuple(std::move(elements));
      }
      if (value.kind == SemTypeKind::Slice) {
        auto element = map_type(TypeId(value.arg0));
        return element ? std::optional(PublicType::slice(std::move(*element),
                                                         value.arg1 != 0))
                       : std::nullopt;
      }
      if (value.kind == SemTypeKind::TypeParameter) {
        const auto owner = GenericId(value.arg0);
        if (owner == mapped_generic)
          return PublicType(PublicTypeKind::TypeParameter, value.arg1);
        InterfaceId owner_interface;
        const SemInterfaceRequirement *associated_requirement = nullptr;
        for (std::uint32_t index = 0; index < sem_ir.interfaceCount();
             ++index) {
          const auto candidate = InterfaceId(index);
          const auto &interface_value = sem_ir.interface(candidate);
          if (interface_value.generic != owner)
            continue;
          const auto requirement = std::ranges::find_if(
              interface_value.requirements,
              [&](const SemInterfaceRequirement &item) {
                return item.kind ==
                           SemInterfaceRequirementKind::AssociatedAlias &&
                       item.binding_index == value.arg1;
              });
          if (requirement != interface_value.requirements.end()) {
            owner_interface = candidate;
            associated_requirement = &*requirement;
          }
          break;
        }
        if (!owner_interface.hasValue() || !associated_requirement)
          return PublicType(PublicTypeKind::TypeParameter, value.arg1);
        const SemInterfaceConstraint *source_constraint = nullptr;
        for (const auto &constraint : mapped_constraints) {
          if (constraint.interface_id != owner_interface)
            continue;
          if (source_constraint) {
            error = "public associated type has an ambiguous source constraint";
            return std::nullopt;
          }
          source_constraint = &constraint;
        }
        if (!source_constraint)
          return PublicType(PublicTypeKind::TypeParameter, value.arg1);
        auto subject = map_type(source_constraint->subject);
        if (!subject)
          return std::nullopt;
        const auto &interface_value = sem_ir.interface(owner_interface);
        PublicEntityReferenceArtifact entity;
        if (interface_value.canonical_entity.hasValue()) {
          const auto *canonical =
              registry.tryGetEntity(interface_value.canonical_entity);
          if (!canonical || canonical->kind != PublicEntityKind::Interface) {
            error = "associated type interface has no canonical entity";
            return std::nullopt;
          }
          entity = {PublicEntityKind::Interface,
                    std::string(sem_ir.identifier(canonical->package_name)),
                    std::string(sem_ir.identifier(canonical->module_name)),
                    std::string(sem_ir.identifier(canonical->name)),
                    canonical->fingerprint};
        } else {
          entity = {PublicEntityKind::Interface,
                    std::string(sem_ir.identifier(package_name)),
                    std::string(sem_ir.identifier(sem_ir.moduleName())),
                    std::string(sem_ir.identifier(
                        sem_ir.name(interface_value.name).text)),
                    interface_value.fingerprint};
        }
        return PublicType::associated(std::move(*subject), std::move(entity),
                                      value.arg1);
      }
      if (value.kind == SemTypeKind::TypeProjection) {
        auto source = map_type(TypeId(value.arg0));
        if (!source)
          return std::nullopt;
        return PublicType::projection(
            std::move(*source),
            static_cast<PublicTypeProjectionKind>(value.arg1 >> 31U),
            value.arg1 & 0x7fffffffU);
      }
      if (value.kind == SemTypeKind::Reference) {
        auto pointee = map_type(TypeId(value.arg0));
        if (!pointee)
          return std::nullopt;
        PublicReferenceProvenance provenance;
        if (sem_ir.referenceProvenanceKind(type) ==
            SemReferenceProvenanceKind::Parameter) {
          provenance.kind = PublicReferenceProvenanceKind::Parameter;
          provenance.index = sem_ir.referenceProvenanceIndex(type);
        }
        return PublicType(std::move(*pointee),
                          sem_ir.referenceMutability(type) ==
                                  SemReferenceMutability::Mutable
                              ? PublicReferenceMutability::Mutable
                              : PublicReferenceMutability::ReadOnly,
                          provenance);
      }
      if (value.kind == SemTypeKind::RawPointer) {
        auto pointee = map_type(TypeId(value.arg0));
        return pointee ? std::optional(PublicType::rawPointer(
                             std::move(*pointee), value.arg1 != 0))
                       : std::nullopt;
      }
      if (value.kind == SemTypeKind::Function) {
        std::vector<PublicType> parameters;
        for (const auto parameter : sem_ir.typeBlock(TypeBlockId(value.arg0))) {
          auto mapped = map_type(parameter);
          if (!mapped)
            return std::nullopt;
          parameters.push_back(std::move(*mapped));
        }
        auto result = map_type(TypeId(value.arg1));
        return result ? std::optional(PublicType::function(
                            std::move(parameters), std::move(*result)))
                      : std::nullopt;
      }
      if (value.kind == SemTypeKind::CFunctionPointer ||
          value.kind == SemTypeKind::CVariadicFunctionPointer) {
        std::vector<PublicType> parameters;
        for (const auto parameter : sem_ir.typeBlock(TypeBlockId(value.arg0))) {
          auto mapped = map_type(parameter);
          if (!mapped)
            return std::nullopt;
          parameters.push_back(std::move(*mapped));
        }
        auto result = map_type(TypeId(value.arg1));
        return result ? std::optional(PublicType::cFunctionPointer(
                            std::move(parameters), std::move(*result),
                            value.kind == SemTypeKind::CVariadicFunctionPointer,
                            sem_ir.callbackContract(type),
                            sem_ir.callbackContextParameter(type),
                            sem_ir.cFunctionCallingConvention(type)))
                      : std::nullopt;
      }
      if (value.kind == SemTypeKind::CallbackAdapter) {
        const auto fields = sem_ir.typeBlock(TypeBlockId(value.arg0));
        if (fields.size() != 3)
          return std::nullopt;
        auto entry = map_type(fields[0]);
        auto context = map_type(fields[1]);
        auto release = map_type(fields[2]);
        return entry && context && release
                   ? std::optional(PublicType::callbackAdapter(
                         std::move(*entry), std::move(*context),
                         std::move(*release)))
                   : std::nullopt;
      }
      if (value.kind == SemTypeKind::CallbackRegistration) {
        const auto fields = sem_ir.typeBlock(TypeBlockId(value.arg0));
        if (fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
            fields.size() != 10)
          return std::nullopt;
        std::vector<std::optional<PublicType>> mapped(fields.size());
        for (std::size_t index = 0; index < fields.size(); ++index) {
          mapped[index] = map_type(fields[index]);
          if (!mapped[index])
            return std::nullopt;
        }
        const auto marked = sem_ir.callbackRegistrationParameters(type);
        return PublicType::callbackRegistration(
            std::move(*mapped[0]), std::move(*mapped[1]), std::move(*mapped[2]),
            std::move(*mapped[3]), std::move(*mapped[4]),
            static_cast<std::uint8_t>(
                sem_ir.callbackRegistrationAuthority(type)),
            marked[0], marked[1], marked[2],
            std::vector<CallbackRegistrationBinding>(
                sem_ir.callbackRegistrationBindings(type).begin(),
                sem_ir.callbackRegistrationBindings(type).end()),
            fields.size() >= 7
                ? std::optional<PublicType>(std::move(*mapped[5]))
                : std::nullopt,
            fields.size() >= 7
                ? std::optional<PublicType>(std::move(*mapped[6]))
                : std::nullopt,
            fields.size() >= 8
                ? std::optional<PublicType>(std::move(*mapped[7]))
                : std::nullopt,
            fields.size() == 10
                ? std::optional<PublicType>(std::move(*mapped[8]))
                : std::nullopt,
            fields.size() == 10
                ? std::optional<PublicType>(std::move(*mapped[9]))
                : std::nullopt,
            sem_ir.callbackArmParameters(type),
            sem_ir.callbackDetachParameters(type));
      }
      if (value.kind == SemTypeKind::CallbackCompletion) {
        const auto fields = sem_ir.typeBlock(TypeBlockId(value.arg0));
        if (fields.size() != 4 && fields.size() != 5 && fields.size() != 7)
          return std::nullopt;
        std::vector<std::optional<PublicType>> mapped(fields.size());
        for (std::size_t index = 0; index < fields.size(); ++index) {
          mapped[index] = map_type(fields[index]);
          if (!mapped[index])
            return std::nullopt;
        }
        return PublicType::callbackCompletion(
            std::move(*mapped[0]), std::move(*mapped[1]), std::move(*mapped[2]),
            std::move(*mapped[3]),
            fields.size() >= 5
                ? std::optional<PublicType>(std::move(*mapped[4]))
                : std::nullopt,
            static_cast<std::uint8_t>(sem_ir.callbackCompletionAuthority(type)),
            fields.size() == 7
                ? std::optional<PublicType>(std::move(*mapped[5]))
                : std::nullopt,
            fields.size() == 7
                ? std::optional<PublicType>(std::move(*mapped[6]))
                : std::nullopt,
            sem_ir.callbackArmParameters(type),
            sem_ir.callbackDetachParameters(type));
      }
      if (value.kind == SemTypeKind::CallbackWake) {
        const auto fields = sem_ir.typeBlock(TypeBlockId(value.arg0));
        if (fields.size() != 1)
          return std::nullopt;
        auto completion = map_type(fields.front());
        return completion ? std::optional(PublicType::callbackWake(
                                std::move(*completion)))
                          : std::nullopt;
      }
      if (value.kind == SemTypeKind::ForeignCompletion ||
          value.kind == SemTypeKind::ForeignWake) {
        const auto nominal = NominalTypeId(value.arg0);
        if (!build_nominal(nominal))
          return std::nullopt;
        const auto &entity = nominal_artifacts[nominal.index]->entity;
        return value.kind == SemTypeKind::ForeignCompletion
                   ? std::optional(PublicType::foreignCompletion(entity))
                   : std::optional(PublicType::foreignWake(entity));
      }
      if (value.kind == SemTypeKind::Nominal) {
        const auto nominal = NominalTypeId(value.arg0);
        if (!build_nominal(nominal))
          return std::nullopt;
        std::vector<PublicType> arguments;
        for (const auto argument : sem_ir.typeBlock(TypeBlockId(value.arg1))) {
          auto mapped = map_type(argument);
          if (!mapped)
            return std::nullopt;
          arguments.push_back(std::move(*mapped));
        }
        return PublicType(nominal_artifacts[nominal.index]->entity,
                          std::move(arguments));
      }
      error = "public signature contains an unsupported type";
      return std::nullopt;
    };
    map_constant = [this](ConstantId id) -> std::optional<PublicConstantValue> {
      const auto &source = sem_ir.constantValue(id);
      auto type = map_type(source.type);
      if (!type)
        return std::nullopt;
      PublicConstantValue value;
      value.kind = static_cast<PublicConstantValueKind>(source.kind);
      value.type = std::move(*type);
      value.target_dependent = source.target_dependent;
      if (source.kind == ConstantValueKind::String) {
        value.string_payload = std::string(sem_ir.string(
            StringLiteralId(static_cast<std::uint32_t>(source.payload))));
      } else if (source.kind != ConstantValueKind::Null &&
                 source.kind != ConstantValueKind::Array &&
                 source.kind != ConstantValueKind::Aggregate &&
                 source.kind != ConstantValueKind::Tuple) {
        value.payload = static_cast<std::uint64_t>(sem_ir.integer(
            IntegerId(static_cast<std::uint32_t>(source.payload))));
      }
      for (const auto element : sem_ir.constantBlock(source.elements)) {
        auto mapped = map_constant(element);
        if (!mapped)
          return std::nullopt;
        value.elements.push_back(std::move(*mapped));
      }
      return value;
    };
    map_function_ref =
        [this](
            FunctionRefId id) -> std::optional<PublicEntityReferenceArtifact> {
      const auto &reference = sem_ir.functionRef(id);
      if (reference.public_entity.hasValue()) {
        const auto *entity = registry.tryGetEntity(reference.public_entity);
        if (!entity || entity->kind != PublicEntityKind::Function)
          return std::nullopt;
        return PublicEntityReferenceArtifact{
            PublicEntityKind::Function,
            std::string(sem_ir.identifier(entity->package_name)),
            std::string(sem_ir.identifier(entity->module_name)),
            std::string(sem_ir.identifier(entity->name)), entity->fingerprint};
      }
      if (!reference.local_function.hasValue())
        return std::nullopt;
      const auto &function = sem_ir.function(reference.local_function);
      const auto &type = sem_ir.type(function.type);
      const auto &declaration =
          sem_ir.functionDeclaration(reference.local_function);
      if (type.kind != SemTypeKind::Function || function.generic.hasValue())
        return std::nullopt;
      std::vector<PublicType> parameters;
      for (const auto parameter : sem_ir.typeBlock(TypeBlockId(type.arg0))) {
        auto mapped = map_type(parameter);
        if (!mapped)
          return std::nullopt;
        parameters.push_back(std::move(*mapped));
      }
      auto result = map_type(TypeId(type.arg1));
      if (!result)
        return std::nullopt;
      const auto name = sem_ir.identifier(sem_ir.name(function.name).text);
      const auto foreign_abi = declaration.foreign_abi.hasValue()
                                   ? sem_ir.identifier(declaration.foreign_abi)
                                   : std::string_view{};
      std::vector<std::string> parameter_names;
      parameter_names.reserve(declaration.parameter_names.size());
      for (const auto parameter_name : declaration.parameter_names)
        parameter_names.emplace_back(sem_ir.identifier(parameter_name));
      if (parameter_names.empty())
        parameter_names =
            callbacks.canonical_parameter_names(parameters.size(), {});
      std::vector<std::optional<PublicConstantValue>> default_arguments;
      default_arguments.reserve(declaration.default_arguments.size());
      for (const auto entity : declaration.default_arguments) {
        if (!entity.hasValue()) {
          default_arguments.emplace_back();
          continue;
        }
        const auto &constant = sem_ir.constantEntity(entity);
        if (!constant.result.isConcrete())
          return std::nullopt;
        auto mapped = map_constant(constant.result.value);
        if (!mapped)
          return std::nullopt;
        default_arguments.push_back(std::move(*mapped));
      }
      if (default_arguments.empty())
        default_arguments =
            callbacks.canonical_default_arguments(parameters.size(), {});
      auto interop_artifact = declaration.interop_artifact;
      return PublicEntityReferenceArtifact{
          PublicEntityKind::Function,
          std::string(sem_ir.identifier(package_name)),
          std::string(sem_ir.identifier(sem_ir.moduleName())),
          std::string(name),
          callbacks.entity_fingerprint(
              sem_ir.identifier(package_name),
              sem_ir.identifier(sem_ir.moduleName()), name, std::nullopt,
              PublicFunctionArtifact::MemberKind::None, 0, parameters, *result,
              std::nullopt, PublicFunctionExecutionKind::Immediate, {}, {}, {},
              function.intrinsic_role,
              sem_ir.functionOwnership(reference.local_function), std::nullopt,
              declaration.kind == SemCallableDeclarationKind::Foreign
                  ? PublicCallableDeclarationKind::Foreign
              : declaration.kind == SemCallableDeclarationKind::Forward
                  ? PublicCallableDeclarationKind::Forward
                  : PublicCallableDeclarationKind::Definition,
              declaration.is_unsafe, declaration.is_const, foreign_abi,
              declaration.foreign_signature, parameter_names, default_arguments,
              {}, interop_artifact,
              declaration.external_symbol.hasValue()
                  ? sem_ir.identifier(declaration.external_symbol)
                  : std::string_view{})};
    };
  }

  const SemIR &sem_ir;
  PublicInterfaceRegistry &registry;
  IdentifierId package_name;
  std::string &error;
  PublicInterfaceTypeConstructionCallbacks callbacks;
  std::vector<std::optional<PublicNominalTypeArtifact>> nominal_artifacts;
  std::vector<std::uint8_t> nominal_state;
  GenericId mapped_generic;
  std::span<const SemInterfaceConstraint> mapped_constraints;
  std::function<std::optional<PublicType>(TypeId)> map_type;
  std::function<std::optional<PublicEntityReferenceArtifact>(FunctionRefId)>
      map_function_ref;
  std::function<bool(NominalTypeId)> build_nominal;
  std::function<std::optional<PublicConstantValue>(ConstantId)> map_constant;
};

PublicInterfaceTypeConstructionContext::PublicInterfaceTypeConstructionContext(
    const SemIR &sem_ir, PublicInterfaceRegistry &registry,
    IdentifierId package_name, std::string &error,
    PublicInterfaceTypeConstructionCallbacks callbacks)
    : impl_(std::make_unique<Impl>(sem_ir, registry, package_name, error,
                                   std::move(callbacks))) {}

PublicInterfaceTypeConstructionContext::
    ~PublicInterfaceTypeConstructionContext() = default;

std::optional<PublicType>
PublicInterfaceTypeConstructionContext::mapType(TypeId type) {
  return impl_->map_type(type);
}

std::optional<PublicConstantValue>
PublicInterfaceTypeConstructionContext::mapConstant(ConstantId id) {
  return impl_->map_constant(id);
}

std::optional<PublicEntityReferenceArtifact>
PublicInterfaceTypeConstructionContext::mapFunctionReference(FunctionRefId id) {
  return impl_->map_function_ref(id);
}

bool PublicInterfaceTypeConstructionContext::buildNominals() {
  for (std::uint32_t index = 0; index < impl_->sem_ir.nominalTypeCount();
       ++index)
    if (!impl_->build_nominal(NominalTypeId(index)))
      return false;
  return true;
}

const std::vector<std::optional<PublicNominalTypeArtifact>> &
PublicInterfaceTypeConstructionContext::nominalArtifacts() const {
  return impl_->nominal_artifacts;
}

GenericId &PublicInterfaceTypeConstructionContext::mappedGeneric() {
  return impl_->mapped_generic;
}

std::span<const SemInterfaceConstraint> &
PublicInterfaceTypeConstructionContext::mappedConstraints() {
  return impl_->mapped_constraints;
}

} // namespace chtholly::compiler::internal
