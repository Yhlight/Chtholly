// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  void requireVersion(LanguageVersion minimum, DiagnosticKind diagnostic,
                      TokenId token_id) {
    if (language_version_ < minimum) {
      const auto &info = tokens_.get(token_id);
      diagnostics_.emit(diagnostic, info.offset,
                        std::max<std::uint16_t>(info.length, 1));
    }
  }

  [[nodiscard]] TokenId current() const {
    return TokenId(position_);
  }
  [[nodiscard]] const TokenInfo &token() const {
    return tokens_.get(current());
  }
  [[nodiscard]] bool at(TokenKind kind) const {
    return token().kind == kind;
  }
  [[nodiscard]] bool atContextualKeyword(std::string_view spelling,
                                         std::uint32_t lookahead = 0) const {
    const auto index = position_ + lookahead;
    return index < tokens_.size() &&
           tokens_.get(TokenId(index)).kind == TokenKind::Identifier &&
           tokens_.text(TokenId(index)) == spelling;
  }
  [[nodiscard]] bool atEnd() const {
    return at(TokenKind::EndOfFile);
  }

  [[nodiscard]] bool atDeclarationStart() const {
    return at(TokenKind::KwModule) || at(TokenKind::KwImport) ||
           at(TokenKind::KwExport) || at(TokenKind::KwFn) ||
           at(TokenKind::KwStruct) || at(TokenKind::KwUnion) ||
           at(TokenKind::KwEnum) || at(TokenKind::KwImpl) ||
           at(TokenKind::KwPub) || at(TokenKind::KwConst) ||
           at(TokenKind::KwStatic) || at(TokenKind::KwUnsafe) ||
           (at(TokenKind::Identifier) &&
            (tokens_.text(current()) == "alias" ||
             tokens_.text(current()) == "async" ||
             tokens_.text(current()) == "repr" ||
             tokens_.text(current()) == "lifecycle"));
  }

  [[nodiscard]] bool atUnambiguousDeclarationStartInBlock() const {
    return at(TokenKind::KwModule) || at(TokenKind::KwImport) ||
           at(TokenKind::KwExport) || at(TokenKind::KwFn) ||
           at(TokenKind::KwStruct) || at(TokenKind::KwUnion) ||
           at(TokenKind::KwEnum) || at(TokenKind::KwImpl) ||
           at(TokenKind::KwPub) || at(TokenKind::KwStatic);
  }

  [[nodiscard]] bool looksLikeGenericAggregate() const {
    if (!at(TokenKind::Less))
      return false;
    std::uint32_t depth = 0;
    for (std::uint32_t index = position_; index < tokens_.size(); ++index) {
      const auto kind = tokens_.get(TokenId(index)).kind;
      if (kind == TokenKind::Less)
        ++depth;
      else if (kind == TokenKind::Greater) {
        if (--depth == 0)
          return index + 1 < tokens_.size() &&
                 (tokens_.get(TokenId(index + 1)).kind == TokenKind::LBrace ||
                  tokens_.get(TokenId(index + 1)).kind == TokenKind::Scope);
      } else if (kind == TokenKind::Semicolon || kind == TokenKind::EndOfFile)
        return false;
    }
    return false;
  }

  TokenId consume() {
    const auto result = current();
    if (!atEnd())
      ++position_;
    return result;
  }

  bool consumeIf(TokenKind kind, TokenId *result = nullptr) {
    if (!at(kind))
      return false;
    const auto value = consume();
    if (result)
      *result = value;
    return true;
  }

  bool consumeContextualKeyword(std::string_view spelling,
                                TokenId *result = nullptr) {
    if (!atContextualKeyword(spelling))
      return false;
    const auto value = consume();
    if (result)
      *result = value;
    return true;
  }

  TokenId expectContextualKeyword(std::string_view spelling) {
    if (atContextualKeyword(spelling))
      return consume();
    diagnostics_.emit(DiagnosticKind::ExpectedToken, token().offset,
                      std::max<std::uint16_t>(token().length, 1),
                      TokenKind::Identifier, token().kind);
    appendError(current());
    return current();
  }

  TokenId expect(TokenKind kind) {
    if (at(kind))
      return consume();
    diagnostics_.emit(DiagnosticKind::ExpectedToken, token().offset,
                      std::max<std::uint16_t>(token().length, 1), kind,
                      token().kind);
    appendError(current());
    return current();
  }

  void appendError(TokenId token) {
    if (tree_.size() != 0) {
      const auto last = NodeId(static_cast<std::uint32_t>(tree_.size() - 1));
      if (tree_.kind(last) == NodeKind::Error && tree_.token(last) == token)
        return;
    }
    (void)tree_.appendLeaf(NodeKind::Error, token, true);
  }

  [[nodiscard]] static bool isOpeningDelimiter(TokenKind kind) {
    return kind == TokenKind::LParen || kind == TokenKind::LBrace ||
           kind == TokenKind::LBracket;
  }

  [[nodiscard]] static bool delimitersMatch(TokenKind open, TokenKind close) {
    return (open == TokenKind::LParen && close == TokenKind::RParen) ||
           (open == TokenKind::LBrace && close == TokenKind::RBrace) ||
           (open == TokenKind::LBracket && close == TokenKind::RBracket);
  }

  void skipNestedUntil(TokenKind first, TokenKind second) {
    std::vector<TokenKind> delimiters;
    while (!atEnd()) {
      const auto kind = token().kind;
      if (delimiters.empty() && (kind == first || kind == second))
        return;
      if (isOpeningDelimiter(kind)) {
        delimiters.push_back(kind);
        consume();
        continue;
      }
      if (kind == TokenKind::RParen || kind == TokenKind::RBrace ||
          kind == TokenKind::RBracket) {
        if (delimiters.empty())
          return;
        if (!delimitersMatch(delimiters.back(), kind))
          return;
        delimiters.pop_back();
      }
      consume();
    }
  }

  void skipNestedGroup() {
    if (!isOpeningDelimiter(token().kind))
      return;
    std::vector<TokenKind> delimiters{token().kind};
    consume();
    while (!atEnd() && !delimiters.empty()) {
      const auto kind = token().kind;
      if (isOpeningDelimiter(kind)) {
        delimiters.push_back(kind);
      } else if (kind == TokenKind::RParen || kind == TokenKind::RBrace ||
                 kind == TokenKind::RBracket) {
        if (!delimitersMatch(delimiters.back(), kind))
          return;
        delimiters.pop_back();
      }
      consume();
    }
  }

  bool consumeListSeparator(TokenKind close) {
    if (consumeIf(TokenKind::Comma))
      return !at(close);
    if (at(close) || atEnd())
      return false;
    (void)expect(TokenKind::Comma);
    skipNestedUntil(TokenKind::Comma, close);
    return consumeIf(TokenKind::Comma) && !at(close);
  }
