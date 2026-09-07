// Included by CFDL.cpp; this fragment aliases the existing CFDL parser,
// validation, rendering, and SemIR state without duplicating ownership.
enum class TokenKind : std::uint8_t {
  Word,
  Number,
  String,
  LParen,
  RParen,
  Semicolon,
  Comma,
  Colon,
  Dot,
  Star,
  Less,
  Greater,
  LBrace,
  RBrace,
  Equal,
  EqualEqual,
  BangEqual,
  Minus,
  Arrow,
  End,
};

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;
  std::uint32_t offset = 0;
};

std::vector<Token> lexCFDL(const SourceBuffer &source,
                           std::vector<CFDLDiagnostic> &diagnostics) {
  std::vector<Token> result;
  const auto text = source.text();
  std::size_t offset = 0;
  while (offset < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[offset]))) {
      ++offset;
      continue;
    }
    if (text[offset] == '/' && offset + 1 < text.size() &&
        text[offset + 1] == '/') {
      offset = text.find('\n', offset);
      if (offset == std::string_view::npos)
        break;
      continue;
    }
    const auto start = offset;
    TokenKind kind = TokenKind::End;
    if (text[offset] == '(') {
      kind = TokenKind::LParen;
      ++offset;
    } else if (text[offset] == ')') {
      kind = TokenKind::RParen;
      ++offset;
    } else if (text[offset] == ';') {
      kind = TokenKind::Semicolon;
      ++offset;
    } else if (text[offset] == ',') {
      kind = TokenKind::Comma;
      ++offset;
    } else if (text[offset] == ':') {
      kind = TokenKind::Colon;
      ++offset;
    } else if (text[offset] == '.') {
      kind = TokenKind::Dot;
      ++offset;
    } else if (text[offset] == '*') {
      kind = TokenKind::Star;
      ++offset;
    } else if (text[offset] == '<') {
      kind = TokenKind::Less;
      ++offset;
    } else if (text[offset] == '>') {
      kind = TokenKind::Greater;
      ++offset;
    } else if (text[offset] == '{') {
      kind = TokenKind::LBrace;
      ++offset;
    } else if (text[offset] == '}') {
      kind = TokenKind::RBrace;
      ++offset;
    } else if (text[offset] == '!' && offset + 1 < text.size() &&
               text[offset + 1] == '=') {
      kind = TokenKind::BangEqual;
      offset += 2;
    } else if (text[offset] == '=' && offset + 1 < text.size() &&
               text[offset + 1] == '=') {
      kind = TokenKind::EqualEqual;
      offset += 2;
    } else if (text[offset] == '=') {
      kind = TokenKind::Equal;
      ++offset;
    } else if (text[offset] == '-' && offset + 1 < text.size() &&
               text[offset + 1] == '>') {
      kind = TokenKind::Arrow;
      offset += 2;
    } else if (text[offset] == '-') {
      kind = TokenKind::Minus;
      ++offset;
    } else if (text[offset] == '"') {
      kind = TokenKind::String;
      std::string value;
      ++offset;
      bool closed = false;
      while (offset < text.size()) {
        if (text[offset] == '"') {
          ++offset;
          closed = true;
          break;
        }
        if (text[offset] == '\\' && offset + 1 < text.size() &&
            (text[offset + 1] == '\\' || text[offset + 1] == '"')) {
          value.push_back(text[offset + 1]);
          offset += 2;
          continue;
        }
        const auto byte = static_cast<unsigned char>(text[offset]);
        if (byte < 0x20 || byte > 0x7e || text[offset] == '\\')
          break;
        value.push_back(text[offset++]);
      }
      if (!closed) {
        diagnostics.push_back(
            {static_cast<std::uint32_t>(start), "invalid CFDL string literal"});
        continue;
      }
      result.push_back(
          {kind, std::move(value), static_cast<std::uint32_t>(start)});
      continue;
    } else if (std::isalpha(static_cast<unsigned char>(text[offset])) ||
               text[offset] == '_') {
      kind = TokenKind::Word;
      while (offset < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[offset])) ||
              text[offset] == '_' ||
              (text[offset] == ':' && offset + 1 < text.size() &&
               text[offset + 1] == ':'))) {
        if (text[offset] == ':')
          offset += 2;
        else
          ++offset;
      }
    } else if (std::isdigit(static_cast<unsigned char>(text[offset]))) {
      kind = TokenKind::Number;
      while (offset < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[offset])))
        ++offset;
    } else {
      diagnostics.push_back({static_cast<std::uint32_t>(offset),
                             "unexpected character in CFDL input"});
      ++offset;
      continue;
    }
    result.push_back({kind, std::string(text.substr(start, offset - start)),
                      static_cast<std::uint32_t>(start)});
  }
  result.push_back(
      {TokenKind::End, {}, static_cast<std::uint32_t>(text.size())});
  return result;
}

class Parser {
public:
  Parser(std::vector<Token> tokens, std::vector<CFDLDiagnostic> &diagnostics)
      : tokens_(std::move(tokens)), diagnostics_(diagnostics) {}

  bool parse(CFDLSyntaxFile &file) {
    file = {};
    if (!consume("module")) {
      error("CFDL file must begin with a module declaration", 0,
            CFDLDiagnostic::Kind::InvalidModule);
    } else {
      file.module_offset = tokens_[index_ - 1].offset;
      if (!name(file.module_name))
        return false;
      expect(TokenKind::Semicolon, "expected `;` after module declaration");
    }
    while (!at(TokenKind::End)) {
      if (consume("import")) {
        std::string imported;
        const auto offset = tokens_[index_ - 1].offset;
        if (name(imported)) {
          file.imports.push_back(std::move(imported));
          file.import_offsets.push_back(offset);
        }
        expect(TokenKind::Semicolon, "expected `;` after CFDL import");
      } else if (consume("foreign")) {
        const auto declaration_offset = tokens_[index_ - 1].offset;
        if (consume("type")) {
          CFDLForeignTypeSyntax type;
          type.offset = declaration_offset;
          if (name(type.name) && parseForeignType(type, false)) {
            file.foreign_types.push_back(std::move(type));
          }
        } else if (consume("struct") || consume("union") || consume("enum")) {
          const auto kind = tokens_[index_ - 1].text;
          CFDLForeignTypeSyntax type;
          type.offset = declaration_offset;
          type.carrier_kind = kind == "struct"  ? CFDLForeignCarrierKind::Record
                              : kind == "union" ? CFDLForeignCarrierKind::Union
                                                : CFDLForeignCarrierKind::Enum;
          if (name(type.name) && parseForeignType(type, true)) {
            file.foreign_types.push_back(std::move(type));
          }
        } else if (consume("const")) {
          CFDLForeignConstantSyntax constant;
          constant.offset = declaration_offset;
          if (parseForeignConstant(constant))
            file.foreign_constants.push_back(std::move(constant));
        } else if (consume("fn")) {
          CFDLCallableSyntax callable;
          if (parseCallable(callable, declaration_offset))
            file.callables.push_back(std::move(callable));
        } else {
          error("expected `type`, `struct`, `union`, `enum`, `const`, or `fn` "
                "after `foreign`");
          recoverDeclaration();
        }
      } else {
        error("expected CFDL import or foreign declaration");
        recoverDeclaration();
      }
    }
    return diagnostics_.empty();
  }

private:
  bool parseUnsignedInteger(std::uint64_t &result) {
    if (!at(TokenKind::Number)) {
      error("expected integer literal");
      return false;
    }
    std::uint64_t value = 0;
    for (const char ch : tokens_[index_].text) {
      const auto digit = static_cast<std::uint64_t>(ch - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
        error("integer literal is out of range");
        return false;
      }
      value = value * 10 + digit;
    }
    ++index_;
    result = value;
    return true;
  }

  bool parseForeignConstant(CFDLForeignConstantSyntax &constant) {
    if (!name(constant.name) ||
        !expect(TokenKind::Colon, "expected `:` after foreign constant name") ||
        !parsePhysicalType(constant.physical_type) ||
        !expect(TokenKind::Equal, "expected `=` after foreign constant type"))
      return false;
    if (consume("true") || consume("false")) {
      constant.kind = CFDLForeignConstantKind::Bool;
      constant.bool_value = tokens_[index_ - 1].text == "true";
    } else {
      constant.kind = CFDLForeignConstantKind::Integer;
      constant.integer_negative = consumeKind(TokenKind::Minus);
      if (!parseUnsignedInteger(constant.integer_payload))
        return false;
    }
    if (!expect(TokenKind::Semicolon,
                "expected `;` after foreign constant declaration"))
      return false;
    constant.end_offset = tokens_[index_ - 1].offset + 1;
    return true;
  }

  bool parseForeignInvalid(CFDLForeignTypeSyntax &type) {
    if (!consume("invalid"))
      return true;
    if (consume("null")) {
      type.invalid_kind = CFDLForeignInvalidKind::Null;
      return true;
    }
    const bool negative = consumeKind(TokenKind::Minus);
    if (!at(TokenKind::Number)) {
      error("foreign invalid value must be `null` or an integer");
      return false;
    }
    std::int64_t value = 0;
    for (const char ch : tokens_[index_].text) {
      if (value >
          (std::numeric_limits<std::int64_t>::max() - (ch - '0')) / 10) {
        error("foreign invalid integer is out of range");
        return false;
      }
      value = value * 10 + (ch - '0');
    }
    ++index_;
    type.invalid_kind = CFDLForeignInvalidKind::Integer;
    type.invalid_integer = negative ? -value : value;
    return true;
  }

  bool parseForeignType(CFDLForeignTypeSyntax &type, bool explicit_kind) {
    if (type.carrier_kind == CFDLForeignCarrierKind::Enum) {
      if (!expect(TokenKind::Colon, "expected `:` before enum carrier") ||
          !parsePhysicalType(type.scalar_carrier) ||
          !expect(TokenKind::LBrace, "expected `{` after enum carrier"))
        return false;
      while (!at(TokenKind::RBrace) && !at(TokenKind::End)) {
        CFDLEnumConstantSyntax constant;
        constant.offset = tokens_[index_].offset;
        if (!name(constant.name) ||
            !expect(TokenKind::Equal, "expected `=` after enum constant") ||
            !parseSignedInteger(constant.value) ||
            !expect(TokenKind::Semicolon, "expected `;` after enum constant"))
          return false;
        type.enum_constants.push_back(std::move(constant));
      }
      if (!expect(TokenKind::RBrace, "expected `}` after enum body"))
        return false;
      if (!parseForeignInvalid(type))
        return false;
    } else if (type.carrier_kind == CFDLForeignCarrierKind::Record ||
               type.carrier_kind == CFDLForeignCarrierKind::Union) {
      if (!parseForeignFields(type.fields,
                              type.carrier_kind == CFDLForeignCarrierKind::Union
                                  ? "foreign union"
                                  : "foreign struct"))
        return false;
    } else if (consumeKind(TokenKind::Colon)) {
      type.carrier_kind = CFDLForeignCarrierKind::Scalar;
      if (!parsePhysicalType(type.scalar_carrier))
        return false;
      if (!parseForeignInvalid(type))
        return false;
    } else if (explicit_kind) {
      error("foreign carrier declaration is incomplete");
      return false;
    }
    if (!expect(TokenKind::Semicolon,
                "expected `;` after foreign type declaration"))
      return false;
    type.end_offset = tokens_[index_ - 1].offset + 1;
    return true;
  }

  bool parseForeignFields(std::vector<CFDLForeignFieldSyntax> &fields,
                          std::string_view owner) {
    if (!expect(TokenKind::LBrace, "expected `{` after " + std::string(owner)))
      return false;
    while (!at(TokenKind::RBrace) && !at(TokenKind::End)) {
      CFDLForeignFieldSyntax field;
      field.offset = tokens_[index_].offset;
      if (!name(field.name) ||
          !expect(TokenKind::Colon, "expected `:` after foreign field name") ||
          !parsePhysicalType(field.physical_type) ||
          !expect(TokenKind::Semicolon,
                  "expected `;` after foreign field declaration"))
        return false;
      fields.push_back(std::move(field));
    }
    return expect(TokenKind::RBrace,
                  "expected `}` after " + std::string(owner) + " body");
  }

  bool parseSignedInteger(std::int64_t &result) {
    const bool negative = consumeKind(TokenKind::Minus);
    if (!at(TokenKind::Number)) {
      error("expected integer literal");
      return false;
    }
    std::int64_t value = 0;
    for (const char ch : tokens_[index_].text) {
      if (value >
          (std::numeric_limits<std::int64_t>::max() - (ch - '0')) / 10) {
        error("integer literal is out of range");
        return false;
      }
      value = value * 10 + (ch - '0');
    }
    ++index_;
    result = negative ? -value : value;
    return true;
  }

  bool parseCallingConvention(ForeignCallingConvention &convention) {
    if (consume("c"))
      convention = ForeignCallingConvention::C;
    else if (consume("win64"))
      convention = ForeignCallingConvention::Win64;
    else if (consume("sysv64"))
      convention = ForeignCallingConvention::SysV64;
    else {
      error("expected `c`, `win64`, or `sysv64` calling convention");
      return false;
    }
    return true;
  }

  bool parsePhysicalType(std::string &value) {
    value.clear();
    if (consume("const"))
      value = "const ";
    if (consume("c_fn")) {
      value += "c_fn";
      if (word("c") || word("win64") || word("sysv64")) {
        value += " " + take().text;
      }
      value += '(';
      if (!expect(TokenKind::LParen, "expected `(` after `c_fn`"))
        return false;
      bool first = true;
      while (!at(TokenKind::RParen) && !at(TokenKind::End)) {
        std::string parameter;
        if (!parsePhysicalType(parameter))
          return false;
        if (!first)
          value += ',';
        value += parameter;
        first = false;
        if (!consumeKind(TokenKind::Comma))
          break;
      }
      if (!expect(TokenKind::RParen, "expected `)` after C function pointer") ||
          !expect(TokenKind::Arrow,
                  "expected `->` before C function pointer result"))
        return false;
      std::string result;
      if (!parsePhysicalType(result))
        return false;
      value += ")->" + result;
    } else {
      std::string type_name;
      if (!name(type_name))
        return false;
      value += type_name;
      if (consumeKind(TokenKind::Less)) {
        value += '<';
        bool first = true;
        while (!at(TokenKind::Greater) && !at(TokenKind::End)) {
          std::string argument;
          if (at(TokenKind::Number)) {
            argument = tokens_[index_++].text;
          } else if (!parsePhysicalType(argument)) {
            return false;
          }
          if (!first)
            value += ',';
          value += argument;
          first = false;
          if (!consumeKind(TokenKind::Comma))
            break;
        }
        if (!expect(TokenKind::Greater, "expected `>` after type arguments"))
          return false;
        value += '>';
      }
    }
    while (consumeKind(TokenKind::Star))
      value += '*';
    return true;
  }

  CFDLFlowQualifier parseQualifier() {
    if (consume("owned"))
      return CFDLFlowQualifier::Owned;
    if (consume("ref_mut"))
      return CFDLFlowQualifier::RefMut;
    if (consume("ref"))
      return CFDLFlowQualifier::Ref;
    if (consume("view_mut"))
      return CFDLFlowQualifier::ViewMut;
    if (consume("view"))
      return CFDLFlowQualifier::View;
    if (consume("move"))
      return CFDLFlowQualifier::Move;
    if (consume("out"))
      return CFDLFlowQualifier::Out;
    if (consume("inout"))
      return CFDLFlowQualifier::InOut;
    return CFDLFlowQualifier::Value;
  }

  bool parseFlowType(CFDLFlowTypeSyntax &type) {
    type.qualifier = parseQualifier();
    return parsePhysicalType(type.physical_type);
  }

  bool parseEndpoint(std::string &value) {
    return name(value);
  }

  bool parseWhereFact(CFDLWhereFactSyntax &fact) {
    fact = {};
    fact.offset = tokens_[index_].offset;
    if (!name(fact.subject))
      return false;
    if (consume("escapes")) {
      fact.kind = CFDLWhereFactKind::Escapes;
      if (!parseEndpoint(fact.target))
        return false;
    } else if (consume("stores")) {
      fact.kind = CFDLWhereFactKind::Stores;
      if (!parseEndpoint(fact.target))
        return false;
    } else if (consume("derives")) {
      fact.kind = CFDLWhereFactKind::Derives;
      if (!parseEndpoint(fact.target))
        return false;
    } else if (consume("invokes")) {
      fact.kind = CFDLWhereFactKind::Invokes;
      if (!parseEndpoint(fact.target))
        return false;
    } else if (consume("obliges")) {
      fact.kind = CFDLWhereFactKind::Obliges;
      if (!name(fact.action))
        return false;
    } else if (consume("discharges")) {
      fact.kind = CFDLWhereFactKind::Discharges;
      if (!name(fact.action))
        return false;
    } else if (consume("requires")) {
      fact.kind = CFDLWhereFactKind::Requires;
      if (!name(fact.predicate))
        return false;
    } else {
      error("unknown CFDL where relation", fact.offset,
            CFDLDiagnostic::Kind::InvalidProtocol);
      return false;
    }
    return true;
  }

  bool parseErrorContract(CFDLErrorContractSyntax &contract) {
    contract.offset = tokens_[index_ - 1].offset;
    if (consume("code"))
      contract.domain = CFDLErrorDomainKind::Code;
    else if (consume("errno"))
      contract.domain = CFDLErrorDomainKind::Errno;
    else if (consume("win32"))
      contract.domain = CFDLErrorDomainKind::Win32;
    else {
      error("expected `code`, `errno`, or `win32` after `error`",
            contract.offset, CFDLDiagnostic::Kind::InvalidProtocol);
      return false;
    }
    if (!consume("when") || !name(contract.observed)) {
      error("expected `when <observed-value>` in error contract",
            contract.offset, CFDLDiagnostic::Kind::InvalidProtocol);
      return false;
    }
    if (consumeKind(TokenKind::EqualEqual))
      contract.predicate = CFDLErrorPredicateKind::Equal;
    else if (consumeKind(TokenKind::BangEqual))
      contract.predicate = CFDLErrorPredicateKind::NotEqual;
    else if (consumeKind(TokenKind::Less))
      contract.predicate = CFDLErrorPredicateKind::Less;
    else if (consume("in"))
      contract.predicate = CFDLErrorPredicateKind::InSet;
    else if (consume("not")) {
      if (!consume("in")) {
        error("expected `in` after `not` in error contract", contract.offset,
              CFDLDiagnostic::Kind::InvalidProtocol);
        return false;
      }
      contract.predicate = CFDLErrorPredicateKind::NotInSet;
    } else {
      error("error contract requires a comparison or set membership",
            contract.offset, CFDLDiagnostic::Kind::InvalidProtocol);
      return false;
    }
    if (consume("null")) {
      if (contract.predicate != CFDLErrorPredicateKind::Equal) {
        error("null failure sentinel requires `==`", contract.offset,
              CFDLDiagnostic::Kind::InvalidProtocol);
        return false;
      }
      contract.predicate = CFDLErrorPredicateKind::Null;
      return true;
    }
    if (consume("invalid")) {
      if (contract.predicate != CFDLErrorPredicateKind::Equal) {
        error("declared invalid sentinel requires `==`", contract.offset,
              CFDLDiagnostic::Kind::InvalidProtocol);
        return false;
      }
      contract.predicate = CFDLErrorPredicateKind::Invalid;
      return true;
    }
    if (contract.predicate == CFDLErrorPredicateKind::InSet ||
        contract.predicate == CFDLErrorPredicateKind::NotInSet) {
      if (!expect(TokenKind::LBrace, "expected `{` after set membership"))
        return false;
      while (!at(TokenKind::RBrace) && !at(TokenKind::End)) {
        if (contract.ranges.size() >= 64) {
          error("error predicate admits at most 64 set items", contract.offset,
                CFDLDiagnostic::Kind::InvalidProtocol);
          return false;
        }
        CFDLErrorRangeSyntax range;
        if (!parseErrorValue(range.lower))
          return false;
        if (consume("through")) {
          range.upper.emplace();
          if (!parseErrorValue(*range.upper))
            return false;
        }
        contract.ranges.push_back(std::move(range));
        if (!consumeKind(TokenKind::Comma))
          break;
      }
      if (contract.ranges.empty()) {
        error("error predicate set cannot be empty", contract.offset,
              CFDLDiagnostic::Kind::InvalidProtocol);
        return false;
      }
      return expect(TokenKind::RBrace,
                    "expected `}` after error predicate set");
    }
    CFDLErrorRangeSyntax value;
    if (!parseErrorValue(value.lower))
      return false;
    contract.ranges.push_back(std::move(value));
    return true;
  }

  bool parseErrorValue(CFDLErrorValueSyntax &value) {
    value.offset = tokens_[index_].offset;
    value.negative = consumeKind(TokenKind::Minus);
    if (at(TokenKind::Number))
      return parseUnsignedInteger(value.magnitude);
    if (value.negative || !at(TokenKind::Word)) {
      error("expected integer literal or local foreign constant");
      return false;
    }
    return name(value.name);
  }

  bool parseOutcomeContract(CFDLOutcomeContractSyntax &contract) {
    contract.offset = tokens_[index_ - 1].offset;
    if (consume("posix_read")) {
      contract.kind = CFDLOutcomeKind::PosixRead;
    } else if (consume("win32_read")) {
      contract.kind = CFDLOutcomeKind::Win32Read;
    } else if (consume("fread")) {
      contract.kind = CFDLOutcomeKind::Fread;
    } else {
      error("expected `posix_read`, `win32_read`, or `fread` after `outcome`",
            contract.offset, CFDLDiagnostic::Kind::InvalidProtocol);
      return false;
    }
    if (!expect(TokenKind::Less, "expected `<` before outcome element type") ||
        !parsePhysicalType(contract.element_type) ||
        !expect(TokenKind::Greater,
                "expected `>` after outcome element type") ||
        !expect(TokenKind::LParen, "expected `(` after foreign outcome") ||
        !name(contract.buffer) ||
        !expect(TokenKind::Comma, "expected `,` after outcome buffer"))
      return false;
    if (contract.kind == CFDLOutcomeKind::Fread) {
      if (!name(contract.element_size) ||
          !expect(TokenKind::Comma, "expected `,` after fread element size") ||
          !name(contract.element_count) ||
          !expect(TokenKind::Comma, "expected `,` after fread element count") ||
          !name(contract.stream))
        return false;
      contract.capacity = contract.element_count;
    } else if (!name(contract.capacity)) {
      return false;
    } else if (contract.kind == CFDLOutcomeKind::Win32Read &&
               (!expect(TokenKind::Comma,
                        "expected `,` after outcome capacity") ||
                !name(contract.count) ||
                !expect(TokenKind::Comma, "expected `,` after outcome count") ||
                !name(contract.context)))
      return false;
    if (!expect(TokenKind::RParen, "expected `)` after foreign outcome"))
      return false;
    return true;
  }

  bool parseCallable(CFDLCallableSyntax &callable,
                     std::uint32_t declaration_offset) {
    callable.offset = declaration_offset;
    if (!name(callable.name) ||
        !expect(TokenKind::LParen, "expected `(` after foreign function name"))
      return false;
    while (!at(TokenKind::RParen) && !at(TokenKind::End)) {
      CFDLCallableParameterSyntax parameter;
      parameter.offset = tokens_[index_].offset;
      if (!name(parameter.name) ||
          !expect(TokenKind::Colon, "expected `:` after parameter name") ||
          !parseFlowType(parameter.type))
        return false;
      callable.parameters.push_back(std::move(parameter));
      if (!consumeKind(TokenKind::Comma))
        break;
    }
    if (!expect(TokenKind::RParen, "expected `)` after foreign parameters") ||
        !expect(TokenKind::Arrow, "expected `->` before foreign result") ||
        !parseFlowType(callable.result))
      return false;
    bool saw_link = false;
    bool saw_where = false;
    while (!at(TokenKind::Semicolon) && !at(TokenKind::End)) {
      if (consume("link")) {
        if (saw_link || !at(TokenKind::String)) {
          error(saw_link ? "foreign function has more than one link clause"
                         : "expected external symbol string after `link`");
          return false;
        }
        saw_link = true;
        callable.external_symbol = take().text;
        if (callable.external_symbol.empty() ||
            callable.external_symbol.size() > 1024) {
          error("external symbol must contain 1 to 1024 printable ASCII bytes");
          return false;
        }
      } else if (consume("call")) {
        if (callable.explicit_calling_convention) {
          error("foreign function has more than one call clause");
          return false;
        }
        callable.explicit_calling_convention = true;
        if (!parseCallingConvention(callable.calling_convention))
          return false;
      } else if (consume("outcome")) {
        if (callable.outcome_contract) {
          error("foreign function has more than one outcome contract");
          return false;
        }
        CFDLOutcomeContractSyntax contract;
        if (!parseOutcomeContract(contract))
          return false;
        callable.outcome_contract = std::move(contract);
      } else if (consume("eof")) {
        if (!callable.outcome_contract ||
            callable.outcome_contract->kind != CFDLOutcomeKind::Fread ||
            !at(TokenKind::String)) {
          error("`eof` accessor is only valid on fread outcomes");
          return false;
        }
        callable.outcome_contract->eof_symbol = take().text;
        if (!expect(TokenKind::LParen, "expected `(` after eof accessor") ||
            !name(callable.outcome_contract->stream) ||
            !expect(TokenKind::RParen, "expected `)` after eof accessor"))
          return false;
      } else if (consume("ferror")) {
        if (!callable.outcome_contract ||
            callable.outcome_contract->kind != CFDLOutcomeKind::Fread ||
            !at(TokenKind::String)) {
          error("`ferror` accessor is only valid on fread outcomes");
          return false;
        }
        callable.outcome_contract->ferror_symbol = take().text;
        if (!expect(TokenKind::LParen, "expected `(` after ferror accessor") ||
            !name(callable.outcome_contract->stream) ||
            !expect(TokenKind::RParen, "expected `)` after ferror accessor"))
          return false;
      } else if (consume("error")) {
        if (callable.error_contract) {
          error("foreign function has more than one error contract");
          return false;
        }
        CFDLErrorContractSyntax contract;
        if (!parseErrorContract(contract))
          return false;
        callable.error_contract = std::move(contract);
      } else if (consume("where")) {
        if (saw_where) {
          error("foreign function has more than one where clause");
          return false;
        }
        saw_where = true;
        while (!at(TokenKind::Semicolon) && !at(TokenKind::End)) {
          CFDLWhereFactSyntax fact;
          if (!parseWhereFact(fact)) {
            return false;
          }
          callable.where_facts.push_back(std::move(fact));
          if (!consumeKind(TokenKind::Comma))
            break;
        }
      } else {
        error("expected foreign function clause or `;`");
        return false;
      }
    }
    if (!expect(TokenKind::Semicolon, "expected `;` after foreign function"))
      return false;
    callable.end_offset = tokens_[index_ - 1].offset + 1;
    return true;
  }

  bool at(TokenKind kind) const {
    return tokens_[index_].kind == kind;
  }
  bool word(std::string_view text) const {
    return at(TokenKind::Word) && tokens_[index_].text == text;
  }
  bool consume(std::string_view text) {
    if (!word(text))
      return false;
    ++index_;
    return true;
  }
  bool consumeKind(TokenKind kind) {
    if (!at(kind))
      return false;
    ++index_;
    return true;
  }
  Token take() {
    return tokens_[index_++];
  }
  bool name(std::string &value) {
    if (!at(TokenKind::Word)) {
      error("expected name");
      return false;
    }
    value = take().text;
    return true;
  }
  bool expect(TokenKind kind, std::string message) {
    if (consumeKind(kind))
      return true;
    error(std::move(message));
    return false;
  }
  void error(std::string message) {
    error(std::move(message), tokens_[index_].offset);
  }
  void error(std::string message, std::uint32_t offset,
             CFDLDiagnostic::Kind kind = CFDLDiagnostic::Kind::InvalidSyntax) {
    diagnostics_.push_back({kind, offset, 1, std::move(message)});
  }
  void recoverCallable() {
    while (!at(TokenKind::Semicolon) && !at(TokenKind::End))
      ++index_;
    consumeKind(TokenKind::Semicolon);
  }
  void recoverDeclaration() {
    while (!at(TokenKind::End) && !word("import") && !word("foreign"))
      ++index_;
  }

  std::vector<Token> tokens_;
  std::vector<CFDLDiagnostic> &diagnostics_;
  std::size_t index_ = 0;
};
