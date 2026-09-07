#include "chtholly/Compiler/Lexer.h"

#include <limits>
#include <string_view>

namespace chtholly::compiler {
namespace {

constexpr bool isAsciiAlpha(char value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

constexpr bool isAsciiDigit(char value) {
  return value >= '0' && value <= '9';
}

constexpr bool isAsciiWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\v' || value == '\f';
}

constexpr bool isUtf8Continuation(unsigned char value) {
  return (value & 0xC0U) == 0x80U;
}

std::size_t validUtf8Length(std::string_view text, std::size_t offset) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80U)
    return 1;
  std::size_t length = 0;
  std::uint32_t minimum = 0;
  std::uint32_t value = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    minimum = 0x80;
    value = first & 0x1FU;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    minimum = 0x800;
    value = first & 0x0FU;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    minimum = 0x10000;
    value = first & 0x07U;
  } else {
    return 0;
  }
  if (offset + length > text.size())
    return 0;
  for (std::size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[offset + index]);
    if (!isUtf8Continuation(byte))
      return 0;
    value = (value << 6U) | (byte & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU))
    return 0;
  return length;
}

class Lexer {
public:
  Lexer(const SourceBuffer &source, SharedValueStores &values,
        DiagnosticEmitter &diagnostics, LanguageVersion language_version)
      : source_(source), values_(values), diagnostics_(diagnostics),
        language_version_(language_version),
        tokens_(source, values, language_version) {}

  TokenBuffer run() {
    if (source_.size() > std::numeric_limits<std::uint32_t>::max()) {
      diagnostics_.emit(DiagnosticKind::SourceTooLarge, 0);
      add(TokenKind::EndOfFile, 0, 0);
      return std::move(tokens_);
    }
    validateUtf8();
    while (!atEnd()) {
      skipTrivia();
      if (!atEnd())
        lexToken();
    }
    add(TokenKind::EndOfFile, offset_, 0);
    return std::move(tokens_);
  }

private:
  [[nodiscard]] bool atEnd() const {
    return offset_ >= source_.size();
  }
  [[nodiscard]] char peek(std::size_t lookahead = 0) const {
    return source_.at(offset_ + lookahead);
  }
  char advance() {
    const auto value = peek();
    if (!atEnd())
      ++offset_;
    return value;
  }
  bool match(char expected) {
    if (peek() != expected)
      return false;
    ++offset_;
    return true;
  }

  static bool identifierStart(char value) {
    return isAsciiAlpha(value) || value == '_';
  }
  static bool identifierContinue(char value) {
    return isAsciiAlpha(value) || isAsciiDigit(value) || value == '_';
  }

  void validateUtf8() {
    const auto text = source_.text();
    for (std::size_t index = 0; index < text.size();) {
      const auto length = validUtf8Length(text, index);
      if (length == 0) {
        diagnostics_.emit(DiagnosticKind::InvalidUtf8,
                          static_cast<std::uint32_t>(index));
        ++index;
      } else {
        index += length;
      }
    }
  }

  void add(TokenKind kind, std::size_t start, std::size_t length,
           bool intern = false, bool has_error = false) {
    if (length > std::numeric_limits<std::uint16_t>::max()) {
      diagnostics_.emit(DiagnosticKind::TokenTooLong,
                        static_cast<std::uint32_t>(start));
      length = std::numeric_limits<std::uint16_t>::max();
      has_error = true;
    }
    const auto identifier =
        intern ? values_.internIdentifier(source_.slice(start, length))
               : IdentifierId::invalid();
    (void)tokens_.add({static_cast<std::uint32_t>(start),
                       static_cast<std::uint16_t>(length), kind,
                       has_error ? TokenFlags::HasError : TokenFlags::None},
                      identifier);
  }

  void skipTrivia() {
    while (true) {
      while (isAsciiWhitespace(peek()))
        advance();
      if (peek() == '/' && peek(1) == '/') {
        while (!atEnd() && peek() != '\n')
          advance();
        continue;
      }
      if (peek() == '/' && peek(1) == '*') {
        const auto start = offset_;
        advance();
        advance();
        while (!atEnd() && !(peek() == '*' && peek(1) == '/'))
          advance();
        if (atEnd()) {
          diagnostics_.emit(DiagnosticKind::UnterminatedComment,
                            static_cast<std::uint32_t>(start));
          return;
        }
        advance();
        advance();
        continue;
      }
      return;
    }
  }

  void lexDelimited(std::size_t start, char delimiter, TokenKind kind) {
    bool valid = true;
    std::size_t scalar_count = 0;
    while (!atEnd() && peek() != delimiter) {
      if (peek() == '\n' || peek() == '\r') {
        diagnostics_.emit(DiagnosticKind::UnterminatedLiteral,
                          static_cast<std::uint32_t>(start));
        add(TokenKind::Invalid, start, offset_ - start, false, true);
        return;
      }
      if (peek() == '\\') {
        advance();
        if (atEnd())
          break;
        const auto escaped = advance();
        if (escaped != '\\' && escaped != '"' && escaped != '\'' &&
            escaped != '0' && escaped != 'n' && escaped != 'r' &&
            escaped != 't') {
          diagnostics_.emit(DiagnosticKind::InvalidEscape,
                            static_cast<std::uint32_t>(offset_ - 2), 2);
          valid = false;
        }
        ++scalar_count;
      } else {
        const auto length = validUtf8Length(source_.text(), offset_);
        offset_ += length == 0 ? 1 : length;
        ++scalar_count;
      }
    }
    if (atEnd()) {
      diagnostics_.emit(DiagnosticKind::UnterminatedLiteral,
                        static_cast<std::uint32_t>(start));
      add(TokenKind::Invalid, start, offset_ - start, false, true);
      return;
    }
    advance();
    if (kind == TokenKind::CharLiteral && scalar_count != 1) {
      diagnostics_.emit(DiagnosticKind::InvalidCharLiteral,
                        static_cast<std::uint32_t>(start));
      valid = false;
    }
    add(kind, start, offset_ - start, false, !valid);
  }

  static bool digitForBase(char value, unsigned base) {
    if (value >= '0' && value <= '9')
      return static_cast<unsigned>(value - '0') < base;
    if (value >= 'a' && value <= 'f')
      return base == 16;
    if (value >= 'A' && value <= 'F')
      return base == 16;
    return false;
  }

  bool lexDigitSequence(unsigned base) {
    bool saw_digit = false;
    bool previous_separator = false;
    while (digitForBase(peek(), base) || peek() == '_') {
      const auto value = advance();
      if (value == '_') {
        if (!saw_digit || previous_separator)
          return false;
        previous_separator = true;
      } else {
        saw_digit = true;
        previous_separator = false;
      }
    }
    return saw_digit && !previous_separator;
  }

  void lexNumber(std::size_t start, char first) {
    auto kind = TokenKind::IntegerLiteral;
    bool valid = true;
    unsigned base = 10;
    // A digit after postfix `.` is a tuple member index. Do not let a
    // following postfix `.` turn `value.0.len` into an invalid float token.
    const auto postfix_member_index =
        tokens_.size() != 0 &&
        tokens_.get(TokenId(static_cast<std::uint32_t>(tokens_.size() - 1)))
                .kind == TokenKind::Dot;
    if (first == '0' && (peek() == 'b' || peek() == 'B' || peek() == 'o' ||
                         peek() == 'O' || peek() == 'x' || peek() == 'X')) {
      const auto prefix = advance();
      base = prefix == 'b' || prefix == 'B' ? 2
             : prefix == 'o' || prefix == 'O' ? 8
                                               : 16;
      valid = lexDigitSequence(base);
    } else {
      bool previous_separator = false;
      while (isAsciiDigit(peek()) || peek() == '_') {
        const auto value = advance();
        if (value == '_') {
          if (previous_separator)
            valid = false;
          previous_separator = true;
        } else {
          previous_separator = false;
        }
      }
      valid &= !previous_separator;
      if (!postfix_member_index && peek() == '.' && peek(1) != '.') {
        kind = TokenKind::FloatLiteral;
        advance();
        valid &= lexDigitSequence(10);
      }
      if (!postfix_member_index && (peek() == 'e' || peek() == 'E')) {
        kind = TokenKind::FloatLiteral;
        advance();
        if (peek() == '+' || peek() == '-')
          advance();
        valid &= lexDigitSequence(10);
      }
    }

    const auto suffix_start = offset_;
    while (identifierContinue(peek()))
      advance();
    const auto suffix = source_.slice(suffix_start, offset_ - suffix_start);
    const auto integer_suffix = suffix == "i8" || suffix == "i16" ||
                                suffix == "i32" || suffix == "i64" ||
                                suffix == "isize" || suffix == "u8" ||
                                suffix == "u16" || suffix == "u32" ||
                                suffix == "u64" || suffix == "usize";
    const auto float_suffix = suffix == "f32" || suffix == "f64";
    if (!suffix.empty()) {
      if (float_suffix)
        kind = TokenKind::FloatLiteral;
      valid &= (kind == TokenKind::IntegerLiteral && integer_suffix) ||
               (kind == TokenKind::FloatLiteral && float_suffix);
    }
    if (base != 10 && kind == TokenKind::FloatLiteral)
      valid = false;
    if (!valid)
      diagnostics_.emit(DiagnosticKind::InvalidNumericLiteral,
                        static_cast<std::uint32_t>(start));
    add(kind, start, offset_ - start, false, !valid);
  }

  void lexToken() {
    const auto start = offset_;
    const auto value = advance();
    if (value == 'c' && peek() == '"') {
      advance();
      lexDelimited(start, '"', TokenKind::CStringLiteral);
      return;
    }
    if (identifierStart(value)) {
      while (identifierContinue(peek()))
        advance();
      const auto text = source_.slice(start, offset_ - start);
      const auto kind = keywordKind(text);
      add(kind, start, offset_ - start,
          kind == TokenKind::Identifier || kind == TokenKind::KwCopy);
      return;
    }
    if (isAsciiDigit(value)) {
      lexNumber(start, value);
      return;
    }
    if (value == '"') {
      lexDelimited(start, '"', TokenKind::StringLiteral);
      return;
    }
    if (value == '\'') {
      lexDelimited(start, '\'', TokenKind::CharLiteral);
      return;
    }
    if (static_cast<unsigned char>(value) >= 0x80U) {
      const auto length = validUtf8Length(source_.text(), start);
      offset_ = start + (length == 0 ? 1 : length);
      diagnostics_.emit(DiagnosticKind::NonAsciiIdentifier,
                        static_cast<std::uint32_t>(start),
                        static_cast<std::uint32_t>(offset_ - start));
      add(TokenKind::Invalid, start, offset_ - start, false, true);
      return;
    }

    TokenKind kind = TokenKind::Invalid;
    switch (value) {
    case '(':
      kind = TokenKind::LParen;
      break;
    case ')':
      kind = TokenKind::RParen;
      break;
    case '{':
      kind = TokenKind::LBrace;
      break;
    case '}':
      kind = TokenKind::RBrace;
      break;
    case '[':
      kind = TokenKind::LBracket;
      break;
    case ']':
      kind = TokenKind::RBracket;
      break;
    case ',':
      kind = TokenKind::Comma;
      break;
    case ':':
      kind = match(':') ? TokenKind::Scope : TokenKind::Colon;
      break;
    case '.':
      kind = match('.')
                 ? (match('.') ? TokenKind::DotDotDot : TokenKind::DotDot)
                 : TokenKind::Dot;
      break;
    case ';':
      kind = TokenKind::Semicolon;
      break;
    case '?':
      kind = TokenKind::Question;
      break;
    case '+':
      kind = match('=') ? TokenKind::PlusEqual : TokenKind::Plus;
      break;
    case '-':
      kind = match('=') ? TokenKind::MinusEqual : TokenKind::Minus;
      break;
    case '*':
      kind = match('=') ? TokenKind::StarEqual : TokenKind::Star;
      break;
    case '/':
      kind = match('=') ? TokenKind::SlashEqual : TokenKind::Slash;
      break;
    case '%':
      kind = match('=') ? TokenKind::PercentEqual : TokenKind::Percent;
      break;
    case '!':
      kind = match('=') ? TokenKind::BangEqual : TokenKind::Bang;
      break;
    case '=':
      kind = match('>')
                 ? TokenKind::FatArrow
                 : (match('=') ? TokenKind::EqualEqual : TokenKind::Equal);
      break;
    case '<':
      if (match('<'))
        kind = match('=') ? TokenKind::LessLessEqual : TokenKind::LessLess;
      else if (peek() == '=' && peek(1) == '>') {
        advance();
        advance();
        kind = TokenKind::Spaceship;
      } else
        kind = match('=') ? TokenKind::LessEqual : TokenKind::Less;
      break;
    case '>':
      if (match('>'))
        kind = match('=') ? TokenKind::GreaterGreaterEqual
                          : TokenKind::GreaterGreater;
      else
        kind = match('=') ? TokenKind::GreaterEqual : TokenKind::Greater;
      break;
    case '&':
      kind = match('&') ? TokenKind::AmpAmp
                        : (match('=') ? TokenKind::AmpEqual : TokenKind::Amp);
      break;
    case '|':
      kind = match('|') ? TokenKind::PipePipe
                        : (match('=') ? TokenKind::PipeEqual : TokenKind::Pipe);
      break;
    case '^':
      kind = match('=') ? TokenKind::CaretEqual : TokenKind::Caret;
      break;
    case '~':
      kind = TokenKind::Tilde;
      break;
    default:
      break;
    }
    if (kind == TokenKind::Invalid) {
      diagnostics_.emit(DiagnosticKind::UnexpectedCharacter,
                        static_cast<std::uint32_t>(start));
      add(kind, start, offset_ - start, false, true);
    } else {
      add(kind, start, offset_ - start);
    }
  }

  const SourceBuffer &source_;
  SharedValueStores &values_;
  DiagnosticEmitter &diagnostics_;
  LanguageVersion language_version_;
  TokenBuffer tokens_;
  std::size_t offset_ = 0;
};

} // namespace

TokenBuffer lex(const SourceBuffer &source, SharedValueStores &values,
                DiagnosticEmitter &diagnostics,
                LanguageVersion language_version) {
  return Lexer(source, values, diagnostics, language_version).run();
}

} // namespace chtholly::compiler
