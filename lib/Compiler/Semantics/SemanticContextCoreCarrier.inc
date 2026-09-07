// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
[[nodiscard]] SemanticContext::CheckedValueBlock
SemanticContext::checkValueBlock(NodeId node, TypeId target) {
  CheckedValueBlock result;
  const auto children = tree_.children(node);
  const auto has_value =
      !children.empty() && !isStatementNode(tree_.kind(children.back()));
  const auto statement_count = children.size() - (has_value ? 1U : 0U);
  lexical_scopes_.push();
  bool reachable = true;
  for (std::size_t index = 0; index < statement_count; ++index) {
    if (reachable) {
      checkStatement(children[index], result.instructions);
      reachable = blockFallsThrough(sem_ir_, result.instructions);
    } else {
      emit(DiagnosticKind::UnreachableCode, children[index]);
      std::vector<InstId> detached;
      checkStatement(children[index], detached);
    }
  }
  result.falls_through = blockFallsThrough(sem_ir_, result.instructions);
  if (has_value && result.falls_through) {
    result.value_node = children.back();
    result.value =
        checkExpression(result.value_node, result.instructions, target);
    if (expressionCategory(sem_ir_, result.value) == SemExprCategory::Diverging)
      result.falls_through = false;
  }
  lexical_scopes_.pop();
  return result;
}

[[nodiscard]] bool
SemanticContext::isReferenceFieldInitialization(InstId target) const {
  if (!current_function_.hasValue() || !current_owner_parameter_.hasValue() ||
      sem_ir_.functionSemanticContract(current_function_).role !=
          SemCanonicalFunctionRole::Init ||
      sem_ir_.type(instType(target)).kind != SemTypeKind::Reference)
    return false;
  const auto &field = sem_ir_.inst(target);
  if (field.kind != SemInstKind::StructFieldAccess)
    return false;
  const auto &base = sem_ir_.inst(InstId(field.arg0));
  if (base.kind != SemInstKind::Dereference)
    return false;
  const auto &destination = sem_ir_.inst(InstId(base.arg0));
  return destination.kind == SemInstKind::NameRef &&
         LocalId(destination.arg0) == current_owner_parameter_;
}

[[nodiscard]] bool SemanticContext::isCoreCarrierName(NodeId node) const {
  if (tree_.kind(node) != NodeKind::QualifiedName)
    return false;
  const auto components = tree_.children(node);
  return components.size() == 2 &&
         values_.identifier(tokenString(components[0])) == "core" &&
         values_.identifier(tokenString(components[1])) == "carrier";
}

[[nodiscard]] InstId
SemanticContext::checkCarrierView(NodeId call, std::span<const NodeId> children,
                                  std::vector<InstId> &block) {
  if (children.size() != 2 || !current_function_.hasValue()) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  const auto &semantic_contract =
      sem_ir_.functionSemanticContract(current_function_);
  const auto role = semantic_contract.role;
  const auto allowed =
      (role >= SemCanonicalFunctionRole::ProjectionLoad &&
       role <= SemCanonicalFunctionRole::ProjectionBorrowMut) ||
      (role >= SemCanonicalFunctionRole::ObjectInit &&
       role <= SemCanonicalFunctionRole::ObjectDrop);
  if (!allowed || !semantic_contract.owner.hasValue() ||
      tree_.kind(children[1]) != NodeKind::Name) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  const auto local = lookup(nameFor(children[1]));
  if (!local.hasValue() || !current_owner_parameter_.hasValue() ||
      local != current_owner_parameter_) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  const auto input_type = sem_ir_.local(local).type;
  if (sem_ir_.type(input_type).kind != SemTypeKind::Reference) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  const auto owner_type = sem_ir_.referencePointee(input_type);
  const auto &owner = sem_ir_.type(owner_type);
  if (owner.kind != SemTypeKind::Nominal ||
      NominalTypeId(owner.arg0) != semantic_contract.owner) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  const auto &nominal = sem_ir_.nominalType(semantic_contract.owner);
  if (!nominal.object_repr_pattern.hasValue()) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  TypeId carrier = sem_ir_.objectRepresentationType(owner_type);
  if (carrier == owner_type) {
    std::vector<CanonicalTypeId> arguments;
    for (const auto argument : sem_ir_.typeBlock(TypeBlockId(owner.arg1)))
      arguments.push_back(sem_ir_.canonicalType(argument));
    carrier = nominal.generic.hasValue()
                  ? substituteType(nominal.object_repr_pattern, nominal.generic,
                                   arguments)
                  : nominal.object_repr_pattern;
  }
  if (!carrier.hasValue()) {
    emit(DiagnosticKind::InvalidCarrierView, call);
    return appendInvalid(block, call);
  }
  const auto parameter =
      appendInst<SemNameRef>(block, children[1], input_type, local);
  std::uint32_t contract_field =
      static_cast<std::uint32_t>(nominal.fields.size());
  if (role >= SemCanonicalFunctionRole::ProjectionLoad &&
      role <= SemCanonicalFunctionRole::ProjectionBorrowMut) {
    if (semantic_contract.projector_field >= nominal.fields.size()) {
      emit(DiagnosticKind::InvalidCarrierView, call);
      return appendInvalid(block, call);
    }
    contract_field = semantic_contract.projector_field;
  }
  const auto view_type = sem_ir_.addReferenceType(
      carrier, sem_ir_.referenceMutability(input_type));
  return appendInst<SemCarrierView>(block, call, view_type, parameter,
                                    sem_ir_.addInteger(contract_field));
}

[[nodiscard]] CanonicalTypeId SemanticContext::substituteCanonicalType(
    CanonicalTypeId type_id, GenericId generic,
    std::span<const CanonicalTypeId> arguments) {
  const auto type = values_.generics().type(type_id);
  if (type.kind == CanonicalTypeKind::TypeParameter &&
      type.arg0 == generic.index)
    return type.arg1 < arguments.size() && arguments[type.arg1].hasValue()
               ? arguments[type.arg1]
               : type_id;
  if (type.kind == CanonicalTypeKind::TypeProjection) {
    const auto source =
        substituteCanonicalType(CanonicalTypeId(type.arg0), generic, arguments);
    const auto &source_type = values_.generics().type(source);
    if (type.projection_kind == CanonicalTypeProjectionKind::Element) {
      if (source_type.kind == CanonicalTypeKind::Array &&
          type.arg1 < source_type.arg1)
        return CanonicalTypeId(source_type.arg0);
      if (source_type.kind == CanonicalTypeKind::Tuple &&
          type.arg1 < source_type.elements.size())
        return source_type.elements[type.arg1];
    } else if (type.projection_kind == CanonicalTypeProjectionKind::Pointee &&
               ((source_type.kind == CanonicalTypeKind::Reference) ||
                (source_type.kind == CanonicalTypeKind::RawPointer) ||
                (source_type.kind == CanonicalTypeKind::Slice)) &&
               source_type.elements.size() == 1) {
      return source_type.elements.front();
    }
    CanonicalType projection = type;
    projection.arg0 = source.index;
    return values_.generics().internType(std::move(projection));
  }
  if (type.kind == CanonicalTypeKind::Array) {
    const auto element =
        substituteCanonicalType(CanonicalTypeId(type.arg0), generic, arguments);
    return values_.generics().internType({.kind = CanonicalTypeKind::Array,
                                          .arg0 = element.index,
                                          .arg1 = type.arg1});
  }
  if (type.kind == CanonicalTypeKind::Tuple ||
      type.kind == CanonicalTypeKind::Slice ||
      type.kind == CanonicalTypeKind::Function ||
      type.kind == CanonicalTypeKind::Reference ||
      type.kind == CanonicalTypeKind::RawPointer ||
      type.kind == CanonicalTypeKind::CFunctionPointer ||
      type.kind == CanonicalTypeKind::CVariadicFunctionPointer ||
      type.kind == CanonicalTypeKind::CallbackAdapter ||
      type.kind == CanonicalTypeKind::CallbackRegistration ||
      type.kind == CanonicalTypeKind::CallbackCompletion ||
      type.kind == CanonicalTypeKind::CallbackWake) {
    CanonicalType result = type;
    result.elements.clear();
    result.elements.reserve(type.elements.size());
    for (const auto element : type.elements)
      result.elements.push_back(
          substituteCanonicalType(element, generic, arguments));
    return values_.generics().internType(std::move(result));
  }
  if (type.kind == CanonicalTypeKind::Nominal) {
    CanonicalType result = type;
    result.elements.clear();
    for (const auto argument : type.elements)
      result.elements.push_back(
          substituteCanonicalType(argument, generic, arguments));
    return values_.generics().internType(std::move(result));
  }
  return type_id;
}

[[nodiscard]] TypeId
