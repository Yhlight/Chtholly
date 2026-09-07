// Included by SemanticContextSpecialization.cpp; this fragment aliases the
// single SemanticContext specialization state and preserves queue ordering.
[[nodiscard]] InstId SemanticContext::checkEnumConstruction(
    NodeId node, const SemanticContext::ResolvedEnumVariant &resolved,
    std::vector<InstId> &block) {
  const auto &semantic_type = sem_ir_.type(resolved.type);
  const auto &nominal = sem_ir_.nominalType(NominalTypeId(semantic_type.arg0));
  if (resolved.variant >= nominal.variants.size()) {
    emit(DiagnosticKind::InvalidEnumVariant, node);
    return appendInvalid(block, node);
  }
  const auto &variant = nominal.variants[resolved.variant];
  if (nominal.is_value_enum && tree_.kind(node) == NodeKind::EnumLiteral) {
    emit(DiagnosticKind::ValueEnumBracedConstruction, node);
    return appendInvalid(block, node);
  }
  const auto children = tree_.children(node);
  std::size_t payload_begin = children.size();
  if (tree_.kind(node) == NodeKind::EnumLiteral) {
    payload_begin = 1;
    if (!children.empty() &&
        tree_.kind(children.front()) == NodeKind::GenericType)
      payload_begin = 2;
  }
  std::vector<InstId> payload(variant.fields.size(), InstId::invalid());
  if (variant.shape == SemEnumPayloadShape::Struct) {
    for (std::size_t index = payload_begin; index < children.size(); ++index) {
      const auto init_children = tree_.children(children[index]);
      if (tree_.kind(children[index]) != NodeKind::NamedFieldInit ||
          init_children.size() != 2) {
        emit(DiagnosticKind::InvalidEnumVariant, children[index]);
        continue;
      }
      const auto field_name = tokenString(init_children.front());
      std::size_t field = 0;
      while (field < variant.fields.size() &&
             sem_ir_.name(variant.fields[field].name).text != field_name)
        ++field;
      auto value = checkExpression(init_children.back(), block);
      if (field == variant.fields.size()) {
        emit(DiagnosticKind::InvalidMember, init_children.front());
        continue;
      }
      if (payload[field].hasValue())
        emit(DiagnosticKind::DuplicateName, init_children.front());
      const auto expected =
          nominalFieldType(resolved.type, variant.fields[field]);
      if (!adjustExpression(value, expected, init_children.back(), block))
        emit(DiagnosticKind::TypeMismatch, init_children.back());
      payload[field] = value;
    }
  } else {
    std::size_t field = 0;
    for (std::size_t index = payload_begin; index < children.size(); ++index) {
      if (tree_.kind(children[index]) == NodeKind::NamedFieldInit ||
          field >= variant.fields.size()) {
        emit(DiagnosticKind::InvalidEnumVariant, children[index]);
        continue;
      }
      auto value = checkExpression(children[index], block);
      const auto expected =
          nominalFieldType(resolved.type, variant.fields[field]);
      if (!adjustExpression(value, expected, children[index], block))
        emit(DiagnosticKind::TypeMismatch, children[index]);
      payload[field++] = value;
    }
  }
  // `void` is the canonical unit payload used by Result<void, E>::Ok.  The
  // source spelling `Ok {}` intentionally supplies no expression; materialize
  // the unit value so the enum payload remains structurally one-field and
  // generic/result machinery can use the same representation as other T.
  for (std::size_t index = 0; index < payload.size(); ++index) {
    if (payload[index].hasValue())
      continue;
    const auto expected = nominalFieldType(resolved.type, variant.fields[index]);
    if (expected == sem_ir_.voidType())
      payload[index] = appendInst<SemVoidValue>(block, node, expected);
  }
  if (std::ranges::any_of(payload,
                          [](InstId value) { return !value.hasValue(); })) {
    emit(DiagnosticKind::ArgumentCountMismatch, node);
    return appendInvalid(block, node);
  }
  return appendInst<SemEnumInit>(
      block, node, resolved.type, sem_ir_.addInstBlock(payload, true),
      sem_ir_.addInteger(static_cast<std::int64_t>(resolved.variant)));
}

[[nodiscard]] std::optional<SemanticContext::OrderingShape>
SemanticContext::inspectOrderingShape(TypeId type, NodeId location,
                                      DiagnosticKind diagnostic) {
  const auto entity = sem_ir_.importIRs().registry().findEntity(
      "std", "std::compare", "Ordering", PublicEntityKind::NominalType);
  const auto &semantic_type = sem_ir_.type(type);
  if (!entity.hasValue() || semantic_type.kind != SemTypeKind::Nominal) {
    emit(diagnostic, location);
    return std::nullopt;
  }
  const auto &nominal = sem_ir_.nominalType(NominalTypeId(semantic_type.arg0));
  if (nominal.canonical_entity != entity || nominal.kind != NominalKind::Enum ||
      nominal.variants.size() != 4) {
    emit(diagnostic, location);
    return std::nullopt;
  }
  constexpr std::array<std::string_view, 4> names{"Less", "Equal", "Greater",
                                                  "Unordered"};
  OrderingShape shape{type, {}};
  shape.variants.fill(core::AnyId::InvalidIndex);
  for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
    const auto &variant = nominal.variants[index];
    if (variant.shape != SemEnumPayloadShape::Unit || !variant.fields.empty())
      continue;
    const auto name = values_.identifier(sem_ir_.name(variant.name).text);
    for (std::uint32_t expected = 0; expected < names.size(); ++expected)
      if (name == names[expected])
        shape.variants[expected] = index;
  }
  if (std::ranges::any_of(shape.variants, [](std::uint32_t value) {
        return value == core::AnyId::InvalidIndex;
      })) {
    emit(diagnostic, location);
    return std::nullopt;
  }
  return shape;
}

[[nodiscard]] std::optional<SemanticContext::OrderingShape>
SemanticContext::orderingShape(NodeId location) {
  const auto module = values_.internIdentifier("std::compare");
  if (!sem_ir_.importIRs().findByModule(module).hasValue()) {
    emit(DiagnosticKind::MissingOrderingImport, location);
    return std::nullopt;
  }
  const auto entity = sem_ir_.importIRs().registry().findEntity(
      "std", "std::compare", "Ordering", PublicEntityKind::NominalType);
  if (!entity.hasValue()) {
    emit(DiagnosticKind::MissingOrderingImport, location);
    return std::nullopt;
  }
  const auto id = materializeImportedNominal(entity, location);
  if (!id.hasValue())
    return std::nullopt;
  return inspectOrderingShape(sem_ir_.addNominalType(id), location,
                              DiagnosticKind::MissingOrderingImport);
}

[[nodiscard]] std::optional<SemanticContext::CheckedCastShape>
SemanticContext::checkedCastShape(TypeId target, NodeId location) {
  const auto result_module = values_.internIdentifier("std::result");
  const auto convert_module = values_.internIdentifier("std::convert");
  if (!sem_ir_.importIRs().findByModule(result_module).hasValue() ||
      !sem_ir_.importIRs().findByModule(convert_module).hasValue()) {
    emit(DiagnosticKind::MissingCheckedCastImports, location);
    return std::nullopt;
  }
  const auto result_entity = sem_ir_.importIRs().registry().findEntity(
      "std", "std::result", "Result", PublicEntityKind::NominalType);
  const auto error_entity = sem_ir_.importIRs().registry().findEntity(
      "std", "std::convert", "CastError", PublicEntityKind::NominalType);
  if (!result_entity.hasValue() || !error_entity.hasValue()) {
    emit(DiagnosticKind::MissingCheckedCastImports, location);
    return std::nullopt;
  }
  const auto result_nominal =
      materializeImportedNominal(result_entity, location);
  const auto error_nominal = materializeImportedNominal(error_entity, location);
  if (!result_nominal.hasValue() || !error_nominal.hasValue())
    return std::nullopt;
  const auto &error_definition = sem_ir_.nominalType(error_nominal);
  std::array<bool, 3> error_variants{};
  for (const auto &variant : error_definition.variants) {
    if (variant.shape != SemEnumPayloadShape::Unit || !variant.fields.empty())
      continue;
    const auto name = values_.identifier(sem_ir_.name(variant.name).text);
    if (name == "Inexact")
      error_variants[0] = true;
    else if (name == "OutOfRange")
      error_variants[1] = true;
    else if (name == "NonFinite")
      error_variants[2] = true;
  }
  if (error_definition.kind != NominalKind::Enum ||
      error_definition.variants.size() != error_variants.size() ||
      !std::ranges::all_of(error_variants, std::identity{})) {
    emit(DiagnosticKind::MissingCheckedCastImports, location);
    return std::nullopt;
  }
  const auto error = sem_ir_.addNominalType(error_nominal);
  const std::array arguments{target, error};
  const auto result = sem_ir_.addNominalType(result_nominal, arguments);
  const auto shape = sem_ir_.canonicalResultShape(result);
  if (!shape || shape->success != target || shape->error != error) {
    emit(DiagnosticKind::MissingCheckedCastImports, location);
    return std::nullopt;
  }
  return CheckedCastShape{result, error, shape->ok_variant, shape->err_variant};
}

[[nodiscard]] InstId
SemanticContext::checkTryExpression(NodeId node, std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  if (children.size() != 1)
    return appendInvalid(block, node);
  if (defer_depth_ != 0) {
    emit(DiagnosticKind::DeferControlFlowEscape, node);
    return appendInvalid(block, node);
  }
  auto operand = checkExpression(children.front(), block);
  const auto operand_shape = sem_ir_.canonicalResultShape(instType(operand));
  if (!operand_shape) {
    emit(DiagnosticKind::InvalidTryOperand, node);
    return appendInvalid(block, node);
  }
  const auto return_shape = sem_ir_.canonicalResultShape(return_type_);
  if (!return_shape || return_shape->error != operand_shape->error) {
    emit(DiagnosticKind::InvalidTryReturn, node);
    return appendInvalid(block, node);
  }

  const auto operand_type = instType(operand);
  const auto temporary = sem_ir_.addLocal(
      {sem_ir_.addName(values_.internIdentifier("__try_operand")), operand_type,
       node, 0U});
  (void)appendInst<SemBindName>(block, node, sem_ir_.voidType(), temporary,
                                operand);
  const auto scrutinee =
      appendInst<SemNameRef>(block, node, operand_type, temporary);
  const auto payload = [&](std::vector<InstId> &arm, TypeId payload_type,
                           std::uint32_t variant) {
    if (payload_type == sem_ir_.voidType())
      return appendInst<SemVoidValue>(arm, node, payload_type);
    const auto encoded = (static_cast<std::uint64_t>(variant) << 32U);
    const auto access = appendInst<SemEnumPayloadAccess>(
        arm, node, payload_type, scrutinee,
        sem_ir_.addInteger(static_cast<std::int64_t>(encoded)));
    return appendInst<SemMove>(arm, node, payload_type, access);
  };

  std::vector<InstId> ok_body;
  const auto ok_value =
      payload(ok_body, operand_shape->success, operand_shape->ok_variant);
  (void)appendInst<SemYield>(ok_body, node, sem_ir_.voidType(), ok_value);
  const auto ok_arm = sem_ir_.addInst(
      SemSwitchArm{operand_shape->success,
                   sem_ir_.addInteger(operand_shape->ok_variant),
                   sem_ir_.addInstBlock(ok_body)},
      node);

  std::vector<InstId> err_body;
  const auto error =
      payload(err_body, operand_shape->error, operand_shape->err_variant);
  const std::array error_payload{error};
  const auto returned_error = appendInst<SemEnumInit>(
      err_body, node, return_type_, sem_ir_.addInstBlock(error_payload, true),
      sem_ir_.addInteger(return_shape->err_variant));
  appendReturnTerminal(returned_error, node, err_body);
  const auto err_arm = sem_ir_.addInst(
      SemSwitchArm{operand_shape->success,
                   sem_ir_.addInteger(operand_shape->err_variant),
                   sem_ir_.addInstBlock(err_body)},
      node);
  const std::array arms{ok_arm, err_arm};
  return appendInst<SemSwitch>(block, node, operand_shape->success, scrutinee,
                               sem_ir_.addInstBlock(arms, true));
}

[[nodiscard]] TypeId
SemanticContext::ensureResultOutcomeType(TypeId result_type) {
  const auto shape = sem_ir_.canonicalResultShape(result_type);
  if (!shape)
    return TypeId::invalid();
  const auto &result = sem_ir_.type(result_type);
  const std::array arguments{sem_ir_.voidType(), shape->error};
  return sem_ir_.addNominalType(NominalTypeId(result.arg0), arguments);
}

bool SemanticContext::addEnumPatternCoverage(
    SemanticContext::EnumPatternCoverage &coverage,
    std::optional<std::uint32_t> variant, NodeId location) {
  if (coverage.wildcard) {
    emit(DiagnosticKind::UnreachableSwitchArm, location);
    return false;
  }
  if (!variant) {
    coverage.wildcard = true;
    return true;
  }
  if (*variant >= coverage.covered.size())
    return false;
  if (coverage.covered[*variant]) {
    emit(DiagnosticKind::DuplicateSwitchArm, location);
    return false;
  }
  coverage.covered[*variant] = true;
  return true;
}

[[nodiscard]] InstId
SemanticContext::checkSwitchExpression(NodeId node, std::vector<InstId> &block,
                                       TypeId expected_type) {
  const auto children = tree_.children(node);
  if (children.size() < 2) {
    emit(DiagnosticKind::NonExhaustiveSwitch, node);
    return appendInvalid(block, node);
  }
  auto scrutinee = checkExpression(children.front(), block);
  if (expressionCategory(sem_ir_, scrutinee) == SemExprCategory::Diverging)
    return scrutinee;
  while (sem_ir_.type(instType(scrutinee)).kind == SemTypeKind::Reference)
    scrutinee = acquireCheckedReference(scrutinee, children.front(), block);
  const auto scrutinee_type = instType(scrutinee);
  // A moved or computed enum is a value. Give it one owning destination
  // before projecting/moving its selected payload; never project a moved source.
  if (!isPlace(scrutinee)) {
    scrutinee = materializeTemporary(scrutinee, node, block);
    if (!scrutinee.hasValue()) return appendInvalid(block, node);
  }
  const auto &type = sem_ir_.type(scrutinee_type);
  if (type.kind != SemTypeKind::Nominal ||
      sem_ir_.nominalType(NominalTypeId(type.arg0)).kind != NominalKind::Enum) {
    emit(DiagnosticKind::TypeMismatch, children.front());
    return appendInvalid(block, node);
  }
  const auto &nominal = sem_ir_.nominalType(NominalTypeId(type.arg0));
  EnumPatternCoverage coverage{std::vector<bool>(nominal.variants.size())};
  struct CheckedSwitchArm {
    NodeId arm_node;
    NodeId body_node;
    IntegerId pattern;
    std::vector<InstId> instructions;
    InstId value;
    bool falls_through = true;
  };
  std::vector<CheckedSwitchArm> checked_arms;
  for (std::size_t arm_index = 1; arm_index < children.size(); ++arm_index) {
    const auto arm_node = children[arm_index];
    const auto arm_children = tree_.children(arm_node);
    if (tree_.kind(arm_node) != NodeKind::SwitchArm || arm_children.size() != 2)
      continue;
    const auto pattern = arm_children.front();
    std::optional<std::uint32_t> variant_index;
    std::size_t binding_begin = 0;
    if (tree_.kind(pattern) == NodeKind::WildcardPattern) {
      binding_begin = tree_.children(pattern).size();
    } else if (tree_.kind(pattern) == NodeKind::EnumPattern) {
      const auto pattern_children = tree_.children(pattern);
      std::optional<ResolvedEnumVariant> resolved;
      if (!pattern_children.empty() &&
          (tree_.kind(pattern_children.front()) == NodeKind::QualifiedType ||
           tree_.kind(pattern_children.front()) == NodeKind::QualifiedName)) {
        resolved = resolveQualifiedEnumVariant(pattern_children.front());
        binding_begin = 1;
      } else if (pattern_children.size() >= 2 &&
                 tree_.kind(pattern_children.front()) ==
                     NodeKind::GenericType &&
                 tree_.kind(pattern_children[1]) == NodeKind::Name) {
        const auto pattern_type = checkType(pattern_children.front());
        const auto &pattern_sem_type = sem_ir_.type(pattern_type);
        if (pattern_sem_type.kind == SemTypeKind::Nominal) {
          const auto &pattern_nominal =
              sem_ir_.nominalType(NominalTypeId(pattern_sem_type.arg0));
          const auto name = tokenString(pattern_children[1]);
          for (std::uint32_t index = 0; index < pattern_nominal.variants.size();
               ++index)
            if (sem_ir_.name(pattern_nominal.variants[index].name).text ==
                name) {
              resolved =
                  ResolvedEnumVariant{pattern_type, index, pattern_children[1]};
              break;
            }
        }
        binding_begin = 2;
      }
      if (!resolved || resolved->type != scrutinee_type) {
        emit(DiagnosticKind::InvalidEnumVariant, pattern);
        continue;
      }
      variant_index = resolved->variant;
      const auto checked_pattern = checkPatternBindings(
          pattern, binding_begin, scrutinee_type, variant_index);
      lexical_scopes_.push();
      std::vector<InstId> arm_body;
      if (checked_pattern.valid)
        for (const auto &binding : checked_pattern.bindings) {
          auto value = emitPatternProjection(scrutinee, *variant_index, binding,
                                             arm_body);
          value = applyPatternTransfer(value, binding, arm_body);
          if (!value.hasValue())
            continue;
          const auto binding_name = nameFor(binding.name);
          if (!binding_name.hasValue()) {
            emit(DiagnosticKind::UnknownName, binding.name);
            continue;
          }
          const auto local = sem_ir_.addLocal(
              {binding_name, instType(value), binding.name, 0U});
          sem_ir_.addLocalOccurrence(
              binding.name, SemSymbolOccurrenceKind::Declaration, local);
          bind(binding_name, local, binding.name);
          (void)appendInst<SemBindName>(arm_body, binding.node,
                                        sem_ir_.voidType(), local, value);
        }
      const auto body_node = arm_children.back();
      InstId result;
      bool falls_through = true;
      if (tree_.kind(body_node) == NodeKind::ValueBlock) {
        auto checked = checkValueBlock(body_node, expected_type);
        arm_body.insert(arm_body.end(), checked.instructions.begin(),
                        checked.instructions.end());
        result = checked.value;
        falls_through = checked.falls_through;
      } else if (tree_.kind(body_node) == NodeKind::Block) {
        checkBlockInto(body_node, arm_body);
        falls_through = blockFallsThrough(sem_ir_, arm_body);
        if (falls_through)
          result =
              appendInst<SemVoidValue>(arm_body, body_node, sem_ir_.voidType());
      } else if (isStatementNode(tree_.kind(body_node))) {
        lexical_scopes_.push();
        checkStatement(body_node, arm_body);
        lexical_scopes_.pop();
        falls_through = blockFallsThrough(sem_ir_, arm_body);
        if (falls_through)
          result =
              appendInst<SemVoidValue>(arm_body, body_node, sem_ir_.voidType());
      } else {
        result = checkExpression(body_node, arm_body, expected_type);
        falls_through =
            expressionCategory(sem_ir_, result) != SemExprCategory::Diverging;
      }
      lexical_scopes_.pop();
      if (falls_through && !result.hasValue())
        emit(DiagnosticKind::TypeMismatch, body_node);
      checked_arms.push_back(
          {arm_node, body_node,
           sem_ir_.addInteger(static_cast<std::int64_t>(*variant_index)),
           std::move(arm_body), result, falls_through});
    } else {
      emit(DiagnosticKind::InvalidEnumVariant, pattern);
      continue;
    }

    if (!variant_index) {
      std::vector<InstId> arm_body;
      const auto body_node = arm_children.back();
      InstId result;
      bool falls_through = true;
      if (tree_.kind(body_node) == NodeKind::ValueBlock) {
        auto checked = checkValueBlock(body_node, expected_type);
        arm_body.insert(arm_body.end(), checked.instructions.begin(),
                        checked.instructions.end());
        result = checked.value;
        falls_through = checked.falls_through;
      } else if (tree_.kind(body_node) == NodeKind::Block) {
        checkBlockInto(body_node, arm_body);
        falls_through = blockFallsThrough(sem_ir_, arm_body);
        if (falls_through)
          result =
              appendInst<SemVoidValue>(arm_body, body_node, sem_ir_.voidType());
      } else if (isStatementNode(tree_.kind(body_node))) {
        lexical_scopes_.push();
        checkStatement(body_node, arm_body);
        lexical_scopes_.pop();
        falls_through = blockFallsThrough(sem_ir_, arm_body);
        if (falls_through)
          result =
              appendInst<SemVoidValue>(arm_body, body_node, sem_ir_.voidType());
      } else {
        result = checkExpression(body_node, arm_body, expected_type);
        falls_through =
            expressionCategory(sem_ir_, result) != SemExprCategory::Diverging;
      }
      if (falls_through && !result.hasValue())
        emit(DiagnosticKind::TypeMismatch, body_node);
      checked_arms.push_back({arm_node, body_node, sem_ir_.addInteger(-1),
                              std::move(arm_body), result, falls_through});
    }
    (void)addEnumPatternCoverage(coverage, variant_index, pattern);
  }
  if (!coverage.wildcard &&
      std::ranges::any_of(coverage.covered, [](bool value) { return !value; }))
    emit(DiagnosticKind::NonExhaustiveSwitch, node);
  if (checked_arms.empty())
    return appendInvalid(block, node);
  std::vector<InstId> live_values;
  for (const auto &arm : checked_arms) {
    if (arm.falls_through && !arm.value.hasValue())
      return appendInvalid(block, node);
    if (arm.falls_through)
      live_values.push_back(arm.value);
  }
  const auto result_type =
      live_values.empty()
          ? sem_ir_.neverType()
          : convergeExpressionTypes(live_values, expected_type, node);
  if (!result_type.hasValue())
    return appendInvalid(block, node);
  std::vector<InstId> arm_instructions;
  for (auto &arm : checked_arms) {
    if (arm.falls_through) {
      if (!adjustExpression(arm.value, result_type, arm.body_node,
                            arm.instructions)) {
        emit(DiagnosticKind::TypeMismatch, arm.body_node);
        return appendInvalid(block, node);
      }
      (void)appendInst<SemYield>(arm.instructions, arm.body_node,
                                 sem_ir_.voidType(), arm.value);
    }
    arm_instructions.push_back(
        sem_ir_.addInst(SemSwitchArm{result_type, arm.pattern,
                                     sem_ir_.addInstBlock(arm.instructions)},
                        arm.arm_node));
  }
  return appendInst<SemSwitch>(block, node, result_type, scrutinee,
                               sem_ir_.addInstBlock(arm_instructions));
}
