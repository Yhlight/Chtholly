#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

// Internal-only model of the iterator expansion. This is deliberately not a
// source-language AST node: it lets the checker validate the expansion
// contract before a foreach spelling is committed to the language.
enum class IteratorDesugaringNodeKind : std::uint8_t {
  LoopHeader, CompilerCall, ExhaustiveSwitch, ItemArm, ItemProjection, ItemBody,
  ItemCleanup, ContinuationUpdate, DoneArm, BreakCleanup, ReturnCleanup,
  LoopExit,
};

enum class IteratorDesugaringEdgeKind : std::uint8_t {
  Fallthrough, Item, Done, Continue, Break, Return, Backedge,
};

enum class IteratorBindingKind : std::uint8_t { Shared, Mutable, Owned };

struct IteratorDesugaringNode {
  std::uint32_t id = 0;
  IteratorDesugaringNodeKind kind = IteratorDesugaringNodeKind::LoopHeader;
  std::uint32_t scope_depth = 0;
  std::uint32_t cleanup_depth = 0;
  // Number of explicit defer/drop actions represented by this cleanup node.
  std::uint32_t cleanup_actions = 0;
  InstId source_inst = InstId::invalid();
  InstBlockId source_block = InstBlockId::invalid();
  std::string role;
  friend bool operator==(const IteratorDesugaringNode &,
                         const IteratorDesugaringNode &) = default;
};

struct IteratorSemIRFact {
  SemInstKind kind = SemInstKind::Invalid;
  std::uint32_t nesting_depth = 0;
  std::uint32_t loop_distance = 0;
  std::vector<SemInstKind> operand_kinds;
  std::vector<PlaceCleanupKind> cleanup_kinds;
  std::vector<SemInstKind> cleanup_operand_kinds;

  friend bool operator==(const IteratorSemIRFact &,
                         const IteratorSemIRFact &) = default;
};

struct IteratorSemIREdgeFact {
  IteratorDesugaringEdgeKind kind = IteratorDesugaringEdgeKind::Fallthrough;
  SemInstKind from_kind = SemInstKind::Invalid;
  SemInstKind to_kind = SemInstKind::Invalid;
  std::uint32_t loop_distance = 0;
  std::vector<PlaceCleanupKind> cleanup_kinds;
  friend bool operator==(const IteratorSemIREdgeFact &,
                         const IteratorSemIREdgeFact &) = default;
};

struct IteratorDesugaringEdge {
  std::uint32_t from = 0;
  std::uint32_t to = 0;
  IteratorDesugaringEdgeKind kind = IteratorDesugaringEdgeKind::Fallthrough;
  std::uint32_t cleanup_depth = 0;
  friend bool operator==(const IteratorDesugaringEdge &,
                         const IteratorDesugaringEdge &) = default;
};

struct IteratorProjectionLoan {
  std::uint32_t item_node = 0;
  std::uint32_t projection_node = 0;
  std::uint32_t owner_local = 0;
  std::uint32_t begin_scope = 0;
  std::uint32_t end_scope = 0;
  SemReferenceMutability mutability = SemReferenceMutability::ReadOnly;
  friend bool operator==(const IteratorProjectionLoan &,
                         const IteratorProjectionLoan &) = default;
};

struct IteratorDesugaringOptions {
  IteratorBindingKind binding = IteratorBindingKind::Shared;
  bool item_defer = false;
  bool has_continue = true;
  bool has_break = true;
  bool has_return = true;
};

struct IteratorDesugaringGraph {
  FunctionId function = FunctionId::invalid();
  std::vector<IteratorDesugaringNode> nodes;
  std::vector<IteratorDesugaringEdge> edges;
  std::vector<IteratorProjectionLoan> loans;
  std::vector<IteratorSemIRFact> semir_facts;
  std::vector<IteratorSemIREdgeFact> semir_edges;
  [[nodiscard]] IteratorDesugaringNode *findNode(std::string_view role);
  [[nodiscard]] const IteratorDesugaringNode *findNode(
      std::string_view role) const;
};

[[nodiscard]] IteratorDesugaringGraph buildIteratorDesugaring(
    const SemIR &sem_ir, IteratorDesugaringOptions options = {});
[[nodiscard]] IteratorDesugaringGraph buildIteratorDesugaring(
    const SemIR &sem_ir, FunctionId function,
    IteratorDesugaringOptions options = {});
[[nodiscard]] IteratorDesugaringGraph buildHandWrittenIteratorExpansion(
    const SemIR &sem_ir, IteratorDesugaringOptions options = {});
[[nodiscard]] IteratorDesugaringGraph buildHandWrittenIteratorExpansion(
    const SemIR &sem_ir, FunctionId function,
    IteratorDesugaringOptions options = {});
[[nodiscard]] bool equivalentIteratorDesugarings(
    const IteratorDesugaringGraph &generated,
    const IteratorDesugaringGraph &reference, std::string &error);
[[nodiscard]] bool validateIteratorProjectionLoans(
    const IteratorDesugaringGraph &graph, std::string &error);

} // namespace chtholly::compiler
