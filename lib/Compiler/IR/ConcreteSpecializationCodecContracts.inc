// Included by ConcreteSpecializationSupportInternal.h; this fragment keeps
// concrete-specialization codec and verification state in the private scope.
bool concreteType(PublicType type, bool allow_void) {
  if (type.binding_index != core::AnyId::InvalidIndex ||
      (type.kind != PublicTypeKind::CFunctionPointer &&
       (type.callable_variadic ||
        type.callable_context_parameter != core::AnyId::InvalidIndex ||
        type.callable_contract != CallableOwnershipSummary{})))
    return false;
  if (type.kind == PublicTypeKind::Bool ||
      type.kind == PublicTypeKind::Char ||
      type.kind == PublicTypeKind::String ||
      type.kind == PublicTypeKind::Never ||
      (allow_void && type.kind == PublicTypeKind::Void))
    return type.arguments.empty();
  if (type.kind == PublicTypeKind::Integer)
    return type.arguments.empty() &&
           (type.scalar_width == 8 || type.scalar_width == 16 ||
            type.scalar_width == 32 || type.scalar_width == 64);
  if (type.kind == PublicTypeKind::Float)
    return type.arguments.empty() &&
           (type.scalar_width == 32 || type.scalar_width == 64);
  if (type.kind == PublicTypeKind::Reference)
    return type.arguments.size() == 1 &&
           type.reference_mutability < PublicReferenceMutability::Count &&
           type.reference_provenance.kind <
               PublicReferenceProvenanceKind::Count &&
           (type.reference_provenance.kind ==
                    PublicReferenceProvenanceKind::Erased
                ? type.reference_provenance.index == core::AnyId::InvalidIndex
                : type.reference_provenance.index !=
                      core::AnyId::InvalidIndex) &&
           concreteType(type.arguments.front(), false);
  if (type.kind == PublicTypeKind::Tuple)
    return std::ranges::all_of(type.arguments, [](const PublicType &element) {
      return concreteType(element, false);
    });
  if (type.kind == PublicTypeKind::Array)
    return type.array_bound != 0 && type.arguments.size() == 1 &&
           concreteType(type.arguments.front(), false);
  if (type.kind == PublicTypeKind::Slice)
    return type.arguments.size() == 1 &&
           concreteType(type.arguments.front(), false);
  if (type.kind == PublicTypeKind::RawPointer)
    return type.arguments.size() == 1 &&
           concreteType(type.arguments.front(), true);
  if (type.kind == PublicTypeKind::Function) {
    if (type.arguments.empty())
      return false;
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index)
      if (!concreteType(type.arguments[index], false))
        return false;
    return concreteType(type.arguments.back(), true);
  }
  if (type.kind == PublicTypeKind::CFunctionPointer) {
    if (type.arguments.empty())
      return false;
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index)
      if (!concreteType(type.arguments[index], false))
        return false;
    std::string error;
    const auto parameter_count =
        static_cast<std::uint32_t>(type.arguments.size() - 1);
    return (!type.callable_variadic ||
            type.callable_context_parameter == core::AnyId::InvalidIndex) &&
           (type.callable_context_parameter == core::AnyId::InvalidIndex ||
            type.callable_context_parameter < parameter_count) &&
           concreteType(type.arguments.back(), true) &&
           type.callable_contract.verify(parameter_count, error);
  }
  if (type.kind == PublicTypeKind::CallbackAdapter)
    return validCallbackAdapterContract(type) &&
           std::ranges::all_of(type.arguments, [](const PublicType &field) {
             return concreteType(field, false);
           });
  if (type.kind == PublicTypeKind::CallbackRegistration)
    return validCallbackRegistrationContract(type) &&
           std::ranges::all_of(type.arguments, [](const PublicType &field) {
             return concreteType(field, false);
           });
  if (type.kind == PublicTypeKind::CallbackCompletion)
    return validCallbackCompletionContract(type) &&
           std::ranges::all_of(type.arguments, [](const PublicType &field) {
             return concreteType(field, false);
           });
  if (type.kind == PublicTypeKind::CallbackWake)
    return validCallbackWakeContract(type) &&
           concreteType(type.arguments[0], false);
  return type.kind == PublicTypeKind::Nominal &&
         type.nominal_entity.kind == PublicEntityKind::NominalType &&
         !type.nominal_entity.canonical_package.empty() &&
         !type.nominal_entity.canonical_module.empty() &&
         !type.nominal_entity.canonical_name.empty() &&
         type.nominal_entity.expected_fingerprint.hasValue() &&
         std::ranges::all_of(type.arguments, [](const PublicType &argument) {
           return concreteType(argument, false);
         });
}

bool containsBorrowedView(const PublicType &type) {
  if (type.kind == PublicTypeKind::Reference ||
      type.kind == PublicTypeKind::Slice)
    return true;
  if (type.kind != PublicTypeKind::Array && type.kind != PublicTypeKind::Tuple)
    return false;
  return std::ranges::any_of(type.arguments, containsBorrowedView);
}

bool validEntity(const PublicEntityReferenceArtifact &entity) {
  return entity.kind < PublicEntityKind::Count &&
         !entity.canonical_package.empty() &&
         !entity.canonical_module.empty() && !entity.canonical_name.empty() &&
         entity.expected_fingerprint.hasValue();
}

bool readSemanticContract(Reader &reader, CallableSemanticContract &contract) {
  std::uint8_t domain = 0, role = 0, whole = 0, has_bits = 0;
  std::uint32_t capability = 0, path_count = 0;
  if (!reader.u8(domain) || !reader.u8(role) || !reader.u32(capability) ||
      capability > 0xffffU || !reader.type(contract.owner) ||
      !reader.u32(contract.projector_field) || !reader.u8(whole) || whole > 1 ||
      !reader.u32(path_count) || path_count > 256 ||
      !reader.records(path_count, 4))
    return false;
  contract.domain = static_cast<CallableSemanticDomain>(domain);
  contract.role = static_cast<CallableSemanticRole>(role);
  contract.capability = static_cast<std::uint16_t>(capability);
  contract.whole_carrier = whole != 0;
  contract.carrier_path.resize(path_count);
  for (auto &field : contract.carrier_path)
    if (!reader.u32(field))
      return false;
  if (!reader.u8(has_bits) || has_bits > 1 || !reader.u32(contract.bit_begin) ||
      !reader.u32(contract.bit_end))
    return false;
  contract.has_bit_range = has_bits != 0;
  std::string error;
  return contract.verify(0, error);
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

void appendRegion(std::string &out,
                  const GenericEvaluationRegionArtifact &region) {
  appendU32(out, region.entry_block);
  appendU32(out, static_cast<std::uint32_t>(region.results.size()));
  for (const auto type : region.results)
    appendType(out, type);
  appendU32(out, static_cast<std::uint32_t>(region.instructions.size()));
  for (const auto &instruction : region.instructions) {
    out.push_back(static_cast<char>(instruction.opcode));
    appendType(out, instruction.type);
    appendU32(out, instruction.arg0);
    appendU32(out, instruction.arg1);
  }
  const auto append_blocks = [&](const auto &blocks) {
    appendU32(out, static_cast<std::uint32_t>(blocks.size()));
    for (const auto &block : blocks) {
      appendU32(out, static_cast<std::uint32_t>(block.size()));
      for (const auto instruction : block)
        appendU32(out, instruction);
    }
  };
  append_blocks(region.blocks);
  append_blocks(region.instruction_value_blocks);
}

bool readCount(Reader &reader, std::uint32_t &count,
               std::size_t minimum_size = 1) {
  return reader.u32(count) && reader.records(count, minimum_size);
}

void appendOwnershipRegion(std::string &out, const OwnershipRegion &region) {
  appendU32(out, region.parameter_index);
  appendU32(out, static_cast<std::uint32_t>(region.path.size()));
  for (const auto step : region.path) {
    out.push_back(static_cast<char>(step.kind));
    appendU32(out, step.index);
  }
  out.push_back(region.has_bit_range ? 1 : 0);
  appendU32(out, region.bit_begin);
  appendU32(out, region.bit_end);
}

void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary) {
  appendU32(out, static_cast<std::uint32_t>(summary.effects.size()));
  for (const auto &effect : summary.effects) {
    out.push_back(static_cast<char>(effect.kind));
    appendOwnershipRegion(out, effect.region);
  }
  appendU32(out, static_cast<std::uint32_t>(summary.postconditions.size()));
  for (const auto &postcondition : summary.postconditions) {
    appendU32(out, postcondition.result_variant);
    appendOwnershipRegion(out, postcondition.region);
    out.push_back(static_cast<char>(postcondition.outcomes));
    appendU32(
        out, static_cast<std::uint32_t>(postcondition.condition.clauses.size()));
    for (const auto &clause : postcondition.condition.clauses) {
      appendU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendU32(out, atom.variant);
        appendU32(out, atom.parameter_index);
        out.push_back(atom.expected ? 1 : 0);
      }
    }
  }
  out.push_back(summary.returns_owned ? 1 : 0);
  appendU32(out, static_cast<std::uint32_t>(summary.return_provenance.size()));
  for (const auto &source : summary.return_provenance) {
    appendOwnershipRegion(out, source.region);
    appendU32(out, static_cast<std::uint32_t>(source.carrier_path.size()));
    for (const auto &step : source.carrier_path) {
      out.push_back(static_cast<char>(step.kind));
      appendU32(out, step.index);
    }
    appendU32(out, static_cast<std::uint32_t>(source.condition.clauses.size()));
    for (const auto &clause : source.condition.clauses) {
      appendU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendU32(out, atom.variant);
        appendU32(out, atom.parameter_index);
        out.push_back(atom.expected ? 1 : 0);
      }
    }
  }
}

bool readOwnershipRegion(Reader &reader, OwnershipRegion &region) {
  std::uint32_t count = 0;
  std::uint8_t bits = 0;
  if (!reader.u32(region.parameter_index) || !readCount(reader, count, 5) ||
      count > 256)
    return false;
  region.path.resize(count);
  for (auto &step : region.path) {
    std::uint8_t kind = 0;
    if (!reader.u8(kind) || !reader.u32(step.index))
      return false;
    step.kind = static_cast<OwnershipRegionStepKind>(kind);
  }
  if (!reader.u8(bits) || bits > 1 || !reader.u32(region.bit_begin) ||
      !reader.u32(region.bit_end))
    return false;
  region.has_bit_range = bits != 0;
  return true;
}

bool readOwnershipSummary(Reader &reader, CallableOwnershipSummary &summary,
                          std::uint32_t parameter_count) {
  std::uint32_t count = 0;
  if (!readCount(reader, count, 18))
    return false;
  summary.effects.resize(count);
  for (auto &effect : summary.effects) {
    std::uint8_t kind = 0;
    if (!reader.u8(kind) || !readOwnershipRegion(reader, effect.region))
      return false;
    effect.kind = static_cast<CallableEffectKind>(kind);
  }
  if (!readCount(reader, count, 18))
    return false;
  summary.postconditions.resize(count);
  for (auto &postcondition : summary.postconditions) {
    std::uint32_t clause_count = 0;
    if (!reader.u32(postcondition.result_variant) || !readOwnershipRegion(reader, postcondition.region) ||
        !reader.u8(postcondition.outcomes) || !reader.u32(clause_count) ||
        clause_count > 32 || !reader.records(clause_count, 4))
      return false;
    postcondition.condition.clauses.resize(clause_count);
    for (auto &clause : postcondition.condition.clauses) {
      std::uint32_t atom_count = 0;
      if (!reader.u32(atom_count) || atom_count > 8 ||
          !reader.records(atom_count, 9))
        return false;
      clause.atoms.resize(atom_count);
      for (auto &atom : clause.atoms) {
        std::uint8_t expected = 0;
        if (!reader.u32(atom.variant) || !reader.u32(atom.parameter_index) || !reader.u8(expected) ||
            expected > 1)
          return false;
        atom.expected = expected != 0;
      }
    }
  }
  std::uint8_t owned = 0;
  if (!reader.u8(owned) || owned > 1 || !readCount(reader, count, 17))
    return false;
  summary.returns_owned = owned != 0;
  summary.return_provenance.resize(count);
  for (auto &source : summary.return_provenance) {
    std::uint32_t clause_count = 0;
    std::uint32_t path_count = 0;
    if (!readOwnershipRegion(reader, source.region) ||
        !readCount(reader, path_count, 5) || path_count > 256)
      return false;
    source.carrier_path.resize(path_count);
    for (auto &step : source.carrier_path) {
      std::uint8_t kind = 0;
      if (!reader.u8(kind) || !reader.u32(step.index))
        return false;
      step.kind = static_cast<CallableReturnSource::CarrierStepKind>(kind);
    }
    if (!readCount(reader, clause_count, 4) || clause_count > 32)
      return false;
    source.condition.clauses.resize(clause_count);
    for (auto &clause : source.condition.clauses) {
      std::uint32_t atom_count = 0;
      if (!readCount(reader, atom_count, 9) || atom_count > 8)
        return false;
      clause.atoms.resize(atom_count);
      for (auto &atom : clause.atoms) {
        std::uint8_t expected = 0;
        if (!reader.u32(atom.variant) || !reader.u32(atom.parameter_index) || !reader.u8(expected) ||
            expected > 1)
          return false;
        atom.expected = expected != 0;
      }
    }
  }
  std::string error;
  return summary.verify(parameter_count, error);
}
