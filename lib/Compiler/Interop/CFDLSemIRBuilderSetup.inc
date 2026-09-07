// Included inside buildCFDLSemIR; this fragment aliases the one SemIR
// builder state and preserves CFDL ABI/resource ordering.
  const auto target_is_windows =
      target_triple.find("windows") != std::string_view::npos;
  const auto target_has_c_model =
      target_is_windows ||
      target_triple.find("linux") != std::string_view::npos;
  const auto pointer_width = target_triple.starts_with("i386") ||
                                     target_triple.starts_with("i486") ||
                                     target_triple.starts_with("i586") ||
                                     target_triple.starts_with("i686") ||
                                     target_triple.starts_with("arm-")
                                 ? 32U
                                 : 64U;
  sem_ir.setTargetLayout(
      {std::string(target_triple), pointer_width, NominalLayoutAbiEpoch});
  const auto normalize_convention = [&](ForeignCallingConvention value) {
    if (value != ForeignCallingConvention::C)
      return value;
    if (target_triple.starts_with("x86_64") && target_is_windows)
      return ForeignCallingConvention::Win64;
    if (target_triple.starts_with("x86_64") &&
        target_triple.find("linux") != std::string_view::npos)
      return ForeignCallingConvention::SysV64;
    return ForeignCallingConvention::C;
  };
  const auto report = [&](CFDLDiagnostic::Kind kind, std::uint32_t offset) {
    DiagnosticKind diagnostic = DiagnosticKind::CFDLInvalidSyntax;
    switch (kind) {
    case CFDLDiagnostic::Kind::InvalidModule:
      diagnostic = DiagnosticKind::CFDLInvalidModule;
      break;
    case CFDLDiagnostic::Kind::InvalidImport:
      diagnostic = DiagnosticKind::CFDLInvalidImport;
      break;
    case CFDLDiagnostic::Kind::DuplicateName:
      diagnostic = DiagnosticKind::CFDLDuplicateName;
      break;
    case CFDLDiagnostic::Kind::UnknownType:
      diagnostic = DiagnosticKind::CFDLUnknownType;
      break;
    case CFDLDiagnostic::Kind::UnknownCallable:
      diagnostic = DiagnosticKind::CFDLUnknownCallable;
      break;
    case CFDLDiagnostic::Kind::InvalidCallable:
      diagnostic = DiagnosticKind::CFDLInvalidCallable;
      break;
    case CFDLDiagnostic::Kind::IncompleteCarrier:
      diagnostic = DiagnosticKind::CFDLIncompleteCarrier;
      break;
    case CFDLDiagnostic::Kind::InvalidCarrier:
      diagnostic = DiagnosticKind::CFDLInvalidCarrier;
      break;
    case CFDLDiagnostic::Kind::CompletionFailed:
      diagnostic = DiagnosticKind::CFDLCompletionFailed;
      break;
    case CFDLDiagnostic::Kind::InvalidProtocol:
      diagnostic = DiagnosticKind::CFDLInvalidProtocol;
      break;
    case CFDLDiagnostic::Kind::InvalidSyntax:
      break;
    }
    diagnostics.emit(diagnostic, offset);
  };

  std::unordered_map<std::string, TypeId> types;
  types.emplace("void", sem_ir.voidType());
  types.emplace("bool", sem_ir.boolType());
  for (const auto width : {8U, 16U, 32U, 64U}) {
    types.emplace("i" + std::to_string(width),
                  sem_ir.addIntegerType(width, true));
    types.emplace("u" + std::to_string(width),
                  sem_ir.addIntegerType(width, false));
  }
  types.emplace("f32", sem_ir.addFloatType(32));
  types.emplace("f64", sem_ir.addFloatType(64));
  const auto add_c_integer = [&](std::string name, std::uint32_t width,
                                 bool is_signed) {
    types.emplace(std::move(name), sem_ir.addIntegerType(width, is_signed));
  };
  if (target_has_c_model) {
    types.emplace("c_bool", sem_ir.boolType());
    add_c_integer("c_char", 8, true);
    add_c_integer("c_schar", 8, true);
    add_c_integer("c_uchar", 8, false);
    add_c_integer("c_short", 16, true);
    add_c_integer("c_ushort", 16, false);
    add_c_integer("c_int", 32, true);
    add_c_integer("c_uint", 32, false);
    add_c_integer("c_long", target_is_windows ? 32U : 64U, true);
    add_c_integer("c_ulong", target_is_windows ? 32U : 64U, false);
    add_c_integer("c_long_long", 64, true);
    add_c_integer("c_ulong_long", 64, false);
    add_c_integer("c_size", pointer_width, false);
    add_c_integer("c_ptrdiff", pointer_width, true);
    add_c_integer("c_wchar", target_is_windows ? 16U : 32U, !target_is_windows);
  }
  std::vector<NominalTypeId> foreign_nominals;
  foreign_nominals.reserve(file.foreign_types.size());
  std::set<std::string> foreign_names;
  for (const auto &foreign_type : file.foreign_types) {
    if (types.contains(foreign_type.name) ||
        !foreign_names.insert(foreign_type.name).second)
      report(CFDLDiagnostic::Kind::DuplicateName, foreign_type.offset);
    SemNominalType nominal;
    nominal.name = sem_ir.addName(values.internIdentifier(foreign_type.name));
    nominal.declaration = NodeId(foreign_type.offset);
    nominal.flags = SemNominalTypePublic;
    nominal.kind = NominalKind::ForeignHandle;
    nominal.representation_policy = NominalRepresentationPolicy::Opaque;
    nominal.completion_state = SemNominalCompletionState::Unloaded;
    const auto nominal_id = sem_ir.addNominalTypeDecl(std::move(nominal));
    foreign_nominals.push_back(nominal_id);
    types.emplace(foreign_type.name, sem_ir.addNominalType(nominal_id));
  }
  // Imported CFDL nominal types are materialized from the provider's public
  // interface. Their canonical entity and fingerprint remain provider-owned;
  // only the local TypeId is created here for use by this module.
  for (const auto &import : imports) {
    const auto *interface = public_interfaces.tryGet(import.interface_id);
    if (!interface)
      continue;
    for (const auto &nominal : interface->nominalArtifacts()) {
      if (!nominal.is_exported)
        continue;
      const auto name = nominal.entity.canonical_name;
      if (types.contains(name)) {
        report(CFDLDiagnostic::Kind::DuplicateName, 0);
        continue;
      }
      std::string materialize_error;
      PublicType public_type(nominal.entity);
      const auto type = sem_ir.materializePublicTypeForImport(
          public_type, GenericId::invalid(), NodeId(0), materialize_error);
      if (!type.hasValue()) {
        report(CFDLDiagnostic::Kind::UnknownType, 0);
        continue;
      }
      types.emplace(name, type);
    }
  }
