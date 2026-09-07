// Included by CFDL.cpp; this fragment aliases the existing CFDL parser,
// validation, rendering, and SemIR state without duplicating ownership.
bool elaborateCFDLResourceFlow(
    const CFDLCallableSyntax &callable,
    const CFDLBindingEnvironment &environment, ElaboratedCFDLResourceFlow &flow,
    std::vector<CFDLDiagnostic> &diagnostics) {
  return elaborateCFDLResourceFlowImpl(callable, environment, flow,
                                       diagnostics);
}

std::string printCFDLSyntax(const CFDLSyntaxFile &file) {
  return printCFDLSyntaxImpl(file);
}

std::string renderCFDLForeignType(const CFDLForeignTypeSyntax &type) {
  std::ostringstream out;
  switch (type.carrier_kind) {
  case CFDLForeignCarrierKind::Incomplete:
    out << "foreign type " << type.name;
    break;
  case CFDLForeignCarrierKind::Scalar:
    out << "foreign type " << type.name << ": " << type.scalar_carrier;
    break;
  case CFDLForeignCarrierKind::Record:
    out << "foreign struct " << type.name;
    out << " {\n";
    for (const auto &field : type.fields)
      out << "  " << field.name << ": " << field.physical_type << ";\n";
    out << '}';
    break;
  case CFDLForeignCarrierKind::Union:
    out << "foreign union " << type.name;
    out << " {\n";
    for (const auto &field : type.fields)
      out << "  " << field.name << ": " << field.physical_type << ";\n";
    out << '}';
    break;
  case CFDLForeignCarrierKind::Enum:
    out << "foreign enum " << type.name << ": " << type.scalar_carrier
        << " {\n";
    for (const auto &constant : type.enum_constants)
      out << "  " << constant.name << " = " << constant.value << ";\n";
    out << '}';
    break;
  case CFDLForeignCarrierKind::Count:
    break;
  }
  if (type.invalid_kind == CFDLForeignInvalidKind::Null)
    out << " invalid null";
  else if (type.invalid_kind == CFDLForeignInvalidKind::Integer)
    out << " invalid " << type.invalid_integer;
  out << ';';
  return out.str();
}

std::string renderCFDLCallable(const CFDLCallableSyntax &callable) {
  std::ostringstream out;
  out << "foreign fn " << callable.name << '(';
  for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
    if (index != 0)
      out << ", ";
    const auto &parameter = callable.parameters[index];
    out << parameter.name << ": " << qualifierName(parameter.type.qualifier)
        << parameter.type.physical_type;
  }
  out << ") -> " << qualifierName(callable.result.qualifier)
      << callable.result.physical_type;
  if (!callable.external_symbol.empty()) {
    out << " link \"";
    for (const char ch : callable.external_symbol) {
      if (ch == '\\' || ch == '"')
        out << '\\';
      out << ch;
    }
    out << '"';
  }
  if (callable.explicit_calling_convention) {
    out << " call ";
    switch (callable.calling_convention) {
    case ForeignCallingConvention::C:
      out << 'c';
      break;
    case ForeignCallingConvention::Win64:
      out << "win64";
      break;
    case ForeignCallingConvention::SysV64:
      out << "sysv64";
      break;
    case ForeignCallingConvention::Count:
      out << "<invalid>";
      break;
    }
  }
  if (callable.outcome_contract) {
    out << " outcome ";
    switch (callable.outcome_contract->kind) {
    case CFDLOutcomeKind::PosixRead:
      out << "posix_read<" << callable.outcome_contract->element_type << ">("
          << callable.outcome_contract->buffer << ", "
          << callable.outcome_contract->capacity << ')';
      break;
    case CFDLOutcomeKind::Win32Read:
      out << "win32_read<" << callable.outcome_contract->element_type << ">("
          << callable.outcome_contract->buffer << ", "
          << callable.outcome_contract->capacity << ", "
          << callable.outcome_contract->count << ", "
          << callable.outcome_contract->context << ')';
      break;
    case CFDLOutcomeKind::Fread:
      out << "fread<" << callable.outcome_contract->element_type << ">("
          << callable.outcome_contract->buffer << ", "
          << callable.outcome_contract->element_size << ", "
          << callable.outcome_contract->element_count << ", "
          << callable.outcome_contract->stream << ')';
      break;
    case CFDLOutcomeKind::None:
    case CFDLOutcomeKind::Count:
      out << "<invalid>";
      break;
    }
    if (callable.outcome_contract->kind == CFDLOutcomeKind::Fread) {
      out << " eof \"" << callable.outcome_contract->eof_symbol << "\"("
          << callable.outcome_contract->stream << ") ferror \""
          << callable.outcome_contract->ferror_symbol << "\"("
          << callable.outcome_contract->stream << ')';
    }
  }
  if (callable.error_contract)
    out << ' ' << errorContractName(*callable.error_contract);
  if (!callable.where_facts.empty()) {
    out << " where ";
    for (std::size_t index = 0; index < callable.where_facts.size(); ++index) {
      if (index != 0)
        out << ", ";
      out << whereFactName(callable.where_facts[index]);
    }
  }
  out << ';';
  return out.str();
}

std::string
renderCFDLForeignConstant(const CFDLForeignConstantSyntax &constant) {
  std::ostringstream out;
  out << "foreign const " << constant.name << ": " << constant.physical_type
      << " = ";
  if (constant.kind == CFDLForeignConstantKind::Bool)
    out << (constant.bool_value ? "true" : "false");
  else {
    if (constant.integer_negative)
      out << '-';
    out << constant.integer_payload;
  }
  out << ';';
  return out.str();
}

std::string renderCFDLSource(const CFDLSyntaxFile &file) {
  std::ostringstream out;
  out << "module " << file.module_name << ";\n";
  for (const auto &imported : file.imports)
    out << "import " << imported << ";\n";
  if (!file.imports.empty())
    out << '\n';
  for (const auto &type : file.foreign_types)
    out << '\n' << renderCFDLForeignType(type) << '\n';
  for (const auto &constant : file.foreign_constants)
    out << '\n' << renderCFDLForeignConstant(constant) << '\n';
  for (const auto &callable : file.callables)
    out << '\n' << renderCFDLCallable(callable) << '\n';
  return out.str();
}

std::vector<std::string> cfdlCompletionCandidates(const CFDLSyntaxFile &file) {
  return cfdlCompletionCandidatesImpl(file);
}

bool validateCFDLCallable(const CFDLCallableSyntax &callable,
                          std::vector<CFDLDiagnostic> &diagnostics) {
  const auto report = [&](std::string message, std::uint32_t offset) {
    diagnostics.push_back(
        {CFDLDiagnostic::Kind::InvalidProtocol, offset, 1, std::move(message)});
  };

  std::set<std::string> names;
  for (const auto &parameter : callable.parameters) {
    if (parameter.name.empty() || !names.insert(parameter.name).second)
      report("CFDL callable has a duplicate or empty parameter name",
             parameter.offset);
  }
  if (callable.error_contract) {
    if (callable.error_contract->observed != "result" &&
        (!callable.outcome_contract ||
         callable.outcome_contract->kind != CFDLOutcomeKind::Fread ||
         callable.error_contract->observed != "ferror"))
      report("error accessor is only valid as fread `ferror`",
             callable.error_contract->offset);
    if (callable.error_contract->domain == CFDLErrorDomainKind::None ||
        callable.error_contract->domain >= CFDLErrorDomainKind::Count ||
        callable.error_contract->predicate == CFDLErrorPredicateKind::None ||
        callable.error_contract->predicate >= CFDLErrorPredicateKind::Count)
      report("foreign error contract has an invalid domain or predicate",
             callable.error_contract->offset);
    if (callable.result.qualifier != CFDLFlowQualifier::Value)
      report("foreign error contract requires a by-value result",
             callable.error_contract->offset);
    const bool status_plus_output =
        callable.error_contract->domain == CFDLErrorDomainKind::Code &&
        (callable.result.physical_type == "c_int" ||
         callable.result.physical_type == "c_long");
    if (std::ranges::any_of(callable.parameters,
                            [&](const auto &parameter) {
                              return parameter.type.qualifier ==
                                         CFDLFlowQualifier::Out ||
                                     parameter.type.qualifier ==
                                         CFDLFlowQualifier::InOut;
                            }) &&
        (!status_plus_output &&
         (!callable.outcome_contract ||
          callable.outcome_contract->kind != CFDLOutcomeKind::Win32Read ||
         std::ranges::any_of(callable.parameters, [&](const auto &parameter) {
           return (parameter.type.qualifier == CFDLFlowQualifier::Out ||
                   parameter.type.qualifier == CFDLFlowQualifier::InOut) &&
                  parameter.name != callable.outcome_contract->count;
         }))))
      report("foreign error contract does not admit out or inout parameters",
             callable.error_contract->offset);
  }
  if (callable.outcome_contract) {
    const auto win32 =
        callable.outcome_contract->kind == CFDLOutcomeKind::Win32Read;
    const auto fread =
        callable.outcome_contract->kind == CFDLOutcomeKind::Fread;
    if ((callable.outcome_contract->kind != CFDLOutcomeKind::PosixRead &&
         !win32 && !fread) ||
        callable.outcome_contract->element_type.empty() ||
        callable.outcome_contract->buffer.empty() ||
        callable.outcome_contract->capacity.empty() ||
        callable.outcome_contract->buffer ==
            callable.outcome_contract->capacity ||
        !names.contains(callable.outcome_contract->buffer) ||
        !names.contains(callable.outcome_contract->capacity) ||
        (win32 && (callable.outcome_contract->count.empty() ||
                   callable.outcome_contract->context.empty() ||
                   !names.contains(callable.outcome_contract->count) ||
                   !names.contains(callable.outcome_contract->context) ||
                   callable.outcome_contract->count ==
                       callable.outcome_contract->context)) ||
        (fread && (callable.outcome_contract->element_size.empty() ||
                   callable.outcome_contract->element_count.empty() ||
                   callable.outcome_contract->stream.empty() ||
                   callable.outcome_contract->eof_symbol.empty() ||
                   callable.outcome_contract->ferror_symbol.empty() ||
                   !names.contains(callable.outcome_contract->element_size) ||
                   !names.contains(callable.outcome_contract->element_count) ||
                   !names.contains(callable.outcome_contract->stream))))
      report("foreign outcome contract has invalid parameters",
             callable.outcome_contract->offset);
  }

  const auto isPlace = [&](std::string_view value) {
    return value == "result" || names.contains(std::string(value));
  };
  const auto isEndpoint = [&](std::string_view value) {
    return value == "call_return" ||
           (!value.empty() && value.find('.') == std::string_view::npos);
  };

  for (const auto &fact : callable.where_facts) {
    if (fact.subject.empty()) {
      report("CFDL fact has an empty subject", fact.offset);
      continue;
    }
    switch (fact.kind) {
    case CFDLWhereFactKind::Escapes:
    case CFDLWhereFactKind::Invokes:
      if (!isPlace(fact.subject) || !isEndpoint(fact.target))
        report("CFDL escape or invoke fact has an invalid place or endpoint",
               fact.offset);
      break;
    case CFDLWhereFactKind::Stores:
      if (!isPlace(fact.subject) || !isPlace(fact.target))
        report("CFDL store fact must name places", fact.offset);
      break;
    case CFDLWhereFactKind::Derives:
      if (fact.subject != "result" || !isPlace(fact.target))
        report("CFDL derive fact must derive result from a place", fact.offset);
      break;
    case CFDLWhereFactKind::Obliges:
    case CFDLWhereFactKind::Discharges:
      if (!isPlace(fact.subject) || fact.action.empty())
        report("CFDL obligation fact has an invalid place or action",
               fact.offset);
      break;
    case CFDLWhereFactKind::Requires:
      if (fact.subject.empty() || fact.predicate.empty())
        report("CFDL requirement fact has an invalid action or predicate",
               fact.offset);
      break;
    case CFDLWhereFactKind::Count:
      report("CFDL fact has an invalid relation", fact.offset);
      break;
    }
  }
  return diagnostics.empty();
}

bool validateCFDLPackageProtocol(
    std::span<const CFDLSyntaxFile *const> files,
    std::vector<CFDLPackageDiagnostic> &diagnostics) {
  diagnostics.clear();
  struct ActionState {
    std::uint32_t obligations = 0;
    std::uint32_t discharges = 0;
    std::uint32_t first_offset = 0;
    std::uint32_t first_unit = 0;
    bool requires_quiescence = false;
  };
  std::unordered_map<std::string, ActionState> actions;
  const auto report = [&](std::uint32_t unit, std::uint32_t offset,
                          std::string message) {
    diagnostics.push_back({unit,
                           {CFDLDiagnostic::Kind::InvalidProtocol, offset, 1,
                            std::move(message)}});
  };

  for (std::uint32_t unit = 0; unit < files.size(); ++unit) {
    const auto *file = files[unit];
    if (!file)
      continue;
    for (const auto &callable : file->callables) {
      std::unordered_map<std::string, std::uint32_t> callable_events;
      for (const auto &fact : callable.where_facts) {
        if (fact.kind == CFDLWhereFactKind::Invokes &&
            fact.target != "call_return") {
          const auto event = fact.target;
          const auto count = ++callable_events[event];
          if (count > 1)
            report(unit, fact.offset,
                   "CFDL event endpoint `" + event +
                       "` is declared more than once");
        }
      }
      for (const auto &fact : callable.where_facts) {
        if ((fact.kind == CFDLWhereFactKind::Escapes ||
             fact.kind == CFDLWhereFactKind::Invokes) &&
            fact.target != "call_return" &&
            !callable_events.contains(fact.target)) {
          report(unit, fact.offset,
                 "CFDL event endpoint `" + fact.target +
                     "` has no invocation declaration");
        }
        if (fact.kind != CFDLWhereFactKind::Obliges &&
            fact.kind != CFDLWhereFactKind::Discharges &&
            fact.kind != CFDLWhereFactKind::Requires)
          continue;
        const auto action = fact.kind == CFDLWhereFactKind::Requires
                                ? fact.subject
                                : fact.action;
        if (action.empty())
          continue;
        const auto separator = action.rfind("::");
        const auto action_key = separator == std::string::npos
                                    ? action
                                    : action.substr(separator + 2);
        auto &state = actions[action_key];
        if (state.obligations == 0 && state.discharges == 0 &&
            !state.requires_quiescence) {
          state.first_offset = fact.offset;
          state.first_unit = unit;
        }
        if (fact.kind == CFDLWhereFactKind::Obliges)
          ++state.obligations;
        else if (fact.kind == CFDLWhereFactKind::Discharges)
          ++state.discharges;
        else if (fact.predicate == "quiescent")
          state.requires_quiescence = true;
      }
    }
  }

  for (const auto &[action, state] : actions) {
    if (state.obligations != 0 && state.discharges != 1) {
      report(state.first_unit, state.first_offset,
             "CFDL package obligation action `" + action +
                 "` must have exactly one discharge");
    }
    if (state.requires_quiescence && state.discharges == 0) {
      report(state.first_unit, state.first_offset,
             "CFDL quiescent action `" + action +
                 "` has no discharge declaration");
    }
  }
  return diagnostics.empty();
}

bool parseCFDL(const SourceBuffer &source, CFDLSyntaxFile &file,
               std::vector<CFDLDiagnostic> &diagnostics) {
  diagnostics.clear();
  Parser parser(lexCFDL(source, diagnostics), diagnostics);
  return parser.parse(file);
}

std::string printCFDLTokens(const SourceBuffer &source) {
  std::vector<CFDLDiagnostic> diagnostics;
  const auto tokens = lexCFDL(source, diagnostics);
  std::ostringstream out;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    out << index << ' ' << static_cast<unsigned>(tokens[index].kind);
    if (!tokens[index].text.empty())
      out << " '" << tokens[index].text << '\'';
    out << '\n';
  }
  return out.str();
}
