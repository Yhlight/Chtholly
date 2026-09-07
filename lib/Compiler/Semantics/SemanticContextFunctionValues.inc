// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
[[nodiscard]] InstId
SemanticContext::makeFunctionValue(NodeId node, NodeId occurrence,
                                   FunctionRefId function_ref,
                                   std::vector<InstId> &block) {
  const auto &reference = sem_ir_.functionRef(function_ref);
  if (reference.generic.hasValue() &&
      reference.specific ==
          values_.generics().generic(reference.generic).self_specific) {
    emit(DiagnosticKind::NotCallable, node);
    return appendInvalid(block, node);
  }
  const auto &function_type = sem_ir_.type(reference.local_type);
  bool valid = function_type.kind == SemTypeKind::Function;
  if (reference.local_function.hasValue()) {
    const auto function = reference.local_function;
    const auto &declaration = sem_ir_.functionDeclaration(function);
    valid = valid &&
            declaration.kind == SemCallableDeclarationKind::Definition &&
            !declaration.is_unsafe &&
            sem_ir_.functionSemanticContract(function).domain ==
                CallableSemanticDomain::Ordinary;
    for (std::uint32_t index = 0; valid && index < sem_ir_.nominalTypeCount();
         ++index) {
      const auto &members =
          sem_ir_.nominalType(NominalTypeId(index)).member_functions;
      const auto member = std::ranges::find(members, function_ref,
                                            &SemNominalMemberFunction::target);
      if (member != members.end() &&
          (member->flags & SemNominalMemberFunctionAssociated) == 0)
        valid = false;
    }
  } else {
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(reference.public_entity);
    valid =
        valid && entity && entity->kind == PublicEntityKind::Function &&
        entity->generic_parameter_count == 0 &&
        entity->declaration_kind == PublicCallableDeclarationKind::Definition &&
        !entity->is_unsafe &&
        entity->semantic_contract.domain == CallableSemanticDomain::Ordinary &&
        entity->member_kind != PublicFunctionArtifact::MemberKind::Instance;
  }
  if (!valid) {
    emit(DiagnosticKind::NotCallable, node);
    return appendInvalid(block, node);
  }
  sem_ir_.addFunctionOccurrence(occurrence, SemSymbolOccurrenceKind::Reference,
                                function_ref);
  return appendInst<SemFunctionValue>(block, node, reference.local_type,
                                      function_ref);
}

[[nodiscard]] InstId
SemanticContext::makeForeignFunctionReference(NodeId node, NodeId occurrence,
                                              FunctionRefId function_ref,
                                              std::vector<InstId> &block) {
  const auto &reference = sem_ir_.functionRef(function_ref);
  if (reference.generic.hasValue()) {
    emit(DiagnosticKind::InvalidForeignFunctionReference, node);
    return appendInvalid(block, node);
  }
  const auto &function_type = sem_ir_.type(reference.local_type);
  const CallableOwnershipSummary *ownership = nullptr;
  std::optional<CallableOwnershipSummary> declared_ownership;
  const ForeignAbiSignature *signature = nullptr;
  bool valid = function_type.kind == SemTypeKind::Function;
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir_.functionDeclaration(reference.local_function);
    valid = valid && declaration.kind == SemCallableDeclarationKind::Foreign &&
            declaration.is_unsafe && declaration.foreign_abi.hasValue() &&
            sem_ir_.identifier(declaration.foreign_abi) == "C" &&
            declaration.foreign_signature.has_value();
    if (declaration.foreign_signature)
      signature = &*declaration.foreign_signature;
    if (declaration.declared_contract) {
      declared_ownership =
          effectiveCallableOwnershipSummary(*declaration.declared_contract);
      ownership = &*declared_ownership;
    } else {
      ownership = &sem_ir_.functionOwnership(reference.local_function);
    }
  } else {
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(reference.public_entity);
    valid =
        valid && entity && entity->kind == PublicEntityKind::Function &&
        entity->generic_parameter_count == 0 &&
        entity->declaration_kind == PublicCallableDeclarationKind::Foreign &&
        entity->is_unsafe && entity->foreign_abi.hasValue() &&
        sem_ir_.identifier(entity->foreign_abi) == "C" &&
        entity->foreign_signature.has_value();
    if (entity) {
      if (entity->foreign_signature)
        signature = &*entity->foreign_signature;
      ownership = &entity->ownership_summary;
    }
  }
  if (!valid || !ownership || !signature) {
    emit(DiagnosticKind::InvalidForeignFunctionReference, node);
    return appendInvalid(block, node);
  }
  const auto parameters = sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
  const auto c_type =
      sem_ir_.addCFunctionPointerType(parameters, TypeId(function_type.arg1),
                                      signature->is_variadic, *ownership);
  (void)requireUnsafe(UnsafeOperationKind::ForeignFunctionReference, node);
  sem_ir_.addFunctionOccurrence(occurrence, SemSymbolOccurrenceKind::Reference,
                                function_ref);
  return appendInst<SemForeignFunctionRef>(block, node, c_type, function_ref);
}
