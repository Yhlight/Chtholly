// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
void SemanticContext::emit(DiagnosticKind kind, NodeId node) {
  const auto &token = tree_.tokens().get(tree_.token(node));
  diagnostics_.emit(kind, token.offset, token.length == 0 ? 1U : token.length);
}

void SemanticContext::emitOwnershipDiagnostic(
    DiagnosticKind kind, InstId instruction,
    std::span<const OwnershipEvidence> evidence, NodeId fallback) {
  const auto primary_node = instruction.hasValue() ? sem_ir_.location(instruction)
                                                   : fallback;
  const auto &primary_token = tree_.tokens().get(tree_.token(primary_node));
  Diagnostic diagnostic{kind, primary_token.offset,
                        primary_token.length == 0 ? 1U : primary_token.length,
                        TokenKind::Invalid, TokenKind::Invalid, {}};
  const auto note_kind = [](OwnershipEvidenceKind value) {
    switch (value) {
    case OwnershipEvidenceKind::BorrowOrigin:
      return DiagnosticKind::OwnershipBorrowOrigin;
    case OwnershipEvidenceKind::ConflictingAccess:
      return DiagnosticKind::OwnershipConflictOrigin;
    case OwnershipEvidenceKind::MoveOrigin:
      return DiagnosticKind::OwnershipMoveOrigin;
    case OwnershipEvidenceKind::CallEffect:
      return DiagnosticKind::OwnershipCallEffect;
    case OwnershipEvidenceKind::ReturnProvenance:
      return DiagnosticKind::OwnershipReturnProvenance;
    case OwnershipEvidenceKind::CleanupBoundary:
      return DiagnosticKind::OwnershipCleanupBoundary;
    case OwnershipEvidenceKind::ContractBoundary:
      return DiagnosticKind::OwnershipContractOrigin;
    case OwnershipEvidenceKind::WidenedRegion:
      return DiagnosticKind::OwnershipWidenedRegion;
    }
    return DiagnosticKind::OwnershipBorrowOrigin;
  };
  for (const auto &item : evidence) {
    const auto node = item.instruction.hasValue() ? sem_ir_.location(item.instruction)
                                                  : fallback;
    const auto &token = tree_.tokens().get(tree_.token(node));
    diagnostic.notes.push_back(
        {.level = DiagnosticLevel::Note,
         .kind = note_kind(item.kind),
         .code = std::string(diagnosticCode(note_kind(item.kind))),
         .message = item.detail,
         .path = {},
         .offset = token.offset,
         .length = token.length == 0 ? 1U : token.length});
    if (diagnostic.notes.size() == 8)
      break;
  }
  diagnostics_.emit(std::move(diagnostic));
}

[[nodiscard]] bool SemanticContext::requireVersion(LanguageVersion minimum,
                                                   DiagnosticKind diagnostic,
                                                   NodeId node) {
  if (sem_ir_.languageVersion() >= minimum)
    return true;
  emit(diagnostic, node);
  return false;
}

[[nodiscard]] bool SemanticContext::hasUnsafeAuthority() const {
  return unsafe_depth_ != 0;
}

[[nodiscard]] bool SemanticContext::requireUnsafe(UnsafeOperationKind operation,
                                                  NodeId node) {
  if (hasUnsafeAuthority())
    return true;
  emit(unsafeOperationDiagnostic(operation), node);
  return false;
}

[[nodiscard]] IdentifierId SemanticContext::tokenString(NodeId node) const {
  return tree_.tokens().value(tree_.token(node));
}

[[nodiscard]] NameId SemanticContext::nameFor(NodeId node) {
  const auto text = tokenString(node);
  if (!text.hasValue()) {
    emit(DiagnosticKind::UnknownName, node);
    return NameId::invalid();
  }
  return sem_ir_.addName(text);
}

void SemanticContext::pushLoop(NodeId label) {
  if (label.hasValue()) {
    const auto text = tokenString(label);
    if (std::ranges::any_of(active_loops_, [&](const ActiveLoop &loop) {
          return loop.label.hasValue() && tokenString(loop.label) == text;
        }))
      emit(DiagnosticKind::DuplicateLoopLabel, label);
  }
  active_loops_.push_back({label});
}

std::optional<std::size_t>
SemanticContext::resolveLoopTarget(std::span<const NodeId> children,
                                   NodeId node, bool is_break) {
  const auto label = std::ranges::find_if(children, [&](NodeId child) {
    return tree_.kind(child) == NodeKind::LoopLabel;
  });
  if (label == children.end()) {
    if (active_loops_.empty()) {
      emit(is_break ? DiagnosticKind::BreakOutsideLoop
                    : DiagnosticKind::ContinueOutsideLoop,
           node);
      return std::nullopt;
    }
    return active_loops_.size() - 1;
  }
  const auto text = tokenString(*label);
  for (std::size_t index = active_loops_.size(); index-- > 0;)
    if (active_loops_[index].label.hasValue() &&
        tokenString(active_loops_[index].label) == text)
      return index;
  emit(DiagnosticKind::UnknownLoopLabel, *label);
  return std::nullopt;
}

[[nodiscard]] StringLiteralId SemanticContext::decodeString(NodeId node) {
  const auto token_id = tree_.token(node);
  const auto spelling = tree_.tokens().text(token_id);
  const auto begin =
      tree_.tokens().get(token_id).kind == TokenKind::CStringLiteral ? 2U : 1U;
  if (spelling.size() <= begin)
    return values_.internStringLiteral({});
  std::string value;
  value.reserve(spelling.size() - begin - 1);
  for (std::size_t index = begin; index + 1 < spelling.size(); ++index) {
    auto character = spelling[index];
    if (character == '\\' && index + 2 < spelling.size()) {
      character = spelling[++index];
      switch (character) {
      case '0':
        character = '\0';
        break;
      case 'n':
        character = '\n';
        break;
      case 'r':
        character = '\r';
        break;
      case 't':
        character = '\t';
        break;
      default:
        break;
      }
    }
    value.push_back(character);
  }
  return values_.internStringLiteral(value);
}

[[nodiscard]] std::optional<std::uint32_t>
SemanticContext::decodeChar(NodeId node) {
  const auto spelling = tree_.tokens().text(tree_.token(node));
  if (spelling.size() < 3 || spelling.front() != '\'' || spelling.back() != '\'')
    return std::nullopt;
  const auto body = spelling.substr(1, spelling.size() - 2);
  if (body.size() == 2 && body.front() == '\\') {
    switch (body[1]) {
    case '0': return 0U;
    case 'n': return static_cast<std::uint32_t>('\n');
    case 'r': return static_cast<std::uint32_t>('\r');
    case 't': return static_cast<std::uint32_t>('\t');
    case '\\': return static_cast<std::uint32_t>('\\');
    case '\'': return static_cast<std::uint32_t>('\'');
    default: return std::nullopt;
    }
  }
  if (body.empty() || body.front() == '\\')
    return std::nullopt;
  const auto byte = [&](std::size_t index) {
    return static_cast<unsigned char>(body[index]);
  };
  const auto first = byte(0);
  std::uint32_t value = 0;
  std::size_t length = 0;
  if (first < 0x80U) {
    value = first;
    length = 1;
  } else if ((first & 0xe0U) == 0xc0U) {
    value = first & 0x1fU;
    length = 2;
  } else if ((first & 0xf0U) == 0xe0U) {
    value = first & 0x0fU;
    length = 3;
  } else if ((first & 0xf8U) == 0xf0U) {
    value = first & 0x07U;
    length = 4;
  } else {
    return std::nullopt;
  }
  if (body.size() != length)
    return std::nullopt;
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = byte(index);
    if ((continuation & 0xc0U) != 0x80U)
      return std::nullopt;
    value = (value << 6U) | (continuation & 0x3fU);
  }
  const auto min_value = length == 1 ? 0U : (1U << (5U * length - 4U));
  if (value < min_value || value > 0x10ffffU ||
      (value >= 0xd800U && value <= 0xdfffU))
    return std::nullopt;
  return value;
}

[[nodiscard]] bool SemanticContext::hasObjectRepresentation(TypeId type) const {
  switch (sem_ir_.type(type).kind) {
  case SemTypeKind::Void:
  case SemTypeKind::Never:
    return false;
  case SemTypeKind::Array:
    return hasObjectRepresentation(TypeId(sem_ir_.type(type).arg0));
  case SemTypeKind::Tuple:
    for (const auto element :
         sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(type).arg0)))
      if (!hasObjectRepresentation(element))
        return false;
    return true;
  default:
    return true;
  }
}
