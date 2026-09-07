#include "chtholly/Compiler/SharedValueStores.h"

namespace chtholly::compiler {

IntegerId SharedValueStores::internInteger(std::int64_t value) {
  if (const auto found = integer_lookup_.find(value);
      found != integer_lookup_.end())
    return found->second;
  const auto id = integers_.add(value);
  integer_lookup_.emplace(value, id);
  return id;
}

void SharedValueStores::collectMetrics(core::CompilerMetrics &metrics,
                                       std::string_view label) const {
  arena_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "arena"));
  identifiers_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "identifiers"));
  string_literals_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "string_literals"));
  integers_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "integers"));
  generics_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "generic_values"));
  metrics.addMemory(
      core::CompilerMetrics::childLabel(label, "integer_lookup"),
      integer_lookup_.size() *
          sizeof(typename decltype(integer_lookup_)::value_type),
      integer_lookup_.bucket_count() * sizeof(void *) +
          integer_lookup_.size() *
              sizeof(typename decltype(integer_lookup_)::value_type));
}

} // namespace chtholly::compiler
