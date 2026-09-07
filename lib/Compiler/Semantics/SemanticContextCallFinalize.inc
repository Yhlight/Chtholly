// Included inside SemanticContext::checkCallExpression; this fragment
// aliases the single semantic call-resolution state.
  {
    const auto &reference = sem_ir_.functionRef(function_ref);
    auto domain = CallableSemanticDomain::Count;
    if (reference.local_function.hasValue()) {
      domain =
          sem_ir_.functionSemanticContract(reference.local_function).domain;
    } else if (const auto *entity =
                   sem_ir_.importIRs().tryGetEntity(reference.public_entity)) {
      domain = entity->semantic_contract.domain;
    }
    if (domain != CallableSemanticDomain::Ordinary &&
        domain != CallableSemanticDomain::NominalConstruction) {
      emit(DiagnosticKind::NotCallable, callee_node);
      return appendInvalid(block, node);
    }
    if (sem_ir_.type(reference.local_type).kind != SemTypeKind::Function) {
      emit(DiagnosticKind::NotCallable, callee_node);
      return appendInvalid(block, node);
    }
    if (reference.local_function.hasValue() &&
        sem_ir_.functionDeclaration(reference.local_function).is_unsafe)
      (void)requireUnsafe(UnsafeOperationKind::UnsafeFunctionCall, callee_node);
    if (!reference.local_function.hasValue()) {
      const auto *entity =
          sem_ir_.importIRs().tryGetEntity(reference.public_entity);
      if (entity && entity->is_unsafe)
        (void)requireUnsafe(UnsafeOperationKind::UnsafeFunctionCall,
                            callee_node);
    }
  }
  if (tree_.kind(callee_node) == NodeKind::QualifiedName ||
      tree_.kind(callee_node) == NodeKind::TypeMemberExpr)
    occurrence_node = tree_.children(callee_node).back();
  sem_ir_.addFunctionOccurrence(
      occurrence_node, SemSymbolOccurrenceKind::Reference, function_ref);
  const auto template_type =
      sem_ir_.type(sem_ir_.functionRef(function_ref).local_type);
  const auto template_parameters =
      sem_ir_.typeBlock(TypeBlockId(template_type.arg0));
  std::vector<InstId> arguments;
  arguments.reserve(children.size() - 1 +
                    (method_receiver.hasValue() ? 1U : 0U));
  if (method_receiver.hasValue()) {
    if (template_parameters.empty()) {
      emit(DiagnosticKind::InvalidInherentMethod, callee_node);
      return appendInvalid(block, node);
    }
    if (!adjustMethodReceiver(method_receiver, template_parameters.front(),
                              method_receiver_node, block))
      return appendInvalid(block, node);
    arguments.push_back(method_receiver);
  }
  for (std::size_t index = 1; index < children.size(); ++index) {
    const auto argument = checkExpression(children[index], block);
    arguments.push_back(argument);
  }
  const auto &template_reference = sem_ir_.functionRef(function_ref);
  const auto *template_foreign_signature =
      [&]() -> const ForeignAbiSignature * {
    if (template_reference.local_function.hasValue()) {
      const auto &declaration =
          sem_ir_.functionDeclaration(template_reference.local_function);
      return declaration.foreign_signature ? &*declaration.foreign_signature
                                           : nullptr;
    }
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(template_reference.public_entity);
    return entity && entity->foreign_signature ? &*entity->foreign_signature
                                               : nullptr;
  }();
  const auto is_variadic =
      template_foreign_signature && template_foreign_signature->is_variadic;
  if ((!is_variadic && arguments.size() != template_parameters.size()) ||
      (is_variadic && arguments.size() < template_parameters.size()))
    emit(DiagnosticKind::ArgumentCountMismatch, node);
  if (sem_ir_.functionRef(function_ref).generic.hasValue()) {
    for (std::size_t index = method_receiver.hasValue() ? 1U : 0U;
         index < arguments.size() && index < template_parameters.size();
         ++index) {
      if (sem_ir_.type(template_parameters[index]).kind ==
              SemTypeKind::TypeParameter &&
          sem_ir_.type(instType(arguments[index])).kind ==
              SemTypeKind::Reference)
        arguments[index] = acquireCheckedReference(
            arguments[index],
            children[index + (method_receiver.hasValue() ? 0U : 1U)], block);
    }
    const auto specific = specializeFunction(function_ref, arguments, node,
                                             true, explicit_member_arguments);
    if (!specific.hasValue())
      return appendInvalid(block, node);
    function_ref = specific;
  }
  const auto &function_type =
      sem_ir_.type(sem_ir_.functionRef(function_ref).local_type);
  const auto parameter_types =
      sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
  for (std::size_t index = 0;
       index < arguments.size() && index < parameter_types.size(); ++index) {
    const auto argument_node =
        method_receiver.hasValue() && index == 0
            ? method_receiver_node
            : children[index + (method_receiver.hasValue() ? 0 : 1)];
    if (prepareInitializationArgument(function_ref, index, arguments[index],
                                      parameter_types[index], argument_node,
                                      block))
      continue;
    if (!adjustExpression(arguments[index], parameter_types[index],
                          argument_node, block)) {
      emit(method_receiver.hasValue() && index == 0
               ? DiagnosticKind::InvalidMethodReceiver
               : DiagnosticKind::TypeMismatch,
           argument_node);
      if (method_receiver.hasValue() && index == 0)
        return appendInvalid(block, node);
    }
  }
  if (is_variadic) {
    for (std::size_t index = parameter_types.size(); index < arguments.size();
         ++index) {
      const auto kind = sem_ir_.type(instType(arguments[index])).kind;
      if (kind != SemTypeKind::Bool && kind != SemTypeKind::Integer &&
          kind != SemTypeKind::Char &&
          kind != SemTypeKind::Float && kind != SemTypeKind::Reference &&
          kind != SemTypeKind::RawPointer)
        emit(DiagnosticKind::InvalidVariadicArgument, children[index + 1]);
    }
  }
  (void)ensureResultOutcomeType(TypeId(function_type.arg1));
  return appendResolvedCall(block, node, function_ref,
                            TypeId(function_type.arg1), arguments);
}
