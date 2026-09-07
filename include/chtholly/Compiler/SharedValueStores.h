#pragma once

#include "chtholly/Core/Arena.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/CompilationIds.h"
#include "chtholly/Compiler/Generic.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace chtholly::compiler {

template <typename IdT> class CanonicalStringStore {
public:
  explicit CanonicalStringStore(core::Arena &arena) : arena_(&arena) {}

  [[nodiscard]] IdT intern(std::string_view value) {
    if (const auto found = lookup_.find(value); found != lookup_.end())
      return found->second;
    const auto stored = arena_->copyString(value);
    const auto id = values_.add(stored);
    lookup_.emplace(stored, id);
    return id;
  }

  [[nodiscard]] std::string_view get(IdT id) const {
    return values_.get(id);
  }

  [[nodiscard]] std::size_t size() const {
    return values_.size();
  }

  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const {
    values_.collectMetrics(metrics,
                           core::CompilerMetrics::childLabel(label, "values"));
    metrics.addMemory(
        core::CompilerMetrics::childLabel(label, "lookup"),
        lookup_.size() * sizeof(typename decltype(lookup_)::value_type),
        lookup_.bucket_count() * sizeof(void *) +
            lookup_.size() * sizeof(typename decltype(lookup_)::value_type));
  }

private:
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };

  core::Arena *arena_;
  core::ValueStore<IdT, std::string_view> values_;
  std::unordered_map<std::string_view, IdT, Hash, std::equal_to<>> lookup_;
};

class SharedValueStores {
public:
  SharedValueStores() : identifiers_(arena_), string_literals_(arena_) {}
  SharedValueStores(const SharedValueStores &) = delete;
  SharedValueStores &operator=(const SharedValueStores &) = delete;

  [[nodiscard]] IdentifierId internIdentifier(std::string_view value) {
    return identifiers_.intern(value);
  }
  [[nodiscard]] StringLiteralId internStringLiteral(std::string_view value) {
    return string_literals_.intern(value);
  }
  [[nodiscard]] IntegerId internInteger(std::int64_t value);

  [[nodiscard]] std::string_view identifier(IdentifierId id) const {
    return identifiers_.get(id);
  }
  [[nodiscard]] std::string_view stringLiteral(StringLiteralId id) const {
    return string_literals_.get(id);
  }
  [[nodiscard]] std::int64_t integer(IntegerId id) const {
    return integers_.get(id);
  }

  [[nodiscard]] std::size_t identifierCount() const {
    return identifiers_.size();
  }
  [[nodiscard]] std::size_t stringLiteralCount() const {
    return string_literals_.size();
  }
  [[nodiscard]] std::size_t integerCount() const {
    return integers_.size();
  }
  [[nodiscard]] GenericValueStores &generics() {
    return generics_;
  }
  [[nodiscard]] const GenericValueStores &generics() const {
    return generics_;
  }

  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  core::Arena arena_;
  CanonicalStringStore<IdentifierId> identifiers_;
  CanonicalStringStore<StringLiteralId> string_literals_;
  core::ValueStore<IntegerId, std::int64_t> integers_;
  std::unordered_map<std::int64_t, IntegerId> integer_lookup_;
  GenericValueStores generics_;
};

} // namespace chtholly::compiler
