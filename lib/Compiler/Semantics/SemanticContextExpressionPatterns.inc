// Included by SemanticContextExpressions.cpp; this fragment aliases the
// shared SemanticContext state and preserves expression phase ordering.
[[nodiscard]] SemanticContext::CheckedPattern
SemanticContext::checkPatternBindings(
    NodeId pattern, std::size_t binding_begin, TypeId source_type,
    std::optional<std::uint32_t> enum_variant) {
  CheckedPattern result;
  const auto children = tree_.children(pattern);
  const SemNominalType *enum_nominal = nullptr;
  const SemEnumVariant *variant = nullptr;
  if (enum_variant) {
    const auto &type = sem_ir_.type(source_type);
    if (type.kind != SemTypeKind::Nominal) {
      result.valid = false;
      return result;
    }
    enum_nominal = &sem_ir_.nominalType(NominalTypeId(type.arg0));
    if (*enum_variant >= enum_nominal->variants.size()) {
      result.valid = false;
      return result;
    }
    variant = &enum_nominal->variants[*enum_variant];
  }
  for (std::size_t child_index = binding_begin; child_index < children.size();
       ++child_index) {
    const auto binding_node = children[child_index];
    if (tree_.kind(binding_node) == NodeKind::PatternRest) {
      if (result.has_rest || child_index + 1 != children.size()) {
        emit(DiagnosticKind::InvalidPatternRest, binding_node);
        result.valid = false;
      }
      result.has_rest = true;
      continue;
    }
    const auto binding_children = tree_.children(binding_node);
    if (tree_.kind(binding_node) != NodeKind::PatternBinding ||
        binding_children.size() != 2) {
      emit(DiagnosticKind::InvalidPatternBinding, binding_node);
      result.valid = false;
      continue;
    }
    const auto projections = tree_.children(binding_children.back());
    if (projections.empty()) {
      emit(DiagnosticKind::InvalidPatternBinding, binding_node);
      result.valid = false;
      continue;
    }
    CheckedPatternBinding binding;
    binding.node = binding_node;
    binding.name = binding_children.front();
    binding.action = tree_.tokens().get(tree_.token(binding_node)).kind;
    if (binding.action != TokenKind::KwMove &&
        binding.action != TokenKind::KwCopy &&
        binding.action != TokenKind::Amp) {
      emit(DiagnosticKind::MissingEnumPayloadTransfer, binding_node);
      result.valid = false;
    }
    TypeId current_type = source_type;
    std::size_t projection_begin = 0;
    if (variant) {
      std::uint32_t field = core::AnyId::InvalidIndex;
      const auto first = projections.front();
      if (tree_.kind(first) == NodeKind::IntegerLiteral) {
        const auto text = tree_.tokens().text(tree_.token(first));
        (void)std::from_chars(text.data(), text.data() + text.size(), field);
      } else if (tree_.kind(first) == NodeKind::Name) {
        const auto name = tokenString(first);
        for (std::uint32_t index = 0; index < variant->fields.size(); ++index)
          if (sem_ir_.name(variant->fields[index].name).text == name) {
            field = index;
            break;
          }
      }
      if (field >= variant->fields.size()) {
        emit(DiagnosticKind::InvalidEnumVariant, first);
        result.valid = false;
        continue;
      }
      current_type = nominalFieldType(source_type, variant->fields[field]);
      binding.steps.push_back({PatternProjectionStep::Kind::EnumPayload, field,
                               first, current_type});
      binding.canonical_path.push_back(field);
      projection_begin = 1;
    }
    if (!resolvePatternProjectionTail(projections, projection_begin,
                                      current_type, binding.steps,
                                      binding.canonical_path)) {
      result.valid = false;
      continue;
    }
    binding.type = current_type;
    for (const auto &previous : result.bindings) {
      const auto common = std::min(previous.canonical_path.size(),
                                   binding.canonical_path.size());
      if (std::equal(previous.canonical_path.begin(),
                     previous.canonical_path.begin() + common,
                     binding.canonical_path.begin())) {
        emit(DiagnosticKind::OverlappingPatternProjection, binding_node);
        result.valid = false;
        break;
      }
    }
    result.bindings.push_back(std::move(binding));
  }
  if (!result.has_rest) {
    bool complete = true;
    std::vector<std::uint32_t> prefix;
    if (variant) {
      for (std::uint32_t field = 0; field < variant->fields.size(); ++field) {
        prefix.push_back(field);
        complete = complete &&
                   patternPathCoversType(
                       nominalFieldType(source_type, variant->fields[field]),
                       prefix, result.bindings);
        prefix.pop_back();
      }
    } else {
      complete = patternPathCoversType(source_type, prefix, result.bindings);
    }
    if (!complete) {
      emit(DiagnosticKind::IncompletePattern, pattern);
      result.valid = false;
    }
  }
  return result;
}

[[nodiscard]] InstId SemanticContext::emitPatternProjection(
    InstId source, std::uint32_t enum_variant,
    const SemanticContext::CheckedPatternBinding &binding,
    std::vector<InstId> &block) {
  auto value = source;
  for (const auto &step : binding.steps) {
    if (step.kind == PatternProjectionStep::Kind::EnumPayload) {
      const auto encoded =
          (static_cast<std::uint64_t>(enum_variant) << 32U) | step.index;
      value = appendInst<SemEnumPayloadAccess>(
          block, step.location, step.type, value,
          sem_ir_.addInteger(static_cast<std::int64_t>(encoded)));
    } else if (step.kind == PatternProjectionStep::Kind::StaticIndex) {
      const auto index =
          appendInst<SemIntegerLiteral>(block, step.location, sem_ir_.i32Type(),
                                        sem_ir_.addInteger(step.index));
      value =
          appendInst<SemIndex>(block, step.location, step.type, value, index);
    } else {
      value = appendInst<SemStructFieldAccess>(block, step.location, step.type,
                                               value,
                                               sem_ir_.addInteger(step.index));
    }
  }
  return value;
}

[[nodiscard]] InstId SemanticContext::applyPatternTransfer(
    InstId value, const SemanticContext::CheckedPatternBinding &binding,
    std::vector<InstId> &block) {
  if (binding.action == TokenKind::KwMove)
    return appendInst<SemMove>(block, binding.node, binding.type, value);
  if (binding.action == TokenKind::KwCopy)
    return appendInst<SemCopy>(block, binding.node, binding.type, value);
  if (binding.action == TokenKind::Amp) {
    const auto reference = sem_ir_.addReferenceType(
        binding.type, SemReferenceMutability::ReadOnly);
    return appendInst<SemBorrowPlace>(block, binding.node, reference, value);
  }
  emit(DiagnosticKind::MissingEnumPayloadTransfer, binding.node);
  return InstId::invalid();
}

bool SemanticContext::bindStructuredPattern(NodeId pattern, InstId source,
                                            std::vector<InstId> &block,
                                            NodeId location,
                                            bool mutable_local) {
  if (tree_.kind(pattern) != NodeKind::StructuredPattern)
    return false;
  const auto checked = checkPatternBindings(pattern, 0, instType(source));
  if (!checked.valid)
    return true;
  for (const auto &binding : checked.bindings) {
    auto value = emitPatternProjection(source, 0, binding, block);
    value = applyPatternTransfer(value, binding, block);
    if (!value.hasValue())
      continue;
    const auto name = nameFor(binding.name);
    if (!name.hasValue()) {
      emit(DiagnosticKind::UnknownName, binding.name);
      continue;
    }
    const auto local = sem_ir_.addLocal(
        {name, instType(value), binding.name,
         mutable_local ? static_cast<std::uint32_t>(SemLocalMutable) : 0U});
    sem_ir_.addLocalOccurrence(binding.name,
                               SemSymbolOccurrenceKind::Declaration, local);
    bind(name, local, binding.name);
    (void)appendInst<SemBindName>(block, binding.node, sem_ir_.voidType(),
                                  local, value);
  }
  (void)location;
  return true;
}

