#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler {

// A compiler-owned Send/Sync summary. `transferable` and `shareable` are the
// persisted artifact spellings; source diagnostics may present them as
// Send/Sync capabilities.
class TypeConcurrencySummary {
public:
  using SpecificResolver =
      std::function<const NominalTypeSpecificArtifact *(const PublicType &)>;
  using ForeignResolver =
      std::function<std::optional<TypeConcurrencyFacts>(const PublicType &)>;

  explicit TypeConcurrencySummary(SpecificResolver resolve_specific,
                                  ForeignResolver resolve_foreign = {},
                                  std::size_t max_work_items = 4096);

  // Computes a structural summary using canonical type bytes as the cache key.
  // Unknown or borrowed representations fail closed without inventing a
  // runtime witness. `error` is reserved for malformed input or a bounded
  // worklist exhaustion; ordinary non-capability is represented by false bits.
  [[nodiscard]] TypeConcurrencyFacts summarize(const PublicType &type,
                                                std::string &error);

private:
  [[nodiscard]] TypeConcurrencyFacts summarizeImpl(const PublicType &type,
                                                    std::string &error,
                                                    std::size_t depth);

  SpecificResolver resolve_specific_;
  ForeignResolver resolve_foreign_;
  std::size_t max_work_items_;
  std::size_t work_items_ = 0;
  std::unordered_map<std::string, TypeConcurrencyFacts> completed_;
  std::unordered_set<std::string> active_;
};

} // namespace chtholly::compiler
