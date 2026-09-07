// Included by CFDL.cpp; this fragment aliases the existing CFDL parser,
// validation, rendering, and SemIR state without duplicating ownership.
std::string qualifierName(CFDLFlowQualifier qualifier) {
  switch (qualifier) {
  case CFDLFlowQualifier::Value:
    return {};
  case CFDLFlowQualifier::Owned:
    return "owned ";
  case CFDLFlowQualifier::Ref:
    return "ref ";
  case CFDLFlowQualifier::RefMut:
    return "ref_mut ";
  case CFDLFlowQualifier::View:
    return "view ";
  case CFDLFlowQualifier::ViewMut:
    return "view_mut ";
  case CFDLFlowQualifier::Move:
    return "move ";
  case CFDLFlowQualifier::Out:
    return "out ";
  case CFDLFlowQualifier::InOut:
    return "inout ";
  case CFDLFlowQualifier::Count:
    return "";
  }
  return {};
}

std::string whereFactName(const CFDLWhereFactSyntax &fact) {
  std::ostringstream out;
  out << fact.subject << ' ';
  switch (fact.kind) {
  case CFDLWhereFactKind::Escapes:
    out << "escapes " << fact.target;
    break;
  case CFDLWhereFactKind::Stores:
    out << "stores " << fact.target;
    break;
  case CFDLWhereFactKind::Derives:
    out << "derives " << fact.target;
    break;
  case CFDLWhereFactKind::Invokes:
    out << "invokes " << fact.target;
    break;
  case CFDLWhereFactKind::Obliges:
    out << "obliges " << fact.action;
    break;
  case CFDLWhereFactKind::Discharges:
    out << "discharges " << fact.action;
    break;
  case CFDLWhereFactKind::Requires:
    out << "requires " << fact.predicate;
    break;
  case CFDLWhereFactKind::Count:
    out << "<invalid>";
    break;
  }
  return out.str();
}

std::string errorContractName(const CFDLErrorContractSyntax &contract) {
  std::ostringstream out;
  out << "error ";
  switch (contract.domain) {
  case CFDLErrorDomainKind::Code:
    out << "code";
    break;
  case CFDLErrorDomainKind::Errno:
    out << "errno";
    break;
  case CFDLErrorDomainKind::Win32:
    out << "win32";
    break;
  case CFDLErrorDomainKind::None:
  case CFDLErrorDomainKind::Count:
    out << "<invalid>";
    break;
  }
  out << " when " << contract.observed << ' ';
  const auto render_value = [&](const CFDLErrorValueSyntax &value) {
    if (!value.name.empty())
      out << value.name;
    else {
      if (value.negative)
        out << '-';
      out << value.magnitude;
    }
  };
  const auto render_scalar = [&] {
    if (contract.ranges.empty()) {
      out << "<invalid>";
      return;
    }
    render_value(contract.ranges.front().lower);
  };
  switch (contract.predicate) {
  case CFDLErrorPredicateKind::Equal:
    out << "== ";
    render_scalar();
    break;
  case CFDLErrorPredicateKind::NotEqual:
    out << "!= ";
    render_scalar();
    break;
  case CFDLErrorPredicateKind::Less:
    out << "< ";
    render_scalar();
    break;
  case CFDLErrorPredicateKind::Null:
    out << "== null";
    break;
  case CFDLErrorPredicateKind::Invalid:
    out << "== invalid";
    break;
  case CFDLErrorPredicateKind::InSet:
  case CFDLErrorPredicateKind::NotInSet:
    out << (contract.predicate == CFDLErrorPredicateKind::InSet ? "in { "
                                                                : "not in { ");
    for (std::size_t index = 0; index < contract.ranges.size(); ++index) {
      if (index != 0)
        out << ", ";
      render_value(contract.ranges[index].lower);
      if (contract.ranges[index].upper) {
        out << " through ";
        render_value(*contract.ranges[index].upper);
      }
    }
    out << " }";
    break;
  case CFDLErrorPredicateKind::None:
  case CFDLErrorPredicateKind::Count:
    out << "<invalid>";
    break;
  }
  return out.str();
}

std::string printCFDLSyntaxImpl(const CFDLSyntaxFile &file) {
  std::ostringstream out;
  out << "CFDLFile module=" << file.module_name << '\n';
  for (const auto &imported : file.imports)
    out << "  Import " << imported << '\n';
  for (const auto &type : file.foreign_types) {
    out << "  ForeignType " << type.name;
    if (type.carrier_kind == CFDLForeignCarrierKind::Scalar)
      out << " carrier=" << type.scalar_carrier;
    else if (type.carrier_kind == CFDLForeignCarrierKind::Record)
      out << " fields=" << type.fields.size();
    else if (type.carrier_kind == CFDLForeignCarrierKind::Union)
      out << " union_members=" << type.fields.size();
    else if (type.carrier_kind == CFDLForeignCarrierKind::Enum)
      out << " enum_carrier=" << type.scalar_carrier
          << " constants=" << type.enum_constants.size();
    if (type.invalid_kind == CFDLForeignInvalidKind::Null)
      out << " invalid=null";
    else if (type.invalid_kind == CFDLForeignInvalidKind::Integer)
      out << " invalid=" << type.invalid_integer;
    out << '\n';
  }
  for (const auto &constant : file.foreign_constants) {
    out << "  ForeignConstant " << constant.name
        << " type=" << constant.physical_type << " value=";
    if (constant.kind == CFDLForeignConstantKind::Bool)
      out << (constant.bool_value ? "true" : "false");
    else {
      if (constant.integer_negative)
        out << '-';
      out << constant.integer_payload;
    }
    out << '\n';
  }
  for (const auto &callable : file.callables) {
    out << "  ForeignFunction " << callable.name
        << " parameters=" << callable.parameters.size()
        << " result=" << qualifierName(callable.result.qualifier)
        << callable.result.physical_type;
    if (!callable.external_symbol.empty())
      out << " link=" << callable.external_symbol;
    if (callable.explicit_calling_convention)
      out << " call=" << static_cast<unsigned>(callable.calling_convention);
    if (!callable.where_facts.empty() || callable.error_contract) {
      out << " where=";
      for (std::size_t index = 0; index < callable.where_facts.size();
           ++index) {
        if (index != 0)
          out << ",";
        out << whereFactName(callable.where_facts[index]);
      }
      if (callable.error_contract) {
        if (!callable.where_facts.empty())
          out << ',';
        out << errorContractName(*callable.error_contract);
      }
    }
    out << '\n';
  }
  return out.str();
}

std::vector<std::string>
cfdlCompletionCandidatesImpl(const CFDLSyntaxFile &file) {
  std::vector<std::string> result = {
      "foreign",     "type",    "const",       "fn",           "owned",
      "ref",         "ref_mut", "view",        "view_mut",     "move",
      "out",         "inout",   "where",       "escapes",      "stores",
      "derives",     "invokes", "obliges",     "discharges",   "requires",
      "call_return", "valid",   "initialized", "quiescent",    "same_thread",
      "invalid",     "null",    "c_bool",      "c_char",       "c_schar",
      "c_uchar",     "c_short", "c_ushort",    "c_int",        "c_uint",
      "c_long",      "c_ulong", "c_long_long", "c_ulong_long", "c_size",
      "c_ptrdiff",   "c_wchar", "array",       "enum",         "union",
      "link",        "call",    "c",           "win64",        "sysv64",
      "error",       "code",    "errno",       "win32",        "when",
      "result",      "outcome", "posix_read",  "win32_read",   "fread",
      "eof",         "ferror",  "in",          "not",          "through",
      "true",        "false"};
  for (const auto &type : file.foreign_types)
    result.push_back(type.name);
  for (const auto &imported : file.imports)
    result.push_back(imported);
  for (const auto &callable : file.callables) {
    result.push_back(callable.name);
    for (const auto &parameter : callable.parameters)
      result.push_back(parameter.name);
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

ResourceFlowCategory flowCategoryFor(const CFDLCallableSyntax &callable) {
  bool has_resource = callable.result.qualifier == CFDLFlowQualifier::Owned;
  bool has_memory = callable.result.qualifier == CFDLFlowQualifier::View ||
                    callable.result.qualifier == CFDLFlowQualifier::ViewMut;
  for (const auto &parameter : callable.parameters) {
    has_resource |= parameter.type.qualifier == CFDLFlowQualifier::Ref ||
                    parameter.type.qualifier == CFDLFlowQualifier::RefMut ||
                    parameter.type.qualifier == CFDLFlowQualifier::Move;
    has_memory |= parameter.type.qualifier == CFDLFlowQualifier::View ||
                  parameter.type.qualifier == CFDLFlowQualifier::ViewMut;
  }
  for (const auto &fact : callable.where_facts)
    has_resource |= fact.kind != CFDLWhereFactKind::Requires;
  if (has_resource)
    return ResourceFlowCategory::Resource;
  if (has_memory)
    return ResourceFlowCategory::Projection;
  return ResourceFlowCategory::Value;
}

ResourceFlowPlace placeFor(const CFDLCallableSyntax &callable,
                           std::string_view name) {
  if (name == "result")
    return {.kind = ResourceFlowPlaceKind::Result, .index = 0};
  for (std::size_t index = 0; index < callable.parameters.size(); ++index)
    if (callable.parameters[index].name == name)
      return {.kind = ResourceFlowPlaceKind::Parameter,
              .index = static_cast<std::uint32_t>(index)};
  return {};
}

ResourceFlowEndpoint endpointFor(const CFDLCallableSyntax &callable,
                                 std::string_view name) {
  if (name == "call_return")
    return {.kind = ResourceFlowEndpointKind::CallReturn, .index = 0};
  std::uint32_t event_index = 0;
  for (const auto &where : callable.where_facts) {
    if (where.kind != CFDLWhereFactKind::Invokes)
      continue;
    if (name == where.target)
      return {.kind = ResourceFlowEndpointKind::Event, .index = event_index};
    ++event_index;
  }
  return {};
}

ResourceFlowAction actionFor(const std::vector<std::string> &actions,
                             std::string_view name) {
  for (std::size_t index = 0; index < actions.size(); ++index)
    if (actions[index] == name)
      return {.index = static_cast<std::uint32_t>(index)};
  return {};
}

std::optional<ResourceFlowPredicate> predicateFor(std::string_view name) {
  if (name == "valid")
    return ResourceFlowPredicate::Valid;
  if (name == "initialized")
    return ResourceFlowPredicate::Initialized;
  if (name == "quiescent")
    return ResourceFlowPredicate::Quiescent;
  if (name == "same_thread")
    return ResourceFlowPredicate::SameThread;
  return std::nullopt;
}

ResourceFlowFact flowFact(ResourceFlowCategory category,
                          ResourceFlowRelationKind relation,
                          ResourceFlowPlace subject) {
  ResourceFlowFact fact;
  fact.category = category;
  fact.relation = relation;
  fact.subject = subject;
  return fact;
}

std::string protocolResourceName(const CFDLSyntaxFile &file,
                                 const CFDLCallableSyntax &callable) {
  std::vector<std::string_view> spellings;
  spellings.push_back(callable.result.physical_type);
  for (const auto &parameter : callable.parameters)
    spellings.push_back(parameter.type.physical_type);
  for (const auto &foreign_type : file.foreign_types) {
    for (auto spelling : spellings) {
      if (spelling.find(foreign_type.name) != std::string_view::npos)
        return foreign_type.name;
    }
  }
  return "anonymous";
}

StableFingerprint protocolFingerprint(interop::ForeignProtocolIdentityKind kind,
                                      std::string_view package,
                                      std::string_view module,
                                      std::string_view resource,
                                      std::string_view name,
                                      std::string_view owner) {
  std::string canonical = "CFDL-PROTOCOL-ID\0";
  canonical.push_back(static_cast<char>(kind));
  for (const auto value : {package, module, resource, name, owner}) {
    const auto size = static_cast<std::uint32_t>(value.size());
    for (unsigned shift = 0; shift != 32; shift += 8)
      canonical.push_back(static_cast<char>((size >> shift) & 0xffU));
    canonical.append(value);
  }
  return StableFingerprint::fromCanonicalBytes(canonical);
}

ResourceFlowDeclaration
makeResourceFlowDeclaration(const CFDLCallableSyntax &callable,
                            CanonicalTypeId callable_type) {
  ResourceFlowDeclaration declaration;
  declaration.callable_name = callable.name;
  declaration.callable_type = callable_type;
  for (const auto &parameter : callable.parameters)
    declaration.parameter_names.push_back(parameter.name);
  for (const auto &where : callable.where_facts) {
    if (where.kind == CFDLWhereFactKind::Invokes &&
        where.target != "call_return")
      declaration.event_names.push_back(where.target);
    if ((where.kind == CFDLWhereFactKind::Obliges ||
         where.kind == CFDLWhereFactKind::Discharges) &&
        std::ranges::find(declaration.action_names, where.action) ==
            declaration.action_names.end())
      declaration.action_names.push_back(where.action);
    if (where.kind == CFDLWhereFactKind::Requires &&
        std::ranges::find(declaration.action_names, where.subject) ==
            declaration.action_names.end())
      declaration.action_names.push_back(where.subject);
  }
  const auto category = flowCategoryFor(callable);
  for (const auto &parameter : callable.parameters) {
    switch (parameter.type.qualifier) {
    case CFDLFlowQualifier::Ref:
    case CFDLFlowQualifier::View:
      declaration.facts.push_back(flowFact(category,
                                           ResourceFlowRelationKind::LoanBorrow,
                                           placeFor(callable, parameter.name)));
      break;
    case CFDLFlowQualifier::RefMut:
    case CFDLFlowQualifier::ViewMut:
    case CFDLFlowQualifier::InOut:
      declaration.facts.push_back(flowFact(category,
                                           ResourceFlowRelationKind::LoanMutate,
                                           placeFor(callable, parameter.name)));
      break;
    case CFDLFlowQualifier::Move:
      declaration.facts.push_back(
          flowFact(category, ResourceFlowRelationKind::ObligationTransfer,
                   placeFor(callable, parameter.name)));
      break;
    case CFDLFlowQualifier::Out:
      declaration.facts.push_back(
          flowFact(category, ResourceFlowRelationKind::ResourceInitialize,
                   placeFor(callable, parameter.name)));
      break;
    case CFDLFlowQualifier::Value:
    case CFDLFlowQualifier::Owned:
    case CFDLFlowQualifier::Count:
      break;
    }
  }
  if (callable.result.qualifier == CFDLFlowQualifier::Owned)
    declaration.facts.push_back(
        flowFact(category, ResourceFlowRelationKind::ObligationCreate,
                 placeFor(callable, "result")));
  for (const auto &fact : callable.where_facts) {
    switch (fact.kind) {
    case CFDLWhereFactKind::Escapes:
      declaration.facts.push_back([&] {
        auto value = flowFact(category, ResourceFlowRelationKind::LoanEscapes,
                              placeFor(callable, fact.subject));
        value.endpoint = endpointFor(callable, fact.target);
        return value;
      }());
      break;
    case CFDLWhereFactKind::Stores:
      declaration.facts.push_back([&] {
        auto value = flowFact(category, ResourceFlowRelationKind::LoanStores,
                              placeFor(callable, fact.subject));
        value.target_place = placeFor(callable, fact.target);
        return value;
      }());
      break;
    case CFDLWhereFactKind::Derives:
      declaration.facts.push_back([&] {
        auto value = flowFact(category, ResourceFlowRelationKind::Derives,
                              placeFor(callable, "result"));
        value.target_place = placeFor(callable, fact.target);
        return value;
      }());
      break;
    case CFDLWhereFactKind::Invokes:
      declaration.facts.push_back([&] {
        auto value = flowFact(category, ResourceFlowRelationKind::EventInvokes,
                              placeFor(callable, fact.subject));
        value.endpoint = endpointFor(callable, fact.target);
        return value;
      }());
      break;
    case CFDLWhereFactKind::Obliges:
    case CFDLWhereFactKind::Discharges:
      declaration.facts.push_back([&] {
        auto value =
            flowFact(category,
                     fact.kind == CFDLWhereFactKind::Obliges
                         ? ResourceFlowRelationKind::ObligationObliges
                         : ResourceFlowRelationKind::ObligationDischarges,
                     placeFor(callable, fact.subject));
        value.action = actionFor(declaration.action_names, fact.action);
        return value;
      }());
      break;
    case CFDLWhereFactKind::Requires:
      declaration.facts.push_back([&] {
        auto value = flowFact(category, ResourceFlowRelationKind::Requires,
                              ResourceFlowPlace{});
        value.subject_action =
            actionFor(declaration.action_names, fact.subject);
        value.predicate = predicateFor(fact.predicate);
        return value;
      }());
      break;
    case CFDLWhereFactKind::Count:
      break;
    }
  }
  if (declaration.facts.empty())
    declaration.facts.push_back(flowFact(category,
                                         ResourceFlowRelationKind::ValueResult,
                                         placeFor(callable, "result")));
  return declaration;
}

bool elaborateCFDLResourceFlowImpl(
    const CFDLCallableSyntax &callable,
    const CFDLBindingEnvironment &environment, ElaboratedCFDLResourceFlow &flow,
    std::vector<CFDLDiagnostic> &diagnostics) {
  flow = {};
  if (!validateCFDLCallable(callable, diagnostics))
    return false;
  flow.name = callable.name;
  flow.callable_type = environment.resolve_callable
                           ? environment.resolve_callable(callable.name)
                           : CanonicalTypeId{};
  if (!flow.callable_type.hasValue()) {
    diagnostics.push_back({CFDLDiagnostic::Kind::UnknownCallable,
                           callable.offset, 1,
                           "unresolved CFDL foreign callable"});
    return false;
  }
  flow.declaration = makeResourceFlowDeclaration(callable, flow.callable_type);
  std::string error;
  if (!normalizeResourceFlow(flow.declaration, flow.normalized, error)) {
    diagnostics.push_back({CFDLDiagnostic::Kind::InvalidProtocol,
                           callable.offset, 1, std::move(error)});
    return false;
  }
  return true;
}

