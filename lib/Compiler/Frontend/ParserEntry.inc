// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  Parser(const TokenBuffer &tokens, DiagnosticEmitter &diagnostics)
      : tokens_(tokens), diagnostics_(diagnostics), tree_(tokens),
        language_version_(tokens.languageVersion()) {}

  ParseTree run() {
    const auto start = tree_.size();
    while (!at(TokenKind::EndOfFile)) {
      const auto declaration_start = position_;
      if (!parseDeclaration())
        recoverToDeclaration(declaration_start);
      if (position_ == declaration_start && !atEnd()) {
        appendError(current());
        consume();
      }
    }
    (void)tree_.appendSubtree(NodeKind::File, current(), start);
    std::string error;
    if (!tree_.verify(error))
      diagnostics_.emit(DiagnosticKind::InvalidParseTree, token().offset,
                        token().length);
    return std::move(tree_);
  }

