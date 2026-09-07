#include "chtholly/Compiler/ForeignDeclaration.h"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <tuple>

namespace chtholly::compiler {

std::string_view resourceFlowRelationName(ResourceFlowRelationKind relation) {
  switch (relation) {
  case ResourceFlowRelationKind::ValueResult:
    return "value.result";
  case ResourceFlowRelationKind::LoanBorrow:
    return "loan.borrow";
  case ResourceFlowRelationKind::LoanMutate:
    return "loan.mutate";
  case ResourceFlowRelationKind::LoanEscapes:
    return "loan.escapes";
  case ResourceFlowRelationKind::LoanStores:
    return "loan.stores";
  case ResourceFlowRelationKind::Derives:
    return "derive";
  case ResourceFlowRelationKind::EventInvokes:
    return "event.invokes";
  case ResourceFlowRelationKind::ObligationCreate:
    return "obligation.create";
  case ResourceFlowRelationKind::ObligationTransfer:
    return "obligation.transfer";
  case ResourceFlowRelationKind::ObligationObliges:
    return "obligation.obliges";
  case ResourceFlowRelationKind::ObligationDischarges:
    return "obligation.discharges";
  case ResourceFlowRelationKind::Requires:
    return "requires";
  case ResourceFlowRelationKind::ResourceInitialize:
    return "resource.initialize";
  case ResourceFlowRelationKind::Count:
    return "<invalid>";
  }
  return "<invalid>";
}

namespace {

bool validPlace(const ResourceFlowDeclaration &input,
                const ResourceFlowPlace &place) {
  if (place.kind >= ResourceFlowPlaceKind::Count)
    return false;
  return place.kind == ResourceFlowPlaceKind::Result ||
         place.index < input.parameter_names.size();
}

bool validEndpoint(const ResourceFlowDeclaration &input,
                   const ResourceFlowEndpoint &endpoint) {
  if (endpoint.kind >= ResourceFlowEndpointKind::Count)
    return false;
  return endpoint.kind == ResourceFlowEndpointKind::CallReturn ||
         endpoint.index < input.event_names.size();
}

bool validAction(const ResourceFlowDeclaration &input,
                 const ResourceFlowAction &action) {
  return action.index < input.action_names.size();
}

bool hasOnly(const ResourceFlowFact &fact, bool target_place, bool endpoint,
             bool action, bool predicate) {
  return fact.target_place.has_value() == target_place &&
         fact.endpoint.has_value() == endpoint &&
         fact.action.has_value() == action &&
         fact.predicate.has_value() == predicate;
}

auto factKey(const ResourceFlowFact &fact) {
  return std::tie(fact.category, fact.relation, fact.subject,
                  fact.subject_action,
                  fact.target_place, fact.endpoint, fact.action,
                  fact.predicate);
}

void appendU32(std::string &out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendText(std::string &out, std::string_view value) {
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendPlace(std::string &out, const ResourceFlowPlace &place) {
  out.push_back(static_cast<char>(place.kind));
  appendU32(out, place.index);
}

void appendOptionalPlace(std::string &out,
                         const std::optional<ResourceFlowPlace> &place) {
  out.push_back(static_cast<char>(place.has_value()));
  if (place)
    appendPlace(out, *place);
}

void appendOptionalEndpoint(
    std::string &out, const std::optional<ResourceFlowEndpoint> &endpoint) {
  out.push_back(static_cast<char>(endpoint.has_value()));
  if (endpoint) {
    out.push_back(static_cast<char>(endpoint->kind));
    appendU32(out, endpoint->index);
  }
}

void appendOptionalAction(std::string &out,
                          const std::optional<ResourceFlowAction> &action) {
  out.push_back(static_cast<char>(action.has_value()));
  if (action)
    appendU32(out, action->index);
}

void appendOptionalPredicate(
    std::string &out,
    const std::optional<ResourceFlowPredicate> &predicate) {
  out.push_back(static_cast<char>(predicate.has_value()));
  if (predicate)
    out.push_back(static_cast<char>(*predicate));
}

} // namespace

bool normalizeResourceFlow(const ResourceFlowDeclaration &input,
                           NormalizedResourceFlow &normalized,
                           std::string &error) {
  error.clear();
  normalized = {};
  if (input.version != ResourceFlowDeclaration::CurrentVersion ||
      input.callable_name.empty() || !input.callable_type.hasValue() ||
      input.facts.empty()) {
    error = "resource-flow declaration has invalid callable identity or facts";
    return false;
  }

  auto facts = input.facts;
  std::ranges::sort(facts, [](const auto &lhs, const auto &rhs) {
    return factKey(lhs) < factKey(rhs);
  });

  for (std::size_t index = 0; index < facts.size(); ++index) {
    const auto &fact = facts[index];
    if (fact.category >= ResourceFlowCategory::Count ||
        fact.relation >= ResourceFlowRelationKind::Count ||
        (fact.relation != ResourceFlowRelationKind::Requires &&
         !validPlace(input, fact.subject)) ||
        (index != 0 && fact == facts[index - 1])) {
      error = "resource-flow declaration has an invalid or duplicate fact";
      return false;
    }
    if (fact.target_place && !validPlace(input, *fact.target_place)) {
      error = "resource-flow fact has an invalid target place";
      return false;
    }
    if (fact.endpoint && !validEndpoint(input, *fact.endpoint)) {
      error = "resource-flow fact has an invalid endpoint";
      return false;
    }
    if (fact.action && !validAction(input, *fact.action)) {
      error = "resource-flow fact has an invalid action";
      return false;
    }
    if (fact.subject_action && !validAction(input, *fact.subject_action)) {
      error = "resource-flow fact has an invalid subject action";
      return false;
    }
    if (fact.predicate && *fact.predicate >= ResourceFlowPredicate::Count) {
      error = "resource-flow fact has an invalid predicate";
      return false;
    }

    const auto relation = fact.relation;
    const bool shape_ok =
        (relation == ResourceFlowRelationKind::ValueResult &&
         hasOnly(fact, false, false, false, false)) ||
        ((relation == ResourceFlowRelationKind::LoanBorrow ||
          relation == ResourceFlowRelationKind::LoanMutate ||
          relation == ResourceFlowRelationKind::ObligationCreate ||
          relation == ResourceFlowRelationKind::ObligationTransfer ||
          relation == ResourceFlowRelationKind::ResourceInitialize) &&
         hasOnly(fact, false, false, false, false)) ||
        ((relation == ResourceFlowRelationKind::LoanEscapes ||
          relation == ResourceFlowRelationKind::EventInvokes) &&
         hasOnly(fact, false, true, false, false)) ||
        (relation == ResourceFlowRelationKind::LoanStores &&
         hasOnly(fact, true, false, false, false)) ||
        (relation == ResourceFlowRelationKind::Derives &&
         hasOnly(fact, true, false, false, false)) ||
        ((relation == ResourceFlowRelationKind::ObligationObliges ||
          relation == ResourceFlowRelationKind::ObligationDischarges) &&
         hasOnly(fact, false, false, true, false)) ||
        (relation == ResourceFlowRelationKind::Requires &&
         fact.subject_action.has_value() &&
         hasOnly(fact, false, false, false, true));
    if (!shape_ok) {
      error = "resource-flow fact has fields incompatible with its relation";
      return false;
    }
  }

  normalized.facts = facts;
  normalized.plan.facts = facts;
  normalized.plan.category = facts.front().category;
  if (std::ranges::any_of(facts, [&](const auto &fact) {
        return fact.category != normalized.plan.category;
      })) {
    normalized.plan.category =
        std::ranges::any_of(facts, [](const auto &fact) {
          return fact.category == ResourceFlowCategory::Resource;
        })
            ? ResourceFlowCategory::Resource
        : std::ranges::any_of(facts, [](const auto &fact) {
            return fact.category == ResourceFlowCategory::Projection;
          })
            ? ResourceFlowCategory::Projection
            : ResourceFlowCategory::Value;
  }

  std::string fingerprint_input = "CFDL-RESOURCE-FLOW-TYPED";
  appendText(fingerprint_input, input.callable_name);
  appendU32(fingerprint_input,
            static_cast<std::uint32_t>(input.callable_type.index));
  for (const auto &name : input.parameter_names)
    appendText(fingerprint_input, name);
  for (const auto &name : input.event_names)
    appendText(fingerprint_input, name);
  for (const auto &name : input.action_names)
    appendText(fingerprint_input, name);
  for (const auto &fact : facts) {
    fingerprint_input.push_back(static_cast<char>(fact.category));
    fingerprint_input.push_back(static_cast<char>(fact.relation));
    appendPlace(fingerprint_input, fact.subject);
    appendOptionalAction(fingerprint_input, fact.subject_action);
    appendOptionalPlace(fingerprint_input, fact.target_place);
    appendOptionalEndpoint(fingerprint_input, fact.endpoint);
    appendOptionalAction(fingerprint_input, fact.action);
    appendOptionalPredicate(fingerprint_input, fact.predicate);
  }
  normalized.fingerprint =
      StableFingerprint::fromCanonicalBytes(fingerprint_input);
  return true;
}

} // namespace chtholly::compiler
