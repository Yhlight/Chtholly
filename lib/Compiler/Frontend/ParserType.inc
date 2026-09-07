// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  NodeId parseGenericParameterList() {
    const auto generic_start = tree_.size();
    TokenId generic_close = current();
    if (consumeIf(TokenKind::Less)) {
      if (at(TokenKind::Greater))
        (void)expect(TokenKind::Identifier);
      while (!at(TokenKind::Greater) && !atEnd()) {
        if (at(TokenKind::Identifier))
          (void)tree_.appendLeaf(NodeKind::Name, consume());
        else
          (void)expect(TokenKind::Identifier);
        if (!consumeListSeparator(TokenKind::Greater))
          break;
      }
      generic_close = expect(TokenKind::Greater);
    }
    return tree_.appendSubtree(NodeKind::GenericParameterList, generic_close,
                               generic_start);
  }

  NodeId parseWhereClause() {
    const auto list_start = tree_.size();
    TokenId introducer = current();
    if (!consumeIf(TokenKind::KwWhere))
      return tree_.appendSubtree(NodeKind::ConstraintList, introducer,
                                 list_start);
    do {
      const auto clause_start = tree_.size();
      const auto clause_token = current();
      (void)parseType();
      (void)expect(TokenKind::Colon);
      (void)parseType();
      (void)tree_.appendSubtree(NodeKind::ConstraintClause, clause_token,
                                clause_start);
    } while (consumeIf(TokenKind::Comma));
    return tree_.appendSubtree(NodeKind::ConstraintList, introducer,
                               list_start);
  }

  NodeId parseType() {
    const auto start = tree_.size();
    if (at(TokenKind::Identifier) && position_ + 1 < tokens_.size() &&
        tokens_.get(TokenId(position_ + 1)).kind == TokenKind::LParen) {
      const auto name = tokens_.text(current());
      const auto query_kind = name == "element_type" ? NodeKind::ElementTypeExpr
                              : name == "pointee_type"
                                  ? NodeKind::PointeeTypeExpr
                                  : NodeKind::Error;
      if (query_kind != NodeKind::Error) {
        const auto introducer = consume();
        (void)expect(TokenKind::LParen);
        (void)parseType();
        if (query_kind == NodeKind::ElementTypeExpr) {
          (void)expect(TokenKind::Comma);
          (void)parseExpression();
        }
        (void)expect(TokenKind::RParen);
        return tree_.appendSubtree(query_kind, introducer, start);
      }
    }
    if (at(TokenKind::LParen)) {
      const auto open = consume();
      while (!at(TokenKind::RParen) && !atEnd()) {
        (void)parseType();
        if (!consumeListSeparator(TokenKind::RParen))
          break;
      }
      const auto close = expect(TokenKind::RParen);
      return tree_.appendSubtree(NodeKind::TupleType, close, start);
    }
    if (at(TokenKind::Identifier) && tokens_.text(current()) == "owned") {
      const auto introducer = consume();
      (void)parseType();
      return tree_.appendSubtree(NodeKind::OwnedPointerType, introducer, start);
    }
    if (at(TokenKind::KwFn)) {
      const auto introducer = consume();
      (void)expect(TokenKind::LParen);
      const auto parameters_start = tree_.size();
      while (!at(TokenKind::RParen) && !atEnd()) {
        (void)parseType();
        if (!consumeListSeparator(TokenKind::RParen))
          break;
      }
      const auto close = expect(TokenKind::RParen);
      (void)tree_.appendSubtree(NodeKind::ParameterList, close,
                                parameters_start);
      (void)expect(TokenKind::Colon);
      (void)parseType();
      return tree_.appendSubtree(NodeKind::FunctionPointerType, introducer,
                                 start);
    }
    TokenId qualifier;
    const auto is_const = consumeIf(TokenKind::KwConst, &qualifier);
    const auto type_token = consume();
    switch (tokens_.get(type_token).kind) {
    case TokenKind::Identifier:
    case TokenKind::KwBool:
    case TokenKind::KwChar:
    case TokenKind::KwF32:
    case TokenKind::KwF64:
    case TokenKind::KwI8:
    case TokenKind::KwI16:
    case TokenKind::KwI32:
    case TokenKind::KwI64:
    case TokenKind::KwISize:
    case TokenKind::KwNever:
    case TokenKind::KwString:
    case TokenKind::KwU8:
    case TokenKind::KwU16:
    case TokenKind::KwU32:
    case TokenKind::KwU64:
    case TokenKind::KwUSize:
    case TokenKind::KwVoid:
      break;
    default:
      diagnostics_.emit(
          DiagnosticKind::ExpectedToken, tokens_.get(type_token).offset,
          std::max<std::uint16_t>(tokens_.get(type_token).length, 1),
          TokenKind::Identifier, tokens_.get(type_token).kind);
      return tree_.appendLeaf(NodeKind::Error, type_token, true);
    }
    auto result = tree_.appendLeaf(NodeKind::TypeName, type_token);
    if (consumeIf(TokenKind::Scope)) {
      do {
        const auto component = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, component,
                               tokens_.get(component).kind !=
                                   TokenKind::Identifier);
      } while (consumeIf(TokenKind::Scope));
      result = tree_.appendSubtree(NodeKind::QualifiedType, type_token, start);
    }
    if (consumeIf(TokenKind::Less)) {
      while (!at(TokenKind::Greater) && !atEnd()) {
        (void)parseType();
        if (!consumeListSeparator(TokenKind::Greater))
          break;
      }
      const auto close = expect(TokenKind::Greater);
      result = tree_.appendSubtree(NodeKind::GenericType, close, start);
    }
    while (consumeIf(TokenKind::LBracket)) {
      (void)parseExpression();
      const auto close = expect(TokenKind::RBracket);
      result = tree_.appendSubtree(NodeKind::ArrayType, close, start);
    }
    bool first_postfix = true;
    for (;;) {
      TokenId pointer_token;
      TokenId postfix_qualifier;
      const bool postfix_const =
          !first_postfix && consumeIf(TokenKind::KwConst, &postfix_qualifier);
      const bool qualified = (first_postfix && is_const) || postfix_const;
      if (consumeIf(TokenKind::Star, &pointer_token)) {
        result = tree_.appendSubtree(
            NodeKind::RawPointerType,
            qualified ? (first_postfix ? qualifier : postfix_qualifier)
                      : pointer_token,
            start);
        first_postfix = false;
        continue;
      }
      if (consumeIf(TokenKind::Amp, &pointer_token)) {
        result = tree_.appendSubtree(
            NodeKind::ReferenceType,
            qualified ? (first_postfix ? qualifier : postfix_qualifier)
                      : pointer_token,
            start);
        first_postfix = false;
        continue;
      }
      // A qualifier not followed by a pointer/reference is handled by the
      // existing recovery path below.
      if (postfix_const)
        diagnostics_.emit(DiagnosticKind::ExpectedToken,
                          tokens_.get(postfix_qualifier).offset, 1,
                          TokenKind::Amp, token().kind);
      break;
    }
    if (first_postfix && is_const && tree_.kind(result) == NodeKind::GenericType &&
               tree_.children(result).size() >= 1 &&
               tokens_.text(tree_.token(tree_.children(result).front())) ==
                   "slice") {
      // `const slice<T>` keeps the borrowed slice view readonly.
    } else if (first_postfix && is_const) {
      diagnostics_.emit(
          DiagnosticKind::ExpectedToken, tokens_.get(type_token).offset,
          std::max<std::uint16_t>(tokens_.get(type_token).length, 1),
          TokenKind::Star, token().kind);
      result =
          tree_.appendSubtree(NodeKind::RawPointerType, qualifier, start, true);
    }
    return result;
  }
