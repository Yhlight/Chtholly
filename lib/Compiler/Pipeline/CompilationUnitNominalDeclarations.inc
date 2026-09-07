// Included inside buildNominalArtifacts; this fragment aliases the shared
// SemIR/public-interface and nominal-layout state.
struct BuiltNominalArtifacts {
  std::vector<NominalTypeSpecificArtifact> specifics;
  std::vector<NominalSemanticWitnessArtifact> nominal_semantic_witnesses;
  std::vector<NominalTypeLayoutArtifact> layouts;
  std::vector<std::pair<TypeId, std::size_t>> type_witness_bindings;
  std::vector<LowNominalLayoutBinding> type_layout_bindings;
};

using NominalSemanticWitnessResolver =
    std::function<std::optional<NominalSemanticWitnessArtifact>(
        const NominalTypeSpecificArtifact &specific, std::string &error)>;
using NominalSemanticOptionsResolver = std::function<StableFingerprint(
    const PublicEntityReferenceArtifact &entity)>;

std::optional<BuiltNominalArtifacts> buildNominalArtifacts(
    SemIR &sem_ir, const PublicInterfaceArtifact &public_interface,
    const PublicInterfaceRegistry &registry, StableFingerprint semantic_options,
    std::string_view target_triple,
    const NominalSemanticOptionsResolver &resolve_semantic_options,
    const NominalSemanticWitnessResolver &resolve_nominal_semantic_witness,
    std::string &error) {
  std::vector<std::optional<PublicNominalTypeArtifact>> definitions(
      sem_ir.nominalTypeCount());
  std::vector<std::uint8_t> definition_state(sem_ir.nominalTypeCount());
  std::function<std::optional<PublicType>(TypeId)> map_type;
  std::function<bool(NominalTypeId)> build_definition;
  build_definition = [&](NominalTypeId id) {
    if (definition_state[id.index] == 2)
      return true;
    if (definition_state[id.index] == 1) {
      error = "nominal artifact definition has a by-value cycle";
      return false;
    }
    definition_state[id.index] = 1;
    const auto &nominal = sem_ir.nominalType(id);
    if (nominal.canonical_entity.hasValue()) {
      const auto *entity = registry.tryGetEntity(nominal.canonical_entity);
      if (!entity || entity->kind != PublicEntityKind::NominalType) {
        error = "imported nominal artifact has no canonical registry entity";
        return false;
      }
      definitions[id.index] = buildPublicNominalTypeArtifact(
          std::string(sem_ir.identifier(entity->package_name)),
          std::string(sem_ir.identifier(entity->module_name)),
          std::string(sem_ir.identifier(entity->name)),
          entity->generic_parameter_count, entity->nominal_fields,
          entity->nominal_value_repr_pattern,
          entity->nominal_object_repr_pattern,
          entity->nominal_representation_policy, entity->nominal_kind,
          entity->nominal_variants, entity->nominal_is_exported,
          entity->nominal_is_value_enum);
      auto &definition = *definitions[id.index];
      definition.foreign_representation =
          entity->nominal_foreign_representation;
      definition.foreign_invalid_state = entity->nominal_foreign_invalid_state;
      definition.foreign_invalid_integer =
          entity->nominal_foreign_invalid_integer;
      definition.foreign_handle_type = entity->nominal_foreign_handle_type;
      definition.foreign_completion_handle_type =
          entity->nominal_foreign_completion_handle_type;
      definition.foreign_callback_type = entity->nominal_foreign_callback_type;
      definition.foreign_waker_type = entity->nominal_foreign_waker_type;
      definition.foreign_resource_protocol =
          entity->nominal_foreign_resource_protocol;
      definition.foreign_resource_operations =
          entity->nominal_foreign_resource_operations;
      finalizePublicNominalTypeArtifact(definition);
      if (definitions[id.index]->definition_fingerprint !=
          entity->fingerprint) {
        error = "imported nominal artifact disagrees with its registry entity";
        return false;
      }
    } else {
      const auto nominal_name =
          sem_ir.identifier(sem_ir.name(nominal.name).text);
      const auto published_definition = std::ranges::find_if(
          public_interface.nominalTypes(), [&](const auto &artifact) {
            return artifact.entity.canonical_package ==
                       public_interface.packageName() &&
                   artifact.entity.canonical_module ==
                       public_interface.moduleName() &&
                   artifact.entity.canonical_name == nominal_name;
          });
      if (published_definition != public_interface.nominalTypes().end()) {
        definitions[id.index] = *published_definition;
        definition_state[id.index] = 2;
        return true;
      }
      if (nominal.kind == NominalKind::ForeignHandle ||
          nominal.kind == NominalKind::ForeignResource) {
        // Foreign bindings have a dedicated representation and cleanup
        // protocol. Private bindings outside the public ABI closure do not
        // participate in ordinary nominal witness or layout generation.
        definition_state[id.index] = 2;
        return true;
      }
      std::vector<PublicNominalFieldArtifact> fields;
      fields.reserve(nominal.fields.size());
      for (const auto &field : nominal.fields) {
        auto type = map_type(field.type);
        if (!type) {
          error =
              "cannot materialize nominal field `" +
              std::string(sem_ir.identifier(sem_ir.name(field.name).text)) +
              "` in `" +
              std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) +
              "` as a public artifact type";
          return false;
        }
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
          if (!type) {
            error = "cannot materialize enum payload field as a public "
                    "artifact type";
            return false;
          }
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
      definitions[id.index] = buildPublicNominalTypeArtifact(
          std::string(public_interface.packageName()),
          std::string(public_interface.moduleName()),
          std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)),
          nominal.generic.hasValue()
              ? sem_ir.genericValues().generic(nominal.generic).binding_count
              : 0U,
          std::move(fields), std::move(value_repr_pattern),
          std::move(object_repr_pattern), nominal.representation_policy,
          nominal.kind, std::move(variants), true, nominal.is_value_enum);
      if (const auto *published = public_interface.findNominalType(
              definitions[id.index]->entity.canonical_name);
          published && *published != *definitions[id.index]) {
        error = "local nominal artifact disagrees with its public interface";
        return false;
      }
    }
    definition_state[id.index] = 2;
    return true;
  };
  map_type = [&](TypeId id) -> std::optional<PublicType> {
    const auto &type = sem_ir.type(id);
    if (type.kind == SemTypeKind::Void)
      return PublicTypeKind::Void;
    if (type.kind == SemTypeKind::Never)
      return PublicTypeKind::Never;
    if (type.kind == SemTypeKind::Bool)
      return PublicTypeKind::Bool;
    if (type.kind == SemTypeKind::Char)
      return PublicTypeKind::Char;
    if (type.kind == SemTypeKind::Integer)
      return PublicType::integer(type.arg0, type.arg1 != 0);
    if (type.kind == SemTypeKind::Float)
      return PublicType::floating(type.arg0);
    if (type.kind == SemTypeKind::String)
      return PublicTypeKind::String;
    if (type.kind == SemTypeKind::TypeProjection) {
      auto source = map_type(TypeId(type.arg0));
      if (!source)
        return std::nullopt;
      return PublicType::projection(
          std::move(*source),
          static_cast<PublicTypeProjectionKind>(type.arg1 >> 31U),
          type.arg1 & 0x7fffffffU);
    }
    if (type.kind == SemTypeKind::Array) {
      auto element = map_type(TypeId(type.arg0));
      if (!element || type.arg1 == 0)
        return std::nullopt;
      return PublicType(std::move(*element), type.arg1);
    }
    if (type.kind == SemTypeKind::Tuple) {
      std::vector<PublicType> elements;
      for (const auto element_type : sem_ir.typeBlock(TypeBlockId(type.arg0))) {
        auto element = map_type(element_type);
        if (!element)
          return std::nullopt;
        elements.push_back(std::move(*element));
      }
      return sem_ir.isCUnionType(id) ? PublicType::cUnion(std::move(elements))
                                     : PublicType::tuple(std::move(elements));
    }
    if (type.kind == SemTypeKind::Slice) {
      auto element = map_type(TypeId(type.arg0));
      return element ? std::optional(PublicType::slice(std::move(*element),
                                                       type.arg1 != 0))
                     : std::nullopt;
    }
    if (type.kind == SemTypeKind::TypeParameter)
      return PublicType(PublicTypeKind::TypeParameter, type.arg1);
    if (type.kind == SemTypeKind::Reference) {
      auto pointee = map_type(TypeId(type.arg0));
      if (!pointee)
        return std::nullopt;
      PublicReferenceProvenance provenance;
      if (sem_ir.referenceProvenanceKind(id) ==
          SemReferenceProvenanceKind::Parameter) {
        provenance.kind = PublicReferenceProvenanceKind::Parameter;
        provenance.index = sem_ir.referenceProvenanceIndex(id);
      }
      return PublicType(std::move(*pointee),
                        sem_ir.referenceMutability(id) ==
                                SemReferenceMutability::Mutable
                            ? PublicReferenceMutability::Mutable
                            : PublicReferenceMutability::ReadOnly,
                        provenance);
    }
    if (type.kind == SemTypeKind::RawPointer) {
      auto pointee = map_type(TypeId(type.arg0));
      return pointee ? std::optional(PublicType::rawPointer(std::move(*pointee),
                                                            type.arg1 != 0))
                     : std::nullopt;
    }
    if (type.kind == SemTypeKind::Function) {
      std::vector<PublicType> parameters;
      for (const auto parameter : sem_ir.typeBlock(TypeBlockId(type.arg0))) {
        auto mapped = map_type(parameter);
        if (!mapped)
          return std::nullopt;
        parameters.push_back(std::move(*mapped));
      }
      auto result = map_type(TypeId(type.arg1));
      return result ? std::optional(PublicType::function(std::move(parameters),
                                                         std::move(*result)))
                    : std::nullopt;
    }
    if (type.kind == SemTypeKind::CFunctionPointer ||
        type.kind == SemTypeKind::CVariadicFunctionPointer) {
      std::vector<PublicType> parameters;
      for (const auto parameter : sem_ir.typeBlock(TypeBlockId(type.arg0))) {
        auto mapped = map_type(parameter);
        if (!mapped)
          return std::nullopt;
        parameters.push_back(std::move(*mapped));
      }
      auto result = map_type(TypeId(type.arg1));
      return result ? std::optional(PublicType::cFunctionPointer(
                          std::move(parameters), std::move(*result),
                          type.kind == SemTypeKind::CVariadicFunctionPointer,
                          sem_ir.callbackContract(id),
                          sem_ir.callbackContextParameter(id),
                          sem_ir.cFunctionCallingConvention(id)))
                    : std::nullopt;
    }
    if (type.kind == SemTypeKind::CallbackAdapter) {
      const auto fields = sem_ir.typeBlock(TypeBlockId(type.arg0));
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
    if (const auto owner = sem_ir.foreignOperationStateOwner(id))
      return PublicType::foreignOperationState(owner->operation, owner->state);
    if (type.kind == SemTypeKind::CallbackRegistration) {
      const auto fields = sem_ir.typeBlock(TypeBlockId(type.arg0));
      if (fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
          fields.size() != 10)
        return std::nullopt;
      std::vector<std::optional<PublicType>> mapped(fields.size());
      for (std::size_t index = 0; index < fields.size(); ++index) {
        mapped[index] = map_type(fields[index]);
        if (!mapped[index])
          return std::nullopt;
      }
      const auto marked = sem_ir.callbackRegistrationParameters(id);
      return PublicType::callbackRegistration(
          std::move(*mapped[0]), std::move(*mapped[1]), std::move(*mapped[2]),
          std::move(*mapped[3]), std::move(*mapped[4]),
          static_cast<std::uint8_t>(sem_ir.callbackRegistrationAuthority(id)),
          marked[0], marked[1], marked[2],
          std::vector<CallbackRegistrationBinding>(
              sem_ir.callbackRegistrationBindings(id).begin(),
              sem_ir.callbackRegistrationBindings(id).end()),
          fields.size() >= 7 ? std::optional<PublicType>(std::move(*mapped[5]))
                             : std::nullopt,
          fields.size() >= 7 ? std::optional<PublicType>(std::move(*mapped[6]))
                             : std::nullopt,
          fields.size() >= 8 ? std::optional<PublicType>(std::move(*mapped[7]))
                             : std::nullopt,
          fields.size() == 10 ? std::optional<PublicType>(std::move(*mapped[8]))
                              : std::nullopt,
          fields.size() == 10 ? std::optional<PublicType>(std::move(*mapped[9]))
                              : std::nullopt,
          sem_ir.callbackArmParameters(id),
          sem_ir.callbackDetachParameters(id));
    }
    if (type.kind == SemTypeKind::CallbackCompletion) {
      const auto fields = sem_ir.typeBlock(TypeBlockId(type.arg0));
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
          fields.size() >= 5 ? std::optional<PublicType>(std::move(*mapped[4]))
                             : std::nullopt,
          static_cast<std::uint8_t>(sem_ir.callbackCompletionAuthority(id)),
          fields.size() == 7 ? std::optional<PublicType>(std::move(*mapped[5]))
                             : std::nullopt,
          fields.size() == 7 ? std::optional<PublicType>(std::move(*mapped[6]))
                             : std::nullopt,
          sem_ir.callbackArmParameters(id),
          sem_ir.callbackDetachParameters(id));
    }
    if (type.kind == SemTypeKind::CallbackWake) {
      const auto fields = sem_ir.typeBlock(TypeBlockId(type.arg0));
      if (fields.size() != 1)
        return std::nullopt;
      auto completion = map_type(fields.front());
      return completion ? std::optional(
                              PublicType::callbackWake(std::move(*completion)))
                        : std::nullopt;
    }
    if (type.kind == SemTypeKind::ForeignCompletion ||
        type.kind == SemTypeKind::ForeignWake) {
      const auto storage = sem_ir.objectRepresentationType(id);
      if (!storage.hasValue() || storage == id)
        return std::nullopt;
      return map_type(storage);
    }
    if (type.kind != SemTypeKind::Nominal)
      return std::nullopt;
    const auto nominal = NominalTypeId(type.arg0);
    if (!build_definition(nominal))
      return std::nullopt;
    if (!definitions[nominal.index])
      return std::nullopt;
    std::vector<PublicType> arguments;
    for (const auto argument : sem_ir.typeBlock(TypeBlockId(type.arg1))) {
      auto mapped = map_type(argument);
      if (!mapped)
        return std::nullopt;
      arguments.push_back(std::move(*mapped));
    }
    return PublicType(definitions[nominal.index]->entity, std::move(arguments));
  };

  for (std::uint32_t index = 0; index < sem_ir.nominalTypeCount(); ++index)
    if (!build_definition(NominalTypeId(index))) {
      if (error.empty())
        error = "failed to build a nominal definition artifact";
      return std::nullopt;
    }

  const auto find_definition = [&](const PublicEntityReferenceArtifact &entity)
      -> const PublicNominalTypeArtifact * {
    for (const auto &definition : definitions)
      if (definition && definition->entity == entity)
        return &*definition;
    return nullptr;
  };

  const auto find_lifecycle_target =
      [&](const PublicEntityReferenceArtifact &owner,
          SemCanonicalFunctionRole role)
      -> std::optional<PublicEntityReferenceArtifact> {
    for (std::uint32_t index = 0; index < registry.entityCount(); ++index) {
      const auto entity_id = PublicEntityId(index);
      const auto *candidate = registry.tryGetEntity(entity_id);
      if (!candidate || candidate->kind != PublicEntityKind::Function)
        continue;
      const bool lifecycle_match = candidate->member_owner &&
                                   *candidate->member_owner == owner &&
                                   candidate->semantic_contract.domain ==
                                       CallableSemanticDomain::Lifecycle &&
                                   candidate->semantic_contract.role == role;
      const bool intrinsic_match =
          role == SemCanonicalFunctionRole::Drop &&
          candidate->intrinsic_role == CompilerIntrinsicRole::VecDrop &&
          !candidate->member_owner &&
          sem_ir.identifier(candidate->package_name) ==
              owner.canonical_package &&
          sem_ir.identifier(candidate->module_name) == owner.canonical_module &&
          sem_ir.identifier(candidate->name) ==
              "$lifecycle$" + owner.canonical_name + "$drop";
      if (!lifecycle_match && !intrinsic_match)
        continue;
      return PublicEntityReferenceArtifact{
          PublicEntityKind::Function,
          std::string(sem_ir.identifier(candidate->package_name)),
          std::string(sem_ir.identifier(candidate->module_name)),
          std::string(sem_ir.identifier(candidate->name)),
          candidate->fingerprint};
    }
    return std::nullopt;
  };

  BuiltNominalArtifacts result;
