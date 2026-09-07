// Included by SemanticContextExpressions.cpp; this fragment aliases the
// shared SemanticContext state and preserves expression phase ordering.
[[nodiscard]] InstId
SemanticContext::checkShortCircuit(NodeId node, bool is_and,
                                   std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  auto left = checkExpression(children.front(), block, sem_ir_.boolType());
  if (!adjustExpression(left, sem_ir_.boolType(), children.front(), block)) {
    emit(DiagnosticKind::TypeMismatch, children.front());
    return appendInvalid(block, node);
  }
  std::array<std::vector<InstId>, 2> bodies;
  const auto rhs_arm = is_and ? 0U : 1U;
  auto right =
      checkExpression(children.back(), bodies[rhs_arm], sem_ir_.boolType());
  if (!adjustExpression(right, sem_ir_.boolType(), children.back(),
                        bodies[rhs_arm]))
    emit(DiagnosticKind::TypeMismatch, children.back());
  (void)appendInst<SemYield>(bodies[rhs_arm], children.back(),
                             sem_ir_.voidType(), right);
  const auto constant_arm = is_and ? 1U : 0U;
  const auto constant =
      appendInst<SemBoolLiteral>(bodies[constant_arm], node, sem_ir_.boolType(),
                                 sem_ir_.addInteger(is_and ? 0 : 1));
  (void)appendInst<SemYield>(bodies[constant_arm], node, sem_ir_.voidType(),
                             constant);
  std::array<InstId, 2> arms;
  for (std::size_t index = 0; index < arms.size(); ++index)
    arms[index] = sem_ir_.addInst(
        SemIfArm{sem_ir_.boolType(), sem_ir_.addInstBlock(bodies[index])},
        node);
  return appendInst<SemIf>(block, node, sem_ir_.boolType(), left,
                           sem_ir_.addInstBlock(arms));
}

[[nodiscard]] InstId
SemanticContext::checkIfExpression(NodeId node, std::vector<InstId> &block,
                                   TypeId expected_type) {
  const auto children = tree_.children(node);
  if (children.size() != 3)
    return appendInvalid(block, node);
  auto condition = checkExpression(children[0], block, sem_ir_.boolType());
  if (expressionCategory(sem_ir_, condition) == SemExprCategory::Diverging) {
    (void)checkValueBlock(children[1], expected_type);
    if (tree_.kind(children[2]) == NodeKind::IfExpr) {
      std::vector<InstId> detached;
      (void)checkExpression(children[2], detached, expected_type);
    } else {
      (void)checkValueBlock(children[2], expected_type);
    }
    return condition;
  }
  if (!adjustExpression(condition, sem_ir_.boolType(), children[0], block))
    emit(DiagnosticKind::TypeMismatch, children[0]);

  struct Arm {
    NodeId node;
    CheckedValueBlock checked;
  };
  std::array<Arm, 2> arms{
      Arm{children[1], checkValueBlock(children[1], expected_type)},
      Arm{children[2], {}}};
  if (tree_.kind(children[2]) == NodeKind::IfExpr) {
    arms[1].checked.value_node = children[2];
    arms[1].checked.value = checkExpression(
        children[2], arms[1].checked.instructions, expected_type);
    arms[1].checked.falls_through =
        expressionCategory(sem_ir_, arms[1].checked.value) !=
        SemExprCategory::Diverging;
  } else {
    arms[1].checked = checkValueBlock(children[2], expected_type);
  }

  std::vector<InstId> live_values;
  bool valid = true;
  for (auto &arm : arms) {
    if (!arm.checked.falls_through)
      continue;
    if (!arm.checked.value.hasValue()) {
      emit(DiagnosticKind::TypeMismatch, arm.node);
      valid = false;
    } else {
      live_values.push_back(arm.checked.value);
    }
  }
  if (!valid)
    return appendInvalid(block, node);

  const auto result_type =
      live_values.empty()
          ? sem_ir_.neverType()
          : convergeExpressionTypes(live_values, expected_type, node);
  if (!result_type.hasValue())
    return appendInvalid(block, node);

  std::vector<InstId> arm_records;
  for (auto &arm : arms) {
    if (arm.checked.falls_through) {
      if (!adjustExpression(arm.checked.value, result_type,
                            arm.checked.value_node, arm.checked.instructions)) {
        emit(DiagnosticKind::TypeMismatch, arm.checked.value_node);
        valid = false;
      }
      (void)appendInst<SemYield>(arm.checked.instructions,
                                 arm.checked.value_node, sem_ir_.voidType(),
                                 arm.checked.value);
    }
    arm_records.push_back(sem_ir_.addInst(
        SemIfArm{result_type, sem_ir_.addInstBlock(arm.checked.instructions)},
        arm.node));
  }
  if (!valid)
    return appendInvalid(block, node);
  return appendInst<SemIf>(block, node, result_type, condition,
                           sem_ir_.addInstBlock(arm_records, true));
}

[[nodiscard]] InstId
SemanticContext::checkClosureExpression(NodeId node,
                                        std::vector<InstId> &block) {
  if (!requireVersion(FrozenV12LanguageVersion,
                      DiagnosticKind::CallableRequires12, node))
    return appendInvalid(block, node);
  const auto children = tree_.children(node);
  if (children.size() != 4 ||
      tree_.kind(children[0]) != NodeKind::ClosureCaptureList ||
      tree_.kind(children[1]) != NodeKind::ParameterList ||
      (tree_.kind(children[3]) != NodeKind::Block &&
       tree_.kind(children[3]) != NodeKind::ValueBlock)) {
    emit(DiagnosticKind::InvalidClosureCapture, node);
    return appendInvalid(block, node);
  }

  struct Capture {
    NameId name;
    NodeId name_node;
    TypeId type;
    InstId value;
    bool mutable_field = false;
  };
  std::vector<Capture> captures;
  std::unordered_set<std::uint32_t> capture_names;
  bool mutable_environment = false;
  for (const auto capture_node : tree_.children(children[0])) {
    const auto capture_children = tree_.children(capture_node);
    const auto kind = tree_.kind(capture_node);
    if ((kind != NodeKind::ClosureCopyCapture &&
         kind != NodeKind::ClosureMoveCapture &&
         kind != NodeKind::ClosureVarCapture) ||
        capture_children.empty() ||
        tree_.kind(capture_children.front()) != NodeKind::Name) {
      emit(DiagnosticKind::InvalidClosureCapture, capture_node);
      continue;
    }
    const auto name_node = capture_children.front();
    const auto name = nameFor(name_node);
    if (!name.hasValue() || !capture_names.insert(name.index).second) {
      emit(DiagnosticKind::DuplicateName, name_node);
      continue;
    }
    InstId value;
    if (kind == NodeKind::ClosureVarCapture) {
      if (capture_children.size() != 2) {
        emit(DiagnosticKind::InvalidClosureCapture, capture_node);
        continue;
      }
      const auto saved_initializer = initializing_closure_capture_;
      initializing_closure_capture_ = name;
      value = checkExpression(capture_children.back(), block);
      initializing_closure_capture_ = saved_initializer;
      mutable_environment = true;
    } else {
      const auto *binding = lexical_scopes_.lookup(name);
      if (!binding || (binding->kind != SemanticBindingKind::Local &&
                       binding->kind != SemanticBindingKind::ClosureCapture)) {
        emit(DiagnosticKind::InvalidClosureCapture, name_node);
        continue;
      }
      auto source = checkExpression(name_node, block);
      value = kind == NodeKind::ClosureMoveCapture
                  ? appendInst<SemMove>(block, capture_node, instType(source),
                                        source)
                  : appendInst<SemCopy>(block, capture_node, instType(source),
                                        source);
    }
    const auto type = instType(value);
    if (sem_ir_.type(type).kind == SemTypeKind::Reference) {
      emit(DiagnosticKind::ClosureReferenceCapture, capture_node);
      continue;
    }
    captures.push_back(
        {name, name_node, type, value, kind == NodeKind::ClosureVarCapture});
  }

  const auto parameter_syntax = functionParameters(children[1]);
  std::vector<TypeId> source_parameters;
  source_parameters.reserve(parameter_syntax.size());
  for (const auto &parameter : parameter_syntax) {
    const auto name = nameFor(parameter.name);
    if (name.hasValue() && capture_names.contains(name.index))
      emit(DiagnosticKind::DuplicateName, parameter.name);
    source_parameters.push_back(checkType(parameter.type));
  }
  const auto result_type = checkType(children[2]);

  const auto ordinal = closure_counter_++;
  const auto environment_identity =
      callableEnvironmentIdentity(SemCallableEnvironmentKind::Closure, ordinal);
  const auto hidden_text = values_.internIdentifier(
      "$closure$" + environment_identity.hex().substr(0, 24));
  const auto hidden_name = sem_ir_.addName(hidden_text);
  const auto outer_generic = current_generic_;
  GenericId closure_generic;
  std::uint32_t generic_count = 0;
  if (outer_generic.hasValue()) {
    generic_count = values_.generics().generic(outer_generic).binding_count;
    closure_generic = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(), hidden_text, generic_count);
  }
  std::function<TypeId(TypeId)> remap_hidden_type;
  remap_hidden_type = [&](TypeId source) -> TypeId {
    if (!closure_generic.hasValue())
      return source;
    const auto &type = sem_ir_.type(source);
    if (type.kind == SemTypeKind::TypeParameter &&
        type.arg0 == outer_generic.index)
      return sem_ir_.addTypeParameter(closure_generic, type.arg1);
    if (type.kind == SemTypeKind::Array)
      return sem_ir_.addType({SemTypeKind::Array,
                              remap_hidden_type(TypeId(type.arg0)).index,
                              type.arg1, type.reserved});
    if (type.kind == SemTypeKind::Tuple) {
      std::vector<TypeId> elements;
      for (const auto element : sem_ir_.typeBlock(TypeBlockId(type.arg0)))
        elements.push_back(remap_hidden_type(element));
      return sem_ir_.addTupleType(elements);
    }
    if (type.kind == SemTypeKind::Slice)
      return sem_ir_.addSliceType(remap_hidden_type(TypeId(type.arg0)),
                                  type.arg1 != 0);
    if (type.kind == SemTypeKind::Reference)
      return sem_ir_.addReferenceType(remap_hidden_type(TypeId(type.arg0)),
                                      sem_ir_.referenceMutability(source));
    if (type.kind == SemTypeKind::RawPointer)
      return sem_ir_.addRawPointerType(remap_hidden_type(TypeId(type.arg0)),
                                       type.arg1 != 0);
    if (type.kind == SemTypeKind::Function) {
      std::vector<TypeId> parameters;
      for (const auto parameter : sem_ir_.typeBlock(TypeBlockId(type.arg0)))
        parameters.push_back(remap_hidden_type(parameter));
      return sem_ir_.addFunctionType(parameters,
                                     remap_hidden_type(TypeId(type.arg1)));
    }
    if (type.kind == SemTypeKind::Nominal) {
      std::vector<TypeId> arguments;
      for (const auto argument : sem_ir_.typeBlock(TypeBlockId(type.arg1)))
        arguments.push_back(remap_hidden_type(argument));
      return sem_ir_.addNominalType(NominalTypeId(type.arg0), arguments);
    }
    return source;
  };
  std::vector<TypeId> hidden_capture_types;
  hidden_capture_types.reserve(captures.size());
  for (const auto &capture : captures)
    hidden_capture_types.push_back(remap_hidden_type(capture.type));
  std::vector<TypeId> hidden_source_parameters;
  hidden_source_parameters.reserve(source_parameters.size());
  for (const auto parameter : source_parameters)
    hidden_source_parameters.push_back(remap_hidden_type(parameter));
  const auto hidden_result_type = remap_hidden_type(result_type);

  SemNominalType environment_record;
  environment_record.name = hidden_name;
  environment_record.generic = closure_generic;
  environment_record.declaration = node;
  environment_record.flags |= SemNominalTypeClosureEnvironment;
  if (closure_generic.hasValue())
    environment_record.flags |= SemNominalTypeArtifactDependency;
  environment_record.completion_state = SemNominalCompletionState::Complete;
  for (std::size_t index = 0; index < captures.size(); ++index)
    environment_record.fields.push_back({captures[index].name,
                                         hidden_capture_types[index],
                                         captures[index].name_node});
  const auto environment_decl =
      sem_ir_.addNominalTypeDecl(std::move(environment_record));
  std::vector<TypeId> outer_type_arguments;
  std::vector<TypeId> hidden_type_arguments;
  for (std::uint32_t index = 0; index < generic_count; ++index) {
    outer_type_arguments.push_back(
        sem_ir_.addTypeParameter(outer_generic, index));
    hidden_type_arguments.push_back(
        sem_ir_.addTypeParameter(closure_generic, index));
  }
  const auto environment_type =
      sem_ir_.addNominalType(environment_decl, outer_type_arguments);
  const auto hidden_environment_type =
      sem_ir_.addNominalType(environment_decl, hidden_type_arguments);
  auto call_environment_reference = sem_ir_.addReferenceType(
      environment_type, mutable_environment ? SemReferenceMutability::Mutable
                                            : SemReferenceMutability::ReadOnly);
  auto hidden_environment_reference = sem_ir_.addReferenceType(
      hidden_environment_type, mutable_environment
                                   ? SemReferenceMutability::Mutable
                                   : SemReferenceMutability::ReadOnly);

  std::vector<TypeId> hidden_parameters{hidden_environment_reference};
  hidden_parameters.insert(hidden_parameters.end(),
                           hidden_source_parameters.begin(),
                           hidden_source_parameters.end());
  auto hidden_type =
      sem_ir_.addFunctionType(hidden_parameters, hidden_result_type);
  std::vector<TypeId> call_parameters{call_environment_reference};
  call_parameters.insert(call_parameters.end(), source_parameters.begin(),
                         source_parameters.end());
  auto call_type = sem_ir_.addFunctionType(call_parameters, result_type);
  std::uint32_t hidden_flags = 0;
  if (closure_generic.hasValue())
    hidden_flags |= SemFunctionTemplate | SemFunctionPublic;
  const auto hidden_function = sem_ir_.addFunction(
      {hidden_name, hidden_type, LocalBlockId::invalid(),
       InstBlockId::invalid(), hidden_flags, closure_generic,
       closure_generic.hasValue()
           ? values_.generics().generic(closure_generic).self_specific
           : SpecificId::invalid()});
  SemCallableDeclaration hidden_declaration;
  hidden_declaration.parameter_names.push_back(hidden_text);
  for (const auto &parameter : parameter_syntax)
    hidden_declaration.parameter_names.push_back(tokenString(parameter.name));
  sem_ir_.setFunctionDeclaration(hidden_function,
                                 std::move(hidden_declaration));
  const auto hidden_ref = sem_ir_.addFunctionRef(
      {hidden_function, ImportIRInstId::invalid(), call_type,
       PublicEntityId::invalid(), closure_generic,
       closure_generic.hasValue()
           ? values_.generics().generic(closure_generic).self_specific
           : SpecificId::invalid()});
  if (closure_generic.hasValue()) {
    generic_template_refs_.insert_or_assign(closure_generic.index, hidden_ref);
    std::vector<PublicType> symbolic_arguments;
    symbolic_arguments.reserve(generic_count);
    for (std::uint32_t index = 0; index < generic_count; ++index)
      symbolic_arguments.emplace_back(PublicTypeKind::TypeParameter, index);
    sem_ir_.setFunctionRefConcreteArguments(hidden_ref,
                                            std::move(symbolic_arguments));
  }

  const auto saved_function = current_function_;
  const auto saved_owner_parameter = current_owner_parameter_;
  const auto saved_return_type = return_type_;
  const auto saved_impl_owner = current_impl_owner_;
  const auto saved_impl_owner_type = current_impl_owner_type_;
  const auto saved_environment = active_closure_environment_;
  const auto saved_environment_type = active_closure_environment_type_;
  const auto saved_generic = current_generic_;

  current_function_ = hidden_function;
  current_generic_ = closure_generic;
  current_impl_owner_ = NominalTypeId::invalid();
  current_impl_owner_type_ = TypeId::invalid();
  return_type_ = hidden_result_type;
  lexical_scopes_.pushIsolated();
  std::vector<LocalId> parameters;
  std::vector<InstId> body;
  const auto environment_local = sem_ir_.addLocal(
      {hidden_name, hidden_environment_reference, node, SemLocalMutable});
  parameters.push_back(environment_local);
  const auto environment_parameter_inst = appendInst<SemParameter>(
      body, node, hidden_environment_reference, environment_local);
  active_closure_environment_ = environment_local;
  active_closure_environment_type_ = hidden_environment_type;
  for (std::uint32_t index = 0; index < captures.size(); ++index)
    bindClosureCapture(captures[index].name, index, captures[index].name_node);
  for (std::size_t index = 0; index < parameter_syntax.size(); ++index) {
    const auto parameter_name = nameFor(parameter_syntax[index].name);
    const auto local =
        sem_ir_.addLocal({parameter_name, hidden_source_parameters[index],
                          parameter_syntax[index].name, 0});
    parameters.push_back(local);
    bind(parameter_name, local, parameter_syntax[index].name);
    (void)appendInst<SemParameter>(body, parameter_syntax[index].name,
                                   hidden_source_parameters[index], local);
  }
  current_owner_parameter_ = environment_local;
  const auto closure_body_begin = sem_ir_.instCount();
  checkCallableBodyInto(children[3], body);
  lexical_scopes_.pop();

  const auto is_capture_place = [&](auto &&self, InstId value) -> bool {
    if (!value.hasValue() || value.index >= sem_ir_.instCount())
      return false;
    const auto &inst = sem_ir_.inst(value);
    if (inst.kind == SemInstKind::NameRef)
      return LocalId(inst.arg0) == environment_local;
    return (inst.kind == SemInstKind::Dereference ||
            inst.kind == SemInstKind::StructFieldAccess ||
            inst.kind == SemInstKind::UnionFieldAccess ||
            inst.kind == SemInstKind::EnumPayloadAccess ||
            inst.kind == SemInstKind::CarrierView) &&
           self(self, InstId(inst.arg0));
  };
  for (std::size_t index = closure_body_begin;
       !mutable_environment && index < sem_ir_.instCount(); ++index) {
    const auto &inst = sem_ir_.inst(InstId(static_cast<std::uint32_t>(index)));
    if ((inst.kind == SemInstKind::Move || inst.kind == SemInstKind::Assign) &&
        is_capture_place(is_capture_place, InstId(inst.arg0))) {
      mutable_environment = true;
    } else if (inst.kind == SemInstKind::BorrowPlace &&
               sem_ir_.type(TypeId(inst.type)).kind == SemTypeKind::Reference &&
               sem_ir_.referenceMutability(TypeId(inst.type)) ==
                   SemReferenceMutability::Mutable &&
               is_capture_place(is_capture_place, InstId(inst.arg0))) {
      mutable_environment = true;
    }
  }
  if (mutable_environment &&
      sem_ir_.referenceMutability(hidden_environment_reference) !=
          SemReferenceMutability::Mutable) {
    hidden_environment_reference = sem_ir_.addReferenceType(
        hidden_environment_type, SemReferenceMutability::Mutable);
    call_environment_reference = sem_ir_.addReferenceType(
        environment_type, SemReferenceMutability::Mutable);
    hidden_parameters.front() = hidden_environment_reference;
    call_parameters.front() = call_environment_reference;
    hidden_type =
        sem_ir_.addFunctionType(hidden_parameters, hidden_result_type);
    call_type = sem_ir_.addFunctionType(call_parameters, result_type);
    sem_ir_.setLocal(
        environment_local,
        {hidden_name, hidden_environment_reference, node, SemLocalMutable});
    sem_ir_.setInstType(environment_parameter_inst,
                        hidden_environment_reference);
    for (std::size_t index = closure_body_begin; index < sem_ir_.instCount();
         ++index) {
      const auto instruction = InstId(static_cast<std::uint32_t>(index));
      const auto &inst = sem_ir_.inst(instruction);
      if (inst.kind == SemInstKind::NameRef &&
          LocalId(inst.arg0) == environment_local)
        sem_ir_.setInstType(instruction, hidden_environment_reference);
    }
    auto reference = sem_ir_.functionRef(hidden_ref);
    reference.local_type = call_type;
    sem_ir_.setFunctionRef(hidden_ref, std::move(reference));
  }
  sem_ir_.setFunction(
      hidden_function,
      {hidden_name, hidden_type, sem_ir_.addLocalBlock(parameters),
       sem_ir_.addInstBlock(body), hidden_flags, closure_generic,
       closure_generic.hasValue()
           ? values_.generics().generic(closure_generic).self_specific
           : SpecificId::invalid()});

  current_function_ = saved_function;
  current_owner_parameter_ = saved_owner_parameter;
  return_type_ = saved_return_type;
  current_impl_owner_ = saved_impl_owner;
  current_impl_owner_type_ = saved_impl_owner_type;
  active_closure_environment_ = saved_environment;
  active_closure_environment_type_ = saved_environment_type;
  current_generic_ = saved_generic;

  const auto capability = mutable_environment
                              ? SemCallableEnvironmentCapability::Mutable
                              : SemCallableEnvironmentCapability::ReadOnly;
  sem_ir_.setCallableEnvironment({SemCallableEnvironmentKind::Closure,
                                  environment_type, hidden_ref, hidden_ref,
                                  capability, environment_identity});
  synthesizeCallableWitness(environment_type, result_type, source_parameters,
                            hidden_ref, outer_generic,
                            mutable_environment
                                ? SemCallableEnvironmentCapability::Mutable
                                : SemCallableEnvironmentCapability::ReadOnly,
                            mutable_environment ? "InvokeMut" : "Invoke", node);
  if (!mutable_environment && result_type == sem_ir_.boolType() &&
      sem_ir_.languageVersion() >= FrozenV16LanguageVersion)
    synthesizeCallableWitness(
        environment_type, result_type, source_parameters, hidden_ref,
        outer_generic, SemCallableEnvironmentCapability::ReadOnly, "Predicate",
        node, "std::iter::adapters", "test", std::nullopt);
  if (!mutable_environment && source_parameters.size() == 2 &&
      result_type == source_parameters.front() &&
      sem_ir_.languageVersion() >= FrozenV17LanguageVersion)
    synthesizeCallableWitness(
        environment_type, result_type, source_parameters, hidden_ref,
        outer_generic, SemCallableEnvironmentCapability::ReadOnly, "Folder",
        node, "std::iter::algorithms", "combine", std::nullopt, true);
  std::vector<InstId> capture_values;
  capture_values.reserve(captures.size());
  for (const auto &capture : captures)
    capture_values.push_back(capture.value);
  return appendInst<SemClosure>(block, node, environment_type, hidden_ref,
                                sem_ir_.addInstBlock(capture_values, true));
}

