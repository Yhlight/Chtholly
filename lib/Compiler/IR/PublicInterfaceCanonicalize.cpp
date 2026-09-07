#include "chtholly/Compiler/PublicInterface.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <tuple>
#include <vector>

namespace chtholly::compiler {

std::string StableFingerprint::hex() const {
  constexpr std::array digits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.resize(ByteCount * 2);
  for (std::size_t index = 0; index < bytes_.size(); ++index) {
    result[index * 2] = digits[bytes_[index] >> 4U];
    result[index * 2 + 1] = digits[bytes_[index] & 0xfU];
  }
  return result;
}

bool StableFingerprint::hasValue() const {
  return std::ranges::any_of(bytes_,
                             [](std::uint8_t byte) { return byte != 0; });
}

namespace {

bool regionStepCovers(const OwnershipRegionStep &allowed,
                      const OwnershipRegionStep &actual) {
  if (allowed.kind == OwnershipRegionStepKind::AnyElement)
    return actual.kind == OwnershipRegionStepKind::AnyElement ||
           actual.kind == OwnershipRegionStepKind::StaticElement;
  return allowed == actual;
}

auto regionKey(const OwnershipRegion &region) {
  return std::tuple(region.parameter_index, region.path, region.has_bit_range,
                    region.bit_begin, region.bit_end);
}

bool regionsOverlap(const OwnershipRegion &lhs, const OwnershipRegion &rhs) {
  if (lhs.parameter_index != rhs.parameter_index)
    return false;
  const auto common = std::min(lhs.path.size(), rhs.path.size());
  for (std::size_t index = 0; index < common; ++index) {
    const auto &left = lhs.path[index];
    const auto &right = rhs.path[index];
    if (left.kind == OwnershipRegionStepKind::AnyElement ||
        right.kind == OwnershipRegionStepKind::AnyElement)
      continue;
    if (left != right)
      return false;
  }
  if (lhs.path.size() != rhs.path.size() || !lhs.has_bit_range ||
      !rhs.has_bit_range)
    return true;
  return lhs.bit_begin < rhs.bit_end && rhs.bit_begin < lhs.bit_end;
}

} // namespace

bool ownershipRegionCovers(const OwnershipRegion &allowed,
                           const OwnershipRegion &actual) {
  if (allowed.parameter_index != actual.parameter_index ||
      allowed.path.size() > actual.path.size())
    return false;
  for (std::size_t index = 0; index < allowed.path.size(); ++index)
    if (!regionStepCovers(allowed.path[index], actual.path[index]))
      return false;
  if (allowed.path.size() < actual.path.size() || !allowed.has_bit_range)
    return true;
  return actual.has_bit_range && allowed.bit_begin <= actual.bit_begin &&
         allowed.bit_end >= actual.bit_end;
}

namespace {
constexpr std::size_t MaxConditionAtomsPerClause = 8;
constexpr std::size_t MaxConditionClauses = 32;

bool clauseSubsumes(const CallableConditionClause &lhs,
                    const CallableConditionClause &rhs) {
  return std::ranges::includes(rhs.atoms, lhs.atoms);
}
} // namespace

CallableConditionDescriptor CallableConditionDescriptor::always() {
  return {.clauses = {CallableConditionClause{}}};
}

CallableConditionDescriptor CallableConditionDescriptor::never() {
  return {};
}

CallableConditionDescriptor
CallableConditionDescriptor::atom(std::uint32_t parameter_index,
                                  bool expected) {
  return {.clauses = {{.atoms = {{parameter_index, expected}}}}};
}

bool CallableConditionDescriptor::isAlways() const {
  return exact && clauses.size() == 1 && clauses.front().atoms.empty();
}

CallableConditionDescriptor CallableConditionDescriptor::unknown() {
  return {.clauses = {CallableConditionClause{}}, .exact = false};
}
CallableConditionDescriptor CallableConditionDescriptor::enumVariant(std::uint32_t parameter, std::uint32_t variant) {
  return {.clauses = {{.atoms = {{parameter, true, variant}}}}};
}

void CallableConditionDescriptor::canonicalize() {
  if (!exact) { *this = unknown(); return; }
  for (auto &clause : clauses) {
    std::ranges::sort(clause.atoms);
    clause.atoms.erase(std::unique(clause.atoms.begin(), clause.atoms.end()),
                       clause.atoms.end());
  }
  std::erase_if(clauses, [](const auto &clause) {
    for (const auto &a : clause.atoms)
      for (const auto &b : clause.atoms)
        if (a.parameter_index == b.parameter_index && a.expected && b.expected &&
            a.variant != core::AnyId::InvalidIndex && b.variant != core::AnyId::InvalidIndex &&
            a.variant != b.variant) return true;
    for (std::size_t index = 1; index < clause.atoms.size(); ++index)
      if (clause.atoms[index - 1].parameter_index ==
              clause.atoms[index].parameter_index &&
          clause.atoms[index - 1].variant == clause.atoms[index].variant &&
          clause.atoms[index - 1].expected != clause.atoms[index].expected)
        return true;
    return false;
  });
  if (std::ranges::any_of(
          clauses, [](const auto &clause) { return clause.atoms.empty(); })) {
    *this = always();
    return;
  }
  if (std::ranges::any_of(clauses,
                          [](const auto &clause) {
                            return clause.atoms.size() >
                                   MaxConditionAtomsPerClause;
                          }) ||
      clauses.size() > MaxConditionClauses) {
    *this = unknown();
    return;
  }
  std::ranges::sort(clauses);
  clauses.erase(std::unique(clauses.begin(), clauses.end()), clauses.end());
  std::vector<CallableConditionClause> minimal;
  for (const auto &clause : clauses) {
    if (std::ranges::any_of(minimal, [&](const auto &candidate) {
          return clauseSubsumes(candidate, clause);
        }))
      continue;
    std::erase_if(minimal, [&](const auto &candidate) {
      return clauseSubsumes(clause, candidate);
    });
    minimal.push_back(clause);
    std::ranges::sort(minimal);
  }
  clauses = std::move(minimal);
}

bool CallableConditionDescriptor::verify(std::uint32_t parameter_count,
                                         std::string &error) const {
  if (!exact || clauses.size() > MaxConditionClauses ||
      std::ranges::any_of(clauses, [&](const auto &clause) {
        return clause.atoms.size() > MaxConditionAtomsPerClause ||
               std::ranges::any_of(clause.atoms, [&](const auto &atom) {
                 return atom.parameter_index >= parameter_count;
               });
      })) {
    error = "callable condition descriptor exceeds its parameter or complexity "
            "boundary";
    return false;
  }
  auto canonical = *this;
  canonical.canonicalize();
  if (canonical != *this) {
    error = "callable condition descriptor is not canonical";
    return false;
  }
  return true;
}

CallableConditionDescriptor
conditionOr(CallableConditionDescriptor lhs,
            const CallableConditionDescriptor &rhs) {
  if (lhs.isAlways() || rhs.isAlways()) return CallableConditionDescriptor::always();
  if (!lhs.exact || !rhs.exact) return CallableConditionDescriptor::unknown();
  lhs.clauses.insert(lhs.clauses.end(), rhs.clauses.begin(), rhs.clauses.end());
  lhs.canonicalize();
  return lhs;
}

CallableConditionDescriptor
conditionAnd(CallableConditionDescriptor lhs,
             const CallableConditionDescriptor &rhs) {
  if (lhs.isNever() || rhs.isNever())
    return CallableConditionDescriptor::never();
  if (!lhs.exact || !rhs.exact) return CallableConditionDescriptor::unknown();
  if (lhs.isAlways())
    return rhs;
  if (rhs.isAlways())
    return lhs;
  if (lhs.clauses.size() > MaxConditionClauses / rhs.clauses.size())
    return CallableConditionDescriptor::unknown();
  std::vector<CallableConditionClause> clauses;
  clauses.reserve(lhs.clauses.size() * rhs.clauses.size());
  for (const auto &left : lhs.clauses)
    for (const auto &right : rhs.clauses) {
      auto &clause = clauses.emplace_back(left);
      clause.atoms.insert(clause.atoms.end(), right.atoms.begin(),
                          right.atoms.end());
    }
  CallableConditionDescriptor result{std::move(clauses)};
  result.canonicalize();
  return result;
}

CallableConditionDescriptor
conditionNot(const CallableConditionDescriptor &condition) {
  if (!condition.exact) return CallableConditionDescriptor::unknown();
  if (condition.isNever())
    return CallableConditionDescriptor::always();
  auto result = CallableConditionDescriptor::always();
  for (const auto &clause : condition.clauses) {
    CallableConditionDescriptor negated_clause;
    for (const auto &atom : clause.atoms)
      negated_clause.clauses.push_back(
          {.atoms = {{atom.parameter_index, !atom.expected, atom.variant}}});
    negated_clause.canonicalize();
    result = conditionAnd(std::move(result), negated_clause);
    if (!result.exact) return result;
  }
  return result;
}

void CallableOwnershipSummary::canonicalize() {
  const auto effect_less = [](const CallableRegionEffect &lhs,
                              const CallableRegionEffect &rhs) {
    if (lhs.kind != rhs.kind)
      return lhs.kind < rhs.kind;
    return regionKey(lhs.region) < regionKey(rhs.region);
  };
  std::ranges::sort(effects, effect_less);
  effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
  std::vector<CallableRegionEffect> minimal_effects;
  for (const auto &effect : effects) {
    if (std::ranges::any_of(minimal_effects, [&](const auto &candidate) {
          return candidate.kind == effect.kind &&
                 ownershipRegionCovers(candidate.region, effect.region);
        }))
      continue;
    std::erase_if(minimal_effects, [&](const auto &candidate) {
      return candidate.kind == effect.kind &&
             ownershipRegionCovers(effect.region, candidate.region);
    });
    minimal_effects.push_back(effect);
    std::ranges::sort(minimal_effects, effect_less);
  }
  effects = std::move(minimal_effects);

  for (auto &postcondition : postconditions) {
    postcondition.condition.canonicalize();
    if (!postcondition.condition.exact) {
      postcondition.condition = CallableConditionDescriptor::always();
      postcondition.outcomes = CallableOutcomeAll;
    }
  }
  std::erase_if(postconditions, [](const auto &postcondition) {
    return postcondition.condition.isNever();
  });
  std::ranges::sort(postconditions, [](const auto &lhs, const auto &rhs) {
    if (regionKey(lhs.region) != regionKey(rhs.region))
      return regionKey(lhs.region) < regionKey(rhs.region);
    if (lhs.result_variant != rhs.result_variant) return lhs.result_variant < rhs.result_variant;
    if (lhs.condition != rhs.condition)
      return lhs.condition.clauses < rhs.condition.clauses;
    return lhs.outcomes < rhs.outcomes;
  });
  std::vector<CallableRegionPostcondition> merged_postconditions;
  for (const auto &postcondition : postconditions) {
    if (!merged_postconditions.empty() &&
        merged_postconditions.back().region == postcondition.region &&
        merged_postconditions.back().condition == postcondition.condition &&
        merged_postconditions.back().result_variant == postcondition.result_variant) {
      merged_postconditions.back().outcomes |= postcondition.outcomes;
      continue;
    }
    merged_postconditions.push_back(postcondition);
  }
  std::vector<CallableRegionPostcondition> minimal_postconditions;
  for (const auto &postcondition : merged_postconditions) {
    auto merged = postcondition;
    bool found_overlap = true;
    while (found_overlap) {
      found_overlap = false;
      for (std::size_t index = 0; index < minimal_postconditions.size();
           ++index) {
        const auto existing_covers = ownershipRegionCovers(
            minimal_postconditions[index].region, merged.region);
        const auto merged_covers = ownershipRegionCovers(
            merged.region, minimal_postconditions[index].region);
        if (!existing_covers && !merged_covers)
          continue;
        if (minimal_postconditions[index].result_variant != merged.result_variant ||
            minimal_postconditions[index].condition != merged.condition)
          continue;
        if (existing_covers)
          merged.region = minimal_postconditions[index].region;
        merged.outcomes |= minimal_postconditions[index].outcomes;
        minimal_postconditions.erase(minimal_postconditions.begin() + index);
        found_overlap = true;
        break;
      }
    }
    minimal_postconditions.push_back(std::move(merged));
  }
  std::ranges::sort(minimal_postconditions,
                    [](const auto &lhs, const auto &rhs) {
                      if (regionKey(lhs.region) != regionKey(rhs.region))
                        return regionKey(lhs.region) < regionKey(rhs.region);
                      return std::tie(lhs.result_variant, lhs.condition.clauses) < std::tie(rhs.result_variant, rhs.condition.clauses);
                    });
  postconditions = std::move(minimal_postconditions);

  for (auto &source : return_provenance) {
    source.condition.canonicalize();
    if (!source.condition.exact) source.condition = CallableConditionDescriptor::always();
  }
  std::erase_if(return_provenance,
                [](const auto &source) { return source.condition.isNever(); });
  const auto return_less = [](const auto &lhs, const auto &rhs) {
    if (regionKey(lhs.region) != regionKey(rhs.region))
      return regionKey(lhs.region) < regionKey(rhs.region);
    if (lhs.carrier_path != rhs.carrier_path)
      return lhs.carrier_path < rhs.carrier_path;
    return lhs.condition.clauses < rhs.condition.clauses;
  };
  std::ranges::sort(return_provenance, return_less);
  std::vector<CallableReturnSource> merged_returns;
  for (const auto &source : return_provenance) {
    if (!merged_returns.empty() &&
        merged_returns.back().region == source.region &&
        merged_returns.back().carrier_path == source.carrier_path) {
      merged_returns.back().condition = conditionOr(
          std::move(merged_returns.back().condition), source.condition);
      continue;
    }
    merged_returns.push_back(source);
  }
  std::vector<CallableReturnSource> minimal_returns;
  for (const auto &source : merged_returns) {
    if (std::ranges::any_of(minimal_returns, [&](const auto &candidate) {
          return candidate.carrier_path == source.carrier_path &&
                 ownershipRegionCovers(candidate.region, source.region) &&
                 (candidate.condition.isAlways() ||
                  candidate.condition == source.condition);
        }))
      continue;
    std::erase_if(minimal_returns, [&](const auto &candidate) {
      return source.carrier_path == candidate.carrier_path &&
             ownershipRegionCovers(source.region, candidate.region) &&
             (source.condition.isAlways() ||
              source.condition == candidate.condition);
    });
    minimal_returns.push_back(source);
    std::ranges::sort(minimal_returns, return_less);
  }
  return_provenance = std::move(minimal_returns);
}

bool CallableOwnershipSummary::verify(std::uint32_t parameter_count,
                                      std::string &error) const {
  const auto valid_region = [&](const OwnershipRegion &region) {
    if (region.parameter_index >= parameter_count || region.path.size() > 256 ||
        (region.has_bit_range ? region.bit_begin >= region.bit_end
                              : region.bit_begin != 0 || region.bit_end != 0))
      return false;
    return std::ranges::all_of(region.path, [](const auto &step) {
      return step.kind < OwnershipRegionStepKind::Count &&
             (step.kind != OwnershipRegionStepKind::AnyElement ||
              step.index == 0) &&
             (step.kind != OwnershipRegionStepKind::Dereference ||
              step.index == 0);
    });
  };
  const auto valid_carrier_path = [](const CallableReturnSource &source) {
    if (source.carrier_path.size() > 256)
      return false;
    bool variant_needs_field = false;
    for (const auto &step : source.carrier_path) {
      if (step.kind == CallableReturnSource::CarrierStepKind::EnumVariant) {
        if (variant_needs_field)
          return false;
        variant_needs_field = true;
      } else if (step.kind == CallableReturnSource::CarrierStepKind::Field) {
        variant_needs_field = false;
      } else {
        return false;
      }
      if (step.index == core::AnyId::InvalidIndex)
        return false;
    }
    return !variant_needs_field;
  };
  if ((!returns_owned && return_provenance.empty()) ||
      std::ranges::any_of(effects,
                          [&](const auto &effect) {
                            return effect.kind >= CallableEffectKind::Count ||
                                   !valid_region(effect.region);
                          }) ||
      std::ranges::any_of(postconditions,
                          [&](const auto &postcondition) {
                            std::string condition_error;
                            return !valid_region(postcondition.region) ||
                                   postcondition.outcomes == 0 ||
                                   (postcondition.outcomes &
                                    ~CallableOutcomeAll) != 0 ||
                                   !postcondition.condition.verify(
                                       parameter_count, condition_error);
                          }) ||
      std::ranges::any_of(return_provenance, [&](const auto &source) {
        std::string condition_error;
        return !valid_region(source.region) || !valid_carrier_path(source) ||
               !source.condition.verify(parameter_count, condition_error);
      })) {
    error = "callable ownership summary has invalid regions";
    return false;
  }
  if (return_provenance.size() > 64) {
    error = "callable ownership summary has too many return provenance arms";
    return false;
  }
  for (std::size_t left = 0; left < postconditions.size(); ++left)
    for (std::size_t right = left + 1; right < postconditions.size(); ++right)
      if ((postconditions[left].result_variant == postconditions[right].result_variant ||
           postconditions[left].result_variant == core::AnyId::InvalidIndex ||
           postconditions[right].result_variant == core::AnyId::InvalidIndex) &&
          postconditions[left].outcomes != postconditions[right].outcomes &&
          regionsOverlap(postconditions[left].region,
                         postconditions[right].region) &&
          !conditionAnd(postconditions[left].condition,
                        postconditions[right].condition)
               .isNever()) {
        error = "callable postconditions have conflicting overlapping regions";
        return false;
      }
  auto canonical = *this;
  canonical.canonicalize();
  if (canonical != *this) {
    error = "callable ownership summary is not canonical";
    return false;
  }
  return true;
}


} // namespace chtholly::compiler
