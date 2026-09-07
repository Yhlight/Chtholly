#include "chtholly/Compiler/TokenBuffer.h"

#include <cassert>

namespace chtholly::compiler {

TokenId TokenBuffer::add(TokenInfo token, IdentifierId value) {
  const auto id = tokens_.add(token);
  const auto value_id = identifiers_.add(value);
  assert(value_id == id);
  return id;
}

std::string_view TokenBuffer::text(TokenId id) const {
  const auto identifier = value(id);
  if (identifier.hasValue())
    return values_->identifier(identifier);
  const auto &token = get(id);
  return source_->slice(token.offset, token.length);
}

void TokenBuffer::collectMetrics(core::CompilerMetrics &metrics,
                                 std::string_view label) const {
  tokens_.collectMetrics(metrics,
                         core::CompilerMetrics::childLabel(label, "tokens"));
  identifiers_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "identifiers"));
}

} // namespace chtholly::compiler
