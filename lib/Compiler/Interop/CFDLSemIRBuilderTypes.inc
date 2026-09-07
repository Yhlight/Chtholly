// Included inside buildCFDLSemIR; this fragment aliases the one SemIR
// builder state and preserves CFDL ABI/resource ordering.
  std::function<TypeId(std::string_view, std::uint32_t)> resolve_type;
  resolve_type = [&](std::string_view spelling,
                     std::uint32_t offset) -> TypeId {
    bool pointee_const = false;
    if (spelling.starts_with("const ")) {
      pointee_const = true;
      spelling.remove_prefix(6);
    }
    if (spelling.starts_with("c_fn") &&
        spelling.find(")->") != std::string_view::npos) {
      auto convention = ForeignCallingConvention::C;
      std::size_t parameter_begin = 5;
      if (spelling.starts_with("c_fn win64(")) {
        convention = ForeignCallingConvention::Win64;
        parameter_begin = 11;
      } else if (spelling.starts_with("c_fn sysv64(")) {
        convention = ForeignCallingConvention::SysV64;
        parameter_begin = 12;
      } else if (spelling.starts_with("c_fn c(")) {
        parameter_begin = 7;
      } else if (!spelling.starts_with("c_fn(")) {
        report(CFDLDiagnostic::Kind::UnknownType, offset);
        return TypeId::invalid();
      }
      const auto split = spelling.rfind(")->");
      const auto parameter_text =
          spelling.substr(parameter_begin, split - parameter_begin);
      std::vector<TypeId> parameters;
      std::size_t begin = 0;
      std::uint32_t depth = 0;
      for (std::size_t index = 0; index <= parameter_text.size(); ++index) {
        const auto ch =
            index < parameter_text.size() ? parameter_text[index] : ',';
        if (ch == '(')
          ++depth;
        if (ch == ')')
          --depth;
        if (ch == ',' && depth == 0) {
          if (index != begin)
            parameters.push_back(resolve_type(
                parameter_text.substr(begin, index - begin), offset));
          begin = index + 1;
        }
      }
      const auto result = resolve_type(spelling.substr(split + 3), offset);
      if (!result.hasValue() ||
          std::ranges::any_of(parameters,
                              [](TypeId type) { return !type.hasValue(); }))
        return TypeId::invalid();
      return sem_ir.addCFunctionPointerType(parameters, result, false, {},
                                            core::AnyId::InvalidIndex,
                                            normalize_convention(convention));
    }
    if (spelling.ends_with('*')) {
      spelling.remove_suffix(1);
      const auto pointee = resolve_type(spelling, offset);
      return pointee.hasValue()
                 ? sem_ir.addRawPointerType(pointee, pointee_const)
                 : TypeId::invalid();
    }
    if (spelling.starts_with("array<") && spelling.ends_with('>')) {
      const auto comma = spelling.rfind(',');
      if (comma == std::string_view::npos) {
        report(CFDLDiagnostic::Kind::UnknownType, offset);
        return TypeId::invalid();
      }
      const auto element = resolve_type(spelling.substr(6, comma - 6), offset);
      std::uint64_t bound = 0;
      for (const char ch :
           spelling.substr(comma + 1, spelling.size() - comma - 2)) {
        if (ch < '0' || ch > '9' ||
            bound >
                (std::numeric_limits<std::uint32_t>::max() - (ch - '0')) / 10) {
          report(CFDLDiagnostic::Kind::UnknownType, offset);
          return TypeId::invalid();
        }
        bound = bound * 10 + (ch - '0');
      }
      return element.hasValue() && bound != 0
                 ? sem_ir.addType({SemTypeKind::Array, element.index,
                                   static_cast<std::uint32_t>(bound), 0})
                 : TypeId::invalid();
    }
    const auto found = types.find(std::string(spelling));
    if (found != types.end())
      return found->second;
    report(CFDLDiagnostic::Kind::UnknownType, offset);
    return TypeId::invalid();
  };

  std::vector<std::uint8_t> completion(file.foreign_types.size(), 0);
  std::unordered_map<std::uint32_t, std::uint32_t> nominal_to_foreign;
  for (std::uint32_t index = 0; index < foreign_nominals.size(); ++index)
    nominal_to_foreign.emplace(foreign_nominals[index].index, index);
  std::function<TypeId(TypeId, std::uint32_t)> physicalize;
  std::function<bool(std::uint32_t)> complete_foreign;
  complete_foreign = [&](std::uint32_t index) {
    if (completion[index] == 2)
      return true;
    if (completion[index] == 1) {
      report(CFDLDiagnostic::Kind::InvalidCarrier,
             file.foreign_types[index].offset);
      return false;
    }
    completion[index] = 1;
    const auto &syntax = file.foreign_types[index];
    auto nominal = sem_ir.nominalType(foreign_nominals[index]);
    TypeId representation;
    if (syntax.carrier_kind == CFDLForeignCarrierKind::Scalar) {
      representation = physicalize(
          resolve_type(syntax.scalar_carrier, syntax.offset), syntax.offset);
      if (representation.hasValue()) {
        const auto kind = sem_ir.type(representation).kind;
        if (kind != SemTypeKind::Integer && kind != SemTypeKind::RawPointer) {
          report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
          representation = TypeId::invalid();
        } else if (kind == SemTypeKind::RawPointer &&
                   sem_ir.rawPointerPointeeConst(representation)) {
          report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
          representation = TypeId::invalid();
        }
      }
    } else if (syntax.carrier_kind == CFDLForeignCarrierKind::Record ||
               syntax.carrier_kind == CFDLForeignCarrierKind::Union) {
      std::set<std::string> field_names;
      std::vector<TypeId> fields;
      for (const auto &field : syntax.fields) {
        if (!field_names.insert(field.name).second) {
          report(CFDLDiagnostic::Kind::DuplicateName, field.offset);
          continue;
        }
        auto type = physicalize(resolve_type(field.physical_type, field.offset),
                                field.offset);
        if (type.hasValue())
          fields.push_back(type);
      }
      if (fields.size() == syntax.fields.size() && !fields.empty())
        representation = syntax.carrier_kind == CFDLForeignCarrierKind::Union
                             ? sem_ir.addCUnionType(fields)
                             : sem_ir.addTupleType(fields);
      else
        report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
    } else if (syntax.carrier_kind == CFDLForeignCarrierKind::Enum) {
      representation = physicalize(
          resolve_type(syntax.scalar_carrier, syntax.offset), syntax.offset);
      if (!representation.hasValue() ||
          sem_ir.type(representation).kind != SemTypeKind::Integer ||
          syntax.enum_constants.empty()) {
        report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
        representation = TypeId::invalid();
      } else {
        std::set<std::string> constants;
        const auto &integer = sem_ir.type(representation);
        const auto width = integer.arg0;
        const auto is_signed = integer.arg1 != 0;
        for (const auto &constant : syntax.enum_constants) {
          const auto fits =
              width == 64 ? (is_signed || constant.value >= 0)
              : is_signed
                  ? constant.value >= -(std::int64_t{1} << (width - 1)) &&
                        constant.value <= (std::int64_t{1} << (width - 1)) - 1
                  : constant.value >= 0 &&
                        static_cast<std::uint64_t>(constant.value) <=
                            (std::uint64_t{1} << width) - 1;
          if (!constants.insert(constant.name).second || !fits)
            report(CFDLDiagnostic::Kind::InvalidCarrier, constant.offset);
        }
      }
    }
    if (syntax.invalid_kind != CFDLForeignInvalidKind::None) {
      if (!representation.hasValue() ||
          (syntax.invalid_kind == CFDLForeignInvalidKind::Null &&
           sem_ir.type(representation).kind != SemTypeKind::RawPointer) ||
          (syntax.invalid_kind == CFDLForeignInvalidKind::Integer &&
           sem_ir.type(representation).kind != SemTypeKind::Integer &&
           sem_ir.type(representation).kind != SemTypeKind::RawPointer)) {
        report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
      } else if (syntax.invalid_kind == CFDLForeignInvalidKind::Null) {
        nominal.foreign_invalid_state = ForeignResourceInvalidState::Null;
      } else if (sem_ir.type(representation).kind == SemTypeKind::RawPointer) {
        const auto value = syntax.invalid_integer;
        const auto fits_32 =
            value >= std::numeric_limits<std::int32_t>::min() &&
            (value < 0 || static_cast<std::uint64_t>(value) <=
                              std::numeric_limits<std::uint32_t>::max());
        const auto fits = value != 0 && (pointer_width == 64 || fits_32);
        if (!fits) {
          report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
        } else {
          nominal.foreign_invalid_state =
              ForeignResourceInvalidState::PointerBitPattern;
          nominal.foreign_invalid_integer = value;
        }
      } else {
        const auto &integer = sem_ir.type(representation);
        const auto width = integer.arg0;
        const auto is_signed = integer.arg1 != 0;
        const auto fits =
            width == 64 ? (is_signed || syntax.invalid_integer >= 0)
            : is_signed
                ? syntax.invalid_integer >= -(std::int64_t{1} << (width - 1)) &&
                      syntax.invalid_integer <=
                          (std::int64_t{1} << (width - 1)) - 1
                : syntax.invalid_integer >= 0 &&
                      static_cast<std::uint64_t>(syntax.invalid_integer) <=
                          (std::uint64_t{1} << width) - 1;
        if (!fits) {
          report(CFDLDiagnostic::Kind::InvalidCarrier, syntax.offset);
        } else {
          nominal.foreign_invalid_state = ForeignResourceInvalidState::Integer;
          nominal.foreign_invalid_integer = syntax.invalid_integer;
        }
      }
    }
    nominal.foreign_representation = representation;
    nominal.completion_state = SemNominalCompletionState::Complete;
    sem_ir.setNominalType(foreign_nominals[index], std::move(nominal));
    completion[index] = 2;
    return representation.hasValue() ||
           syntax.carrier_kind == CFDLForeignCarrierKind::Incomplete;
  };
  physicalize = [&](TypeId type, std::uint32_t offset) -> TypeId {
    if (!type.hasValue())
      return TypeId::invalid();
    const auto value = sem_ir.type(type);
    if (value.kind == SemTypeKind::Nominal) {
      const auto found = nominal_to_foreign.find(value.arg0);
      if (found != nominal_to_foreign.end() && !complete_foreign(found->second))
        return TypeId::invalid();
      const auto result = sem_ir.foreignRepresentationType(type);
      if (!result.hasValue())
        report(CFDLDiagnostic::Kind::InvalidCarrier, offset);
      return result;
    }
    if (value.kind == SemTypeKind::Array) {
      const auto element = physicalize(TypeId(value.arg0), offset);
      return element.hasValue() ? sem_ir.addType({SemTypeKind::Array,
                                                  element.index, value.arg1, 0})
                                : TypeId::invalid();
    }
    return type;
  };
  for (std::uint32_t index = 0; index < file.foreign_types.size(); ++index)
    (void)complete_foreign(index);

