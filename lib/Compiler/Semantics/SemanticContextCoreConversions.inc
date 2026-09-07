// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
[[nodiscard]] TypeId SemanticContext::typeForSuffix(NumericSuffix suffix) {
  return conversions_.typeForSuffix(suffix);
}

[[nodiscard]] bool
SemanticContext::integerMagnitudeFits(std::uint64_t magnitude, TypeId target,
                                      bool negative) const {
  return conversions_.integerMagnitudeFits(magnitude, target, negative);
}

[[nodiscard]] bool SemanticContext::integerMagnitudeExactlyRepresentableAsFloat(
    std::uint64_t magnitude, std::uint32_t width) {
  return SemanticConversionPlanner::integerMagnitudeExactlyRepresentableAsFloat(
      magnitude, width);
}

[[nodiscard]] bool
SemanticContext::losslessNumericConversion(TypeId source, TypeId target) const {
  return conversions_.losslessNumericConversion(source, target);
}

[[nodiscard]] ConversionPlan
SemanticContext::queryConversion(InstId value, TypeId target) const {
  return conversions_.query(value, target);
}

[[nodiscard]] bool
SemanticContext::applyConversion(InstId &value, TypeId target, NodeId node,
                                 std::vector<InstId> &block,
                                 SemanticConversionPlan plan) {
  if (!plan.valid())
    return false;
  if (plan.kind == ConversionKind::Identity ||
      plan.kind == ConversionKind::Never)
    return true;
  if (plan.kind == ConversionKind::ContextualInteger) {
    sem_ir_.setInstType(value, target);
    return true;
  }
  if (plan.kind == ConversionKind::ContextualIntegerToFloat) {
    const auto *literal = conversions_.integerLiteral(value);
    assert(literal != nullptr);
    const auto magnitude = literal->magnitude;
    const auto negative = literal->negative;
    const auto width = sem_ir_.type(target).arg0;
    const auto bits = width == 32
                          ? static_cast<std::uint64_t>(
                                std::bit_cast<std::uint32_t>(static_cast<float>(
                                    negative ? -static_cast<double>(magnitude)
                                             : static_cast<double>(magnitude))))
                          : std::bit_cast<std::uint64_t>(
                                negative ? -static_cast<double>(magnitude)
                                         : static_cast<double>(magnitude));
    value = appendInst<SemFloatLiteral>(
        block, node, target,
        sem_ir_.addInteger(std::bit_cast<std::int64_t>(bits)));
    return true;
  }
  if (plan.kind == ConversionKind::ContextualFloat) {
    const auto *literal = conversions_.floatLiteral(value);
    assert(literal != nullptr);
    const auto numeric = literal->value;
    const auto width = sem_ir_.type(target).arg0;
    const auto bits =
        width == 32 ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(
                          static_cast<float>(numeric)))
                    : std::bit_cast<std::uint64_t>(numeric);
    value = appendInst<SemFloatLiteral>(
        block, node, target,
        sem_ir_.addInteger(std::bit_cast<std::int64_t>(bits)));
    return true;
  }
  if (plan.kind == ConversionKind::ContextualNull) {
    sem_ir_.setInstType(value, target);
    return true;
  }
  if (plan.kind == ConversionKind::Numeric) {
    value = appendInst<SemNumericConvert>(block, node, target, value);
    return true;
  }
  if (plan.kind == ConversionKind::PointerAuthority) {
    sem_ir_.setInstType(value, target);
    return true;
  }
  const auto actual = instType(value);
  if (plan.kind == ConversionKind::ValueToReference) {
    const auto &target_type = sem_ir_.type(target);
    if (target_type.kind != SemTypeKind::Reference)
      return false;
    if (!isPlace(value)) {
      // C++-style const-reference binding may materialize a temporary, but
      // the temporary remains owned by the current full expression and can
      // never escape through a returned or stored reference.
      if (sem_ir_.referenceMutability(target) ==
          SemReferenceMutability::Mutable)
        return false;
      value = materializeTemporary(value, node, block);
      if (!value.hasValue())
        return false;
    }
    if (sem_ir_.referenceMutability(target) ==
            SemReferenceMutability::Mutable &&
        !isMutablePlace(value))
      return false;
    value = appendInst<SemBorrowPlace>(block, node, target, value);
    return true;
  }
  if (plan.kind == ConversionKind::ReferenceToValue) {
    auto current = value;
    auto current_type = instType(current);
    for (std::uint8_t depth = 0; depth < plan.reference_depth; ++depth) {
      if (sem_ir_.type(current_type).kind != SemTypeKind::Reference)
        return false;
      const auto pointee = sem_ir_.referencePointee(current_type);
      current = appendInst<SemDereference>(block, node, pointee, current);
      current_type = pointee;
    }
    value = current;
    return current_type == target;
  }
  if (plan.kind == ConversionKind::ReferenceAuthority) {
    auto current = value;
    auto current_type = instType(current);
    // A borrow expression already carries the requested pointee and
    // mutability. Re-forming it through `Dereference` + `BorrowPlace` loses
    // the observable BorrowLocal place (resolvePlace intentionally does not
    // treat a borrow value as an addressable root), which made an explicit
    // `var r: const T& = &local` appear to move from an invalid place. Keep
    // the existing borrow node when only the erased provenance differs.
    const auto source_kind = sem_ir_.inst(value).kind;
    if ((source_kind == SemInstKind::BorrowLocal ||
         source_kind == SemInstKind::BorrowPlace) &&
        sem_ir_.type(current_type).kind == SemTypeKind::Reference &&
        sem_ir_.type(target).kind == SemTypeKind::Reference &&
        sem_ir_.canonicalType(sem_ir_.referencePointee(current_type)) ==
            sem_ir_.canonicalType(sem_ir_.referencePointee(target)) &&
        (sem_ir_.referenceMutability(current_type) ==
             sem_ir_.referenceMutability(target) ||
         sem_ir_.referenceMutability(target) ==
             SemReferenceMutability::ReadOnly)) {
      sem_ir_.setInstType(value, target);
      return true;
    }
    // Re-form a borrow from the final place instead of merely changing the
    // type of a borrow instruction. This preserves provenance for nested
    // references and gives the ownership analyzer one canonical path.
    for (std::uint8_t depth = 0; depth < plan.reference_depth; ++depth) {
      if (sem_ir_.type(current_type).kind != SemTypeKind::Reference)
        return false;
      const auto pointee = sem_ir_.referencePointee(current_type);
      current = appendInst<SemDereference>(block, node, pointee, current);
      current_type = pointee;
    }
    if (sem_ir_.type(target).kind != SemTypeKind::Reference ||
        sem_ir_.referencePointee(target) != current_type)
      return false;
    if (sem_ir_.referenceMutability(target) ==
            SemReferenceMutability::Mutable &&
        !isMutablePlace(current))
      return false;
    value = appendInst<SemBorrowPlace>(block, node, target, current);
    return true;
  }
  const auto source_kind = sem_ir_.inst(value).kind;
  if (source_kind == SemInstKind::BorrowLocal ||
      source_kind == SemInstKind::BorrowPlace) {
    sem_ir_.setInstType(value, target);
    return true;
  }
  const auto place = appendInst<SemDereference>(
      block, node, sem_ir_.referencePointee(actual), value);
  value = appendInst<SemBorrowPlace>(block, node, target, place);
  return true;
}

[[nodiscard]] bool
SemanticContext::adjustExpression(InstId &value, TypeId target, NodeId node,
                                  std::vector<InstId> &block) {
  const auto plan = conversions_.query(value, target);
  return plan.valid() ? applyConversion(value, target, node, block, plan)
                      : contextualizeLiteral(value, target);
}

[[nodiscard]] bool SemanticContext::canAdjustExpression(InstId value,
                                                        TypeId target) const {
  return conversions_.query(value, target).valid();
}

[[nodiscard]] bool SemanticContext::isStatementNode(NodeKind kind) {
  switch (kind) {
  case NodeKind::LetStmt:
  case NodeKind::UninitializedVarDecl:
  case NodeKind::UninitializedLetDecl:
  case NodeKind::ReturnStmt:
  case NodeKind::DeferStmt:
  case NodeKind::AssertStmt:
  case NodeKind::UnreachableStmt:
  case NodeKind::IfStmt:
  case NodeKind::WhileStmt:
  case NodeKind::ForStmt:
  case NodeKind::ForeachStmt:
  case NodeKind::DoWhileStmt:
  case NodeKind::BreakStmt:
  case NodeKind::ContinueStmt:
  case NodeKind::AssignmentStmt:
  case NodeKind::UnsafeStmt:
  case NodeKind::BlockStmt:
  case NodeKind::SwitchStmt:
  case NodeKind::ExprStmt:
    return true;
  default:
    return false;
  }
}
