#include "SemanticNameScopes.h"

#include <cassert>
#include <utility>

namespace chtholly::compiler::semantics_internal {

void SemanticNameScopes::push() {
  const auto id =
      SemanticNameScopeId(static_cast<std::uint32_t>(scopes_.size()));
  scopes_.push_back({.parent = current_, .resume = current_});
  current_ = id;
}

void SemanticNameScopes::pushIsolated() {
  const auto id =
      SemanticNameScopeId(static_cast<std::uint32_t>(scopes_.size()));
  scopes_.push_back({.resume = current_});
  current_ = id;
}

void SemanticNameScopes::pop() {
  assert(current_.hasValue());
  current_ = scopes_[current_.index].resume;
}

const SemanticBinding *SemanticNameScopes::lookup(NameId name) const {
  for (auto scope = current_; scope.hasValue();
       scope = scopes_[scope.index].parent) {
    const auto &bindings = scopes_[scope.index].bindings;
    if (const auto found = bindings.find(name.index); found != bindings.end())
      return &found->second;
  }
  return nullptr;
}

const SemanticBinding *
SemanticNameScopes::lookupOutsideIsolation(NameId name) const {
  for (auto scope = current_; scope.hasValue();
       scope = scopes_[scope.index].parent) {
    const auto &value = scopes_[scope.index];
    if (value.parent.hasValue() || !value.resume.hasValue())
      continue;
    for (auto resumed = value.resume; resumed.hasValue();
         resumed = scopes_[resumed.index].parent) {
      const auto &bindings = scopes_[resumed.index].bindings;
      if (const auto found = bindings.find(name.index); found != bindings.end())
        return &found->second;
    }
    return nullptr;
  }
  return nullptr;
}

bool SemanticNameScopes::insert(NameId name, SemanticBinding binding) {
  assert(current_.hasValue());
  return scopes_[current_.index]
      .bindings.emplace(name.index, std::move(binding))
      .second;
}

} // namespace chtholly::compiler::semantics_internal
