// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  NodeId parseModulePath() {
    const auto start = tree_.size();
    const auto first = expect(TokenKind::Identifier);
    (void)tree_.appendLeaf(NodeKind::Name, first,
                           tokens_.get(first).kind != TokenKind::Identifier);
    while (consumeIf(TokenKind::Scope)) {
      const auto component = expect(TokenKind::Identifier);
      (void)tree_.appendLeaf(NodeKind::Name, component,
                             tokens_.get(component).kind !=
                                 TokenKind::Identifier);
    }
    return tree_.appendSubtree(NodeKind::ModulePath, first, start);
  }

  NodeId parseAttribute() {
    const auto start = tree_.size();
    const auto name = expect(TokenKind::Identifier);
    const auto introducer = name;
    if (tokens_.get(name).kind == TokenKind::Identifier)
      (void)tree_.appendLeaf(NodeKind::Name, name);
    const auto is_representation = tokens_.text(name) == "repr";
    (void)expect(TokenKind::LParen);
    if (is_representation && at(TokenKind::Identifier) &&
        tokens_.text(current()) == "C") {
      const auto entry_start = tree_.size();
      const auto policy = consume();
      (void)tree_.appendSubtree(NodeKind::RepresentationEntry, policy,
                                entry_start);
      (void)expect(TokenKind::RParen);
      return tree_.appendSubtree(NodeKind::RepresentationAttribute, introducer,
                                 start);
    }
    while (!at(TokenKind::RParen) && !atEnd()) {
      const auto entry_start = tree_.size();
      const auto key = consume();
      if (!is_representation && tokens_.get(key).kind != TokenKind::KwCopy &&
          tokens_.get(key).kind != TokenKind::KwMove &&
          tokens_.get(key).kind != TokenKind::Identifier) {
        diagnostics_.emit(DiagnosticKind::ExpectedToken,
                          tokens_.get(key).offset,
                          std::max<std::uint16_t>(tokens_.get(key).length, 1),
                          TokenKind::Identifier, tokens_.get(key).kind);
      }
      (void)expect(TokenKind::Equal);
      if (is_representation &&
          (tokens_.text(key) == "storage" || tokens_.text(key) == "bits" ||
           tokens_.text(key) == "region")) {
        const auto path_start = tree_.size();
        auto component = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, component,
                               tokens_.get(component).kind !=
                                   TokenKind::Identifier);
        while (consumeIf(TokenKind::Dot)) {
          component = expect(TokenKind::Identifier);
          (void)tree_.appendLeaf(NodeKind::Name, component,
                                 tokens_.get(component).kind !=
                                     TokenKind::Identifier);
        }
        (void)tree_.appendSubtree(NodeKind::RepresentationPath, component,
                                  path_start);
        if (tokens_.text(key) == "bits") {
          (void)expect(TokenKind::LBracket);
          const auto begin = expect(TokenKind::IntegerLiteral);
          (void)tree_.appendLeaf(NodeKind::IntegerLiteral, begin,
                                 tokens_.get(begin).kind !=
                                     TokenKind::IntegerLiteral);
          (void)expect(TokenKind::DotDot);
          const auto end = expect(TokenKind::IntegerLiteral);
          (void)tree_.appendLeaf(NodeKind::IntegerLiteral, end,
                                 tokens_.get(end).kind !=
                                     TokenKind::IntegerLiteral);
          (void)expect(TokenKind::RBracket);
          (void)tree_.appendSubtree(NodeKind::RepresentationBitRange, key,
                                    path_start);
        }
        (void)tree_.appendSubtree(NodeKind::RepresentationEntry, key,
                                  entry_start);
      } else if (is_representation) {
        (void)parseType();
        (void)tree_.appendSubtree(NodeKind::RepresentationEntry, key,
                                  entry_start);
      } else {
        const auto value = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, value,
                               tokens_.get(value).kind !=
                                   TokenKind::Identifier);
        (void)tree_.appendSubtree(NodeKind::LifecyclePolicyEntry, key,
                                  entry_start);
      }
      if (!consumeListSeparator(TokenKind::RParen))
        break;
    }
    (void)expect(TokenKind::RParen);
    return tree_.appendSubtree(is_representation
                                   ? NodeKind::RepresentationAttribute
                                   : NodeKind::LifecycleAttribute,
                               introducer, start);
  }

  NodeId parseCallableContract() {
    const auto start = tree_.size();
    const auto introducer = consume();
    (void)expect(TokenKind::LBrace);
    while (!at(TokenKind::RBrace) && !atEnd()) {
      const auto entry_start = tree_.size();
      const auto operation = expect(TokenKind::Identifier);
      const auto spelling = tokens_.text(operation);
      if (spelling == "reads" || spelling == "writes" ||
          spelling == "takes" || spelling == "initializes") {
        do {
          (void)parseExpression();
        } while (consumeIf(TokenKind::Comma));
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::ContractEffectEntry, operation,
                                  entry_start);
        continue;
      }
      if (spelling == "borrows") {
        const auto mode = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, mode,
                               tokens_.get(mode).kind != TokenKind::Identifier);
        do {
          (void)parseExpression();
        } while (consumeIf(TokenKind::Comma));
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::ContractEffectEntry, operation,
                                  entry_start);
        continue;
      }
      if (spelling == "ensures") {
        const auto outcome = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, outcome,
                               tokens_.get(outcome).kind !=
                                   TokenKind::Identifier);
        (void)parseExpression();
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::ContractPostconditionEntry,
                                  operation, entry_start);
        continue;
      }
      if (spelling == "returns") {
        const auto borrow = expect(TokenKind::Identifier);
        if (tokens_.text(borrow) != "borrow")
          diagnostics_.emit(
              DiagnosticKind::ExpectedToken, tokens_.get(borrow).offset,
              std::max<std::uint16_t>(tokens_.get(borrow).length, 1),
              TokenKind::Identifier, tokens_.get(borrow).kind);
        (void)parseExpression();
        if (at(TokenKind::Identifier) && tokens_.text(current()) == "when") {
          (void)consume();
          (void)parseExpression();
        } else if (at(TokenKind::Identifier) &&
                   tokens_.text(current()) == "otherwise") {
          const auto otherwise = consume();
          (void)tree_.appendLeaf(NodeKind::ContractOtherwise, otherwise);
        }
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(NodeKind::ContractReturnEntry, operation,
                                  entry_start);
        continue;
      }
      diagnostics_.emit(
          DiagnosticKind::ExpectedDeclaration, tokens_.get(operation).offset,
          std::max<std::uint16_t>(tokens_.get(operation).length, 1));
      while (!at(TokenKind::Semicolon) && !at(TokenKind::RBrace) && !atEnd())
        (void)consume();
      (void)consumeIf(TokenKind::Semicolon);
    }
    const auto close = expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::CallableContract, close, start);
  }

  NodeId
  parseFunctionDeclaration(bool is_public, bool is_unsafe = false,
                           bool is_const = false,
                           TokenId async_introducer = TokenId::invalid()) {
    const auto start = tree_.size();
    const auto introducer = expect(TokenKind::KwFn);
    if (async_introducer.hasValue()) {
      requireVersion(FrozenV11LanguageVersion, DiagnosticKind::AsyncRequires11,
                     async_introducer);
      (void)tree_.appendLeaf(NodeKind::AsyncModifier, async_introducer);
    }
    if (at(TokenKind::Identifier) || at(TokenKind::KwCopy))
      (void)tree_.appendLeaf(NodeKind::Name, consume());
    else
      (void)expect(TokenKind::Identifier);
    (void)parseGenericParameterList();
    (void)expect(TokenKind::LParen);
    const auto parameters_start = tree_.size();
    while (!at(TokenKind::RParen) && !atEnd()) {
      if (at(TokenKind::DotDotDot)) {
        (void)tree_.appendLeaf(NodeKind::VariadicParameter, consume());
        (void)consumeIf(TokenKind::Comma);
        break;
      }
      const auto parameter_start = tree_.size();
      if (at(TokenKind::LBrace)) {
        const auto pattern = parseStructuredPattern();
        const auto pattern_children = tree_.children(pattern);
        TokenId synthetic_name = tree_.token(pattern);
        if (!pattern_children.empty()) {
          const auto binding_children =
              tree_.children(pattern_children.front());
          if (!binding_children.empty())
            synthetic_name = tree_.token(binding_children.front());
        }
        (void)tree_.appendLeaf(NodeKind::Name, synthetic_name);
        (void)expect(TokenKind::Colon);
        (void)parseType();
        (void)tree_.appendSubtree(NodeKind::PatternParameter,
                                  tree_.token(pattern), parameter_start);
      } else if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::Name, consume());
      else
        (void)expect(TokenKind::Identifier);
      if (tree_.size() == parameter_start + 1) {
        (void)expect(TokenKind::Colon);
        (void)parseType();
      }
      TokenId equal;
      if (consumeIf(TokenKind::Equal, &equal)) {
        (void)parseExpression();
        (void)tree_.appendSubtree(NodeKind::DefaultParameter, equal,
                                  parameter_start);
      }
      if (!consumeListSeparator(TokenKind::RParen))
        break;
    }
    const auto close = expect(TokenKind::RParen);
    (void)tree_.appendSubtree(NodeKind::ParameterList, close, parameters_start);
    (void)expect(TokenKind::Colon);
    (void)parseType();
    (void)parseWhereClause();
    const auto has_contract =
        at(TokenKind::Identifier) && tokens_.text(current()) == "contract";
    if (has_contract)
      (void)parseCallableContract();
    const auto bodyless = has_contract || consumeIf(TokenKind::Semicolon);
    if (!bodyless) {
      if (language_version_ >= FrozenV17LanguageVersion)
        (void)parseValueBlock();
      else
        (void)parseBlock();
    }
    auto flags = is_public ? NodeFlags::IsPublic : NodeFlags::None;
    if (bodyless)
      flags = flags | NodeFlags::IsBodyless;
    if (is_unsafe)
      flags = flags | NodeFlags::IsUnsafe;
    if (is_const)
      flags = flags | NodeFlags::IsConst;
    return tree_.appendSubtree(NodeKind::FunctionDecl, introducer, start, false,
                               flags);
  }

  bool parseDeclaration() {
    const auto start = tree_.size();
    if (at(TokenKind::KwModule)) {
      const auto introducer = consume();
      (void)parseModulePath();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::ModuleDecl, introducer, start);
      return true;
    }
    if (at(TokenKind::KwImport) || at(TokenKind::KwExport)) {
      const auto is_export = consumeIf(TokenKind::KwExport);
      const auto introducer =
          is_export ? expect(TokenKind::KwImport) : consume();
      (void)parseModulePath();
      TokenId as_token;
      if (consumeIf(TokenKind::KwAs, &as_token)) {
        requireVersion(FrozenV13LanguageVersion,
                       DiagnosticKind::ModuleAliasRequires13, as_token);
        const auto alias = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, alias,
                               tokens_.get(alias).kind !=
                                   TokenKind::Identifier);
      }
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::ImportDecl, introducer, start, false,
                                is_export ? NodeFlags::IsExport
                                          : NodeFlags::None);
      return true;
    }
    while (at(TokenKind::Identifier) &&
           (tokens_.text(current()) == "repr" ||
            tokens_.text(current()) == "lifecycle"))
      (void)parseAttribute();
    const auto is_public = consumeIf(TokenKind::KwPub);
    TokenId async_introducer;
    (void)consumeContextualKeyword("async", &async_introducer);
    TokenId const_introducer;
    const auto is_const = consumeIf(TokenKind::KwConst, &const_introducer);
    const auto is_unsafe = consumeIf(TokenKind::KwUnsafe);
    if (is_unsafe && !at(TokenKind::KwFn)) {
      diagnostics_.emit(DiagnosticKind::ExpectedDeclaration, token().offset,
                        std::max<std::uint16_t>(token().length, 1));
      return false;
    }
    if (is_const && !at(TokenKind::KwFn)) {
      const auto value_start = tree_.size();
      const auto name = expect(TokenKind::Identifier);
      if (tokens_.get(name).kind == TokenKind::Identifier)
        (void)tree_.appendLeaf(NodeKind::Name, name);
      (void)expect(TokenKind::Colon);
      (void)parseType();
      (void)expect(TokenKind::Equal);
      (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(
          NodeKind::ConstDecl, const_introducer, value_start, false,
          is_public ? NodeFlags::IsPublic : NodeFlags::None);
      return true;
    }
    if (at(TokenKind::KwStatic)) {
      const auto value_start = tree_.size();
      const auto introducer = consume();
      const auto name = expect(TokenKind::Identifier);
      if (tokens_.get(name).kind == TokenKind::Identifier)
        (void)tree_.appendLeaf(NodeKind::Name, name);
      (void)expect(TokenKind::Colon);
      (void)parseType();
      (void)expect(TokenKind::Equal);
      (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(
          NodeKind::StaticDecl, introducer, value_start, false,
          is_public ? NodeFlags::IsPublic : NodeFlags::None);
      return true;
    }
    if (atContextualKeyword("alias")) {
      const auto introducer = consume();
      const auto name = expect(TokenKind::Identifier);
      if (tokens_.get(name).kind == TokenKind::Identifier)
        (void)tree_.appendLeaf(NodeKind::Name, name);
      (void)parseGenericParameterList();
      (void)parseWhereClause();
      (void)expect(TokenKind::Equal);
      (void)parseType();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::AliasDecl, introducer, start, false,
                                is_public ? NodeFlags::IsPublic
                                          : NodeFlags::None);
      return true;
    }
    if (at(TokenKind::KwTrait)) {
      const auto introducer = consume();
      const auto name = expect(TokenKind::Identifier);
      if (tokens_.get(name).kind == TokenKind::Identifier)
        (void)tree_.appendLeaf(NodeKind::Name, name);
      (void)parseGenericParameterList();
      (void)parseWhereClause();
      (void)expect(TokenKind::LBrace);
      while (!at(TokenKind::RBrace) && !atEnd()) {
        const auto member_public = consumeIf(TokenKind::KwPub);
        if (atContextualKeyword("alias")) {
          const auto member_start = tree_.size();
          const auto alias = consume();
          const auto member_name = expect(TokenKind::Identifier);
          if (tokens_.get(member_name).kind == TokenKind::Identifier)
            (void)tree_.appendLeaf(NodeKind::Name, member_name);
          auto flags = NodeFlags::None;
          if (consumeIf(TokenKind::Equal))
            (void)parseType();
          else
            flags = flags | NodeFlags::IsBodyless;
          (void)expect(TokenKind::Semicolon);
          (void)tree_.appendSubtree(NodeKind::AssociatedAliasDecl, alias,
                                    member_start, false, flags);
          continue;
        }
        TokenId member_async;
        (void)consumeContextualKeyword("async", &member_async);
        const auto member_const = consumeIf(TokenKind::KwConst);
        if (!at(TokenKind::KwFn)) {
          diagnostics_.emit(DiagnosticKind::ExpectedDeclaration, token().offset,
                            std::max<std::uint16_t>(token().length, 1));
          recoverToStatement();
          continue;
        }
        (void)parseFunctionDeclaration(member_public, false, member_const,
                                       member_async);
      }
      (void)expect(TokenKind::RBrace);
      (void)tree_.appendSubtree(
          NodeKind::InterfaceDecl, introducer, start, false,
          is_public ? NodeFlags::IsPublic : NodeFlags::None);
      return true;
    }
    if (at(TokenKind::KwEnum)) {
      const auto introducer = consume();
      if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::Name, consume());
      else
        (void)expect(TokenKind::Identifier);
      (void)parseGenericParameterList();
      (void)parseWhereClause();
      (void)expect(TokenKind::LBrace);
      while (!at(TokenKind::RBrace) && !atEnd()) {
        const auto variant_start = tree_.size();
        const auto variant = expect(TokenKind::Identifier);
        if (tokens_.get(variant).kind != TokenKind::Identifier) {
          recoverToStatement();
          continue;
        }
        (void)tree_.appendLeaf(NodeKind::Name, variant);
        if (consumeIf(TokenKind::LBrace)) {
          while (!at(TokenKind::RBrace) && !atEnd()) {
            const auto field_start = tree_.size();
            const auto named =
                at(TokenKind::Identifier) && position_ + 1 < tokens_.size() &&
                tokens_.get(TokenId(position_ + 1)).kind == TokenKind::Colon;
            TokenId field_token = current();
            if (named) {
              field_token = consume();
              (void)tree_.appendLeaf(NodeKind::Name, field_token);
              (void)consume();
            }
            (void)parseType();
            (void)tree_.appendSubtree(NodeKind::EnumPayloadField, field_token,
                                      field_start);
            if (named) {
              if (!consumeIf(TokenKind::Semicolon))
                (void)expect(TokenKind::Semicolon);
            } else if (!consumeIf(TokenKind::Comma)) {
              break;
            }
          }
          (void)expect(TokenKind::RBrace);
        }
        if (at(TokenKind::Equal)) {
          const auto equal = consume();
          requireVersion(FrozenV18LanguageVersion,
                         DiagnosticKind::ValueEnumRequires18, equal);
          const auto discriminant_start = tree_.size();
          (void)parseExpression();
          (void)tree_.appendSubtree(NodeKind::EnumDiscriminant, equal,
                                    discriminant_start);
        }
        (void)tree_.appendSubtree(NodeKind::EnumVariant, variant,
                                  variant_start);
        if (!consumeIf(TokenKind::Comma) && !at(TokenKind::RBrace))
          (void)expect(TokenKind::Comma);
      }
      (void)expect(TokenKind::RBrace);
      (void)tree_.appendSubtree(NodeKind::EnumDecl, introducer, start, false,
                                is_public ? NodeFlags::IsPublic
                                          : NodeFlags::None);
      return true;
    }
    if (at(TokenKind::KwStruct) || at(TokenKind::KwUnion)) {
      const auto is_union = at(TokenKind::KwUnion);
      const auto introducer = consume();
      if (at(TokenKind::Identifier))
        (void)tree_.appendLeaf(NodeKind::Name, consume());
      else
        (void)expect(TokenKind::Identifier);
      (void)parseGenericParameterList();
      (void)parseWhereClause();
      (void)expect(TokenKind::LBrace);
      while (!at(TokenKind::RBrace) && !atEnd()) {
        const auto field_start = tree_.size();
        while (at(TokenKind::Identifier) && tokens_.text(current()) == "repr")
          (void)parseAttribute();
        const auto field_public = consumeIf(TokenKind::KwPub);
        const auto field_token = expect(TokenKind::Identifier);
        if (tokens_.get(field_token).kind == TokenKind::Identifier)
          (void)tree_.appendLeaf(NodeKind::Name, field_token);
        (void)expect(TokenKind::Colon);
        (void)parseType();
        (void)expect(TokenKind::Semicolon);
        (void)tree_.appendSubtree(
            is_union ? NodeKind::UnionField : NodeKind::StructField,
            field_token, field_start, false,
            field_public ? NodeFlags::IsPublic : NodeFlags::None);
      }
      (void)expect(TokenKind::RBrace);
      (void)tree_.appendSubtree(
          is_union ? NodeKind::UnionDecl : NodeKind::StructDecl, introducer,
          start, false, is_public ? NodeFlags::IsPublic : NodeFlags::None);
      return true;
    }
    if (at(TokenKind::KwImpl)) {
      const auto introducer = consume();
      // Impl-level parameters belong to the impl pattern, not to each
      // lifecycle member. Keep the generic parameter list as the first child
      // so semantic checking can establish one canonical region.
      (void)parseGenericParameterList();
      (void)parseType();
      const auto is_trait = consumeIf(TokenKind::KwFor);
      if (is_trait)
        (void)parseType();
      if (is_trait && consumeIf(TokenKind::KwAs)) {
        const auto alias = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, alias,
                               tokens_.get(alias).kind !=
                                   TokenKind::Identifier);
      }
      (void)parseWhereClause();
      (void)expect(TokenKind::LBrace);
      while (!at(TokenKind::RBrace) && !atEnd()) {
        const auto member_public = consumeIf(TokenKind::KwPub);
        if (atContextualKeyword("alias")) {
          const auto member_start = tree_.size();
          const auto alias = consume();
          const auto member_name = expect(TokenKind::Identifier);
          if (tokens_.get(member_name).kind == TokenKind::Identifier)
            (void)tree_.appendLeaf(NodeKind::Name, member_name);
          (void)expect(TokenKind::Equal);
          (void)parseType();
          (void)expect(TokenKind::Semicolon);
          (void)tree_.appendSubtree(NodeKind::AssociatedAliasDecl, alias,
                                    member_start);
          continue;
        }
        TokenId member_async;
        (void)consumeContextualKeyword("async", &member_async);
        const auto member_const = consumeIf(TokenKind::KwConst);
        if (!at(TokenKind::KwFn)) {
          diagnostics_.emit(DiagnosticKind::ExpectedDeclaration, token().offset,
                            std::max<std::uint16_t>(token().length, 1));
          recoverToStatement();
          continue;
        }
        (void)parseFunctionDeclaration(member_public, false, member_const,
                                       member_async);
      }
      (void)expect(TokenKind::RBrace);
      auto flags = is_public ? NodeFlags::IsPublic : NodeFlags::None;
      if (is_trait)
        flags = flags | NodeFlags::IsTraitImpl;
      (void)tree_.appendSubtree(NodeKind::ImplDecl, introducer, start, false,
                                flags);
      return true;
    }
    if (!at(TokenKind::KwFn)) {
      diagnostics_.emit(DiagnosticKind::ExpectedDeclaration, token().offset,
                        std::max<std::uint16_t>(token().length, 1));
      return false;
    }
    (void)parseFunctionDeclaration(is_public, is_unsafe, is_const,
                                   async_introducer);
    return true;
  }
