// Included inside SemanticContext::checkExpression; this fragment retains
// the shared expression conversion and diagnostic state.
  case NodeKind::UnaryExpr: {
    if (children.size() != 1)
      return appendInvalid(block, node);
    const auto operator_kind = tree_.tokens().get(tree_.token(node)).kind;
    if (operator_kind == TokenKind::Minus &&
        tree_.kind(children.front()) == NodeKind::IntegerLiteral) {
      const auto parsed = parseIntegerLiteral(
          tree_.tokens().text(tree_.token(children.front())));
      if (!parsed || parsed->suffix == NumericSuffix::U8 ||
          parsed->suffix == NumericSuffix::U16 ||
          parsed->suffix == NumericSuffix::U32 ||
          parsed->suffix == NumericSuffix::U64 ||
          parsed->suffix == NumericSuffix::USize) {
        if (parsed && parsed->suffix != NumericSuffix::None) {
          const auto operand =
              checkExpression(children.front(), block, expected_type);
          return appendBuiltinUnary(block, node, instType(operand),
                                    BuiltinOperatorKind::Negate, operand);
        }
        emit(DiagnosticKind::InvalidNumericLiteral, node);
        return appendInvalid(block, node);
      }
      TypeId type;
      if (parsed->suffix != NumericSuffix::None) {
        type = typeForSuffix(parsed->suffix);
      } else if (expected_type.hasValue() &&
                 integerMagnitudeFits(parsed->magnitude, expected_type, true)) {
        type = expected_type;
      } else if (parsed->magnitude <= (std::uint64_t{1} << 31U)) {
        type = sem_ir_.i32Type();
      } else if (parsed->magnitude <= (std::uint64_t{1} << 63U)) {
        type = sem_ir_.addIntegerType(64, true);
      }
      if (!type.hasValue() || sem_ir_.type(type).kind != SemTypeKind::Integer ||
          sem_ir_.type(type).arg1 == 0 ||
          !integerMagnitudeFits(parsed->magnitude, type, true)) {
        emit(DiagnosticKind::InvalidNumericLiteral, node);
        return appendInvalid(block, node);
      }
      const auto value = parsed->magnitude == (std::uint64_t{1} << 63U)
                             ? std::numeric_limits<std::int64_t>::min()
                             : -static_cast<std::int64_t>(parsed->magnitude);
      const auto result = appendInst<SemIntegerLiteral>(
          block, node, type, sem_ir_.addInteger(value));
      if (parsed->suffix == NumericSuffix::None)
        conversions_.recordInteger(
            result, SemanticIntegerLiteral{parsed->magnitude, true});
      return result;
    }
    if (operator_kind == TokenKind::Minus &&
        tree_.kind(children.front()) == NodeKind::FloatLiteral) {
      NumericSuffix suffix = NumericSuffix::None;
      const auto parsed = parseFloatLiteral(
          tree_.tokens().text(tree_.token(children.front())), suffix);
      if (!parsed) {
        emit(DiagnosticKind::InvalidNumericLiteral, node);
        return appendInvalid(block, node);
      }
      const auto numeric = -*parsed;
      const auto type =
          suffix == NumericSuffix::F32   ? sem_ir_.addFloatType(32)
          : suffix == NumericSuffix::F64 ? sem_ir_.f64Type()
          : expected_type.hasValue() &&
                  sem_ir_.type(expected_type).kind == SemTypeKind::Float
              ? expected_type
              : sem_ir_.f64Type();
      const auto width = sem_ir_.type(type).arg0;
      if (width == 32 && !std::isfinite(static_cast<float>(numeric))) {
        emit(DiagnosticKind::InvalidNumericLiteral, node);
        return appendInvalid(block, node);
      }
      const auto bits =
          width == 32 ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(
                            static_cast<float>(numeric)))
                      : std::bit_cast<std::uint64_t>(numeric);
      const auto result = appendInst<SemFloatLiteral>(
          block, node, type,
          sem_ir_.addInteger(std::bit_cast<std::int64_t>(bits)));
      if (suffix == NumericSuffix::None)
        conversions_.recordFloat(result, SemanticFloatLiteral{numeric});
      return result;
    }
    if (operator_kind == TokenKind::Star) {
      const auto operand = checkExpression(children.front(), block);
      const auto operand_type = instType(operand);
      const auto operand_kind = sem_ir_.type(operand_type).kind;
      if (operand_kind == SemTypeKind::Reference) {
        return appendInst<SemDereference>(
            block, node, sem_ir_.referencePointee(operand_type), operand);
      }
      if (operand_kind != SemTypeKind::RawPointer ||
          sem_ir_.rawPointerPointee(operand_type) == sem_ir_.voidType()) {
        emit(DiagnosticKind::InvalidPointerDereference, node);
        return appendInvalid(block, node);
      }
      if (sem_ir_.inst(operand).kind == SemInstKind::NullPointer) {
        emit(DiagnosticKind::InvalidPointerDereference, node);
        return appendInvalid(block, node);
      }
      (void)requireUnsafe(UnsafeOperationKind::RawPointerDereference, node);
      return appendInst<SemDereference>(
          block, node, sem_ir_.rawPointerPointee(operand_type), operand);
    }
    if (operator_kind == TokenKind::Amp &&
        tree_.kind(children.front()) == NodeKind::Name) {
      const auto name = nameFor(children.front());
      const auto local = lookup(name);
      if (!local.hasValue()) {
        emit(DiagnosticKind::UnknownName, children.front());
        return appendInvalid(block, node);
      }
      sem_ir_.addLocalOccurrence(children.front(),
                                 SemSymbolOccurrenceKind::Reference, local);
      const auto local_type = sem_ir_.local(local).type;
      if (sem_ir_.type(local_type).kind == SemTypeKind::Reference) {
        auto reference =
            appendInst<SemNameRef>(block, children.front(), local_type, local);
        const auto place = acquireCheckedReference(reference, node, block);
        return appendInst<SemBorrowPlace>(block, node, local_type, place);
      }
      const auto type = sem_ir_.addReferenceType(
          local_type, (sem_ir_.local(local).flags & SemLocalMutable) != 0
                          ? SemReferenceMutability::Mutable
                          : SemReferenceMutability::ReadOnly);
      return appendInst<SemBorrowLocal>(block, node, type, local);
    }
    if (operator_kind == TokenKind::Amp) {
      auto operand = checkExpression(children.front(), block);
      auto operand_snapshot = sem_ir_.inst(operand);
      if (operand_snapshot.kind == SemInstKind::StructFieldAccess &&
          expressionCategory(sem_ir_, InstId(operand_snapshot.arg0)) ==
              SemExprCategory::Temporary) {
        const auto root_type = instType(InstId(operand_snapshot.arg0));
        const auto &root_sem_type = sem_ir_.type(root_type);
        const auto field = static_cast<std::uint32_t>(
            sem_ir_.integer(IntegerId(operand_snapshot.arg1)));
        if (root_sem_type.kind != SemTypeKind::Nominal ||
            field >= sem_ir_.nominalType(NominalTypeId(root_sem_type.arg0))
                         .fields.size() ||
            sem_ir_.nominalType(NominalTypeId(root_sem_type.arg0))
                    .fields[field]
                    .projection_kind !=
                PublicObjectProjectionKind::StableAddress) {
          emit(DiagnosticKind::ProjectionCapabilityUnavailable, node);
          return appendInvalid(block, node);
        }
        if (!block.empty() && block.back() == operand)
          block.pop_back();
        const auto materialized = materializeTemporary(
            InstId(operand_snapshot.arg0), children.front(), block);
        if (!materialized.hasValue()) {
          emit(DiagnosticKind::InvalidBorrowOperand, node);
          return appendInvalid(block, node);
        }
        operand_snapshot.arg0 = materialized.index;
        sem_ir_.replaceInst(
            operand,
            SemStructFieldAccess{TypeId(operand_snapshot.type), materialized,
                                 IntegerId(operand_snapshot.arg1)});
        block.push_back(operand);
      }
      if (containsRawPointerDereference(operand)) {
        emit(DiagnosticKind::RawPointerCapabilityUnavailable, node);
        return appendInvalid(block, node);
      }
      if (expressionCategory(sem_ir_, operand) == SemExprCategory::Temporary) {
        const auto materialized = materializeTemporary(operand, node, block);
        if (!materialized.hasValue()) {
          emit(DiagnosticKind::InvalidBorrowOperand, node);
          return appendInvalid(block, node);
        }
        const auto type = sem_ir_.addReferenceType(
            instType(materialized), SemReferenceMutability::ReadOnly);
        return appendInst<SemBorrowPlace>(block, node, type, materialized);
      }
      const auto &operand_inst = sem_ir_.inst(operand);
      if (operand_inst.kind == SemInstKind::Index) {
        const auto root_mutable = [&](auto &&self, InstId value) -> bool {
          const auto &inst = sem_ir_.inst(value);
          if (inst.kind == SemInstKind::NameRef) {
            const auto &local = sem_ir_.local(LocalId(inst.arg0));
            return (local.flags & SemLocalMutable) != 0 ||
                   (sem_ir_.type(local.type).kind == SemTypeKind::Reference &&
                    sem_ir_.referenceMutability(local.type) ==
                        SemReferenceMutability::Mutable);
          }
          if (inst.kind == SemInstKind::Index ||
              inst.kind == SemInstKind::StructFieldAccess ||
              inst.kind == SemInstKind::Dereference)
            return self(self, InstId(inst.arg0));
          return false;
        };
        const auto type = sem_ir_.addReferenceType(
            instType(operand), root_mutable(root_mutable, operand)
                                   ? SemReferenceMutability::Mutable
                                   : SemReferenceMutability::ReadOnly);
        return appendInst<SemBorrowPlace>(block, node, type, operand);
      }
      if (operand_inst.kind != SemInstKind::StructFieldAccess) {
        emit(DiagnosticKind::InvalidBorrowOperand, node);
        return appendInvalid(block, node);
      }
      const auto base_type =
          TypeId(sem_ir_.inst(InstId(operand_inst.arg0)).type);
      const auto &base_sem_type = sem_ir_.type(base_type);
      if (base_sem_type.kind != SemTypeKind::Nominal) {
        emit(DiagnosticKind::InvalidBorrowOperand, node);
        return appendInvalid(block, node);
      }
      const auto field = static_cast<std::uint32_t>(
          sem_ir_.integer(IntegerId(operand_inst.arg1)));
      const auto &nominal =
          sem_ir_.nominalType(NominalTypeId(base_sem_type.arg0));
      if (field >= nominal.fields.size() ||
          nominal.fields[field].projection_kind ==
              PublicObjectProjectionKind::BitPacked) {
        emit(DiagnosticKind::ProjectionCapabilityUnavailable, node);
        return appendInvalid(block, node);
      }
      const auto root_mutable = [&](auto &&self, InstId value) -> bool {
        const auto &inst = sem_ir_.inst(value);
        if (inst.kind == SemInstKind::NameRef) {
          const auto &local = sem_ir_.local(LocalId(inst.arg0));
          return (local.flags & SemLocalMutable) != 0 ||
                 (sem_ir_.type(local.type).kind == SemTypeKind::Reference &&
                  sem_ir_.referenceMutability(local.type) ==
                      SemReferenceMutability::Mutable);
        }
        if (inst.kind == SemInstKind::StructFieldAccess ||
            inst.kind == SemInstKind::Dereference)
          return self(self, InstId(inst.arg0));
        return false;
      };
      const auto mutable_ref = root_mutable(root_mutable, operand);
      if (nominal.fields[field].projection_kind ==
          PublicObjectProjectionKind::Computed) {
        const auto projector = std::ranges::find(
            nominal.object_projectors, nominal.fields[field].projector_name,
            &SemObjectProjector::name);
        const auto capability = projector == nominal.object_projectors.end()
                                    ? FunctionId::invalid()
                                : mutable_ref ? projector->borrow_mut_function
                                              : projector->borrow_function;
        if (!capability.hasValue()) {
          emit(DiagnosticKind::ProjectionCapabilityUnavailable, node);
          return appendInvalid(block, node);
        }
      }
      const auto type = sem_ir_.addReferenceType(
          instType(operand), mutable_ref ? SemReferenceMutability::Mutable
                                         : SemReferenceMutability::ReadOnly);
      return appendInst<SemBorrowPlace>(block, node, type, operand);
    }
    if (operator_kind == TokenKind::Bang) {
      auto operand = checkExpression(children.front(), block);
      if (!adjustExpression(operand, sem_ir_.boolType(), children.front(),
                            block))
        emit(DiagnosticKind::TypeMismatch, children.front());
      return appendInst<SemLogicalNot>(block, node, sem_ir_.boolType(),
                                       operand);
    }
    if (operator_kind == TokenKind::Plus || operator_kind == TokenKind::Minus ||
        operator_kind == TokenKind::Tilde) {
      auto operand = checkExpression(children.front(), block, expected_type);
      while (sem_ir_.type(instType(operand)).kind == SemTypeKind::Reference)
        operand = acquireCheckedReference(operand, children.front(), block);
      const auto kind = sem_ir_.type(instType(operand)).kind;
      if (kind == SemTypeKind::Nominal || kind == SemTypeKind::TypeParameter) {
        if (const auto protocol = unaryOperatorProtocol(operator_kind))
          return checkOperatorProtocol(node, *protocol, operand, std::nullopt,
                                       children.front(), std::nullopt, block);
      }
      if ((operator_kind == TokenKind::Tilde && kind != SemTypeKind::Integer) ||
          (operator_kind != TokenKind::Tilde && kind != SemTypeKind::Integer &&
           kind != SemTypeKind::Float)) {
        emit(DiagnosticKind::TypeMismatch, node);
        return appendInvalid(block, node);
      }
      const auto operation =
          operator_kind == TokenKind::Plus    ? BuiltinOperatorKind::Positive
          : operator_kind == TokenKind::Minus ? BuiltinOperatorKind::Negate
                                              : BuiltinOperatorKind::BitNot;
      return appendBuiltinUnary(block, node, instType(operand), operation,
                                operand);
    }
    emit(DiagnosticKind::TypeMismatch, node);
    return appendInvalid(block, node);
  }
  case NodeKind::CallExpr:
    return checkCallExpression(node, block);
  case NodeKind::ArrayLiteral: {
    std::vector<InstId> elements;
    for (const auto child : children) {
      const auto element = checkExpression(child, block);
      elements.push_back(element);
    }
    if (elements.empty()) {
      emit(DiagnosticKind::InvalidArrayLiteral, node);
      return appendInvalid(block, node);
    }
    const auto element_type = convergeExpressionTypes(elements, {}, node);
    if (!element_type.hasValue())
      return appendInvalid(block, node);
    for (std::size_t index = 0; index < elements.size(); ++index)
      if (!adjustExpression(elements[index], element_type, children[index],
                            block)) {
        emit(DiagnosticKind::TypeMismatch, children[index]);
        return appendInvalid(block, node);
      }
    const auto type =
        sem_ir_.addType({SemTypeKind::Array, element_type.index,
                         static_cast<std::uint32_t>(elements.size()), 0});
    const auto values = sem_ir_.addInstBlock(elements, true);
    return appendInst<SemArrayLiteral>(block, node, type, values);
  }
  case NodeKind::TupleLiteral: {
    std::vector<InstId> elements;
    std::vector<TypeId> element_types;
    for (const auto child : children) {
      const auto element = checkExpression(child, block);
      elements.push_back(element);
      element_types.push_back(instType(element));
    }
    const auto type = sem_ir_.addTupleType(element_types);
    const auto values = sem_ir_.addInstBlock(elements, true);
    return appendInst<SemTupleLiteral>(block, node, type, values);
  }
  case NodeKind::CallbackAdapterLiteral: {
    (void)requireUnsafe(UnsafeOperationKind::CallbackAdapterConstruction, node);
    std::array<InstId, 3> values{InstId::invalid(), InstId::invalid(),
                                 InstId::invalid()};
    for (const auto child : children) {
      const auto field_children = tree_.children(child);
      if (tree_.kind(child) != NodeKind::NamedFieldInit ||
          field_children.size() != 2)
        continue;
      const auto name = values_.identifier(tokenString(field_children[0]));
      const auto index = name == "entry"     ? 0
                         : name == "context" ? 1
                         : name == "release" ? 2
                                             : -1;
      if (index < 0 || values[static_cast<std::size_t>(index)].hasValue()) {
        emit(DiagnosticKind::InvalidCallbackAdapter, child);
        continue;
      }
      values[static_cast<std::size_t>(index)] =
          checkExpression(field_children[1], block);
    }
    if (std::ranges::any_of(values,
                            [](InstId value) { return !value.hasValue(); }))
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
    return appendInst<SemMakeCallbackAdapter>(
        block, node, sem_ir_.voidType(), sem_ir_.addInstBlock(values, true));
  }
  case NodeKind::AggregateLiteral: {
    if (children.empty())
      return appendInvalid(block, node);
    const auto aggregate_type = tree_.kind(children.front()) == NodeKind::QualifiedName
        ? resolveNominalTypeComponents(tree_.children(children.front()), children.front())
        : checkType(children.front());
    if (!aggregate_type.hasValue()) {
      emit(DiagnosticKind::UnknownType, children.front());
      return appendInvalid(block, node);
    }
    const auto &type = sem_ir_.type(aggregate_type);
    if (type.kind != SemTypeKind::Nominal) {
      emit(DiagnosticKind::UnknownType, children.front());
      return appendInvalid(block, node);
    }
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(type.arg0));
    const auto field_accessible = [&](const SemNominalField &field) {
      return !nominal.canonical_entity.hasValue() || (field.flags & 1U) != 0;
    };
    const bool has_inaccessible_struct_field =
        nominal.kind == NominalKind::Struct &&
        std::ranges::any_of(nominal.fields, [&](const auto &field) {
          return !field_accessible(field);
        });
    std::vector<InstId> values_by_field(nominal.fields.size(),
                                        InstId::invalid());
    std::optional<std::uint32_t> union_member;
    for (std::size_t init_index = 1; init_index < children.size();
         ++init_index) {
      const auto init_children = tree_.children(children[init_index]);
      if (tree_.kind(children[init_index]) != NodeKind::NamedFieldInit ||
          init_children.size() != 2)
        continue;
      const auto field_name = tokenString(init_children[0]);
      std::size_t field_index = 0;
      while (field_index < nominal.fields.size() &&
             sem_ir_.name(nominal.fields[field_index].name).text != field_name)
        ++field_index;
      auto value = checkExpression(init_children[1], block);
      if (field_index == nominal.fields.size()) {
        emit(DiagnosticKind::InvalidMember, init_children[0]);
        continue;
      }
      if (nominal.kind == NominalKind::Union &&
          !field_accessible(nominal.fields[field_index]))
        emit(DiagnosticKind::PrivateMemberAccess, init_children[0]);
      if (values_by_field[field_index].hasValue())
        emit(DiagnosticKind::DuplicateName, init_children[0]);
      const auto expected =
          nominalFieldType(aggregate_type, nominal.fields[field_index]);
      if (!adjustExpression(value, expected, init_children[1], block))
        emit(DiagnosticKind::TypeMismatch, init_children[1]);
      values_by_field[field_index] = value;
      if (nominal.kind == NominalKind::Union)
        union_member = static_cast<std::uint32_t>(field_index);
    }
    if (nominal.kind == NominalKind::Union) {
      if (!union_member || children.size() != 2) {
        emit(DiagnosticKind::ArgumentCountMismatch, node);
        return appendInvalid(block, node);
      }
      return appendInst<SemUnionInit>(
          block, node, aggregate_type, values_by_field[*union_member],
          sem_ir_.addInteger(static_cast<std::int64_t>(*union_member)));
    }
    if (has_inaccessible_struct_field) {
      emit(DiagnosticKind::PrivateAggregateInitialization, node);
      return appendInvalid(block, node);
    }
    if (std::ranges::any_of(values_by_field,
                            [](InstId value) { return !value.hasValue(); })) {
      emit(DiagnosticKind::ArgumentCountMismatch, node);
      return appendInvalid(block, node);
    }
    return appendInst<SemAggregateInit>(
        block, node, aggregate_type,
        sem_ir_.addInstBlock(values_by_field, true));
  }
  case NodeKind::MemberExpr:
    if (children.size() != 2) return appendInvalid(block, node);
    return checkMemberValue(node, checkExpression(children[0], block), block);
  case NodeKind::IndexExpr: {
    if (children.size() != 2)
      return appendInvalid(block, node);
    auto base = checkExpression(children[0], block);
    while (sem_ir_.type(instType(base)).kind == SemTypeKind::Reference)
      base = acquireCheckedReference(base, children[0], block);
    auto index = checkExpression(children[1], block);
    const auto index_type = usesWideSliceIndices(sem_ir_.languageVersion())
                                ? sem_ir_.addIntegerType(64, false)
                                : sem_ir_.i32Type();
    if (!adjustExpression(index, index_type, children[1], block))
      emit(DiagnosticKind::InvalidIndex, node);
    const auto &base_type = sem_ir_.type(instType(base));
    if ((base_type.kind != SemTypeKind::Array &&
         base_type.kind != SemTypeKind::Slice) ||
        instType(index) != index_type) {
      emit(DiagnosticKind::InvalidIndex, node);
      return appendInvalid(block, node);
    }
    return appendInst<SemIndex>(block, node, TypeId(base_type.arg0), base,
                                index);
  }
  case NodeKind::SliceExpr: {
    auto base =
        checkExpression(children.empty() ? node : children.front(), block);
    while (sem_ir_.type(instType(base)).kind == SemTypeKind::Reference)
      base = acquireCheckedReference(
          base, children.empty() ? node : children.front(), block);
    if (expressionCategory(sem_ir_, base) != SemExprCategory::Place) {
      emit(DiagnosticKind::InvalidSliceSource, node);
      return appendInvalid(block, node);
    }
    const auto base_type = instType(base);
    const auto &source_type = sem_ir_.type(base_type);
    if (source_type.kind != SemTypeKind::Array &&
        source_type.kind != SemTypeKind::Slice) {
      emit(DiagnosticKind::InvalidSliceSource, node);
      return appendInvalid(block, node);
    }
    const auto element_type = TypeId(source_type.arg0);
    const auto index_type = usesWideSliceIndices(sem_ir_.languageVersion())
                                ? sem_ir_.addIntegerType(64, false)
                                : sem_ir_.i32Type();
    const auto source_length = [&]() {
      if (source_type.kind == SemTypeKind::Array)
        return appendInst<SemIntegerLiteral>(
            block, node, index_type, sem_ir_.addInteger(source_type.arg1));
      const auto length_name = sem_ir_.addName(values_.internIdentifier("len"));
      return appendInst<SemMemberAccess>(block, node, index_type, base,
                                         length_name);
    }();
    InstId start = appendInst<SemIntegerLiteral>(block, node, index_type,
                                                 sem_ir_.addInteger(0));
    InstId end = source_length;
    for (const auto child : children) {
      if (tree_.kind(child) != NodeKind::SliceStart &&
          tree_.kind(child) != NodeKind::SliceEnd)
        continue;
      const auto bound_children = tree_.children(child);
      if (bound_children.empty())
        continue;
      auto bound = checkExpression(bound_children.front(), block);
      if (!adjustExpression(bound, index_type, bound_children.front(), block)) {
        emit(DiagnosticKind::InvalidIndex, bound_children.front());
        return appendInvalid(block, node);
      }
      if (tree_.kind(child) == NodeKind::SliceStart)
        start = bound;
      else
        end = bound;
    }
    const auto reference =
        sem_ir_.addReferenceType(base_type, SemReferenceMutability::ReadOnly);
    const auto borrowed =
        appendInst<SemBorrowPlace>(block, node, reference, base);
    const std::array operands{borrowed, start, end};
    return appendInst<SemSlice>(block, node,
                                sem_ir_.addSliceType(element_type,
                                                      isMutablePlace(base)),
                                sem_ir_.addInstBlock(operands));
  }
  case NodeKind::BinaryExpr: {
    if (children.size() != 2)
      return appendInvalid(block, node);
    const auto operator_kind = tree_.tokens().get(tree_.token(node)).kind;
    if (operator_kind == TokenKind::AmpAmp)
      return checkShortCircuit(node, true, block);
    if (operator_kind == TokenKind::PipePipe)
      return checkShortCircuit(node, false, block);
    auto left = checkExpression(children[0], block);
    while (sem_ir_.type(instType(left)).kind == SemTypeKind::Reference)
      left = acquireCheckedReference(left, children[0], block);
    auto right = checkExpression(children[1], block);
    while (sem_ir_.type(instType(right)).kind == SemTypeKind::Reference)
      right = acquireCheckedReference(right, children[1], block);
    const auto left_kind = sem_ir_.type(instType(left)).kind;
    if (const auto protocol = binaryOperatorProtocol(operator_kind);
        protocol && (left_kind == SemTypeKind::Nominal ||
                     left_kind == SemTypeKind::TypeParameter))
      return checkOperatorProtocol(node, *protocol, left, right, children[0],
                                   children[1], block);
    if (const auto operation = binaryOperator(operator_kind))
      return checkBuiltinBinary(node, *operation, left, right, block);
    emit(DiagnosticKind::TypeMismatch, node);
    return appendInvalid(block, node);
  }
  default:
    emit(DiagnosticKind::InvalidSemanticShape, node);
    return appendInvalid(block, node);
  }
}
