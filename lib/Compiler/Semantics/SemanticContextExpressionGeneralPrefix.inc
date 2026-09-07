// Included inside SemanticContext::checkExpression; this fragment retains
// the shared expression conversion and diagnostic state.
[[nodiscard]] InstId
SemanticContext::checkExpression(NodeId node, std::vector<InstId> &block,
                                 TypeId expected_type) {
  const auto children = tree_.children(node);
  switch (tree_.kind(node)) {
  case NodeKind::ClosureExpr:
    return checkClosureExpression(node, block);
  case NodeKind::WaitExpr:
    return checkWaitExpression(node, block);
  case NodeKind::TryExpr:
    return checkTryExpression(node, block);
  case NodeKind::PlacementExpr: {
    if (children.size() != 2)
      return appendInvalid(block, node);
    const auto call = checkExpression(children[0], block);
    if (sem_ir_.inst(call).kind != SemInstKind::Call) {
      emit(DiagnosticKind::InvalidConstructor, children[0]);
      return appendInvalid(block, node);
    }
    const auto target = FunctionRefId(sem_ir_.inst(call).arg0);
    const auto &reference = sem_ir_.functionRef(target);
    const auto constructor =
        reference.local_function.hasValue()
            ? sem_ir_.functionSemanticContract(reference.local_function).role ==
                  CallableSemanticRole::Constructor
            : [&] {
                const auto *entity =
                    sem_ir_.importIRs().tryGetEntity(reference.public_entity);
                return entity && entity->semantic_contract.role ==
                                     CallableSemanticRole::Constructor;
              }();
    if (!constructor) {
      emit(DiagnosticKind::InvalidConstructor, children[0]);
      return appendInvalid(block, node);
    }

    const auto call_type = instType(call);
    const auto result_shape = sem_ir_.canonicalResultShape(call_type);
    const auto owner_type = result_shape ? result_shape->success : call_type;
    if (sem_ir_.type(owner_type).kind != SemTypeKind::Nominal) {
      emit(DiagnosticKind::InvalidConstructor, children[0]);
      return appendInvalid(block, node);
    }
    const auto destination = checkExpression(children[1], block, owner_type);
    if (expressionCategory(sem_ir_, destination) != SemExprCategory::Place ||
        sem_ir_.canonicalType(instType(destination)) !=
            sem_ir_.canonicalType(owner_type)) {
      emit(DiagnosticKind::TypeMismatch, children[1]);
      return appendInvalid(block, node);
    }

    auto outcome_type = sem_ir_.voidType();
    if (result_shape)
      outcome_type = ensureResultOutcomeType(call_type);
    return appendInst<SemPlacement>(block, node, outcome_type, destination,
                                    call);
  }
  case NodeKind::SwitchExpr:
    return checkSwitchExpression(node, block, expected_type);
  case NodeKind::BlockExpr:
    if (children.size() != 1 ||
        tree_.kind(children.front()) != NodeKind::ValueBlock)
      return appendInvalid(block, node);
    return checkScopedBlockExpression(node, children.front(), block,
                                      expected_type);
  case NodeKind::IfExpr:
    return checkIfExpression(node, block, expected_type);
  case NodeKind::SizeofExpr:
  case NodeKind::AlignofExpr: {
    if (children.size() != 1) {
      emit(DiagnosticKind::InvalidLayoutQuery, node);
      return appendInvalid(block, node);
    }
    const auto queried_type = checkType(children.front());
    std::string layout_error;
    if (!querySemanticTypeLayout(sem_ir_, queried_type, sem_ir_.targetLayout(),
                                 layout_error)) {
      emit(DiagnosticKind::InvalidLayoutQuery, children.front());
      return appendInvalid(block, node);
    }
    const auto result_type = sem_ir_.addIntegerType(pointer_width_, false);
    return tree_.kind(node) == NodeKind::SizeofExpr
               ? appendInst<SemSizeOf>(block, node, result_type, queried_type)
               : appendInst<SemAlignOf>(block, node, result_type, queried_type);
  }
  case NodeKind::TypeSameExpr:
  case NodeKind::TypeIsExpr:
  case NodeKind::TypeHasExpr:
  case NodeKind::ArrayExtentExpr:
  case NodeKind::TupleArityExpr: {
    if (children.empty()) {
      emit(DiagnosticKind::InvalidTypeQuery, node);
      return appendInvalid(block, node);
    }
    const auto queried_type = checkType(children.front());
    const auto is_dependent_type = [&](auto &&self,
                                       CanonicalTypeId type_id) -> bool {
      const auto &type = values_.generics().type(type_id);
      if (type.kind == CanonicalTypeKind::TypeParameter ||
          type.kind == CanonicalTypeKind::TypeProjection)
        return true;
      if (type.kind == CanonicalTypeKind::Array &&
          self(self, CanonicalTypeId(type.arg0)))
        return true;
      return std::ranges::any_of(type.elements, [&](CanonicalTypeId element) {
        return self(self, element);
      });
    };
    const auto dependent = [&](TypeId type) {
      return is_dependent_type(is_dependent_type, sem_ir_.canonicalType(type));
    };
    const auto append_query = [&](SemTypeQueryArtifact::Kind kind,
                                  TypeId source, TypeId other,
                                  std::string property, TypeId result_type) {
      const auto query =
          sem_ir_.addTypeQuery({kind, source, other, std::move(property)});
      return appendInst<SemTypeQuery>(block, node, result_type,
                                      sem_ir_.addInteger(query));
    };
    if (tree_.kind(node) == NodeKind::TupleArityExpr) {
      if (sem_ir_.type(queried_type).kind != SemTypeKind::Tuple &&
          dependent(queried_type))
        return append_query(SemTypeQueryArtifact::Kind::TupleArity,
                            queried_type, TypeId::invalid(), {},
                            sem_ir_.addIntegerType(pointer_width_, false));
      if (sem_ir_.type(queried_type).kind != SemTypeKind::Tuple) {
        emit(DiagnosticKind::TypeMismatch, children.front());
        return appendInvalid(block, node);
      }
      return appendInst<SemIntegerLiteral>(
          block, node, sem_ir_.addIntegerType(pointer_width_, false),
          sem_ir_.addInteger(sem_ir_.tupleArity(queried_type)));
    }
    if (tree_.kind(node) == NodeKind::ArrayExtentExpr) {
      const auto &type = sem_ir_.type(queried_type);
      if (type.kind != SemTypeKind::Array && dependent(queried_type))
        return append_query(SemTypeQueryArtifact::Kind::ArrayExtent,
                            queried_type, TypeId::invalid(), {},
                            sem_ir_.addIntegerType(pointer_width_, false));
      if (type.kind != SemTypeKind::Array) {
        emit(DiagnosticKind::TypeMismatch, children.front());
        return appendInvalid(block, node);
      }
      return appendInst<SemIntegerLiteral>(
          block, node, sem_ir_.addIntegerType(pointer_width_, false),
          sem_ir_.addInteger(type.arg1));
    }
    bool result = false;
    if (tree_.kind(node) == NodeKind::TypeSameExpr) {
      if (children.size() != 2) {
        emit(DiagnosticKind::InvalidTypeQuery, node);
        return appendInvalid(block, node);
      }
      const auto other = checkType(children[1]);
      if (dependent(queried_type) || dependent(other))
        return append_query(SemTypeQueryArtifact::Kind::TypeSame, queried_type,
                            other, {}, sem_ir_.boolType());
      result =
          sem_ir_.canonicalType(queried_type) == sem_ir_.canonicalType(other);
    } else {
      if (children.size() != 2 || tree_.kind(children[1]) != NodeKind::Name) {
        emit(DiagnosticKind::InvalidTypeQuery, node);
        return appendInvalid(block, node);
      }
      const auto property = values_.identifier(tokenString(children[1]));
      const auto &type = sem_ir_.type(queried_type);
      if (tree_.kind(node) == NodeKind::TypeIsExpr) {
        const auto known = property == "integer" || property == "floating" ||
                           property == "raw_pointer" || property == "array" ||
                           property == "nominal" || property == "string" ||
                           property == "reference";
        if (!known) {
          emit(DiagnosticKind::UnknownName, children[1]);
          return appendInvalid(block, node);
        }
        if (type.kind == SemTypeKind::TypeParameter ||
            type.kind == SemTypeKind::TypeProjection)
          return append_query(SemTypeQueryArtifact::Kind::TypeIs, queried_type,
                              TypeId::invalid(), std::string(property),
                              sem_ir_.boolType());
        result = property == "integer"    ? type.kind == SemTypeKind::Integer
                 : property == "floating" ? type.kind == SemTypeKind::Float
                 : property == "raw_pointer"
                     ? type.kind == SemTypeKind::RawPointer
                 : property == "array"     ? type.kind == SemTypeKind::Array
                 : property == "nominal"   ? type.kind == SemTypeKind::Nominal
                 : property == "string"    ? type.kind == SemTypeKind::String
                 : property == "reference" ? type.kind == SemTypeKind::Reference
                                           : false;
      } else {
        const auto known = property == "copy" || property == "move" ||
                           property == "drop" ||
                           property == "object_representation";
        if (!known) {
          emit(DiagnosticKind::UnknownName, children[1]);
          return appendInvalid(block, node);
        }
        if (dependent(queried_type))
          return append_query(SemTypeQueryArtifact::Kind::TypeHas, queried_type,
                              TypeId::invalid(), std::string(property),
                              sem_ir_.boolType());
        const auto facts = sem_ir_.typeRepresentation(queried_type);
        result = property == "copy" ? facts.copy != CopyReprKind::Unavailable &&
                                          facts.copy != CopyReprKind::Dependent
                 : property == "move"
                     ? facts.move != MoveReprKind::Unavailable &&
                           facts.move != MoveReprKind::Dependent
                 : property == "drop"
                     ? facts.destroy != DestroyReprKind::None &&
                           facts.destroy != DestroyReprKind::Dependent
                 : property == "object_representation"
                     ? facts.object_repr != ObjectReprKind::None
                     : false;
      }
    }
    return appendInst<SemBoolLiteral>(block, node, sem_ir_.boolType(),
                                      sem_ir_.addInteger(result ? 1 : 0));
  }
