// Included by SemanticContextSpecialization.cpp; this fragment aliases the
// single SemanticContext specialization state and preserves queue ordering.
[[nodiscard]] TypeId SemanticContext::convergeExpressionTypes(
    std::span<const InstId> values, TypeId expected_type, NodeId location) {
  if (expected_type.hasValue())
    return expected_type;
  std::vector<TypeId> candidates;
  bool all_numeric = true;
  bool has_float = false;
  for (const auto value : values) {
    const auto candidate = instType(value);
    if (candidate == sem_ir_.neverType() ||
        std::ranges::find(candidates, candidate) != candidates.end())
      continue;
    const auto kind = sem_ir_.type(candidate).kind;
    all_numeric &= kind == SemTypeKind::Integer || kind == SemTypeKind::Float;
    has_float |= kind == SemTypeKind::Float;
    candidates.push_back(candidate);
  }
  if (all_numeric) {
    if (has_float) {
      candidates.push_back(sem_ir_.addFloatType(32));
      candidates.push_back(sem_ir_.f64Type());
    } else {
      for (const auto width : {8U, 16U, 32U, 64U}) {
        candidates.push_back(sem_ir_.addIntegerType(width, true));
        candidates.push_back(sem_ir_.addIntegerType(width, false));
      }
    }
  }
  std::ranges::sort(candidates, {}, &TypeId::index);
  candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());
  struct Viable {
    TypeId type;
    std::uint32_t cost;
  };
  std::vector<Viable> viable;
  for (const auto candidate : candidates) {
    std::uint32_t cost = 0;
    bool valid = true;
    for (const auto value : values) {
      if (instType(value) == sem_ir_.neverType())
        continue;
      const auto conversion = queryConversion(value, candidate);
      if (!conversion.valid()) {
        valid = false;
        break;
      }
      cost += conversion.rank;
    }
    if (valid)
      viable.push_back({candidate, cost});
  }
  if (!viable.empty()) {
    const auto minimum_cost = std::ranges::min(viable, {}, &Viable::cost).cost;
    std::erase_if(viable, [&](const Viable &value) {
      return value.cost != minimum_cost;
    });
    std::vector<Viable> minimal;
    for (const auto candidate : viable)
      if (!std::ranges::any_of(viable, [&](const Viable &other) {
            return other.type != candidate.type &&
                   losslessNumericConversion(other.type, candidate.type) &&
                   !losslessNumericConversion(candidate.type, other.type);
          }))
        minimal.push_back(candidate);
    if (minimal.size() == 1)
      return minimal.front().type;
  }
  emit(DiagnosticKind::TypeMismatch, location);
  return TypeId::invalid();
}

[[nodiscard]] std::optional<BuiltinOperatorKind>
SemanticContext::binaryOperator(TokenKind token) {
  switch (token) {
  case TokenKind::Plus:
    return BuiltinOperatorKind::Add;
  case TokenKind::Minus:
    return BuiltinOperatorKind::Subtract;
  case TokenKind::Star:
    return BuiltinOperatorKind::Multiply;
  case TokenKind::Slash:
    return BuiltinOperatorKind::Divide;
  case TokenKind::Percent:
    return BuiltinOperatorKind::Remainder;
  case TokenKind::LessLess:
    return BuiltinOperatorKind::ShiftLeft;
  case TokenKind::GreaterGreater:
    return BuiltinOperatorKind::ShiftRight;
  case TokenKind::Amp:
    return BuiltinOperatorKind::BitAnd;
  case TokenKind::Caret:
    return BuiltinOperatorKind::BitXor;
  case TokenKind::Pipe:
    return BuiltinOperatorKind::BitOr;
  case TokenKind::EqualEqual:
    return BuiltinOperatorKind::Equal;
  case TokenKind::BangEqual:
    return BuiltinOperatorKind::NotEqual;
  case TokenKind::Less:
    return BuiltinOperatorKind::Less;
  case TokenKind::LessEqual:
    return BuiltinOperatorKind::LessEqual;
  case TokenKind::Greater:
    return BuiltinOperatorKind::Greater;
  case TokenKind::GreaterEqual:
    return BuiltinOperatorKind::GreaterEqual;
  case TokenKind::Spaceship:
    return BuiltinOperatorKind::ThreeWay;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<BuiltinOperatorKind>
SemanticContext::compoundOperator(TokenKind token) {
  switch (token) {
  case TokenKind::PlusEqual:
    return BuiltinOperatorKind::Add;
  case TokenKind::MinusEqual:
    return BuiltinOperatorKind::Subtract;
  case TokenKind::StarEqual:
    return BuiltinOperatorKind::Multiply;
  case TokenKind::SlashEqual:
    return BuiltinOperatorKind::Divide;
  case TokenKind::PercentEqual:
    return BuiltinOperatorKind::Remainder;
  case TokenKind::AmpEqual:
    return BuiltinOperatorKind::BitAnd;
  case TokenKind::CaretEqual:
    return BuiltinOperatorKind::BitXor;
  case TokenKind::PipeEqual:
    return BuiltinOperatorKind::BitOr;
  case TokenKind::LessLessEqual:
    return BuiltinOperatorKind::ShiftLeft;
  case TokenKind::GreaterGreaterEqual:
    return BuiltinOperatorKind::ShiftRight;
  default:
    return std::nullopt;
  }
}
