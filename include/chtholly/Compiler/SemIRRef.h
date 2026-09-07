#pragma once

#include <cassert>
#include <cstdlib>

namespace chtholly::compiler {

class SemIR;

// A non-serialized semantic ID which carries the SemIR owner across lowering
// context boundaries.
template <typename Id> struct SemIRRef {
  const SemIR *owner = nullptr;
  Id id = Id::invalid();

  [[nodiscard]] bool hasOwner() const { return owner != nullptr; }
  [[nodiscard]] bool hasId() const { return id.hasValue(); }
  [[nodiscard]] bool matches(const SemIR &candidate) const {
    return owner == &candidate;
  }
  [[nodiscard]] bool valid(const SemIR &candidate) const {
    return hasOwner() && hasId() && matches(candidate);
  }
  [[nodiscard]] Id checked(const SemIR &candidate) const {
    const bool is_valid = valid(candidate);
    assert(is_valid);
    // Returning an invalid ID here would turn an owner mismatch into an
    // unchecked store access in release builds. Keep the phase boundary
    // fail-closed, matching LowIR's checked descriptor accessors.
    if (!is_valid)
      std::abort();
    return id;
  }
};

template <typename Id>
[[nodiscard]] SemIRRef<Id> makeSemIRRef(const SemIR &owner, Id id) {
  return {&owner, id};
}

} // namespace chtholly::compiler
