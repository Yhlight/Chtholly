// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  NodeId parseAggregateLiteral(TokenId type_token, std::size_t start) {
    (void)expect(TokenKind::LBrace);
    while (!at(TokenKind::RBrace) && !atEnd()) {
      const auto field_start = tree_.size();
      (void)expect(TokenKind::Dot);
      const auto field = expect(TokenKind::Identifier);
      if (tokens_.get(field).kind == TokenKind::Identifier)
        (void)tree_.appendLeaf(NodeKind::Name, field);
      (void)expect(TokenKind::Equal);
      (void)parseExpression();
      (void)tree_.appendSubtree(NodeKind::NamedFieldInit, field, field_start);
      if (!consumeIf(TokenKind::Comma))
        break;
    }
    (void)expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::AggregateLiteral, type_token, start);
  }

  void
  recoverToStatement(std::uint32_t failure_start = core::AnyId::InvalidIndex) {
    if (failure_start != core::AnyId::InvalidIndex)
      appendError(TokenId(failure_start));
    while (!atEnd() && !at(TokenKind::Semicolon) && !at(TokenKind::RBrace)) {
      if (isOpeningDelimiter(token().kind))
        skipNestedGroup();
      else
        consume();
    }
    (void)consumeIf(TokenKind::Semicolon);
  }

  void recoverToDeclaration(std::uint32_t failure_start) {
    appendError(TokenId(failure_start));
    while (!atEnd() && !atDeclarationStart()) {
      if (isOpeningDelimiter(token().kind))
        skipNestedGroup();
      else
        consume();
    }
  }

  const TokenBuffer &tokens_;
  DiagnosticEmitter &diagnostics_;
  ParseTree tree_;
  std::uint32_t position_ = 0;
  LanguageVersion language_version_;
