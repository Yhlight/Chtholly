#pragma once

#include "chtholly/Core/Id.h"
#include "chtholly/Compiler/ParseTree.h"
#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace chtholly::compiler::semantics_internal {

struct SemanticNameScopeId : core::IndexBase<SemanticNameScopeId> {
  using IndexBase::IndexBase;
};

enum class SemanticBindingKind : std::uint8_t {
  Local,
  Constant,
  ClosureCapture,
};

struct SemanticBinding {
  SemanticBindingKind kind = SemanticBindingKind::Local;
  std::uint32_t target = core::AnyId::InvalidIndex;
  NodeId declaration;
};

class SemanticNameScopes {
public:
  void push();
  void pushIsolated();
  void pop();

  [[nodiscard]] const SemanticBinding *lookup(NameId name) const;
  [[nodiscard]] const SemanticBinding *
  lookupOutsideIsolation(NameId name) const;
  [[nodiscard]] bool insert(NameId name, SemanticBinding binding);

private:
  struct Scope {
    SemanticNameScopeId parent;
    SemanticNameScopeId resume;
    std::unordered_map<std::uint32_t, SemanticBinding> bindings;
  };

  std::vector<Scope> scopes_;
  SemanticNameScopeId current_;
};

} // namespace chtholly::compiler::semantics_internal
