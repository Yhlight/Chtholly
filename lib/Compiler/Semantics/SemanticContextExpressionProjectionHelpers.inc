// Included by SemanticContextExpressionCheck.inc; this fragment aliases the
// same expression-checking context and preserves pattern diagnostics.
[[nodiscard]] bool SemanticContext::resolvePatternProjectionTail(
    std::span<const NodeId> projections, std::size_t begin,
    TypeId &current_type,
    std::vector<SemanticContext::PatternProjectionStep> &steps,
    std::vector<std::uint32_t> &canonical_path) {
  for (std::size_t index = begin; index < projections.size(); ++index) {
    const auto projection = projections[index];
    const auto &type = sem_ir_.type(current_type);
    if (tree_.kind(projection) == NodeKind::IntegerLiteral &&
        type.kind == SemTypeKind::Array) {
      std::uint32_t element = 0;
      const auto text = tree_.tokens().text(tree_.token(projection));
      if (std::from_chars(text.data(), text.data() + text.size(), element).ec !=
              std::errc{} ||
          element >= type.arg1) {
        emit(DiagnosticKind::InvalidIndex, projection);
        return false;
      }
      current_type = TypeId(type.arg0);
      steps.push_back({PatternProjectionStep::Kind::StaticIndex, element,
                       projection, current_type});
      canonical_path.push_back(element);
      continue;
    }
    if (tree_.kind(projection) == NodeKind::IntegerLiteral &&
        type.kind == SemTypeKind::Tuple) {
      std::uint32_t element = 0;
      const auto text = tree_.tokens().text(tree_.token(projection));
      if (std::from_chars(text.data(), text.data() + text.size(), element).ec !=
              std::errc{} ||
          element >= sem_ir_.tupleArity(current_type)) {
        emit(DiagnosticKind::InvalidIndex, projection);
        return false;
      }
      current_type = sem_ir_.tupleElementType(current_type, element);
      steps.push_back({PatternProjectionStep::Kind::Field, element, projection,
                       current_type});
      canonical_path.push_back(element);
      continue;
    }
    if (tree_.kind(projection) == NodeKind::Name &&
        type.kind == SemTypeKind::Nominal) {
      const auto &nominal = sem_ir_.nominalType(NominalTypeId(type.arg0));
      if (nominal.kind != NominalKind::Struct) {
        emit(DiagnosticKind::TypeMismatch, projection);
        return false;
      }
      const auto field_name = tokenString(projection);
      std::uint32_t field = core::AnyId::InvalidIndex;
      for (std::uint32_t candidate = 0; candidate < nominal.fields.size();
           ++candidate)
        if (sem_ir_.name(nominal.fields[candidate].name).text == field_name) {
          field = candidate;
          break;
        }
      if (field == core::AnyId::InvalidIndex) {
        emit(DiagnosticKind::InvalidMember, projection);
        return false;
      }
      if (nominal.canonical_entity.hasValue() &&
          (nominal.fields[field].flags & 1U) == 0) {
        emit(DiagnosticKind::PrivateMemberAccess, projection);
        return false;
      }
      current_type = nominalFieldType(current_type, nominal.fields[field]);
      steps.push_back({PatternProjectionStep::Kind::Field, field, projection,
                       current_type});
      canonical_path.push_back(field);
      continue;
    }
    emit(DiagnosticKind::TypeMismatch, projection);
    return false;
  }
  return true;
}

[[nodiscard]] bool SemanticContext::patternPathCoversType(
    TypeId type, std::vector<std::uint32_t> &prefix,
    const std::vector<SemanticContext::CheckedPatternBinding> &bindings,
    std::uint32_t depth) {
  if (depth > 128)
    return false;
  for (const auto &binding : bindings)
    if (binding.canonical_path == prefix)
      return true;
  const auto &value = sem_ir_.type(type);
  if (value.kind == SemTypeKind::Array) {
    for (std::uint32_t index = 0; index < value.arg1; ++index) {
      prefix.push_back(index);
      const auto covered = patternPathCoversType(TypeId(value.arg0), prefix,
                                                 bindings, depth + 1);
      prefix.pop_back();
      if (!covered)
        return false;
    }
    return true;
  }
  if (value.kind == SemTypeKind::Tuple) {
    for (std::uint32_t index = 0; index < sem_ir_.tupleArity(type); ++index) {
      prefix.push_back(index);
      const auto covered = patternPathCoversType(
          sem_ir_.tupleElementType(type, index), prefix, bindings, depth + 1);
      prefix.pop_back();
      if (!covered)
        return false;
    }
    return true;
  }
  if (value.kind == SemTypeKind::Nominal) {
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(value.arg0));
    if (nominal.kind != NominalKind::Struct)
      return false;
    for (std::uint32_t index = 0; index < nominal.fields.size(); ++index) {
      prefix.push_back(index);
      const auto covered =
          patternPathCoversType(nominalFieldType(type, nominal.fields[index]),
                                prefix, bindings, depth + 1);
      prefix.pop_back();
      if (!covered)
        return false;
    }
    return true;
  }
  return false;
}
