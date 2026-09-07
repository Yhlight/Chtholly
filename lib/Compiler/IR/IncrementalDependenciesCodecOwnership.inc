// Included by IncrementalDependenciesCodecInternal.h; this fragment keeps
// canonical package-manifest codec helpers in the private namespace.
bool readConstantValue(StateReader &reader, PublicConstantValue &value) {
  internal::ArtifactDecodeRecursionScope recursion(reader.context());
  std::uint8_t kind = 0;
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  std::uint8_t target_dependent = 0;
  std::uint32_t element_count = 0;
  if (!recursion.entered() || !reader.readU8(kind) ||
      kind >= static_cast<std::uint8_t>(PublicConstantValueKind::Count) ||
      !readPublicType(reader, value.type) || !reader.readU32(low) ||
      !reader.readU32(high) || !reader.readString(value.string_payload) ||
      !reader.readU8(target_dependent) || target_dependent > 1 ||
      !reader.readU32(element_count) || !reader.readRecords(element_count, 1))
    return false;
  value.kind = static_cast<PublicConstantValueKind>(kind);
  value.payload = static_cast<std::uint64_t>(low) |
                  (static_cast<std::uint64_t>(high) << 32U);
  value.target_dependent = target_dependent != 0;
  value.elements.resize(element_count);
  for (auto &element : value.elements)
    if (!readConstantValue(reader, element))
      return false;
  return true;
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

bool readOwnershipRegion(StateReader &reader, OwnershipRegion &region) {
  std::uint32_t count = 0;
  std::uint8_t has_bits = 0;
  if (!reader.readU32(region.parameter_index) || !reader.readU32(count) ||
      count > 256 || !reader.readRecords(count, 5))
    return false;
  region.path.resize(count);
  for (auto &step : region.path) {
    std::uint8_t kind = 0;
    if (!reader.readU8(kind) || !reader.readU32(step.index))
      return false;
    step.kind = static_cast<OwnershipRegionStepKind>(kind);
  }
  if (!reader.readU8(has_bits) || has_bits > 1 ||
      !reader.readU32(region.bit_begin) || !reader.readU32(region.bit_end))
    return false;
  region.has_bit_range = has_bits != 0;
  return true;
}

bool readOwnershipSummary(StateReader &reader,
                          CallableOwnershipSummary &summary,
                          std::uint32_t parameter_count) {
  std::uint32_t effect_count = 0;
  if (!reader.readU32(effect_count) || !reader.readRecords(effect_count, 18))
    return false;
  summary.effects.resize(effect_count);
  for (auto &effect : summary.effects) {
    std::uint8_t kind = 0;
    if (!reader.readU8(kind) || !readOwnershipRegion(reader, effect.region))
      return false;
    effect.kind = static_cast<CallableEffectKind>(kind);
  }
  std::uint32_t postcondition_count = 0;
  if (!reader.readU32(postcondition_count) ||
      !reader.readRecords(postcondition_count, 18))
    return false;
  summary.postconditions.resize(postcondition_count);
  for (auto &postcondition : summary.postconditions) {
    std::uint32_t clause_count = 0;
    if (!reader.readU32(postcondition.result_variant) || !readOwnershipRegion(reader, postcondition.region) ||
        !reader.readU8(postcondition.outcomes) ||
        !reader.readU32(clause_count) || clause_count > 32 ||
        !reader.readRecords(clause_count, 4))
      return false;
    postcondition.condition.clauses.resize(clause_count);
    for (auto &clause : postcondition.condition.clauses) {
      std::uint32_t atom_count = 0;
      if (!reader.readU32(atom_count) || atom_count > 8 ||
          !reader.readRecords(atom_count, 9))
        return false;
      clause.atoms.resize(atom_count);
      for (auto &atom : clause.atoms) {
        std::uint8_t expected = 0;
        if (!reader.readU32(atom.variant) || !reader.readU32(atom.parameter_index) ||
            !reader.readU8(expected) || expected > 1)
          return false;
        atom.expected = expected != 0;
      }
    }
  }
  std::uint8_t owned = 0;
  std::uint32_t return_count = 0;
  if (!reader.readU8(owned) || owned > 1 || !reader.readU32(return_count) ||
      !reader.readRecords(return_count, 17))
    return false;
  summary.returns_owned = owned != 0;
  summary.return_provenance.resize(return_count);
  for (auto &source : summary.return_provenance) {
    std::uint32_t clause_count = 0;
    std::uint32_t path_count = 0;
    if (!readOwnershipRegion(reader, source.region) ||
        !reader.readU32(path_count) || path_count > 256 ||
        !reader.readRecords(path_count, 5))
      return false;
    source.carrier_path.resize(path_count);
    for (auto &step : source.carrier_path) {
      std::uint8_t kind = 0;
      if (!reader.readU8(kind) || !reader.readU32(step.index))
        return false;
      step.kind = static_cast<CallableReturnSource::CarrierStepKind>(kind);
    }
    if (!reader.readU32(clause_count) || clause_count > 32 ||
        !reader.readRecords(clause_count, 4))
      return false;
    source.condition.clauses.resize(clause_count);
    for (auto &clause : source.condition.clauses) {
      std::uint32_t atom_count = 0;
      if (!reader.readU32(atom_count) || atom_count > 8 ||
          !reader.readRecords(atom_count, 9))
        return false;
      clause.atoms.resize(atom_count);
      for (auto &atom : clause.atoms) {
        std::uint8_t expected = 0;
        if (!reader.readU32(atom.variant) || !reader.readU32(atom.parameter_index) || !reader.readU8(expected) ||
            expected > 1)
          return false;
        atom.expected = expected != 0;
      }
    }
  }
  std::string error;
  return summary.verify(parameter_count, error);
}
