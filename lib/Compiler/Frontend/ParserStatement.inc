// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  NodeId parseBlock() {
    const auto start = tree_.size();
    if (!at(TokenKind::LBrace)) {
      const auto location = expect(TokenKind::LBrace);
      return tree_.appendSubtree(NodeKind::Block, location, start, true);
    }
    const auto open = consume();
    while (!at(TokenKind::RBrace) && !atEnd()) {
      if (atUnambiguousDeclarationStartInBlock()) {
        (void)expect(TokenKind::RBrace);
        return tree_.appendSubtree(NodeKind::Block, open, start, true);
      }
      const auto statement_start = position_;
      if (!parseStatement())
        recoverToStatement(statement_start);
      if (position_ == statement_start && !atEnd() && !at(TokenKind::RBrace)) {
        appendError(current());
        consume();
      }
    }
    (void)expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::Block, open, start);
  }

  [[nodiscard]] std::size_t matchingDelimiter(std::size_t open) const {
    if (open >= tokens_.size())
      return tokens_.size();
    const auto open_kind =
        tokens_.get(TokenId(static_cast<std::uint32_t>(open))).kind;
    const auto close_kind = open_kind == TokenKind::LParen   ? TokenKind::RParen
                            : open_kind == TokenKind::LBrace ? TokenKind::RBrace
                                                             : TokenKind::Count;
    if (close_kind == TokenKind::Count)
      return tokens_.size();
    std::size_t depth = 0;
    for (std::size_t index = open; index < tokens_.size(); ++index) {
      const auto kind =
          tokens_.get(TokenId(static_cast<std::uint32_t>(index))).kind;
      if (kind == open_kind)
        ++depth;
      else if (kind == close_kind && --depth == 0)
        return index;
    }
    return tokens_.size();
  }

  [[nodiscard]] bool ifExpressionAhead() const {
    if (!at(TokenKind::KwIf) || position_ + 1 >= tokens_.size() ||
        tokens_.get(TokenId(position_ + 1)).kind != TokenKind::LParen)
      return false;
    const auto condition_end = matchingDelimiter(position_ + 1);
    if (condition_end + 1 >= tokens_.size() ||
        tokens_.get(TokenId(static_cast<std::uint32_t>(condition_end + 1)))
                .kind != TokenKind::LBrace)
      return false;
    const auto then_end = matchingDelimiter(condition_end + 1);
    return then_end + 1 < tokens_.size() &&
           tokens_.get(TokenId(static_cast<std::uint32_t>(then_end + 1)))
                   .kind == TokenKind::KwElse;
  }

  [[nodiscard]] bool blockExpressionAhead() const {
    if (!at(TokenKind::LBrace))
      return false;
    const auto close = matchingDelimiter(position_);
    return close + 1 < tokens_.size() &&
           tokens_.get(TokenId(static_cast<std::uint32_t>(close + 1))).kind ==
               TokenKind::RBrace;
  }

  [[nodiscard]] bool switchStatementAhead() const {
    if (!at(TokenKind::KwSwitch))
      return false;
    for (std::size_t index = position_ + 1; index < tokens_.size(); ++index) {
      const auto kind =
          tokens_.get(TokenId(static_cast<std::uint32_t>(index))).kind;
      if (kind != TokenKind::FatArrow)
        continue;
      if (index + 1 >= tokens_.size())
        return true;
      const auto body =
          tokens_.get(TokenId(static_cast<std::uint32_t>(index + 1))).kind;
      if (body == TokenKind::LBrace) {
        const auto close = matchingDelimiter(index + 1);
        return close + 1 >= tokens_.size() ||
               tokens_.get(TokenId(static_cast<std::uint32_t>(close + 1)))
                       .kind != TokenKind::Semicolon;
      }
      return body == TokenKind::KwLet || body == TokenKind::KwVar ||
             body == TokenKind::KwReturn || body == TokenKind::KwBreak ||
             body == TokenKind::KwContinue || body == TokenKind::KwUnsafe ||
             (index + 2 < tokens_.size() &&
              isAssignmentOperator(
                  tokens_.get(TokenId(static_cast<std::uint32_t>(index + 2)))
                      .kind));
    }
    return false;
  }

  NodeId parseValueBlock() {
    const auto start = tree_.size();
    if (!at(TokenKind::LBrace)) {
      const auto location = expect(TokenKind::LBrace);
      return tree_.appendSubtree(NodeKind::ValueBlock, location, start, true);
    }
    const auto open = consume();
    while (!at(TokenKind::RBrace) && !atEnd()) {
      const auto statement_start = position_;
      if ((at(TokenKind::Identifier) && position_ + 1 < tokens_.size() &&
           tokens_.get(TokenId(position_ + 1)).kind == TokenKind::Colon) ||
          at(TokenKind::KwLet) || at(TokenKind::KwVar) ||
          at(TokenKind::KwConst) || at(TokenKind::KwReturn) ||
          (at(TokenKind::KwIf) && !ifExpressionAhead()) ||
          (at(TokenKind::KwSwitch) && switchStatementAhead()) ||
          (at(TokenKind::LBrace) && !blockExpressionAhead()) ||
          at(TokenKind::KwWhile) || at(TokenKind::KwFor) ||
          at(TokenKind::KwDo) || at(TokenKind::KwForeach) ||
          at(TokenKind::KwBreak) || at(TokenKind::KwContinue) ||
          at(TokenKind::KwDefer) || at(TokenKind::KwAssert) ||
          at(TokenKind::KwUnreachable) || at(TokenKind::KwUnsafe) ||
          (atContextualKeyword("task") && atContextualKeyword("scope", 1)) ||
          (atContextualKeyword("check") && atContextualKeyword("cancel", 1))) {
        if (!parseStatement())
          recoverToStatement(statement_start);
        continue;
      }
      const auto expression_start = tree_.size();
      const auto expression = parseExpression();
      if (!expression.hasValue()) {
        recoverToStatement(statement_start);
        continue;
      }
      if (isAssignmentOperator(token().kind)) {
        const auto equal = consume();
        (void)parseExpression();
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::AssignmentStmt, equal,
                                  expression_start);
      } else if (consumeIf(TokenKind::Semicolon)) {
        (void)tree_.appendSubtree(NodeKind::ExprStmt, tree_.token(expression),
                                  expression_start);
      } else if (!at(TokenKind::RBrace)) {
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::ExprStmt, tree_.token(expression),
                                  expression_start, true);
      } else {
        break;
      }
    }
    (void)expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::ValueBlock, open, start);
  }

  void parseForClause(NodeKind wrapper, bool allow_declaration) {
    const auto wrapper_start = tree_.size();
    const auto location = current();
    if (allow_declaration && (at(TokenKind::KwLet) || at(TokenKind::KwVar) ||
                              at(TokenKind::KwConst))) {
      const auto statement_start = tree_.size();
      const auto introducer = consume();
      if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::Name, consume());
      else
        (void)expect(TokenKind::Identifier);
      if (consumeIf(TokenKind::Colon))
        (void)parseType();
      (void)expect(TokenKind::Equal);
      (void)parseExpression();
      (void)tree_.appendSubtree(tokens_.get(introducer).kind ==
                                        TokenKind::KwConst
                                    ? NodeKind::ConstDecl
                                    : NodeKind::LetStmt,
                                introducer, statement_start);
    } else if (!at(TokenKind::Semicolon) && !at(TokenKind::RParen)) {
      const auto statement_start = tree_.size();
      const auto expression = parseExpression();
      if (isAssignmentOperator(token().kind)) {
        const auto equal = consume();
        (void)parseExpression();
        (void)tree_.appendSubtree(NodeKind::AssignmentStmt, equal,
                                  statement_start);
      } else if (expression.hasValue()) {
        (void)tree_.appendSubtree(NodeKind::ExprStmt, tree_.token(expression),
                                  statement_start);
      }
    }
    (void)tree_.appendSubtree(wrapper, location, wrapper_start);
  }

  bool parseStatement() {
    const auto loop_statement_start = tree_.size();
    const auto has_loop_label =
        at(TokenKind::Identifier) && position_ + 1 < tokens_.size() &&
        tokens_.get(TokenId(position_ + 1)).kind == TokenKind::Colon;
    if (has_loop_label) {
      const auto label = consume();
      requireVersion(FrozenV13LanguageVersion,
                     DiagnosticKind::LoopLabelRequires13, label);
      (void)tree_.appendLeaf(NodeKind::LoopLabel, label);
      (void)consume();
      if (!at(TokenKind::KwWhile) && !at(TokenKind::KwFor) &&
          !at(TokenKind::KwDo) && !at(TokenKind::KwForeach)) {
        diagnostics_.emit(
            DiagnosticKind::InvalidLoopLabel, tokens_.get(current()).offset,
            std::max<std::uint16_t>(tokens_.get(current()).length, 1));
        return false;
      }
    }
    if (at(TokenKind::KwSwitch) &&
        language_version_ >= FrozenV17LanguageVersion &&
        switchStatementAhead()) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)expect(TokenKind::LParen);
      (void)parseExpression();
      (void)expect(TokenKind::RParen);
      (void)expect(TokenKind::LBrace);
      while (!at(TokenKind::RBrace) && !atEnd()) {
        const auto arm_start = tree_.size();
        const auto pattern = parseEnumPattern();
        (void)expect(TokenKind::FatArrow);
        if (at(TokenKind::LBrace))
          (void)parseBlock();
        else if (!parseStatement())
          recoverToStatement();
        (void)tree_.appendSubtree(NodeKind::SwitchArm, tree_.token(pattern),
                                  arm_start);
      }
      (void)expect(TokenKind::RBrace);
      (void)tree_.appendSubtree(NodeKind::SwitchStmt, introducer, start);
      return true;
    }
    if (atContextualKeyword("task") && atContextualKeyword("scope", 1)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)expectContextualKeyword("scope");
      (void)parseBlock();
      (void)tree_.appendSubtree(NodeKind::TaskScopeStmt, introducer, start);
      return true;
    }
    if (atContextualKeyword("check") && atContextualKeyword("cancel", 1)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)expectContextualKeyword("cancel");
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::CheckCancelStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwDefer)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)parseBlock();
      (void)tree_.appendSubtree(NodeKind::DeferStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwAssert)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::AssertStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwUnreachable)) {
      const auto introducer = consume();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendLeaf(NodeKind::UnreachableStmt, introducer);
      return true;
    }
    if (at(TokenKind::KwUnsafe)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)parseBlock();
      (void)tree_.appendSubtree(NodeKind::UnsafeStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwLet) || at(TokenKind::KwVar) ||
        at(TokenKind::KwConst)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      if (at(TokenKind::LBrace) &&
          tokens_.get(introducer).kind != TokenKind::KwConst)
        (void)parseStructuredPattern();
      else if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::Name, consume());
      else
        (void)expect(TokenKind::Identifier);
      const auto has_explicit_type = consumeIf(TokenKind::Colon);
      if (has_explicit_type)
        (void)parseType();
      // A typed declaration without an initializer creates a place whose
      // initialization is checked by callable effects. `let` becomes frozen
      // after the first successful initialization; `var` remains mutable.
      if ((tokens_.get(introducer).kind == TokenKind::KwVar ||
           tokens_.get(introducer).kind == TokenKind::KwLet) &&
          has_explicit_type && at(TokenKind::Semicolon)) {
        (void)consume();
        (void)tree_.appendSubtree(tokens_.get(introducer).kind ==
                                          TokenKind::KwVar
                                      ? NodeKind::UninitializedVarDecl
                                      : NodeKind::UninitializedLetDecl,
                                  introducer, start);
        return true;
      }
      (void)expect(TokenKind::Equal);
      (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(tokens_.get(introducer).kind ==
                                        TokenKind::KwConst
                                    ? NodeKind::ConstDecl
                                    : NodeKind::LetStmt,
                                introducer, start);
      return true;
    }
    if (at(TokenKind::LBrace)) {
      const auto close = matchingDelimiter(position_);
      const auto structured_assignment =
          close + 1 < tokens_.size() &&
          isAssignmentOperator(
              tokens_.get(TokenId(static_cast<std::uint32_t>(close + 1))).kind);
      if (!structured_assignment &&
          language_version_ >= FrozenV17LanguageVersion) {
        const auto start = tree_.size();
        const auto open = current();
        (void)parseBlock();
        (void)tree_.appendSubtree(NodeKind::BlockStmt, open, start);
        return true;
      }
      const auto start = tree_.size();
      (void)parseStructuredPattern();
      if (isAssignmentOperator(token().kind)) {
        const auto equal = consume();
        (void)parseExpression();
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::AssignmentStmt, equal, start);
        return true;
      }
      diagnostics_.emit(
          DiagnosticKind::ExpectedToken, tokens_.get(current()).offset,
          std::max<std::uint16_t>(tokens_.get(current()).length, 1),
          TokenKind::Equal, tokens_.get(current()).kind);
      return false;
    }
    if (at(TokenKind::KwReturn)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      if (!at(TokenKind::Semicolon))
        (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::ReturnStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwIf)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)expect(TokenKind::LParen);
      (void)parseExpression();
      (void)expect(TokenKind::RParen);
      (void)parseBlock();
      if (consumeIf(TokenKind::KwElse)) {
        if (at(TokenKind::KwIf))
          (void)parseStatement();
        else
          (void)parseBlock();
      }
      (void)tree_.appendSubtree(NodeKind::IfStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwWhile)) {
      const auto start = has_loop_label ? loop_statement_start : tree_.size();
      const auto introducer = consume();
      (void)expect(TokenKind::LParen);
      (void)parseExpression();
      (void)expect(TokenKind::RParen);
      (void)parseBlock();
      (void)tree_.appendSubtree(NodeKind::WhileStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwFor)) {
      const auto start = has_loop_label ? loop_statement_start : tree_.size();
      const auto introducer = consume();
      (void)expect(TokenKind::LParen);
      parseForClause(NodeKind::ForInit, true);
      (void)expect(TokenKind::Semicolon);
      const auto condition_start = tree_.size();
      const auto condition_location = current();
      if (!at(TokenKind::Semicolon))
        (void)parseExpression();
      (void)tree_.appendSubtree(NodeKind::ForCondition, condition_location,
                                condition_start);
      (void)expect(TokenKind::Semicolon);
      parseForClause(NodeKind::ForStep, false);
      (void)expect(TokenKind::RParen);
      (void)parseBlock();
      (void)tree_.appendSubtree(NodeKind::ForStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwForeach)) {
      const auto start = has_loop_label ? loop_statement_start : tree_.size();
      const auto introducer = consume();
      requireVersion(FrozenV14LanguageVersion,
                     DiagnosticKind::ForeachRequires14, introducer);
      (void)expect(TokenKind::LParen);
      const auto binding_start = tree_.size();
      TokenId binding = current();
      if (!at(TokenKind::KwLet) && !at(TokenKind::KwVar)) {
        diagnostics_.emit(DiagnosticKind::InvalidForeachBinding, token().offset,
                          std::max<std::uint16_t>(token().length, 1));
        appendError(current());
      } else {
        binding = consume();
      }
      if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::Name, consume());
      else {
        diagnostics_.emit(DiagnosticKind::InvalidForeachBinding, token().offset,
                          std::max<std::uint16_t>(token().length, 1));
        (void)expect(TokenKind::Identifier);
      }
      if (consumeIf(TokenKind::Colon))
        (void)parseType();
      (void)tree_.appendSubtree(NodeKind::ForeachBinding, binding,
                                binding_start);
      (void)expect(TokenKind::KwIn);
      (void)parseExpression();
      (void)expect(TokenKind::RParen);
      (void)parseBlock();
      (void)tree_.appendSubtree(NodeKind::ForeachStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwDo)) {
      const auto start = has_loop_label ? loop_statement_start : tree_.size();
      const auto introducer = consume();
      (void)parseBlock();
      (void)expect(TokenKind::KwWhile);
      (void)expect(TokenKind::LParen);
      (void)parseExpression();
      (void)expect(TokenKind::RParen);
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::DoWhileStmt, introducer, start);
      return true;
    }
    if (at(TokenKind::KwBreak) || at(TokenKind::KwContinue)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::LoopLabel, consume());
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(tokens_.get(introducer).kind ==
                                        TokenKind::KwBreak
                                    ? NodeKind::BreakStmt
                                    : NodeKind::ContinueStmt,
                                introducer, start);
      return true;
    }
    const auto start = tree_.size();
    const auto expression = parseExpression();
    if (!expression.hasValue())
      return false;
    if (isAssignmentOperator(token().kind)) {
      const auto equal = consume();
      (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::AssignmentStmt, equal, start);
      return true;
    }
    (void)expect(TokenKind::Semicolon);
    (void)tree_.appendSubtree(NodeKind::ExprStmt, tree_.token(expression),
                              start);
    return true;
  }

