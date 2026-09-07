// Included by SemanticContextDeclarations.cpp; this fragment aliases the
// single SemanticContext and preserves declaration-phase ordering.
[[nodiscard]] LocalId SemanticContext::lookup(NameId name) const {
  const auto *binding = lexical_scopes_.lookup(name);
  return binding && binding->kind == SemanticBindingKind::Local
             ? LocalId(binding->target)
             : LocalId::invalid();
}

[[nodiscard]] SemanticContext::ValueBinding
SemanticContext::lookupValue(NameId name) const {
  if (const auto *binding = lexical_scopes_.lookup(name)) {
    if (binding->kind == SemanticBindingKind::Local)
      return {LocalId(binding->target), ConstantEntityId::invalid()};
    if (binding->kind == SemanticBindingKind::Constant)
      return {LocalId::invalid(), ConstantEntityId(binding->target)};
    return {};
  }
  if (const auto found = module_constant_names_.find(name.index);
      found != module_constant_names_.end())
    return {LocalId::invalid(), found->second};
  return {LocalId::invalid(), ConstantEntityId::invalid()};
}

void SemanticContext::bind(NameId name, LocalId local, NodeId node) {
  if (!lexical_scopes_.insert(name,
                              {SemanticBindingKind::Local, local.index, node}))
    emit(DiagnosticKind::DuplicateName, node);
}

void SemanticContext::bindConstant(NameId name, ConstantEntityId constant,
                                   NodeId node) {
  if (!lexical_scopes_.insert(
          name, {SemanticBindingKind::Constant, constant.index, node}))
    emit(DiagnosticKind::DuplicateName, node);
}

void SemanticContext::bindClosureCapture(NameId name, std::uint32_t field,
                                         NodeId node) {
  if (!lexical_scopes_.insert(
          name, {SemanticBindingKind::ClosureCapture, field, node}))
    emit(DiagnosticKind::DuplicateName, node);
}

[[nodiscard]] FunctionRefId SemanticContext::materializeLocalSpecific(
    FunctionRefId template_ref, SpecificId specific_id,
    std::span<const CanonicalTypeId> type_arguments, NodeId call_node,
    bool declaration_only, bool enqueue_definition,
    std::span<const SemGenericSubstitution> dependent_substitutions,
    TypeId cached_function_type) {
  if (const auto found = specific_function_refs_.find(specific_id.index);
      found != specific_function_refs_.end())
    return found->second;
  const auto &template_reference = sem_ir_.functionRef(template_ref);
  if (!template_reference.local_function.hasValue())
    return FunctionRefId::invalid();
  const auto template_function =
      sem_ir_.function(template_reference.local_function);
  auto specific_type = cached_function_type;
  if (!specific_type.hasValue()) {
    specific_type = substituteType(template_function.type,
                                   template_function.generic, type_arguments);
    for (const auto &substitution : dependent_substitutions) {
      if (!specific_type.hasValue())
        break;
      specific_type = substituteType(specific_type, substitution.generic,
                                     substitution.arguments);
    }
  }
  if (!specific_type.hasValue())
    return FunctionRefId::invalid();

  std::string specific_name = std::string(
      values_.identifier(sem_ir_.name(template_function.name).text));
  specific_name += "$specific";
  for (const auto argument : type_arguments) {
    specific_name += '$';
    const auto &type = values_.generics().type(argument);
    if (type.kind == CanonicalTypeKind::Integer)
      specific_name +=
          std::string(type.arg1 != 0 ? "i" : "u") + std::to_string(type.arg0);
    else if (type.kind == CanonicalTypeKind::Float)
      specific_name += "f" + std::to_string(type.arg0);
    else
      specific_name += canonicalTypeKindName(type.kind);
  }
  const auto name = sem_ir_.addName(values_.internIdentifier(specific_name));
  std::vector<LocalId> placeholder_parameters;
  const auto &specific_function_type = sem_ir_.type(specific_type);
  for (std::uint32_t parameter_index = 0;
       const auto parameter_type :
       sem_ir_.typeBlock(TypeBlockId(specific_function_type.arg0))) {
    const auto parameter_name = sem_ir_.addName(values_.internIdentifier(
        "$specific_parameter" + std::to_string(parameter_index++)));
    placeholder_parameters.push_back(
        sem_ir_.addLocal({parameter_name, parameter_type, call_node, 0}));
  }
  const std::vector<InstId> no_body;
  const auto intrinsic_role = sem_ir_.functionIntrinsicRole(template_ref);
  const auto template_declaration =
      sem_ir_.functionDeclaration(template_reference.local_function);
  const auto contracted_forward =
      (template_declaration.kind == SemCallableDeclarationKind::Forward &&
       template_declaration.declared_contract.has_value()) ||
      ((template_function.flags & SemFunctionEvaluatorArtifact) != 0 &&
       sem_ir_.instBlock(template_function.body).empty());
  auto specific_record = SemFunction{
      name,
      specific_type,
      sem_ir_.addLocalBlock(placeholder_parameters),
      sem_ir_.addInstBlock(no_body),
      declaration_only || intrinsic_role != CompilerIntrinsicRole::None
          ? SemFunctionTemplate |
                (contracted_forward ? SemFunctionEvaluatorArtifact : 0U)
      : contracted_forward ? SemFunctionSpecific | SemFunctionEvaluatorArtifact
                           : SemFunctionSpecific,
      template_function.generic,
      specific_id,
      template_function.semantic_owner,
      template_function.semantic_role,
      template_function.semantic_projector};
  specific_record.intrinsic_role = intrinsic_role;
  const auto function = sem_ir_.addFunction(std::move(specific_record));
  const auto intrinsic = intrinsic_role != CompilerIntrinsicRole::None;
  if (intrinsic || contracted_forward) {
    auto declaration = template_declaration;
    if (intrinsic || contracted_forward)
      declaration.kind = SemCallableDeclarationKind::Forward;
    if (const auto *entity =
            sem_ir_.importIRs().tryGetEntity(template_reference.public_entity))
      declaration.is_unsafe = entity->is_unsafe;
    sem_ir_.setFunctionDeclaration(function, std::move(declaration));
  }
  auto ownership = sem_ir_.functionOwnership(template_reference.local_function);
  auto semantic_contract =
      sem_ir_.functionSemanticContract(template_reference.local_function);
  // A consuming Option::unwrap may specialize T to a borrowed reference. The
  // generic declaration cannot carry a concrete provenance path, so attach a
  // receiver-rooted return source to the specialized ownership summary.
  if (intrinsic_role == CompilerIntrinsicRole::OptionUnwrap &&
      sem_ir_.type(specific_type).kind == SemTypeKind::Function) {
    const auto result_type = TypeId(sem_ir_.type(specific_type).arg1);
    if (sem_ir_.loanCarrierCapability(result_type) !=
            SemLoanCarrierCapability::None &&
        ownership.return_provenance.empty()) {
      ownership.returns_owned = false;
      ownership.return_provenance.push_back({
          OwnershipRegion{0, {}, false, 0, 0}, {},
          CallableConditionDescriptor::always()});
    }
  }
  if (semantic_contract.domain != CallableSemanticDomain::Ordinary &&
      !placeholder_parameters.empty()) {
    const auto parameter_type =
        sem_ir_.local(placeholder_parameters.front()).type;
    if (sem_ir_.type(parameter_type).kind == SemTypeKind::Reference) {
      const auto owner_type = sem_ir_.referencePointee(parameter_type);
      if (sem_ir_.type(owner_type).kind == SemTypeKind::Nominal)
        semantic_contract.owner = NominalTypeId(sem_ir_.type(owner_type).arg0);
    }
  }
  sem_ir_.setFunctionSemanticContract(function, std::move(semantic_contract));
  if (contracted_forward &&
      (template_function.flags & SemFunctionEvaluatorArtifact) != 0) {
    sem_ir_.setFunctionOwnership(function, ownership);
    sem_ir_.expectFunctionOwnership(function, ownership);
  }
  std::vector<PublicType> concrete_arguments;
  concrete_arguments.reserve(type_arguments.size());
  for (const auto type : type_arguments) {
    const auto argument = concretePublicType(type, current_generic_);
    if (!argument) {
      concrete_arguments.clear();
      break;
    }
    concrete_arguments.push_back(*argument);
  }
  const auto reference =
      sem_ir_.addFunctionRef({function, template_reference.import_ir_inst,
                              specific_type, template_reference.public_entity,
                              template_function.generic, specific_id});
  sem_ir_.setFunctionRefConcreteArguments(reference,
                                          std::move(concrete_arguments));
  specific_function_refs_.emplace(specific_id.index, reference);
  auto &specific = values_.generics().specific(specific_id);
  specific.declaration_state = SpecificRegionState::Ready;
  if (declaration_only || contracted_forward) {
    specific.definition_state = SpecificRegionState::Ready;
    return reference;
  }
  if (intrinsic) {
    specific.definition_state = SpecificRegionState::Ready;
    return reference;
  }
  specific.definition_state = enqueue_definition
                                  ? SpecificRegionState::Queued
                                  : SpecificRegionState::Evaluating;
  if (enqueue_definition)
    deferred_definitions_.pushDefinition(
        {template_ref, reference, specific_id,
         std::vector<CanonicalTypeId>(type_arguments.begin(),
                                      type_arguments.end()),
         call_node,
         std::vector<SemGenericSubstitution>(dependent_substitutions.begin(),
                                             dependent_substitutions.end())});
  return reference;
}

[[nodiscard]] std::optional<PublicType>
