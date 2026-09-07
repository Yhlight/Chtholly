// Included by NominalTypeArtifact.cpp; this fragment aliases canonical
// nominal artifact encoding, verification, and decode state.
constexpr std::string_view DefinitionMagic = "CHNXTYPE30";
constexpr std::string_view SpecificMagic = "CHNXSPE32";
constexpr std::string_view LayoutMagic = "CHNXLAY22";
constexpr std::string_view WitnessMagic = "CHNXWIT27";

void appendU8(std::string &out, std::uint8_t value) {
  out.push_back(static_cast<char>(value));
}

void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendU64(std::string &out, std::uint64_t value) {
  for (std::uint32_t shift = 0; shift != 64; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(std::string &out, std::string_view value) {
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendFingerprint(std::string &out, const StableFingerprint &value) {
  for (const auto byte : value.bytes())
    out.push_back(static_cast<char>(byte));
}

void appendEntity(std::string &out, const PublicEntityReferenceArtifact &entity,
                  bool include_fingerprint = true) {
  appendU8(out, static_cast<std::uint8_t>(entity.kind));
  appendField(out, entity.canonical_package);
  appendField(out, entity.canonical_module);
  appendField(out, entity.canonical_name);
  if (include_fingerprint)
    appendFingerprint(out, entity.expected_fingerprint);
}

void appendOwnershipRegion(std::string &out, const OwnershipRegion &region) {
  appendU32(out, region.parameter_index);
  appendU32(out, static_cast<std::uint32_t>(region.path.size()));
  for (const auto &step : region.path) {
    appendU8(out, static_cast<std::uint8_t>(step.kind));
    appendU32(out, step.index);
  }
  appendU8(out, region.has_bit_range ? 1U : 0U);
  appendU32(out, region.bit_begin);
  appendU32(out, region.bit_end);
}

void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary) {
  appendU32(out, static_cast<std::uint32_t>(summary.effects.size()));
  for (const auto &effect : summary.effects) {
    appendU8(out, static_cast<std::uint8_t>(effect.kind));
    appendOwnershipRegion(out, effect.region);
  }
  appendU32(out, static_cast<std::uint32_t>(summary.postconditions.size()));
  for (const auto &postcondition : summary.postconditions) {
    appendU32(out, postcondition.result_variant);
    appendOwnershipRegion(out, postcondition.region);
    appendU8(out, postcondition.outcomes);
    appendU32(
        out, static_cast<std::uint32_t>(postcondition.condition.clauses.size()));
    for (const auto &clause : postcondition.condition.clauses) {
      appendU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendU32(out, atom.variant);
        appendU32(out, atom.parameter_index);
        appendU8(out, atom.expected ? 1U : 0U);
      }
    }
  }
  appendU8(out, summary.returns_owned ? 1U : 0U);
  appendU32(out, static_cast<std::uint32_t>(summary.return_provenance.size()));
  for (const auto &source : summary.return_provenance) {
    appendOwnershipRegion(out, source.region);
    appendU32(out, static_cast<std::uint32_t>(source.carrier_path.size()));
    for (const auto &step : source.carrier_path) {
      appendU8(out, static_cast<std::uint8_t>(step.kind));
      appendU32(out, step.index);
    }
    appendU32(out, static_cast<std::uint32_t>(source.condition.clauses.size()));
    for (const auto &clause : source.condition.clauses) {
      appendU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendU32(out, atom.variant);
        appendU32(out, atom.parameter_index);
        appendU8(out, atom.expected ? 1U : 0U);
      }
    }
  }
}

void appendType(std::string &out, const PublicType &type) {
  appendU8(out, static_cast<std::uint8_t>(type.kind));
  appendU32(out, type.binding_index);
  if (type.kind == PublicTypeKind::Integer) {
    appendU32(out, type.scalar_width);
    appendU8(out, type.integer_signed ? 1U : 0U);
    return;
  }
  if (type.kind == PublicTypeKind::Float) {
    appendU32(out, type.scalar_width);
    return;
  }
  if (type.kind == PublicTypeKind::Reference) {
    appendU8(out, static_cast<std::uint8_t>(type.reference_mutability));
    appendU8(out, static_cast<std::uint8_t>(type.reference_provenance.kind));
    appendU32(out, type.reference_provenance.index);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
    return;
  }
  if (type.kind == PublicTypeKind::Array) {
    appendU32(out, type.array_bound);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
    return;
  }
  if (type.kind == PublicTypeKind::Tuple) {
    appendU8(out, type.abi_union ? 1U : 0U);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    return;
  }
  if (type.kind == PublicTypeKind::Slice) {
    appendU8(out, type.slice_mutable ? 1U : 0U);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
    return;
  }
  if (type.kind == PublicTypeKind::TypeProjection) {
    appendU8(out, static_cast<std::uint8_t>(type.projection_kind));
    appendU32(out, type.projection_index);
    if (type.projection_kind == PublicTypeProjectionKind::Associated)
      appendEntity(out, type.nominal_entity);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
    return;
  }
  if (type.kind == PublicTypeKind::RawPointer) {
    appendU8(out, type.pointer_const ? 1U : 0U);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
    return;
  }
  if (type.kind == PublicTypeKind::Function) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    return;
  }
  if (type.kind == PublicTypeKind::CFunctionPointer) {
    appendU8(out, type.callable_variadic ? 1U : 0U);
    appendU8(out, static_cast<std::uint8_t>(type.foreign_calling_convention));
    appendU32(out, type.callable_context_parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendOwnershipSummary(out, type.callable_contract);
    return;
  }
  if (type.kind == PublicTypeKind::CallbackAdapter) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    return;
  }
  if (type.kind == PublicTypeKind::CallbackRegistration) {
    out.push_back(static_cast<char>(type.registration_authority));
    appendU32(out, type.registration_entry_parameter);
    appendU32(out, type.registration_userdata_parameter);
    appendU32(out, type.registration_release_parameter);
    appendU32(out,
              static_cast<std::uint32_t>(type.registration_bindings.size()));
    for (const auto &binding : type.registration_bindings) {
      appendField(out, binding.name);
      appendU32(out, binding.parameter_index);
    }
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
    return;
  }
  if (type.kind == PublicTypeKind::CallbackCompletion) {
    appendU8(out, type.registration_authority);
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
    return;
  }
  if (type.kind == PublicTypeKind::CallbackWake) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    return;
  }
  if (type.kind == PublicTypeKind::ForeignCompletion ||
      type.kind == PublicTypeKind::ForeignWake) {
    appendEntity(out, type.nominal_entity);
    return;
  }
  if (type.kind == PublicTypeKind::ForeignOperationState) {
    appendEntity(out, type.nominal_entity);
    appendU8(out, static_cast<std::uint8_t>(type.foreign_operation_state));
    return;
  }
  if (type.kind != PublicTypeKind::Nominal)
    return;
  appendEntity(out, type.nominal_entity);
  appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
  for (const auto &argument : type.arguments)
    appendType(out, argument);
}

void appendFields(std::string &out,
                  std::span<const PublicNominalFieldArtifact> fields) {
  appendU32(out, static_cast<std::uint32_t>(fields.size()));
  for (const auto &field : fields) {
    appendField(out, field.name);
    appendType(out, field.type);
    appendU32(out, static_cast<std::uint32_t>(field.storage_path.size()));
    for (const auto &component : field.storage_path)
      appendField(out, component);
    appendU8(out, static_cast<std::uint8_t>(field.projection_kind));
    appendField(out, field.projector_name);
    appendU32(out,
              static_cast<std::uint32_t>(field.projection_region_path.size()));
    for (const auto &component : field.projection_region_path)
      appendField(out, component);
    appendU32(out, field.bit_begin);
    appendU32(out, field.bit_end);
    appendU8(out, field.is_public ? 1U : 0U);
  }
}

void appendVariants(std::string &out,
                    std::span<const PublicEnumVariantArtifact> variants) {
  appendU32(out, static_cast<std::uint32_t>(variants.size()));
  for (const auto &variant : variants) {
    appendField(out, variant.name);
    appendU8(out, static_cast<std::uint8_t>(variant.shape));
    appendU64(out, std::bit_cast<std::uint64_t>(variant.discriminant));
    appendFields(out, variant.fields);
  }
}

void appendForeignResourceMetadata(std::string &out,
                                   const PublicNominalTypeArtifact &value) {
  const auto optional_type = [&](const std::optional<PublicType> &type) {
    appendU8(out, type.has_value() ? 1U : 0U);
    if (type)
      appendType(out, *type);
  };
  optional_type(value.foreign_representation);
  appendU8(out, static_cast<std::uint8_t>(value.foreign_invalid_state));
  appendU64(out, static_cast<std::uint64_t>(value.foreign_invalid_integer));
  optional_type(value.foreign_handle_type);
  optional_type(value.foreign_completion_handle_type);
  optional_type(value.foreign_callback_type);
  optional_type(value.foreign_waker_type);
  const auto protocol =
      value.kind == NominalKind::ForeignResource
          ? encodeForeignResourceProtocol(value.foreign_resource_protocol)
          : std::string{};
  appendField(out, protocol);
  appendU32(out, static_cast<std::uint32_t>(
                     value.foreign_resource_operations.size()));
  for (const auto &operation : value.foreign_resource_operations) {
    appendField(out, operation.name);
    appendU8(out, static_cast<std::uint8_t>(operation.role));
    appendEntity(out, operation.target);
    appendU32(out, operation.resource_parameter);
    appendU32(out, operation.completion_parameter);
  }
}

bool validEntity(const PublicEntityReferenceArtifact &entity,
                 PublicEntityKind kind) {
  return entity.kind == kind && !entity.canonical_package.empty() &&
         !entity.canonical_module.empty() && !entity.canonical_name.empty() &&
         entity.expected_fingerprint.hasValue();
}

StableFingerprint
definitionFingerprint(const PublicEntityReferenceArtifact &entity,
                      std::uint32_t generic_parameter_count,
                      std::span<const PublicNominalFieldArtifact> fields,
                      std::span<const PublicEnumVariantArtifact> variants,
                      const std::optional<PublicType> &value_repr_pattern,
                      const std::optional<PublicType> &object_repr_pattern,
                      NominalRepresentationPolicy representation_policy,
                      NominalKind kind, bool is_exported, bool is_value_enum,
                      const PublicNominalTypeArtifact *metadata = nullptr) {
  std::string bytes("chtholly.next.nominal-definition.v15");
  appendEntity(bytes, entity, false);
  appendU32(bytes, generic_parameter_count);
  appendFields(bytes, fields);
  appendVariants(bytes, variants);
  appendU8(bytes, value_repr_pattern.has_value() ? 1U : 0U);
  if (value_repr_pattern)
    appendType(bytes, *value_repr_pattern);
  appendU8(bytes, object_repr_pattern.has_value() ? 1U : 0U);
  if (object_repr_pattern)
    appendType(bytes, *object_repr_pattern);
  appendU8(bytes, static_cast<std::uint8_t>(representation_policy));
  appendU8(bytes, static_cast<std::uint8_t>(kind));
  appendU8(bytes, is_exported ? 1U : 0U);
  appendU8(bytes, is_value_enum ? 1U : 0U);
  if (metadata)
    appendForeignResourceMetadata(bytes, *metadata);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

StableFingerprint
specificRequestFingerprint(const PublicEntityReferenceArtifact &entity,
                           std::span<const PublicType> arguments,
                           const StableFingerprint &semantic_options) {
  std::string bytes("chtholly.next.nominal-specific-request.v6");
  appendEntity(bytes, entity);
  appendU32(bytes, static_cast<std::uint32_t>(arguments.size()));
  for (const auto &argument : arguments)
    appendType(bytes, argument);
  appendFingerprint(bytes, semantic_options);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

StableFingerprint structuralSpecificFingerprint(
    const PublicEntityReferenceArtifact &entity,
    std::span<const PublicType> arguments,
    const StableFingerprint &semantic_options,
    std::span<const PublicNominalFieldArtifact> fields,
    std::span<const PublicEnumVariantArtifact> variants,
    NominalRepresentationPolicy representation_policy, NominalKind kind,
    bool is_value_enum, const std::optional<PublicType> &object_repr_carrier,
    std::span<const StableFingerprint> child_specifics) {
  std::string bytes("chtholly.next.nominal-structural-specific.v11");
  appendEntity(bytes, entity);
  appendU32(bytes, static_cast<std::uint32_t>(arguments.size()));
  for (const auto &argument : arguments)
    appendType(bytes, argument);
  appendFingerprint(bytes, semantic_options);
  appendFields(bytes, fields);
  appendVariants(bytes, variants);
  appendU8(bytes, static_cast<std::uint8_t>(representation_policy));
  appendU8(bytes, static_cast<std::uint8_t>(kind));
  appendU8(bytes, is_value_enum ? 1U : 0U);
  appendU8(bytes, object_repr_carrier.has_value() ? 1U : 0U);
  if (object_repr_carrier)
    appendType(bytes, *object_repr_carrier);
  appendU32(bytes, static_cast<std::uint32_t>(child_specifics.size()));
  for (const auto &fingerprint : child_specifics)
    appendFingerprint(bytes, fingerprint);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

void appendOptionalEntity(
    std::string &bytes,
    const std::optional<PublicEntityReferenceArtifact> &entity) {
  appendU8(bytes, entity.has_value() ? 1U : 0U);
  if (entity)
    appendEntity(bytes, *entity);
}

bool readOptionalEntity(Reader &reader,
                        std::optional<PublicEntityReferenceArtifact> &entity) {
  std::uint8_t present = 0;
  if (!reader.u8(present) || present > 1)
    return false;
  if (!present) {
    entity.reset();
    return true;
  }
  entity.emplace();
  return reader.entity(*entity);
}

void appendObjectProjection(std::string &bytes,
                            const ObjectFieldProjectionArtifact &projection) {
  appendU8(bytes, static_cast<std::uint8_t>(projection.kind));
  appendU32(bytes, static_cast<std::uint32_t>(projection.field_indices.size()));
  for (const auto index : projection.field_indices)
    appendU32(bytes, index);
  appendU32(bytes,
            static_cast<std::uint32_t>(projection.region_indices.size()));
  for (const auto index : projection.region_indices)
    appendU32(bytes, index);
  appendU32(bytes, projection.bit_begin);
  appendU32(bytes, projection.bit_end);
  appendU32(bytes, projection.capabilities);
  appendOptionalEntity(bytes, projection.load_target);
  appendOptionalEntity(bytes, projection.store_target);
  appendOptionalEntity(bytes, projection.take_target);
  appendOptionalEntity(bytes, projection.init_target);
  appendOptionalEntity(bytes, projection.borrow_target);
  appendOptionalEntity(bytes, projection.borrow_mut_target);
}

bool readObjectProjection(Reader &reader,
                          ObjectFieldProjectionArtifact &projection) {
  std::uint8_t kind = 0;
  std::uint32_t count = 0;
  std::uint32_t capabilities = 0;
  if (!reader.u8(kind) || !reader.u32(count) || !reader.records(count, 4))
    return false;
  projection.kind = static_cast<ObjectFieldProjectionKind>(kind);
  projection.field_indices.resize(count);
  for (auto &index : projection.field_indices)
    if (!reader.u32(index))
      return false;
  if (!reader.u32(count) || !reader.records(count, 4))
    return false;
  projection.region_indices.resize(count);
  for (auto &index : projection.region_indices)
    if (!reader.u32(index))
      return false;
  if (!reader.u32(projection.bit_begin) || !reader.u32(projection.bit_end) ||
      !reader.u32(capabilities) ||
      capabilities > std::numeric_limits<std::uint16_t>::max())
    return false;
  projection.capabilities = static_cast<std::uint16_t>(capabilities);
  return readOptionalEntity(reader, projection.load_target) &&
         readOptionalEntity(reader, projection.store_target) &&
         readOptionalEntity(reader, projection.take_target) &&
         readOptionalEntity(reader, projection.init_target) &&
         readOptionalEntity(reader, projection.borrow_target) &&
         readOptionalEntity(reader, projection.borrow_mut_target);
}

void appendLifecycleBody(std::string &bytes,
                         std::span<const LifecycleBodyOp> body) {
  appendU32(bytes, static_cast<std::uint32_t>(body.size()));
  for (const auto &op : body) {
    appendU8(bytes, static_cast<std::uint8_t>(op.kind));
    appendU32(bytes, op.field_index);
    appendFingerprint(bytes, op.child_witness);
  }
}

bool readLifecycleBody(Reader &reader, std::vector<LifecycleBodyOp> &body) {
  std::uint32_t count = 0;
  if (!reader.u32(count) || !reader.records(count, 37))
    return false;
  body.resize(count);
  for (auto &op : body) {
    std::uint8_t kind = 0;
    if (!reader.u8(kind) || !reader.u32(op.field_index) ||
        !reader.fingerprint(op.child_witness))
      return false;
    op.kind = static_cast<LifecycleBodyOpKind>(kind);
  }
  return true;
}

StableFingerprint semanticWitnessRequestFingerprint(
    const NominalSemanticWitnessArtifact &witness) {
  std::string bytes("chtholly.next.nominal-semantic-witness-request.v11");
  appendEntity(bytes, witness.nominal_template);
  appendU32(bytes, static_cast<std::uint32_t>(witness.arguments.size()));
  for (const auto &argument : witness.arguments)
    appendType(bytes, argument);
  appendFingerprint(bytes, witness.semantic_options_fingerprint);
  appendFingerprint(bytes, witness.structural_specific_fingerprint);
  appendU8(bytes, static_cast<std::uint8_t>(witness.kind));
  appendU32(bytes, static_cast<std::uint32_t>(
                       witness.transitive_specific_fingerprints.size()));
  for (const auto &fingerprint : witness.transitive_specific_fingerprints)
    appendFingerprint(bytes, fingerprint);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

StableFingerprint semanticWitnessResultFingerprint(
    const NominalSemanticWitnessArtifact &witness) {
  std::string bytes("chtholly.next.nominal-semantic-witness-result.v12");
  appendFingerprint(bytes, witness.request_fingerprint);
  appendU8(bytes, static_cast<std::uint8_t>(witness.representation.value_repr));
  appendU8(bytes, static_cast<std::uint8_t>(witness.representation.init_repr));
  appendU8(bytes, static_cast<std::uint8_t>(witness.representation.ownership));
  appendU8(bytes, static_cast<std::uint8_t>(witness.representation.copy));
  appendU8(bytes, static_cast<std::uint8_t>(witness.representation.move));
  appendU8(bytes, static_cast<std::uint8_t>(witness.representation.destroy));
  appendU8(bytes,
           static_cast<std::uint8_t>(witness.representation.object_repr));
  appendU8(bytes, witness.concurrency.transferable ? 1U : 0U);
  appendU8(bytes, witness.concurrency.shareable ? 1U : 0U);
  appendOptionalEntity(bytes, witness.copy_target);
  appendOptionalEntity(bytes, witness.destroy_target);
  appendOptionalEntity(bytes, witness.pack_target);
  appendOptionalEntity(bytes, witness.init_target);
  appendU8(bytes, witness.value_repr_carrier.has_value() ? 1U : 0U);
  if (witness.value_repr_carrier)
    appendType(bytes, *witness.value_repr_carrier);
  appendU8(bytes, witness.object_repr_carrier.has_value() ? 1U : 0U);
  if (witness.object_repr_carrier)
    appendType(bytes, *witness.object_repr_carrier);
  appendU32(bytes, static_cast<std::uint32_t>(
                       witness.object_field_projections.size()));
  for (const auto &projection : witness.object_field_projections)
    appendObjectProjection(bytes, projection);
  appendOptionalEntity(bytes, witness.object_init_target);
  appendOptionalEntity(bytes, witness.object_copy_init_target);
  appendOptionalEntity(bytes, witness.object_move_init_target);
  appendOptionalEntity(bytes, witness.object_drop_target);
  appendLifecycleBody(bytes, witness.copy_body);
  appendLifecycleBody(bytes, witness.drop_body);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

StableFingerprint
specificResultFingerprint(const NominalTypeSpecificArtifact &specific) {
  std::string bytes("chtholly.next.nominal-specific-result.v13");
  appendFingerprint(bytes, specific.request_fingerprint);
  appendEntity(bytes, specific.template_entity);
  appendU32(bytes, static_cast<std::uint32_t>(specific.arguments.size()));
  for (const auto &argument : specific.arguments)
    appendType(bytes, argument);
  appendFingerprint(bytes, specific.semantic_options_fingerprint);
  appendU8(bytes, static_cast<std::uint8_t>(specific.kind));
  appendFields(bytes, specific.fields);
  appendVariants(bytes, specific.variants);
  appendU8(bytes, specific.is_value_enum ? 1U : 0U);
  appendU8(bytes, specific.object_repr_carrier.has_value() ? 1U : 0U);
  if (specific.object_repr_carrier)
    appendType(bytes, *specific.object_repr_carrier);
  appendU32(bytes, static_cast<std::uint32_t>(
                       specific.child_specific_fingerprints.size()));
  for (const auto &fingerprint : specific.child_specific_fingerprints)
    appendFingerprint(bytes, fingerprint);
  appendFingerprint(bytes, specific.structural_fingerprint);
  appendFingerprint(bytes,
                    specific.nominal_semantic_witness.result_fingerprint);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

StableFingerprint
layoutRequestFingerprint(const NominalTypeLayoutArtifact &layout) {
  std::string bytes("chtholly.next.nominal-layout-request.v7");
  appendFingerprint(bytes, layout.type_specific_fingerprint);
  appendFingerprint(bytes, layout.target_fingerprint);
  appendU8(bytes, static_cast<std::uint8_t>(layout.kind));
  return StableFingerprint::fromCanonicalBytes(bytes);
}

StableFingerprint
layoutResultFingerprint(const NominalTypeLayoutArtifact &layout) {
  std::string bytes("chtholly.next.nominal-layout-result.v9");
  appendFingerprint(bytes, layout.request_fingerprint);
  appendFingerprint(bytes, layout.type_specific_fingerprint);
  appendFingerprint(bytes, layout.target_fingerprint);
  appendU8(bytes, static_cast<std::uint8_t>(layout.kind));
  appendU64(bytes, layout.size);
  appendU64(bytes, layout.alignment);
  appendU32(bytes, static_cast<std::uint32_t>(layout.fields.size()));
  for (const auto &field : layout.fields) {
    appendField(bytes, field.name);
    appendType(bytes, field.type);
    appendU8(bytes, static_cast<std::uint8_t>(field.kind));
    appendU8(bytes, field.storage_type.has_value() ? 1U : 0U);
    if (field.storage_type)
      appendType(bytes, *field.storage_type);
    appendU64(bytes, field.offset);
    appendU64(bytes, field.size);
    appendU64(bytes, field.alignment);
    appendU32(bytes, field.bit_begin);
    appendU32(bytes, field.bit_end);
  }
  appendU32(bytes, layout.tag_size);
  appendU64(bytes, layout.payload_offset);
  appendU32(bytes, static_cast<std::uint32_t>(layout.variants.size()));
  for (const auto &variant : layout.variants) {
    appendField(bytes, variant.name);
    appendU64(bytes, variant.size);
    appendU64(bytes, variant.alignment);
    appendU32(bytes, static_cast<std::uint32_t>(variant.fields.size()));
    for (const auto &field : variant.fields) {
      appendField(bytes, field.name);
      appendType(bytes, field.type);
      appendU64(bytes, field.offset);
      appendU64(bytes, field.size);
      appendU64(bytes, field.alignment);
    }
  }
  return StableFingerprint::fromCanonicalBytes(bytes);
}

std::optional<PublicType> substituteType(const PublicType &type,
                                         std::span<const PublicType> arguments,
                                         std::string &error) {
  if (type.kind == PublicTypeKind::TypeParameter) {
    if (type.binding_index >= arguments.size()) {
      error = "nominal type argument index is out of range";
      return std::nullopt;
    }
    return arguments[type.binding_index];
  }
  PublicType result = type;
  for (auto &argument : result.arguments) {
    auto substituted = substituteType(argument, arguments, error);
    if (!substituted)
      return std::nullopt;
    argument = std::move(*substituted);
  }
  return result;
}

bool checkedAlign(std::uint64_t value, std::uint64_t alignment,
                  std::uint64_t &result) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0)
    return false;
  const auto mask = alignment - 1;
  if (value > std::numeric_limits<std::uint64_t>::max() - mask)
    return false;
  result = (value + mask) & ~mask;
  return true;
}

bool hasParameterProvenance(const PublicType &type) {
  if (type.kind == PublicTypeKind::Reference &&
      type.reference_provenance.kind ==
          PublicReferenceProvenanceKind::Parameter)
    return true;
  return std::ranges::any_of(type.arguments, hasParameterProvenance);
}
