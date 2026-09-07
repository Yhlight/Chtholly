#pragma once

#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Basic/LanguageVersion.h"
#include "chtholly/Compiler/SharedValueStores.h"
#include "chtholly/Compiler/Source.h"
#include "chtholly/Compiler/Token.h"

#include <string_view>

namespace chtholly::compiler {

class TokenBuffer {
public:
  TokenBuffer(const SourceBuffer &source, SharedValueStores &values,
              LanguageVersion language_version)
      : source_(&source), values_(&values),
        language_version_(language_version) {}

  [[nodiscard]] TokenId add(TokenInfo token, IdentifierId value);
  [[nodiscard]] const TokenInfo &get(TokenId id) const {
    return tokens_.get(id);
  }
  [[nodiscard]] IdentifierId value(TokenId id) const {
    return identifiers_.get(id);
  }
  [[nodiscard]] std::string_view text(TokenId id) const;
  [[nodiscard]] std::size_t size() const {
    return tokens_.size();
  }
  [[nodiscard]] TokenId eof() const {
    return TokenId(static_cast<std::uint32_t>(size() - 1));
  }
  [[nodiscard]] const SourceBuffer &source() const {
    return *source_;
  }
  [[nodiscard]] LanguageVersion languageVersion() const {
    return language_version_;
  }

  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  const SourceBuffer *source_;
  SharedValueStores *values_;
  LanguageVersion language_version_;
  core::ValueStore<TokenId, TokenInfo> tokens_;
  core::ValueStore<TokenId, IdentifierId> identifiers_;
};

} // namespace chtholly::compiler
