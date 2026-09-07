// Included inside Parser; this fragment aliases the original token/tree
// state and preserves grammar recovery and precedence behavior.
  static bool isAssignmentOperator(TokenKind kind) {
    switch (kind) {
    case TokenKind::Equal:
    case TokenKind::PlusEqual:
    case TokenKind::MinusEqual:
    case TokenKind::StarEqual:
    case TokenKind::SlashEqual:
    case TokenKind::PercentEqual:
    case TokenKind::AmpEqual:
    case TokenKind::PipeEqual:
    case TokenKind::CaretEqual:
    case TokenKind::LessLessEqual:
    case TokenKind::GreaterGreaterEqual:
      return true;
    default:
      return false;
    }
  }

  NodeId parseExpression(int minimum_precedence = 1,
                         bool allow_placement = true) {
    auto left = parseUnary();
    if (!left.hasValue())
      return left;
    while (at(TokenKind::KwAs)) {
      const auto op = consume();
      const auto start = tree_.subtreeStart(left);
      const auto checked = consumeIf(TokenKind::Question);
      (void)parseType();
      left = tree_.appendSubtree(
          checked ? NodeKind::CheckedCastExpr : NodeKind::CastExpr, op, start);
    }
    while (true) {
      const auto operator_info = binaryOperatorInfo(token().kind);
      if (!operator_info || operator_info->precedence < minimum_precedence)
        break;
      const auto op = consume();
      const auto start = tree_.subtreeStart(left);
      const auto next_precedence =
          operator_info->associativity == Associativity::Right
              ? operator_info->precedence
              : operator_info->precedence + 1;
      (void)parseExpression(next_precedence, false);
      left = tree_.appendSubtree(NodeKind::BinaryExpr, op, start);
    }
    if (allow_placement && minimum_precedence == 1 && at(TokenKind::KwIn)) {
      const auto op = consume();
      const auto start = tree_.subtreeStart(left);
      (void)parseExpression(1, false);
      left = tree_.appendSubtree(NodeKind::PlacementExpr, op, start);
    }
    return left;
  }

  NodeId parseUnary() {
    if (atContextualKeyword("wait") &&
        (position_ + 1 >= tokens_.size() ||
         tokens_.get(TokenId(position_ + 1)).kind != TokenKind::LParen)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)parseUnary();
      return tree_.appendSubtree(NodeKind::WaitExpr, introducer, start);
    }
    if (at(TokenKind::KwUnsafe)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      if (at(TokenKind::LBrace)) {
        requireVersion(FrozenV17LanguageVersion,
                       DiagnosticKind::ValueBlockRequires17, introducer);
        (void)parseValueBlock();
      } else {
        (void)parseUnary();
      }
      return tree_.appendSubtree(NodeKind::UnsafeExpr, introducer, start);
    }
    if (at(TokenKind::KwCopy)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)parseUnary();
      return tree_.appendSubtree(NodeKind::CopyExpr, introducer, start);
    }
    if (at(TokenKind::KwMove)) {
      const auto start = tree_.size();
      const auto introducer = consume();
      (void)parseUnary();
      return tree_.appendSubtree(NodeKind::MoveExpr, introducer, start);
    }
    if (at(TokenKind::Bang) || at(TokenKind::Plus) || at(TokenKind::Minus) ||
        at(TokenKind::Tilde) || at(TokenKind::Amp) || at(TokenKind::Star)) {
      const auto start = tree_.size();
      const auto op = consume();
      (void)parseUnary();
      return tree_.appendSubtree(NodeKind::UnaryExpr, op, start);
    }
    return parsePostfix();
  }

  NodeId parsePostfix() {
    auto expression = parsePrimary();
    while (expression.hasValue()) {
      const auto start = tree_.subtreeStart(expression);
      if (consumeIf(TokenKind::Dot)) {
        const auto member = current();
        if (at(TokenKind::IntegerLiteral)) {
          (void)consume();
          (void)tree_.appendLeaf(NodeKind::IntegerLiteral, member);
        } else {
          (void)expect(TokenKind::Identifier);
          (void)tree_.appendLeaf(NodeKind::Name, member,
                                 !at(TokenKind::Identifier) &&
                                     tokens_.get(member).kind !=
                                         TokenKind::Identifier);
        }
        expression = tree_.appendSubtree(NodeKind::MemberExpr, member, start);
        continue;
      }
      TokenId open;
      if (consumeIf(TokenKind::LBracket, &open)) {
        const auto range_start = tree_.size();
        if (at(TokenKind::DotDot)) {
          const auto separator = consume();
          if (!at(TokenKind::RBracket)) {
            const auto end_start = tree_.size();
            (void)parseExpression();
            (void)tree_.appendSubtree(NodeKind::SliceEnd, separator, end_start);
          }
          (void)expect(TokenKind::RBracket);
          expression = tree_.appendSubtree(NodeKind::SliceExpr, open, start);
          continue;
        }
        (void)parseExpression();
        if (at(TokenKind::DotDot)) {
          const auto separator = consume();
          (void)tree_.appendSubtree(NodeKind::SliceStart, separator,
                                    range_start);
          if (!at(TokenKind::RBracket)) {
            const auto end_start = tree_.size();
            (void)parseExpression();
            (void)tree_.appendSubtree(NodeKind::SliceEnd, separator, end_start);
          }
          (void)expect(TokenKind::RBracket);
          expression = tree_.appendSubtree(NodeKind::SliceExpr, open, start);
          continue;
        }
        (void)expect(TokenKind::RBracket);
        expression = tree_.appendSubtree(NodeKind::IndexExpr, open, start);
        continue;
      }
      if (consumeIf(TokenKind::LParen, &open)) {
        while (!at(TokenKind::RParen) && !atEnd()) {
          if (at(TokenKind::Dot) && position_ + 2 < tokens_.size() &&
              tokens_.get(TokenId(position_ + 1)).kind ==
                  TokenKind::Identifier &&
              tokens_.get(TokenId(position_ + 2)).kind == TokenKind::Equal) {
            const auto argument_start = tree_.size();
            (void)consume();
            const auto name = consume();
            (void)tree_.appendLeaf(NodeKind::Name, name);
            (void)consume();
            (void)parseExpression();
            (void)tree_.appendSubtree(NodeKind::NamedArgument, name,
                                      argument_start);
          } else {
            (void)parseExpression();
          }
          if (!consumeListSeparator(TokenKind::RParen))
            break;
        }
        (void)expect(TokenKind::RParen);
        expression = tree_.appendSubtree(NodeKind::CallExpr, open, start);
        continue;
      }
      TokenId question;
      if (consumeIf(TokenKind::Question, &question)) {
        expression = tree_.appendSubtree(NodeKind::TryExpr, question, start);
        continue;
      }
      break;
    }
    return expression;
  }

  NodeId parseEnumLiteral(TokenId introducer, std::size_t start) {
    (void)expect(TokenKind::LBrace);
    while (!at(TokenKind::RBrace) && !atEnd()) {
      const auto field_start = tree_.size();
      if (at(TokenKind::Identifier) && position_ + 1 < tokens_.size() &&
          tokens_.get(TokenId(position_ + 1)).kind == TokenKind::Equal) {
        const auto field = consume();
        (void)tree_.appendLeaf(NodeKind::Name, field);
        (void)consume();
        (void)parseExpression();
        (void)tree_.appendSubtree(NodeKind::NamedFieldInit, field, field_start);
      } else {
        (void)parseExpression();
      }
      if (!consumeIf(TokenKind::Comma))
        break;
    }
    (void)expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::EnumLiteral, introducer, start);
  }

  NodeId parseEnumPattern() {
    const auto start = tree_.size();
    const auto introducer = current();
    if (at(TokenKind::Identifier) && tokens_.text(current()) == "_") {
      (void)consume();
      return tree_.appendSubtree(NodeKind::WildcardPattern, introducer, start);
    }

    (void)parseType();
    if (consumeIf(TokenKind::Scope)) {
      const auto variant = expect(TokenKind::Identifier);
      (void)tree_.appendLeaf(NodeKind::Name, variant,
                             tokens_.get(variant).kind !=
                                 TokenKind::Identifier);
    }
    if (consumeIf(TokenKind::LBrace)) {
      bool saw_rest = false;
      while (!at(TokenKind::RBrace) && !atEnd()) {
        if (at(TokenKind::DotDot)) {
          const auto rest = consume();
          (void)tree_.appendLeaf(NodeKind::PatternRest, rest);
          const auto had_comma = consumeIf(TokenKind::Comma);
          if (saw_rest || !at(TokenKind::RBrace))
            diagnostics_.emit(DiagnosticKind::InvalidPatternRest,
                              tokens_.get(rest).offset,
                              tokens_.get(rest).length);
          saw_rest = true;
          if (!had_comma && !at(TokenKind::RBrace))
            break;
          continue;
        }
        const auto binding_start = tree_.size();
        const auto binding = expect(TokenKind::Identifier);
        (void)tree_.appendLeaf(NodeKind::Name, binding,
                               tokens_.get(binding).kind !=
                                   TokenKind::Identifier);
        (void)expect(TokenKind::Equal);
        TokenId action = current();
        if (at(TokenKind::KwMove) || at(TokenKind::KwCopy) ||
            at(TokenKind::Amp))
          action = consume();
        (void)expect(TokenKind::Dot);
        const auto projection_start = tree_.size();
        const auto projection_token = current();
        while (true) {
          if (at(TokenKind::Identifier))
            (void)tree_.appendLeaf(NodeKind::Name, consume());
          else if (at(TokenKind::IntegerLiteral))
            (void)tree_.appendLeaf(NodeKind::IntegerLiteral, consume());
          else {
            (void)expect(TokenKind::Identifier);
            break;
          }
          if (!consumeIf(TokenKind::Dot))
            break;
        }
        (void)tree_.appendSubtree(NodeKind::PatternProjection, projection_token,
                                  projection_start);
        (void)tree_.appendSubtree(NodeKind::PatternBinding, action,
                                  binding_start);
        if (!consumeIf(TokenKind::Comma))
          break;
      }
      (void)expect(TokenKind::RBrace);
    }
    return tree_.appendSubtree(NodeKind::EnumPattern, introducer, start);
  }

  NodeId parseStructuredPattern() {
    const auto start = tree_.size();
    const auto open = expect(TokenKind::LBrace);
    bool saw_rest = false;
    while (!at(TokenKind::RBrace) && !atEnd()) {
      if (at(TokenKind::DotDot)) {
        const auto rest = consume();
        (void)tree_.appendLeaf(NodeKind::PatternRest, rest);
        const auto had_comma = consumeIf(TokenKind::Comma);
        if (saw_rest || !at(TokenKind::RBrace))
          diagnostics_.emit(DiagnosticKind::InvalidPatternRest,
                            tokens_.get(rest).offset, tokens_.get(rest).length);
        saw_rest = true;
        if (!had_comma && !at(TokenKind::RBrace))
          break;
        continue;
      }
      const auto binding_start = tree_.size();
      const auto binding = expect(TokenKind::Identifier);
      (void)tree_.appendLeaf(NodeKind::Name, binding,
                             tokens_.get(binding).kind !=
                                 TokenKind::Identifier);
      (void)expect(TokenKind::Equal);
      TokenId action = current();
      if (at(TokenKind::KwMove) || at(TokenKind::KwCopy) ||
          at(TokenKind::Amp)) {
        action = consume();
      } else {
        diagnostics_.emit(
            DiagnosticKind::ExpectedToken, tokens_.get(current()).offset,
            std::max<std::uint16_t>(tokens_.get(current()).length, 1),
            TokenKind::KwMove, tokens_.get(current()).kind);
      }
      (void)expect(TokenKind::Dot);
      const auto projection_start = tree_.size();
      while (true) {
        const auto source = current();
        if (at(TokenKind::Identifier))
          (void)tree_.appendLeaf(NodeKind::Name, consume());
        else if (at(TokenKind::IntegerLiteral))
          (void)tree_.appendLeaf(NodeKind::IntegerLiteral, consume());
        else {
          (void)expect(TokenKind::Identifier);
          break;
        }
        if (!consumeIf(TokenKind::Dot))
          break;
      }
      (void)tree_.appendSubtree(NodeKind::PatternProjection, current(),
                                projection_start);
      (void)tree_.appendSubtree(NodeKind::PatternBinding, action,
                                binding_start);
      if (!consumeIf(TokenKind::Comma))
        break;
    }
    (void)expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::StructuredPattern, open, start);
  }

  NodeId parseSwitchExpression() {
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
        (void)parseValueBlock();
      else
        (void)parseExpression();
      (void)expect(TokenKind::Semicolon);
      (void)tree_.appendSubtree(NodeKind::SwitchArm, tree_.token(pattern),
                                arm_start);
    }
    (void)expect(TokenKind::RBrace);
    return tree_.appendSubtree(NodeKind::SwitchExpr, introducer, start);
  }

  NodeId parseIfExpression() {
    const auto start = tree_.size();
    const auto introducer = consume();
    (void)expect(TokenKind::LParen);
    (void)parseExpression();
    (void)expect(TokenKind::RParen);
    (void)parseValueBlock();
    (void)expect(TokenKind::KwElse);
    if (at(TokenKind::KwIf))
      (void)parseIfExpression();
    else
      (void)parseValueBlock();
    return tree_.appendSubtree(NodeKind::IfExpr, introducer, start);
  }

  NodeId parsePrimary() {
    const auto start = tree_.size();
    const auto current_token = current();
    if (at(TokenKind::LBrace)) {
      requireVersion(FrozenV17LanguageVersion,
                     DiagnosticKind::ValueBlockRequires17, current_token);
      (void)parseValueBlock();
      return tree_.appendSubtree(NodeKind::BlockExpr, current_token, start);
    }
    if (at(TokenKind::KwFn)) {
      const auto introducer = consume();
      requireVersion(FrozenV12LanguageVersion,
                     DiagnosticKind::CallableRequires12, introducer);
      (void)expect(TokenKind::LBracket);
      const auto captures_start = tree_.size();
      while (!at(TokenKind::RBracket) && !atEnd()) {
        const auto capture_start = tree_.size();
        const auto capture = current();
        if (consumeIf(TokenKind::KwCopy)) {
          const auto name = expect(TokenKind::Identifier);
          if (tokens_.get(name).kind == TokenKind::Identifier)
            (void)tree_.appendLeaf(NodeKind::Name, name);
          (void)tree_.appendSubtree(NodeKind::ClosureCopyCapture, capture,
                                    capture_start);
        } else if (consumeIf(TokenKind::KwMove)) {
          const auto name = expect(TokenKind::Identifier);
          if (tokens_.get(name).kind == TokenKind::Identifier)
            (void)tree_.appendLeaf(NodeKind::Name, name);
          (void)tree_.appendSubtree(NodeKind::ClosureMoveCapture, capture,
                                    capture_start);
        } else if (consumeIf(TokenKind::KwVar)) {
          const auto name = expect(TokenKind::Identifier);
          if (tokens_.get(name).kind == TokenKind::Identifier)
            (void)tree_.appendLeaf(NodeKind::Name, name);
          (void)expect(TokenKind::Equal);
          (void)parseExpression();
          (void)tree_.appendSubtree(NodeKind::ClosureVarCapture, capture,
                                    capture_start);
        } else {
          diagnostics_.emit(DiagnosticKind::ExpectedExpression, token().offset,
                            std::max<std::uint16_t>(token().length, 1));
          if (!atEnd())
            (void)consume();
        }
        if (!consumeListSeparator(TokenKind::RBracket))
          break;
      }
      const auto capture_close = expect(TokenKind::RBracket);
      (void)tree_.appendSubtree(NodeKind::ClosureCaptureList, capture_close,
                                captures_start);

      (void)expect(TokenKind::LParen);
      const auto parameters_start = tree_.size();
      while (!at(TokenKind::RParen) && !atEnd()) {
        const auto name = expect(TokenKind::Identifier);
        if (tokens_.get(name).kind == TokenKind::Identifier)
          (void)tree_.appendLeaf(NodeKind::Name, name);
        (void)expect(TokenKind::Colon);
        (void)parseType();
        if (!consumeListSeparator(TokenKind::RParen))
          break;
      }
      const auto parameters_close = expect(TokenKind::RParen);
      (void)tree_.appendSubtree(NodeKind::ParameterList, parameters_close,
                                parameters_start);
      (void)expect(TokenKind::Colon);
      (void)parseType();
      if (language_version_ >= FrozenV17LanguageVersion)
        (void)parseValueBlock();
      else
        (void)parseBlock();
      return tree_.appendSubtree(NodeKind::ClosureExpr, introducer, start);
    }
    if (at(TokenKind::KwIf))
      return parseIfExpression();
    if (at(TokenKind::KwSwitch))
      return parseSwitchExpression();
    if (at(TokenKind::KwSizeof) || at(TokenKind::KwAlignof)) {
      const auto is_size = at(TokenKind::KwSizeof);
      const auto introducer = consume();
      (void)expect(TokenKind::LParen);
      (void)parseType();
      (void)expect(TokenKind::RParen);
      return tree_.appendSubtree(is_size ? NodeKind::SizeofExpr
                                         : NodeKind::AlignofExpr,
                                 introducer, start);
    }
    if (at(TokenKind::Identifier) && position_ + 1 < tokens_.size() &&
        tokens_.get(TokenId(position_ + 1)).kind == TokenKind::LParen) {
      const auto query_name = tokens_.text(current());
      const auto query_kind =
          query_name == "type_same"      ? NodeKind::TypeSameExpr
          : query_name == "type_is"      ? NodeKind::TypeIsExpr
          : query_name == "type_has"     ? NodeKind::TypeHasExpr
          : query_name == "tuple_arity"  ? NodeKind::TupleArityExpr
          : query_name == "array_extent" ? NodeKind::ArrayExtentExpr
                                         : NodeKind::Error;
      if (query_kind != NodeKind::Error) {
        const auto introducer = consume();
        (void)expect(TokenKind::LParen);
        (void)parseType();
        if (query_kind == NodeKind::TypeSameExpr) {
          (void)expect(TokenKind::Comma);
          (void)parseType();
        } else if (query_kind == NodeKind::TypeIsExpr ||
                   query_kind == NodeKind::TypeHasExpr) {
          (void)expect(TokenKind::Comma);
          const auto property = current();
          if (at(TokenKind::Identifier) || at(TokenKind::KwCopy) ||
              at(TokenKind::KwMove))
            (void)consume();
          else
            (void)expect(TokenKind::Identifier);
          (void)tree_.appendLeaf(NodeKind::Name, property);
        }
        (void)expect(TokenKind::RParen);
        return tree_.appendSubtree(query_kind, introducer, start);
      }
    }
    switch (token().kind) {
    case TokenKind::Identifier:
      consume();
      (void)tree_.appendLeaf(NodeKind::Name, current_token);
      {
        NodeId result(static_cast<std::uint32_t>(tree_.size() - 1));
        if (consumeIf(TokenKind::Scope)) {
          do {
            const auto member = expect(TokenKind::Identifier);
            (void)tree_.appendLeaf(NodeKind::Name, member,
                                   tokens_.get(member).kind !=
                                       TokenKind::Identifier);
          } while (consumeIf(TokenKind::Scope));
          result = tree_.appendSubtree(NodeKind::QualifiedName, current_token,
                                       start);
        }
        if (looksLikeGenericAggregate()) {
          (void)consume();
          while (!at(TokenKind::Greater) && !atEnd()) {
            (void)parseType();
            if (!consumeListSeparator(TokenKind::Greater))
              break;
          }
          const auto close = expect(TokenKind::Greater);
          result = tree_.appendSubtree(NodeKind::GenericType, close, start);
          if (consumeIf(TokenKind::Scope)) {
            const auto variant = expect(TokenKind::Identifier);
            (void)tree_.appendLeaf(NodeKind::Name, variant,
                                   tokens_.get(variant).kind !=
                                       TokenKind::Identifier);
            if (at(TokenKind::LBrace))
              return parseEnumLiteral(current_token, start);
            return tree_.appendSubtree(NodeKind::TypeMemberExpr, current_token,
                                       start);
          }
        }
        if (at(TokenKind::LBrace)) {
          if (tree_.kind(result) == NodeKind::QualifiedName &&
              !(position_ + 1 < tokens_.size() &&
                tokens_.get(TokenId(position_ + 1)).kind == TokenKind::Dot))
            return parseEnumLiteral(current_token, start);
          return parseAggregateLiteral(current_token, start);
        }
        return result;
      }
    case TokenKind::IntegerLiteral:
      consume();
      return tree_.appendLeaf(NodeKind::IntegerLiteral, current_token);
    case TokenKind::FloatLiteral:
      consume();
      return tree_.appendLeaf(NodeKind::FloatLiteral, current_token);
    case TokenKind::StringLiteral:
      consume();
      return tree_.appendLeaf(NodeKind::StringLiteral, current_token);
    case TokenKind::CStringLiteral:
      consume();
      return tree_.appendLeaf(NodeKind::CStringLiteral, current_token);
    case TokenKind::CharLiteral:
      consume();
      return tree_.appendLeaf(NodeKind::CharLiteral, current_token);
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
      consume();
      return tree_.appendLeaf(NodeKind::BoolLiteral, current_token);
    case TokenKind::KwNull:
      consume();
      return tree_.appendLeaf(NodeKind::NullLiteral, current_token);
    case TokenKind::LParen: {
      const auto open = consume();
      const auto tuple_start = tree_.size();
      if (at(TokenKind::RParen)) {
        (void)consume();
        return tree_.appendSubtree(NodeKind::TupleLiteral, open, start);
      }
      const auto first = parseExpression();
      if (!consumeIf(TokenKind::Comma)) {
        (void)expect(TokenKind::RParen);
        return first;
      }
      while (!at(TokenKind::RParen) && !atEnd()) {
        (void)parseExpression();
        if (!consumeIf(TokenKind::Comma))
          break;
      }
      (void)expect(TokenKind::RParen);
      (void)tuple_start;
      return tree_.appendSubtree(NodeKind::TupleLiteral, open, start);
    }
    case TokenKind::LBracket: {
      const auto open = consume();
      while (!at(TokenKind::RBracket) && !atEnd()) {
        (void)parseExpression();
        if (!consumeListSeparator(TokenKind::RBracket))
          break;
      }
      (void)expect(TokenKind::RBracket);
      return tree_.appendSubtree(NodeKind::ArrayLiteral, open, start);
    }
    default:
      diagnostics_.emit(DiagnosticKind::ExpectedExpression, token().offset,
                        std::max<std::uint16_t>(token().length, 1));
      if (!atEnd())
        consume();
      return tree_.appendLeaf(NodeKind::Error, current_token, true);
    }
  }

