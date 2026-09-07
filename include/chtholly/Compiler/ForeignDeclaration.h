#pragma once

#include "chtholly/Compiler/Generic.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chtholly::compiler {

// CFDL facts are source-independent resource relations. ABI and callable
// lowering consume this normalized record at the Interop boundary. A callable
// with no flow qualifiers or `where` facts is deliberately represented by the
// single ValueResult fact added by makeResourceFlowDeclaration; this is the
// explicit Raw CFFI mode and must not acquire resource obligations implicitly.
enum class ResourceFlowCategory : std::uint8_t {
  Value,
  Resource,
  Projection,
  Count,
};

enum class ResourceFlowRelationKind : std::uint8_t {
  ValueResult,
  LoanBorrow,
  LoanMutate,
  LoanEscapes,
  LoanStores,
  Derives,
  EventInvokes,
  ObligationCreate,
  ObligationTransfer,
  ObligationObliges,
  ObligationDischarges,
  Requires,
  ResourceInitialize,
  Count,
};

enum class ResourceFlowPlaceKind : std::uint8_t {
  Parameter,
  Result,
  Count,
};

struct ResourceFlowPlace {
  ResourceFlowPlaceKind kind = ResourceFlowPlaceKind::Count;
  std::uint32_t index = 0;

  friend bool operator==(const ResourceFlowPlace &, const ResourceFlowPlace &)
      = default;
  friend auto operator<=>(const ResourceFlowPlace &, const ResourceFlowPlace &)
      = default;
};

enum class ResourceFlowEndpointKind : std::uint8_t {
  CallReturn,
  Event,
  Count,
};

struct ResourceFlowEndpoint {
  ResourceFlowEndpointKind kind = ResourceFlowEndpointKind::Count;
  std::uint32_t index = 0;

  friend bool operator==(const ResourceFlowEndpoint &,
                         const ResourceFlowEndpoint &) = default;
  friend auto operator<=>(const ResourceFlowEndpoint &,
                          const ResourceFlowEndpoint &) = default;
};

struct ResourceFlowAction {
  std::uint32_t index = 0;

  friend bool operator==(const ResourceFlowAction &, const ResourceFlowAction &)
      = default;
  friend auto operator<=>(const ResourceFlowAction &, const ResourceFlowAction &)
      = default;
};

enum class ResourceFlowPredicate : std::uint8_t {
  Valid,
  Initialized,
  Quiescent,
  SameThread,
  Count,
};

struct ResourceFlowFact {
  ResourceFlowCategory category = ResourceFlowCategory::Count;
  ResourceFlowRelationKind relation = ResourceFlowRelationKind::Count;
  ResourceFlowPlace subject;
  std::optional<ResourceFlowAction> subject_action;
  std::optional<ResourceFlowPlace> target_place;
  std::optional<ResourceFlowEndpoint> endpoint;
  std::optional<ResourceFlowAction> action;
  std::optional<ResourceFlowPredicate> predicate;

  friend bool operator==(const ResourceFlowFact &, const ResourceFlowFact &)
      = default;
  friend auto operator<=>(const ResourceFlowFact &, const ResourceFlowFact &)
      = default;
};

struct ResourceFlowDeclaration {
  static constexpr std::uint32_t CurrentVersion = 2;

  std::uint32_t version = CurrentVersion;
  std::string callable_name;
  CanonicalTypeId callable_type;
  std::vector<std::string> parameter_names;
  std::vector<std::string> event_names;
  std::vector<std::string> action_names;
  std::vector<ResourceFlowFact> facts;
};

struct ResourceFlowPlan {
  ResourceFlowCategory category = ResourceFlowCategory::Count;
  std::vector<ResourceFlowFact> facts;

  friend bool operator==(const ResourceFlowPlan &, const ResourceFlowPlan &)
      = default;
};

struct NormalizedResourceFlow {
  std::vector<ResourceFlowFact> facts;
  ResourceFlowPlan plan;
  StableFingerprint fingerprint;
};

[[nodiscard]] bool normalizeResourceFlow(
    const ResourceFlowDeclaration &declaration,
    NormalizedResourceFlow &normalized, std::string &error);

[[nodiscard]] std::string_view resourceFlowRelationName(
    ResourceFlowRelationKind relation);

} // namespace chtholly::compiler
