// Included inside buildCFDLSemIR; this fragment aliases the one SemIR
// builder state and preserves CFDL ABI/resource ordering.
  std::set<std::string> value_names;
  struct PredicateConstant {
    std::uint64_t magnitude = 0;
    bool negative = false;
  };
  std::unordered_map<std::string, PredicateConstant> predicate_constants;
  for (std::uint32_t index = 0; index < file.foreign_types.size(); ++index) {
    const auto &syntax = file.foreign_types[index];
    if (syntax.carrier_kind != CFDLForeignCarrierKind::Enum)
      continue;
    const auto nominal_type = types.at(syntax.name);
    for (const auto &constant : syntax.enum_constants) {
      if (types.contains(constant.name) ||
          !value_names.insert(constant.name).second) {
        report(CFDLDiagnostic::Kind::DuplicateName, constant.offset);
        continue;
      }
      sem_ir.addForeignConstant(
          {sem_ir.addName(values.internIdentifier(constant.name)), nominal_type,
           PublicConstantValueKind::ForeignEnum,
           static_cast<std::uint64_t>(constant.value),
           NodeId(constant.offset)});
      predicate_constants.emplace(
          constant.name,
          PredicateConstant{
              .magnitude = constant.value < 0
                               ? std::uint64_t{0} -
                                     static_cast<std::uint64_t>(constant.value)
                               : static_cast<std::uint64_t>(constant.value),
              .negative = constant.value < 0});
    }
  }

  for (const auto &constant : file.foreign_constants) {
    if (types.contains(constant.name) ||
        !value_names.insert(constant.name).second) {
      report(CFDLDiagnostic::Kind::DuplicateName, constant.offset);
      continue;
    }
    const auto type = resolve_type(constant.physical_type, constant.offset);
    if (!type.hasValue())
      continue;
    const auto &semantic_type = sem_ir.type(type);
    PublicConstantValueKind kind = PublicConstantValueKind::Integer;
    std::uint64_t payload = constant.integer_payload;
    bool valid = true;
    if (constant.kind == CFDLForeignConstantKind::Bool) {
      valid = semantic_type.kind == SemTypeKind::Bool;
      kind = PublicConstantValueKind::Bool;
      payload = constant.bool_value ? 1U : 0U;
    } else if (semantic_type.kind == SemTypeKind::Integer) {
      const auto width = semantic_type.arg0;
      const auto is_signed = semantic_type.arg1 != 0;
      if (constant.integer_negative) {
        const auto limit =
            is_signed ? (width == 64 ? (std::uint64_t{1} << 63)
                                     : (std::uint64_t{1} << (width - 1)))
                      : 0;
        valid = constant.integer_payload <= limit && limit != 0;
        payload = std::uint64_t{0} - constant.integer_payload;
      } else if (width < 64) {
        const auto limit = is_signed ? (std::uint64_t{1} << (width - 1)) - 1
                                     : (std::uint64_t{1} << width) - 1;
        valid = constant.integer_payload <= limit;
      } else if (is_signed) {
        valid = constant.integer_payload <=
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max());
      }
    } else {
      valid = false;
    }
    if (!valid) {
      report(CFDLDiagnostic::Kind::InvalidCarrier, constant.offset);
      continue;
    }
    sem_ir.addForeignConstant(
        {sem_ir.addName(values.internIdentifier(constant.name)), type, kind,
         payload, NodeId(constant.offset)});
    if (constant.kind == CFDLForeignConstantKind::Integer)
      predicate_constants.emplace(constant.name,
                                  PredicateConstant{constant.integer_payload,
                                                    constant.integer_negative});
  }

