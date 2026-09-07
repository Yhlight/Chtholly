// Included by SemanticContextExpressions.cpp; this fragment aliases the
// shared SemanticContext state and preserves expression phase ordering.
[[nodiscard]] InstId
SemanticContext::appendBuiltinUnary(std::vector<InstId> &block, NodeId node,
                                    TypeId type, BuiltinOperatorKind operation,
                                    InstId operand) {
  const auto &operand_inst = sem_ir_.inst(operand);
  const auto &operand_type = sem_ir_.type(type);
  if (operation == BuiltinOperatorKind::Negate &&
      operand_inst.kind == SemInstKind::IntegerLiteral &&
      operand_type.kind == SemTypeKind::Integer) {
    const llvm::APInt value(operand_type.arg0,
                            static_cast<std::uint64_t>(
                                sem_ir_.integer(IntegerId(operand_inst.arg0))));
    const llvm::APInt zero(operand_type.arg0, 0);
    bool overflow = false;
    (void)(operand_type.arg1 != 0 ? zero.ssub_ov(value, overflow)
                                  : zero.usub_ov(value, overflow));
    if (overflow) {
      emit(DiagnosticKind::IntegerOverflow, node);
      return appendInvalid(block, node);
    }
  }
  return appendInst<SemBuiltinUnary>(
      block, node, type, operand,
      sem_ir_.addInteger(static_cast<std::int64_t>(operation)));
}

[[nodiscard]] InstId
SemanticContext::appendBuiltinBinary(std::vector<InstId> &block, NodeId node,
                                     TypeId type, BuiltinOperatorKind operation,
                                     InstId left, InstId right) {
  const std::array operands{left, right};
  return appendInst<SemBuiltinBinary>(
      block, node, type, sem_ir_.addInstBlock(operands, true),
      sem_ir_.addInteger(static_cast<std::int64_t>(operation)));
}

[[nodiscard]] std::optional<llvm::APInt>
SemanticContext::integerLiteralBits(InstId value) const {
  const auto &inst = sem_ir_.inst(value);
  const auto &type = sem_ir_.type(instType(value));
  if (inst.kind != SemInstKind::IntegerLiteral ||
      type.kind != SemTypeKind::Integer)
    return std::nullopt;
  return llvm::APInt(type.arg0, static_cast<std::uint64_t>(
                                    sem_ir_.integer(IntegerId(inst.arg0))));
}

[[nodiscard]] bool SemanticContext::validateConstantBinary(
    BuiltinOperatorKind operation, InstId left, InstId right, NodeId node) {
  const auto lhs = integerLiteralBits(left);
  const auto rhs = integerLiteralBits(right);
  if (!lhs || !rhs)
    return true;
  const auto &left_type = sem_ir_.type(instType(left));
  bool overflow = false;
  switch (operation) {
  case BuiltinOperatorKind::Add:
    (void)(left_type.arg1 != 0 ? lhs->sadd_ov(*rhs, overflow)
                               : lhs->uadd_ov(*rhs, overflow));
    break;
  case BuiltinOperatorKind::Subtract:
    (void)(left_type.arg1 != 0 ? lhs->ssub_ov(*rhs, overflow)
                               : lhs->usub_ov(*rhs, overflow));
    break;
  case BuiltinOperatorKind::Multiply:
    (void)(left_type.arg1 != 0 ? lhs->smul_ov(*rhs, overflow)
                               : lhs->umul_ov(*rhs, overflow));
    break;
  case BuiltinOperatorKind::Divide:
  case BuiltinOperatorKind::Remainder:
    if (rhs->isZero()) {
      emit(operation == BuiltinOperatorKind::Divide
               ? DiagnosticKind::DivisionByZero
               : DiagnosticKind::RemainderByZero,
           node);
      return false;
    }
    overflow =
        left_type.arg1 != 0 && lhs->isMinSignedValue() && rhs->isAllOnes();
    break;
  case BuiltinOperatorKind::ShiftLeft:
  case BuiltinOperatorKind::ShiftRight: {
    const auto &right_type = sem_ir_.type(instType(right));
    if ((right_type.arg1 != 0 && rhs->isNegative()) ||
        rhs->getActiveBits() > 64 || rhs->getZExtValue() >= left_type.arg0) {
      emit(DiagnosticKind::ShiftOutOfRange, node);
      return false;
    }
    if (operation == BuiltinOperatorKind::ShiftLeft) {
      const auto amount = static_cast<unsigned>(rhs->getZExtValue());
      (void)(left_type.arg1 != 0 ? lhs->sshl_ov(amount, overflow)
                                 : lhs->ushl_ov(amount, overflow));
    }
    break;
  }
  default:
    return true;
  }
  if (overflow) {
    emit(DiagnosticKind::IntegerOverflow, node);
    return false;
  }
  return true;
}

[[nodiscard]] InstId SemanticContext::checkBuiltinBinary(
    NodeId node, BuiltinOperatorKind operation, InstId left, InstId right,
    std::vector<InstId> &block, bool destination_typed) {
  const auto left_kind = sem_ir_.type(instType(left)).kind;
  const auto right_kind = sem_ir_.type(instType(right)).kind;
  const auto shift = isBuiltinShiftOperator(operation);
  const auto equality = isBuiltinEqualityOperator(operation);
  const auto comparison = isBuiltinComparisonOperator(operation);
  TypeId operand_type;
  if (shift) {
    if (left_kind != SemTypeKind::Integer ||
        right_kind != SemTypeKind::Integer) {
      emit(DiagnosticKind::TypeMismatch, node);
      return appendInvalid(block, node);
    }
    operand_type = instType(left);
  } else if (destination_typed) {
    operand_type = instType(left);
    if (!adjustExpression(right, operand_type, node, block)) {
      emit(DiagnosticKind::TypeMismatch, node);
      return appendInvalid(block, node);
    }
  } else {
    const std::array operands{left, right};
    operand_type = convergeExpressionTypes(operands, {}, node);
    if (!operand_type.hasValue() ||
        !adjustExpression(left, operand_type, node, block) ||
        !adjustExpression(right, operand_type, node, block)) {
      emit(DiagnosticKind::TypeMismatch, node);
      return appendInvalid(block, node);
    }
  }
  const auto kind = sem_ir_.type(operand_type).kind;
  const auto numeric = kind == SemTypeKind::Integer ||
                       kind == SemTypeKind::Float || kind == SemTypeKind::Char;
  const auto invalid_integer_only =
      isBuiltinIntegerOnlyOperator(operation) && kind != SemTypeKind::Integer;
  const auto invalid_arithmetic =
      !comparison && !isBuiltinIntegerOnlyOperator(operation) && !numeric;
  if (invalid_integer_only || invalid_arithmetic) {
    emit(DiagnosticKind::TypeMismatch, node);
    return appendInvalid(block, node);
  }
  if (comparison && !numeric) {
    const auto allowed_equality =
        equality &&
        (kind == SemTypeKind::Bool || kind == SemTypeKind::RawPointer ||
         kind == SemTypeKind::CFunctionPointer ||
         kind == SemTypeKind::CVariadicFunctionPointer ||
         kind == SemTypeKind::Char);
    if (!allowed_equality) {
      emit(DiagnosticKind::TypeMismatch, node);
      return appendInvalid(block, node);
    }
  }
  if (kind == SemTypeKind::Integer &&
      !validateConstantBinary(operation, left, right, node))
    return appendInvalid(block, node);
  TypeId result_type = operand_type;
  if (comparison)
    result_type = sem_ir_.boolType();
  if (operation == BuiltinOperatorKind::ThreeWay) {
    const auto ordering = orderingShape(node);
    if (!ordering)
      return appendInvalid(block, node);
    result_type = ordering->type;
  }
  return appendBuiltinBinary(block, node, result_type, operation, left, right);
}

[[nodiscard]] std::optional<SemanticContext::ResolvedOperatorProtocol>
SemanticContext::resolveOperatorProtocol(const OperatorProtocol &protocol,
                                         NodeId location) {
  if (!requireVersion(FrozenV13LanguageVersion,
                      DiagnosticKind::OperatorRequires13, location))
    return std::nullopt;
  const auto module = values_.internIdentifier("std::ops");
  if (!sem_ir_.importIRs().findByModule(module).hasValue()) {
    emit(DiagnosticKind::MissingOperatorImport, location);
    return std::nullopt;
  }
  const auto entity = sem_ir_.importIRs().registry().findEntity(
      "std", "std::ops", protocol.interface_name, PublicEntityKind::Interface);
  const auto interface_id = materializeImportedInterface(entity, location);
  if (!interface_id.hasValue()) {
    emit(DiagnosticKind::MissingOperatorImplementation, location);
    return std::nullopt;
  }
  const auto &interface_value = sem_ir_.interface(interface_id);
  for (std::uint32_t index = 0; index < interface_value.requirements.size();
       ++index) {
    const auto &requirement = interface_value.requirements[index];
    if (requirement.kind == SemInterfaceRequirementKind::Function &&
        values_.identifier(sem_ir_.name(requirement.name).text) ==
            protocol.requirement_name)
      return ResolvedOperatorProtocol{interface_id, index};
  }
  emit(DiagnosticKind::MissingOperatorImplementation, location);
  return std::nullopt;
}

[[nodiscard]] bool SemanticContext::borrowOperatorOperand(
    InstId &operand, TypeId value_type, SemReferenceMutability mutability,
    NodeId location, std::vector<InstId> &block) {
  const auto expected = sem_ir_.addReferenceType(value_type, mutability);
  if (sem_ir_.type(instType(operand)).kind != SemTypeKind::Reference &&
      !isPlace(operand)) {
    if (mutability == SemReferenceMutability::Mutable) {
      emit(DiagnosticKind::InvalidMethodReceiver, location);
      return false;
    }
    operand = materializeTemporary(operand, location, block);
    if (!operand.hasValue()) {
      emit(DiagnosticKind::InvalidBorrowOperand, location);
      return false;
    }
  }
  if (!adjustMethodReceiver(operand, expected, location, block))
    return false;
  if (sem_ir_.canonicalType(instType(operand)) !=
      sem_ir_.canonicalType(expected)) {
    emit(DiagnosticKind::TypeMismatch, location);
    return false;
  }
  return true;
}

[[nodiscard]] InstId SemanticContext::boolOr(InstId left, InstId right,
                                             NodeId location,
                                             std::vector<InstId> &block) {
  std::array<std::vector<InstId>, 2> bodies;
  const auto true_value = appendInst<SemBoolLiteral>(
      bodies[0], location, sem_ir_.boolType(), sem_ir_.addInteger(1));
  (void)appendInst<SemYield>(bodies[0], location, sem_ir_.voidType(),
                             true_value);
  (void)appendInst<SemYield>(bodies[1], location, sem_ir_.voidType(), right);
  std::array<InstId, 2> arms;
  for (std::size_t index = 0; index < arms.size(); ++index)
    arms[index] = sem_ir_.addInst(
        SemIfArm{sem_ir_.boolType(), sem_ir_.addInstBlock(bodies[index])},
        location);
  return appendInst<SemIf>(block, location, sem_ir_.boolType(), left,
                           sem_ir_.addInstBlock(arms));
}

[[nodiscard]] InstId SemanticContext::finishOperatorProtocolResult(
    InstId value, const OperatorProtocol &protocol, NodeId location,
    std::vector<InstId> &block) {
  if (protocol.result == OperatorProtocolResult::Direct)
    return value;
  if (protocol.result == OperatorProtocolResult::Void) {
    if (instType(value) != sem_ir_.voidType()) {
      emit(DiagnosticKind::MissingOperatorImplementation, location);
      return appendInvalid(block, location);
    }
    return value;
  }
  if (protocol.result == OperatorProtocolResult::LogicalNot) {
    if (instType(value) != sem_ir_.boolType()) {
      emit(DiagnosticKind::MissingOperatorImplementation, location);
      return appendInvalid(block, location);
    }
    return appendInst<SemLogicalNot>(block, location, sem_ir_.boolType(),
                                     value);
  }
  const auto ordering = inspectOrderingShape(
      instType(value), location, DiagnosticKind::MissingOperatorImplementation);
  if (!ordering)
    return appendInvalid(block, location);
  const auto tag_type = sem_ir_.addIntegerType(32, false);
  const auto tag = appendInst<SemEnumTag>(block, location, tag_type, value);
  const auto is_variant = [&](std::uint32_t variant) {
    const auto expected = appendInst<SemIntegerLiteral>(
        block, location, tag_type, sem_ir_.addInteger(variant));
    return appendInst<SemEqual>(block, location, sem_ir_.boolType(), tag,
                                expected);
  };
  switch (protocol.result) {
  case OperatorProtocolResult::OrderingLess:
    return is_variant(ordering->variants[0]);
  case OperatorProtocolResult::OrderingLessEqual: {
    const auto less = is_variant(ordering->variants[0]);
    const auto equal = is_variant(ordering->variants[1]);
    return boolOr(less, equal, location, block);
  }
  case OperatorProtocolResult::OrderingGreater:
    return is_variant(ordering->variants[2]);
  case OperatorProtocolResult::OrderingGreaterEqual: {
    const auto greater = is_variant(ordering->variants[2]);
    const auto equal = is_variant(ordering->variants[1]);
    return boolOr(greater, equal, location, block);
  }
  default:
    break;
  }
  return appendInvalid(block, location);
}

[[nodiscard]] InstId SemanticContext::checkOperatorProtocol(
    NodeId node, const OperatorProtocol &protocol, InstId left,
    std::optional<InstId> right, NodeId left_node,
    std::optional<NodeId> right_node, std::vector<InstId> &block) {
  const auto resolved = resolveOperatorProtocol(protocol, node);
  if (!resolved)
    return appendInvalid(block, node);
  auto left_type = instType(left);
  while (sem_ir_.type(left_type).kind == SemTypeKind::Reference)
    left_type = sem_ir_.referencePointee(left_type);
  TypeId right_type;
  if (right) {
    right_type = instType(*right);
    while (sem_ir_.type(right_type).kind == SemTypeKind::Reference)
      right_type = sem_ir_.referencePointee(right_type);
  }
  const auto &interface_value = sem_ir_.interface(resolved->interface_id);
  const auto &requirement =
      interface_value.requirements[resolved->requirement_index];
  const auto receiver_mutability =
      protocol.receiver == OperatorProtocolReceiver::Mutable
          ? SemReferenceMutability::Mutable
          : SemReferenceMutability::ReadOnly;
  const auto borrow_operands = [&]() {
    if (protocol.receiver == OperatorProtocolReceiver::Mutable) {
      if (right && !borrowOperatorOperand(*right, right_type,
                                          SemReferenceMutability::ReadOnly,
                                          *right_node, block))
        return false;
      return borrowOperatorOperand(left, left_type, receiver_mutability,
                                   left_node, block);
    }
    if (!borrowOperatorOperand(left, left_type, receiver_mutability, left_node,
                               block))
      return false;
    return !right || borrowOperatorOperand(*right, right_type,
                                           SemReferenceMutability::ReadOnly,
                                           *right_node, block);
  };

  if (sem_ir_.type(left_type).kind == SemTypeKind::TypeParameter &&
      current_function_.hasValue()) {
    const auto constraints = sem_ir_.functionConstraints(current_function_);
    for (std::uint32_t constraint_index = 0;
         constraint_index < constraints.size(); ++constraint_index) {
      const auto &constraint = constraints[constraint_index];
      if (constraint.interface_id != resolved->interface_id ||
          sem_ir_.canonicalType(constraint.subject) !=
              sem_ir_.canonicalType(left_type))
        continue;
      const auto constraint_arguments = sem_ir_.typeBlock(constraint.arguments);
      if ((right && constraint_arguments.size() != 1) ||
          (!right && !constraint_arguments.empty()) ||
          (right && sem_ir_.canonicalType(constraint_arguments.front()) !=
                        sem_ir_.canonicalType(right_type)))
        continue;
      std::vector<CanonicalTypeId> substitutions(
          values_.generics().generic(interface_value.generic).binding_count,
          CanonicalTypeId::invalid());
      for (std::uint32_t index = 0; index < constraint_arguments.size();
           ++index)
        substitutions[index] =
            sem_ir_.canonicalType(constraint_arguments[index]);
      if (interface_value.explicit_parameter_count >= substitutions.size())
        break;
      substitutions[interface_value.explicit_parameter_count] =
          sem_ir_.canonicalType(left_type);
      const auto callable = substituteType(
          requirement.type, interface_value.generic, substitutions);
      if (!callable.hasValue() ||
          sem_ir_.type(callable).kind != SemTypeKind::Function)
        break;
      const auto parameters =
          sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callable).arg0));
      if (parameters.size() != (right ? 2U : 1U))
        break;
      if (!borrow_operands())
        return appendInvalid(block, node);
      std::vector<InstId> arguments{left};
      if (right)
        arguments.push_back(*right);
      for (std::uint32_t index = 0; index < arguments.size(); ++index)
        if (sem_ir_.canonicalType(instType(arguments[index])) !=
            sem_ir_.canonicalType(parameters[index])) {
          emit(DiagnosticKind::MissingOperatorImplementation, node);
          return appendInvalid(block, node);
        }
      const auto encoded =
          (static_cast<std::uint64_t>(constraint_index) << 32U) |
          resolved->requirement_index;
      const auto call = appendInst<SemInterfaceCall>(
          block, node, TypeId(sem_ir_.type(callable).arg1),
          sem_ir_.addInteger(static_cast<std::int64_t>(encoded)),
          sem_ir_.addInstBlock(arguments));
      return finishOperatorProtocolResult(call, protocol, node, block);
    }
    emit(DiagnosticKind::MissingOperatorImplementation, node);
    return appendInvalid(block, node);
  }

  const std::array<TypeId, 1> binary_arguments{right_type};
  const auto witness =
      lookupInterfaceWitness(left_type, resolved->interface_id,
                             right ? std::span<const TypeId>(binary_arguments)
                                   : std::span<const TypeId>{},
                             node);
  if (!witness.hasValue()) {
    emit(DiagnosticKind::MissingOperatorImplementation, node);
    return appendInvalid(block, node);
  }
  const auto &witness_value = sem_ir_.interfaceWitness(witness.witness);
  if (resolved->requirement_index >= witness_value.entries.size() ||
      !witness_value.entries[resolved->requirement_index].function.hasValue()) {
    emit(DiagnosticKind::MissingOperatorImplementation, node);
    return appendInvalid(block, node);
  }
  const auto target =
      witness_value.entries[resolved->requirement_index].function;
  const auto target_type = sem_ir_.functionRef(target).local_type;
  if (sem_ir_.type(target_type).kind != SemTypeKind::Function) {
    emit(DiagnosticKind::MissingOperatorImplementation, node);
    return appendInvalid(block, node);
  }
  const auto parameters =
      sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(target_type).arg0));
  if (parameters.size() != (right ? 2U : 1U) || !borrow_operands()) {
    emit(DiagnosticKind::MissingOperatorImplementation, node);
    return appendInvalid(block, node);
  }
  std::vector<InstId> arguments{left};
  if (right)
    arguments.push_back(*right);
  for (std::uint32_t index = 0; index < arguments.size(); ++index)
    if (sem_ir_.canonicalType(instType(arguments[index])) !=
        sem_ir_.canonicalType(parameters[index])) {
      emit(DiagnosticKind::MissingOperatorImplementation, node);
      return appendInvalid(block, node);
    }
  const auto call =
      appendInst<SemCall>(block, node, TypeId(sem_ir_.type(target_type).arg1),
                          target, sem_ir_.addInstBlock(arguments));
  return finishOperatorProtocolResult(call, protocol, node, block);
}
