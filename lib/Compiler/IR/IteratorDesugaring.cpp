#include "chtholly/Compiler/IteratorDesugaring.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <tuple>

namespace chtholly::compiler {
namespace {

struct Builder {
  IteratorDesugaringGraph graph;
  std::uint32_t next_id = 17;

  std::uint32_t node(IteratorDesugaringNodeKind kind, std::string role,
                     std::uint32_t scope, std::uint32_t cleanup) {
    const auto id = next_id++;
    graph.nodes.push_back({id, kind, scope, cleanup, 0, InstId::invalid(),
                           InstBlockId::invalid(), std::move(role)});
    return id;
  }
  void edge(std::uint32_t from, std::uint32_t to,
            IteratorDesugaringEdgeKind kind, std::uint32_t cleanup) {
    graph.edges.push_back({from, to, kind, cleanup});
  }
};

void appendCleanupFacts(const SemIR &sem_ir, InstId instruction,
                        std::vector<PlaceCleanupKind> &target) {
  if (!sem_ir.hasPlaceStates())
    return;
  const auto &plan = sem_ir.placeStates().edgeCleanup(instruction);
  for (const auto &action : plan.actions)
    target.push_back(action.kind);
  const auto &return_plan = sem_ir.placeStates().returnCleanup(instruction);
  if (sem_ir.inst(instruction).kind == SemInstKind::Return)
    for (const auto &action : return_plan.actions)
      target.push_back(action.kind);
}

void appendOperandFacts(const SemIR &sem_ir, const SemInst &inst,
                        std::vector<SemInstKind> &target) {
  for (std::size_t index = 0; index < 2; ++index) {
    if (semInstArgKind(inst.kind, index) != SemArgKind::Inst)
      continue;
    const auto operand = InstId(index == 0 ? inst.arg0 : inst.arg1);
    if (operand.hasValue() && operand.index < sem_ir.instCount())
      target.push_back(sem_ir.inst(operand).kind);
  }
}

void collectSemIRFacts(const SemIR &sem_ir, FunctionId function,
                       IteratorDesugaringGraph &graph) {
  const auto &record = sem_ir.function(function);
  const auto collect = [&](auto &&self, InstBlockId block,
                           std::uint32_t nesting) -> void {
    for (const auto instruction : sem_ir.instBlock(block)) {
      const auto &inst = sem_ir.inst(instruction);
      const bool relevant =
          inst.kind == SemInstKind::Call ||
          inst.kind == SemInstKind::CompilerIntrinsicCall ||
          inst.kind == SemInstKind::Switch ||
          inst.kind == SemInstKind::SwitchArm ||
          inst.kind == SemInstKind::EnumPayloadAccess ||
          inst.kind == SemInstKind::CarrierView ||
          inst.kind == SemInstKind::StructFieldAccess ||
          inst.kind == SemInstKind::UnionFieldAccess ||
          inst.kind == SemInstKind::Move || inst.kind == SemInstKind::Copy ||
          inst.kind == SemInstKind::Break ||
          inst.kind == SemInstKind::Continue ||
          inst.kind == SemInstKind::Return || inst.kind == SemInstKind::While ||
          inst.kind == SemInstKind::For || inst.kind == SemInstKind::DoWhile ||
          inst.kind == SemInstKind::Defer;
      if (relevant) {
        IteratorSemIRFact fact;
        fact.kind = inst.kind;
        fact.nesting_depth = nesting;
        if (inst.kind == SemInstKind::Break ||
            inst.kind == SemInstKind::Continue)
          fact.loop_distance = static_cast<std::uint32_t>(
              sem_ir.integer(IntegerId(inst.arg0)));
        appendOperandFacts(sem_ir, inst, fact.operand_kinds);
        appendCleanupFacts(sem_ir, instruction, fact.cleanup_kinds);
        graph.semir_facts.push_back(std::move(fact));
      }

      if (inst.kind == SemInstKind::While) {
        self(self, InstBlockId(inst.arg0), nesting);
        self(self, InstBlockId(inst.arg1), nesting + 1);
      } else if (inst.kind == SemInstKind::For) {
        for (const auto clause : sem_ir.instBlock(InstBlockId(inst.arg0)))
          self(self, sem_ir.getAs<SemForClause>(clause).arg1, nesting);
        self(self, InstBlockId(inst.arg1), nesting + 1);
      } else if (inst.kind == SemInstKind::DoWhile) {
        self(self, InstBlockId(inst.arg1), nesting + 1);
        self(self, InstBlockId(inst.arg0), nesting);
      } else if (inst.kind == SemInstKind::Switch) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          const auto &arm = sem_ir.getAs<SemSwitchArm>(arm_id);
          self(self, InstBlockId(arm.arg1), nesting + 1);
        }
      } else if (inst.kind == SemInstKind::If) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          const auto &arm = sem_ir.getAs<SemIfArm>(arm_id);
          self(self, arm.arg0, nesting + 1);
        }
      } else if (inst.kind == SemInstKind::Defer) {
        self(self, InstBlockId(inst.arg0), nesting + 1);
      }
    }
  };
  collect(collect, record.body, 0);
  std::ranges::sort(graph.semir_facts, [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.kind, lhs.nesting_depth, lhs.loop_distance,
                    lhs.operand_kinds, lhs.cleanup_kinds) <
           std::tie(rhs.kind, rhs.nesting_depth, rhs.loop_distance,
                    rhs.operand_kinds, rhs.cleanup_kinds);
  });
}

void attachSemIRNodes(const SemIR &sem_ir, FunctionId function,
                      IteratorDesugaringGraph &graph) {
  const auto &record = sem_ir.function(function);
  std::vector<std::pair<InstId, InstBlockId>> instructions;
  const auto collect = [&](auto &&self, InstBlockId block) -> void {
    for (const auto instruction : sem_ir.instBlock(block)) {
      instructions.emplace_back(instruction, block);
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind == SemInstKind::If) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1)))
          self(self, sem_ir.getAs<SemIfArm>(arm_id).arg0);
      } else if (inst.kind == SemInstKind::Switch) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1)))
          self(self, sem_ir.getAs<SemSwitchArm>(arm_id).arg1);
      } else if (inst.kind == SemInstKind::While) {
        self(self, InstBlockId(inst.arg0));
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::For) {
        for (const auto clause : sem_ir.instBlock(InstBlockId(inst.arg0)))
          self(self, sem_ir.getAs<SemForClause>(clause).arg1);
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::DoWhile) {
        self(self, InstBlockId(inst.arg0));
        self(self, InstBlockId(inst.arg1));
      }
    }
  };
  collect(collect, record.body);

  const auto first = [&](auto predicate) -> std::pair<InstId, InstBlockId> {
    const auto found = std::ranges::find_if(instructions, [&](const auto &item) {
      return predicate(sem_ir.inst(item.first));
    });
    return found == instructions.end()
               ? std::pair{InstId::invalid(), InstBlockId::invalid()}
               : *found;
  };
  const auto loop = first([](const auto &inst) {
    return inst.kind == SemInstKind::While || inst.kind == SemInstKind::For ||
           inst.kind == SemInstKind::DoWhile;
  });
  const auto dispatch = first(
      [](const auto &inst) { return inst.kind == SemInstKind::Switch; });
  const auto next = first([&](const auto &inst) {
    if (inst.kind != SemInstKind::Call &&
        inst.kind != SemInstKind::CompilerIntrinsicCall)
      return false;
    const auto role = sem_ir.functionIntrinsicRole(FunctionRefId(inst.arg0));
    if (role == CompilerIntrinsicRole::VecIterNext ||
        role == CompilerIntrinsicRole::VecIterMutNext)
      return true;
    const auto result_type = TypeId(inst.type);
    if (sem_ir.type(result_type).kind != SemTypeKind::Nominal)
      return false;
    const auto &nominal = sem_ir.nominalType(
        NominalTypeId(sem_ir.type(result_type).arg0));
    return nominal.name.hasValue() &&
           sem_ir.identifier(sem_ir.name(nominal.name).text) ==
               "IterationStep";
  });
  auto set_source = [&](std::string_view role,
                        std::pair<InstId, InstBlockId> source) {
    if (auto *node = graph.findNode(role)) {
      node->source_inst = source.first;
      node->source_block = source.second;
    }
  };
  set_source("loop.header", loop);
  set_source("next.call", next);
  set_source("next.dispatch", dispatch);

  if (dispatch.first.hasValue()) {
    const auto &switch_inst = sem_ir.inst(dispatch.first);
    const auto arms = sem_ir.instBlock(InstBlockId(switch_inst.arg1));
    for (const auto arm_id : arms) {
      const auto &arm = sem_ir.getAs<SemSwitchArm>(arm_id);
      const auto body = InstBlockId(arm.arg1);
      const auto has_projection = std::ranges::any_of(
          sem_ir.instBlock(body), [&](const auto item) {
            const auto kind = sem_ir.inst(item).kind;
            return kind == SemInstKind::EnumPayloadAccess ||
                   kind == SemInstKind::CarrierView ||
                   kind == SemInstKind::StructFieldAccess ||
                   kind == SemInstKind::UnionFieldAccess;
          });
      if (has_projection) {
        set_source("item.arm", {arm_id, InstBlockId(switch_inst.arg1)});
        set_source("item.body", {InstId::invalid(), body});
        const auto projection = std::ranges::find_if(
            sem_ir.instBlock(body), [&](const auto item) {
              const auto kind = sem_ir.inst(item).kind;
              return kind == SemInstKind::EnumPayloadAccess ||
                     kind == SemInstKind::CarrierView ||
                     kind == SemInstKind::StructFieldAccess ||
                     kind == SemInstKind::UnionFieldAccess;
            });
        if (projection != sem_ir.instBlock(body).end())
          set_source("item.projection", {*projection, body});
        if (sem_ir.hasPlaceStates())
          if (auto *cleanup = graph.findNode("item.cleanup"))
            cleanup->cleanup_actions = static_cast<std::uint32_t>(
                sem_ir.placeStates().blockCleanup(body).actions.size());
      } else {
        set_source("done.arm", {arm_id, InstBlockId(switch_inst.arg1)});
      }
    }
  }
}

IteratorDesugaringGraph build(const IteratorDesugaringOptions options) {
  Builder b;
  const auto header = b.node(IteratorDesugaringNodeKind::LoopHeader,
                             "loop.header", 1, 0);
  const auto next = b.node(IteratorDesugaringNodeKind::CompilerCall, "next.call",
                           1, 0);
  const auto dispatch = b.node(IteratorDesugaringNodeKind::ExhaustiveSwitch,
                                "next.dispatch", 1, 0);
  const auto item = b.node(IteratorDesugaringNodeKind::ItemArm, "item.arm", 2,
                           0);
  const auto projection = b.node(IteratorDesugaringNodeKind::ItemProjection,
                                 "item.projection", 2, 0);
  const auto body = b.node(IteratorDesugaringNodeKind::ItemBody, "item.body",
                           2, 0);
  const auto item_cleanup = b.node(IteratorDesugaringNodeKind::ItemCleanup,
                                   "item.cleanup", 2, 1);
  if (options.item_defer)
    std::ranges::find(b.graph.nodes, item_cleanup,
                      &IteratorDesugaringNode::id)->cleanup_actions = 1;
  const auto continuation = b.node(IteratorDesugaringNodeKind::ContinuationUpdate,
                                   "continuation.update", 1, 1);
  const auto done = b.node(IteratorDesugaringNodeKind::DoneArm, "done.arm", 1,
                           0);
  const auto exit = b.node(IteratorDesugaringNodeKind::LoopExit, "loop.exit",
                           0, 1);

  b.edge(header, next, IteratorDesugaringEdgeKind::Fallthrough, 0);
  b.edge(next, dispatch, IteratorDesugaringEdgeKind::Fallthrough, 0);
  b.edge(dispatch, item, IteratorDesugaringEdgeKind::Item, 0);
  b.edge(dispatch, done, IteratorDesugaringEdgeKind::Done, 0);
  b.edge(item, projection, IteratorDesugaringEdgeKind::Fallthrough, 0);
  b.edge(projection, body, IteratorDesugaringEdgeKind::Fallthrough, 0);
  b.edge(body, item_cleanup, IteratorDesugaringEdgeKind::Fallthrough, 1);
  b.edge(item_cleanup, continuation, IteratorDesugaringEdgeKind::Fallthrough,
         1);
  b.edge(continuation, header, IteratorDesugaringEdgeKind::Backedge, 0);
  b.edge(done, exit, IteratorDesugaringEdgeKind::Done, 1);

  if (options.has_continue) {
    const auto cleanup = b.node(IteratorDesugaringNodeKind::ItemCleanup,
                                "continue.cleanup", 2, 1);
    b.edge(body, cleanup, IteratorDesugaringEdgeKind::Continue, 1);
    b.edge(cleanup, continuation, IteratorDesugaringEdgeKind::Continue, 1);
  }
  if (options.has_break) {
    const auto cleanup = b.node(IteratorDesugaringNodeKind::BreakCleanup,
                                "break.cleanup", 0, 2);
    b.edge(body, cleanup, IteratorDesugaringEdgeKind::Break, 2);
    b.edge(cleanup, exit, IteratorDesugaringEdgeKind::Break, 2);
  }
  if (options.has_return) {
    const auto cleanup = b.node(IteratorDesugaringNodeKind::ReturnCleanup,
                                "return.cleanup", 0, 3);
    b.edge(body, cleanup, IteratorDesugaringEdgeKind::Return, 3);
    b.edge(cleanup, exit, IteratorDesugaringEdgeKind::Return, 3);
  }
  if (options.binding != IteratorBindingKind::Owned)
    b.graph.loans.push_back(
        {item, projection, 1, 2, 2,
         options.binding == IteratorBindingKind::Mutable
             ? SemReferenceMutability::Mutable
             : SemReferenceMutability::ReadOnly});
  std::ranges::sort(b.graph.nodes, {}, &IteratorDesugaringNode::role);
  return std::move(b.graph);
}

const IteratorDesugaringNode *find(const IteratorDesugaringGraph &graph,
                                   std::uint32_t id) {
  const auto found = std::ranges::find(graph.nodes, id,
                                       &IteratorDesugaringNode::id);
  return found == graph.nodes.end() ? nullptr : &*found;
}

} // namespace

const IteratorDesugaringNode *IteratorDesugaringGraph::findNode(
    std::string_view role) const {
  const auto found = std::ranges::find(nodes, role,
                                       &IteratorDesugaringNode::role);
  return found == nodes.end() ? nullptr : &*found;
}

IteratorDesugaringNode *IteratorDesugaringGraph::findNode(
    std::string_view role) {
  const auto found = std::ranges::find(nodes, role,
                                       &IteratorDesugaringNode::role);
  return found == nodes.end() ? nullptr : &*found;
}

IteratorDesugaringGraph buildIteratorDesugaring(
    const SemIR &sem_ir, IteratorDesugaringOptions options) {
  return buildIteratorDesugaring(sem_ir, FunctionId(0), options);
}

IteratorDesugaringGraph buildIteratorDesugaring(
    const SemIR &sem_ir, FunctionId function,
    IteratorDesugaringOptions options) {
  auto graph = build(options);
  graph.function = function;
  if (function.hasValue() && function.index < sem_ir.functionCount()) {
    collectSemIRFacts(sem_ir, function, graph);
    attachSemIRNodes(sem_ir, function, graph);
    const auto callable = buildCallableControlFlowGraph(sem_ir, function);
    for (const auto &edge : callable.edges) {
      const auto kind = sem_ir.inst(edge.from).kind;
      const auto edge_kind = kind == SemInstKind::Break
                                 ? IteratorDesugaringEdgeKind::Break
                                 : kind == SemInstKind::Continue
                                       ? IteratorDesugaringEdgeKind::Continue
                                       : kind == SemInstKind::Return
                                             ? IteratorDesugaringEdgeKind::Return
                                             : IteratorDesugaringEdgeKind::Fallthrough;
      IteratorSemIREdgeFact fact{edge_kind, kind,
                                 sem_ir.inst(edge.to).kind};
      if (kind == SemInstKind::Break || kind == SemInstKind::Continue)
        fact.loop_distance = static_cast<std::uint32_t>(
            sem_ir.integer(IntegerId(sem_ir.inst(edge.from).arg0)));
      appendCleanupFacts(sem_ir, edge.from, fact.cleanup_kinds);
      graph.semir_edges.push_back(std::move(fact));
    }
    std::ranges::sort(graph.semir_edges, [](const auto &lhs, const auto &rhs) {
      return std::tie(lhs.kind, lhs.from_kind, lhs.to_kind, lhs.loop_distance,
                      lhs.cleanup_kinds) <
             std::tie(rhs.kind, rhs.from_kind, rhs.to_kind, rhs.loop_distance,
                      rhs.cleanup_kinds);
    });
  }
  return graph;
}

IteratorDesugaringGraph buildHandWrittenIteratorExpansion(
    const SemIR &sem_ir, IteratorDesugaringOptions options) {
  return buildHandWrittenIteratorExpansion(sem_ir, FunctionId(0), options);
}

IteratorDesugaringGraph buildHandWrittenIteratorExpansion(
    const SemIR &sem_ir, FunctionId function,
    IteratorDesugaringOptions options) {
  auto graph = buildIteratorDesugaring(sem_ir, function, options);
  for (auto &node : graph.nodes)
    node.id += 1000;
  for (auto &edge : graph.edges) {
    edge.from += 1000;
    edge.to += 1000;
  }
  for (auto &loan : graph.loans) {
    loan.item_node += 1000;
    loan.projection_node += 1000;
  }
  return graph;
}

bool validateIteratorProjectionLoans(const IteratorDesugaringGraph &graph,
                                     std::string &error) {
  for (const auto &loan : graph.loans) {
    const auto *item = find(graph, loan.item_node);
    const auto *projection = find(graph, loan.projection_node);
    if (!item || !projection) {
      error = "projection loan references a missing Item node";
      return false;
    }
    if (item->kind != IteratorDesugaringNodeKind::ItemArm ||
        projection->kind != IteratorDesugaringNodeKind::ItemProjection ||
        loan.begin_scope != 2 || loan.end_scope != 2) {
      error = "Item projection loan is not confined to the Item scope";
      return false;
    }
  }
  for (const auto &edge : graph.edges) {
    if (edge.kind != IteratorDesugaringEdgeKind::Continue &&
        edge.kind != IteratorDesugaringEdgeKind::Break &&
        edge.kind != IteratorDesugaringEdgeKind::Return)
      continue;
    if (!graph.loans.empty() && edge.cleanup_depth == 0) {
      error = "control-flow edge leaves an Item projection loan live";
      return false;
    }
  }
  return true;
}

bool equivalentIteratorDesugarings(const IteratorDesugaringGraph &generated,
                                   const IteratorDesugaringGraph &reference,
                                   std::string &error) {
  if (generated.nodes.size() != reference.nodes.size() ||
      generated.edges.size() != reference.edges.size() ||
      generated.loans.size() != reference.loans.size() ||
      generated.semir_facts != reference.semir_facts ||
      generated.semir_edges != reference.semir_edges) {
    error = "iterator desugaring graph cardinality differs";
    return false;
  }
  for (const auto &node : generated.nodes) {
    const auto *other = reference.findNode(node.role);
    if (!other || node.kind != other->kind ||
        node.scope_depth != other->scope_depth ||
        node.cleanup_depth != other->cleanup_depth ||
        node.cleanup_actions != other->cleanup_actions ||
        node.source_inst.hasValue() != other->source_inst.hasValue() ||
        node.source_block.hasValue() != other->source_block.hasValue()) {
      error = "iterator desugaring node differs at role '" + node.role + "'";
      return false;
    }
  }
  const auto role_of = [](const IteratorDesugaringGraph &graph,
                          std::uint32_t id) -> std::string_view {
    const auto *node = find(graph, id);
    return node ? std::string_view(node->role) : std::string_view{};
  };
  using EdgeKey =
      std::tuple<std::uint8_t, std::uint32_t, std::string, std::string>;
  const auto edge_key = [&](const IteratorDesugaringGraph &graph,
                            const IteratorDesugaringEdge &edge) {
    return EdgeKey{static_cast<std::uint8_t>(edge.kind), edge.cleanup_depth,
                   std::string(role_of(graph, edge.from)),
                   std::string(role_of(graph, edge.to))};
  };
  std::vector<EdgeKey> generated_edges;
  std::vector<EdgeKey> reference_edges;
  for (const auto &edge : generated.edges)
    generated_edges.push_back(edge_key(generated, edge));
  for (const auto &edge : reference.edges)
    reference_edges.push_back(edge_key(reference, edge));
  std::ranges::sort(generated_edges);
  std::ranges::sort(reference_edges);
  if (generated_edges != reference_edges) {
    error = "iterator desugaring cleanup/control-flow edges differ";
    return false;
  }
  for (const auto &loan : generated.loans) {
    const auto *item = find(generated, loan.item_node);
    const auto *projection = find(generated, loan.projection_node);
    const auto *other_item = item ? reference.findNode(item->role) : nullptr;
    const auto *other_projection = projection
                                       ? reference.findNode(projection->role)
                                       : nullptr;
    const auto match = std::ranges::find_if(reference.loans, [&](const auto &c) {
      return other_item && other_projection && c.item_node == other_item->id &&
             c.projection_node == other_projection->id &&
             c.owner_local == loan.owner_local &&
             c.begin_scope == loan.begin_scope && c.end_scope == loan.end_scope &&
             c.mutability == loan.mutability;
    });
    if (match == reference.loans.end()) {
      error = "iterator Item projection loan differs";
      return false;
    }
  }
  return true;
}

} // namespace chtholly::compiler
