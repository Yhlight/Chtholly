// Included by CallableOwnership.cpp; this fragment aliases the existing
// ownership graph, region, and summary state.
constexpr std::size_t MaxOwnershipRegionPath = 256;

struct ConditionedRegion {
  OwnershipRegion region;
  CallableConditionDescriptor condition = CallableConditionDescriptor::always();

  friend bool operator==(const ConditionedRegion &,
                         const ConditionedRegion &) = default;
};

using Regions = std::vector<ConditionedRegion>;
using RegionState = std::unordered_map<std::uint32_t, Regions>;

struct FunctionEdge {
  InstId instruction;
  CallableConditionDescriptor condition = CallableConditionDescriptor::always();
};

struct FunctionGraph {
  std::vector<InstId> instructions;
  std::vector<InstId> entries;
  std::unordered_map<std::uint32_t, std::vector<FunctionEdge>> successors;
  std::unordered_map<std::uint32_t, std::vector<FunctionEdge>> predecessors;
};

void appendUnique(std::vector<InstId> &target, std::span<const InstId> source) {
  for (const auto id : source)
    if (std::ranges::find(target, id) == target.end())
      target.push_back(id);
}

std::optional<std::uint32_t>
parameterIndex(const SemIR &sem_ir, const SemFunction &function, LocalId local);

const CallableOwnershipSummary *
callSummary(const SemIR &sem_ir, InstId instruction, const SemInst &inst,
            std::optional<CallableOwnershipSummary> &adapter_summary);

std::optional<CallableConditionDescriptor>
conditionForValue(const SemIR &sem_ir, const SemFunction &function,
                  InstId value) {
  const auto &inst = sem_ir.inst(value);
  if (inst.kind == SemInstKind::CallbackRegistrationBinding)
    return conditionForValue(sem_ir, function, InstId(inst.arg1));
  if (inst.kind == SemInstKind::BoolLiteral)
    return sem_ir.integer(IntegerId(inst.arg0)) == 0
               ? CallableConditionDescriptor::never()
               : CallableConditionDescriptor::always();
  if (inst.kind == SemInstKind::NameRef) {
    if (const auto parameter =
            parameterIndex(sem_ir, function, LocalId(inst.arg0)))
      return CallableConditionDescriptor::atom(*parameter);
    return std::nullopt;
  }
  if (inst.kind == SemInstKind::LogicalNot) {
    auto operand = conditionForValue(sem_ir, function, InstId(inst.arg0));
    return operand ? std::optional(conditionNot(*operand)) : std::nullopt;
  }
  if (inst.kind == SemInstKind::LogicalAnd) {
    auto lhs = conditionForValue(sem_ir, function, InstId(inst.arg0));
    auto rhs = conditionForValue(sem_ir, function, InstId(inst.arg1));
    return lhs && rhs ? std::optional(conditionAnd(std::move(*lhs), *rhs))
                      : std::nullopt;
  }
  if (inst.kind == SemInstKind::LogicalOr) {
    auto lhs = conditionForValue(sem_ir, function, InstId(inst.arg0));
    auto rhs = conditionForValue(sem_ir, function, InstId(inst.arg1));
    return lhs && rhs ? std::optional(conditionOr(std::move(*lhs), *rhs))
                      : std::nullopt;
  }
  return std::nullopt;
}

CallableConditionDescriptor conditionForEnumParameter(const SemIR &sem_ir,
    const SemFunction &function, InstId value, std::uint32_t variant) {
  const auto &inst = sem_ir.inst(value);
  if (inst.kind == SemInstKind::NameRef) {
    if (auto parameter = parameterIndex(sem_ir, function, LocalId(inst.arg0)))
      return CallableConditionDescriptor::enumVariant(*parameter, variant);
  }
  if (inst.kind == SemInstKind::EnumInit)
    return sem_ir.integer(IntegerId(inst.arg1)) == variant
        ? CallableConditionDescriptor::always() : CallableConditionDescriptor::never();
  return CallableConditionDescriptor::always();
}

FunctionGraph buildFunctionGraph(const SemIR &sem_ir,
                                 const SemFunction &function,
                                 InstBlockId entry) {
  FunctionGraph graph;
  using DeferredBlocks = std::vector<InstBlockId>;
  std::unordered_map<std::uint32_t, DeferredBlocks> block_defers;
  std::unordered_map<std::uint32_t, DeferredBlocks> exit_defers;
  struct LoopDepth {
    std::size_t break_depth = 0;
    std::size_t continue_depth = 0;
  };
  std::function<void(InstBlockId, DeferredBlocks &, std::vector<LoopDepth> &,
                     bool)>
      collect_defers;
  collect_defers = [&](InstBlockId block, DeferredBlocks &active,
                       std::vector<LoopDepth> &loops, bool lexical_scope) {
    const auto begin = active.size();
    for (const auto instruction : sem_ir.instBlock(block)) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind == SemInstKind::Defer) {
        DeferredBlocks nested_active;
        std::vector<LoopDepth> nested_loops;
        collect_defers(InstBlockId(inst.arg0), nested_active, nested_loops,
                       true);
        active.push_back(InstBlockId(inst.arg0));
      } else if (inst.kind == SemInstKind::Return) {
        exit_defers[instruction.index] = active;
      } else if ((inst.kind == SemInstKind::Break ||
                  inst.kind == SemInstKind::Continue) &&
                 !loops.empty()) {
        const auto distance =
            static_cast<std::size_t>(sem_ir.integer(IntegerId(inst.arg0)));
        assert(distance < loops.size());
        const auto &target = loops[loops.size() - 1 - distance];
        const auto depth = inst.kind == SemInstKind::Break
                               ? target.break_depth
                               : target.continue_depth;
        exit_defers[instruction.index] = DeferredBlocks(
            active.begin() + static_cast<std::ptrdiff_t>(depth), active.end());
      } else if (inst.kind == SemInstKind::If) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          auto arm_active = active;
          const auto &arm = sem_ir.getAs<SemIfArm>(arm_id);
          collect_defers(arm.arg0, arm_active, loops, true);
        }
      } else if (inst.kind == SemInstKind::ScopedBlock) {
        collect_defers(InstBlockId(inst.arg0), active, loops, true);
      } else if (inst.kind == SemInstKind::While) {
        auto body_active = active;
        loops.push_back({active.size(), active.size()});
        collect_defers(InstBlockId(inst.arg0), body_active, loops, false);
        body_active = active;
        collect_defers(InstBlockId(inst.arg1), body_active, loops, true);
        loops.pop_back();
      } else if (inst.kind == SemInstKind::For) {
        const auto clauses = sem_ir.instBlock(InstBlockId(inst.arg0));
        const auto outer_depth = active.size();
        collect_defers(sem_ir.getAs<SemForClause>(clauses[0]).arg1, active,
                       loops, false);
        const auto loop_depth = active.size();
        auto nested_active = active;
        loops.push_back({outer_depth, loop_depth});
        collect_defers(sem_ir.getAs<SemForClause>(clauses[1]).arg1,
                       nested_active, loops, false);
        nested_active = active;
        collect_defers(InstBlockId(inst.arg1), nested_active, loops, true);
        nested_active = active;
        collect_defers(sem_ir.getAs<SemForClause>(clauses[2]).arg1,
                       nested_active, loops, false);
        loops.pop_back();
      } else if (inst.kind == SemInstKind::DoWhile) {
        loops.push_back({active.size(), active.size()});
        auto nested_active = active;
        collect_defers(InstBlockId(inst.arg1), nested_active, loops, true);
        nested_active = active;
        collect_defers(InstBlockId(inst.arg0), nested_active, loops, false);
        loops.pop_back();
      } else if (inst.kind == SemInstKind::Switch) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          auto arm_active = active;
          const auto &arm = sem_ir.getAs<SemSwitchArm>(arm_id);
          collect_defers(arm.arg1, arm_active, loops, true);
        }
      }
    }
    if (lexical_scope) {
      block_defers[block.index] = DeferredBlocks(
          active.begin() + static_cast<std::ptrdiff_t>(begin), active.end());
      active.resize(begin);
    }
  };
  DeferredBlocks active_defers;
  std::vector<LoopDepth> loop_depths;
  collect_defers(entry, active_defers, loop_depths, false);
  struct LoopTargets {
    std::vector<InstId> breaks;
    std::vector<InstId> continues;
  };
  std::vector<LoopTargets> active_targets;

  std::function<std::vector<InstId>(InstBlockId, std::vector<InstId>,
                                    std::vector<InstId>, std::vector<InstId>)>
      build;
  build = [&](InstBlockId block, std::vector<InstId> tail,
              std::vector<InstId> break_target,
              std::vector<InstId> continue_target) {
    const auto append_edge = [&](InstId source, InstId target,
                                 CallableConditionDescriptor condition =
                                     CallableConditionDescriptor::always()) {
      auto &edges = graph.successors[source.index];
      bool duplicate = false;
      for (const auto &edge : edges) {
        if (edge.instruction == target && edge.condition == condition) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate)
        edges.push_back({target, std::move(condition)});
    };
    const auto cleanup_entries = [&](std::span<const InstBlockId> cleanups,
                                     std::vector<InstId> successor) {
      for (const auto cleanup : cleanups)
        successor = build(cleanup, std::move(successor), {}, {});
      return successor;
    };
    const auto block_cleanup = block_defers.find(block.index);
    auto current =
        block_cleanup == block_defers.end()
            ? std::move(tail)
            : cleanup_entries(block_cleanup->second, std::move(tail));
    const auto instructions = sem_ir.instBlock(block);
    for (auto position = instructions.rbegin(); position != instructions.rend();
         ++position) {
      const auto instruction = *position;
      const auto &inst = sem_ir.inst(instruction);
      auto next = current;
      if (inst.kind == SemInstKind::Return) {
        const auto cleanups = exit_defers.find(instruction.index);
        next = cleanups == exit_defers.end()
                   ? std::vector<InstId>{}
                   : cleanup_entries(cleanups->second, {});
      } else if (inst.kind == SemInstKind::Break) {
        const auto distance =
            static_cast<std::size_t>(sem_ir.integer(IntegerId(inst.arg0)));
        assert(distance < active_targets.size());
        const auto &target =
            active_targets[active_targets.size() - 1 - distance];
        const auto cleanups = exit_defers.find(instruction.index);
        next = cleanups == exit_defers.end()
                   ? target.breaks
                   : cleanup_entries(cleanups->second, target.breaks);
      } else if (inst.kind == SemInstKind::Continue) {
        const auto distance =
            static_cast<std::size_t>(sem_ir.integer(IntegerId(inst.arg0)));
        assert(distance < active_targets.size());
        const auto &target =
            active_targets[active_targets.size() - 1 - distance];
        const auto cleanups = exit_defers.find(instruction.index);
        next = cleanups == exit_defers.end()
                   ? target.continues
                   : cleanup_entries(cleanups->second, target.continues);
      } else if (inst.kind == SemInstKind::If) {
        const auto arms = sem_ir.instBlock(InstBlockId(inst.arg1));
        const auto &then_arm = sem_ir.getAs<SemIfArm>(arms.front());
        auto then_branch =
            build(then_arm.arg0, current, break_target, continue_target);
        auto else_branch = current;
        if (arms.size() == 2)
          else_branch = build(sem_ir.getAs<SemIfArm>(arms[1]).arg0, current,
                              break_target, continue_target);
        const auto predicate =
            conditionForValue(sem_ir, function, InstId(inst.arg0));
        for (const auto target : then_branch)
          append_edge(
              instruction, target,
              predicate.value_or(CallableConditionDescriptor::always()));
        for (const auto target : else_branch)
          append_edge(instruction, target,
                      predicate ? conditionNot(*predicate)
                                : CallableConditionDescriptor::always());
        current = {instruction};
        continue;
      } else if (inst.kind == SemInstKind::Switch) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          const auto &arm = sem_ir.getAs<SemSwitchArm>(arm_id);
          const auto branch =
              build(arm.arg1, current, break_target, continue_target);
          const auto variant = sem_ir.integer(arm.arg0);
          const auto predicate = variant >= 0
              ? conditionForEnumParameter(sem_ir, function, InstId(inst.arg0), static_cast<std::uint32_t>(variant))
              : CallableConditionDescriptor::always();
          for (const auto target : branch)
            append_edge(instruction, target, predicate);
        }
        current = {instruction};
        continue;
      } else if (inst.kind == SemInstKind::ScopedBlock) {
        next = build(InstBlockId(inst.arg0), current, break_target,
                     continue_target);
      } else if (inst.kind == SemInstKind::While) {
        const auto condition = sem_ir.instBlock(InstBlockId(inst.arg0));
        if (!condition.empty()) {
          const auto condition_entry = condition.front();
          active_targets.push_back({current, {condition_entry}});
          auto body_entries =
              build(InstBlockId(inst.arg1), {condition_entry}, current,
                    {condition_entry});
          active_targets.pop_back();
          next = build(InstBlockId(inst.arg0), {}, break_target,
                       continue_target);
          const auto condition_value = condition.back();
          const auto predicate =
              conditionForValue(sem_ir, function, condition_value);
          for (const auto target : body_entries)
            append_edge(
                condition_value, target,
                predicate.value_or(CallableConditionDescriptor::always()));
          for (const auto target : current)
            append_edge(condition_value, target,
                        predicate ? conditionNot(*predicate)
                                  : CallableConditionDescriptor::always());
        }
      } else if (inst.kind == SemInstKind::For) {
        const auto clauses = sem_ir.instBlock(InstBlockId(inst.arg0));
        const auto init = sem_ir.getAs<SemForClause>(clauses[0]).arg1;
        const auto condition = sem_ir.getAs<SemForClause>(clauses[1]).arg1;
        const auto step = sem_ir.getAs<SemForClause>(clauses[2]).arg1;
        const auto condition_values = sem_ir.instBlock(condition);
        if (!condition_values.empty()) {
          const auto condition_entry = condition_values.front();
          const auto step_entries = build(step, {condition_entry}, break_target,
                                         continue_target);
          active_targets.push_back({current, step_entries});
          const auto body_entries =
              build(InstBlockId(inst.arg1), step_entries, current, step_entries);
          active_targets.pop_back();
          const auto condition_entries =
              build(condition, {}, break_target, continue_target);
          const auto condition_value = condition_values.back();
          const auto predicate =
              conditionForValue(sem_ir, function, condition_value);
          for (const auto target : body_entries)
            append_edge(
                condition_value, target,
                predicate.value_or(CallableConditionDescriptor::always()));
          for (const auto target : current)
            append_edge(condition_value, target,
                        predicate ? conditionNot(*predicate)
                                  : CallableConditionDescriptor::always());
          next = build(init, condition_entries, break_target, continue_target);
        }
      } else if (inst.kind == SemInstKind::DoWhile) {
        const auto body_values = sem_ir.instBlock(InstBlockId(inst.arg1));
        const auto condition_values = sem_ir.instBlock(InstBlockId(inst.arg0));
        if (!condition_values.empty()) {
          const auto condition_entries =
              build(InstBlockId(inst.arg0), {}, break_target, continue_target);
          if (body_values.empty()) {
            next = condition_entries;
          } else {
            active_targets.push_back({current, condition_entries});
            next = build(InstBlockId(inst.arg1), condition_entries, current,
                         condition_entries);
            active_targets.pop_back();
          }
          const auto condition_value = condition_values.back();
          const auto predicate =
              conditionForValue(sem_ir, function, condition_value);
          append_edge(
              condition_value,
              body_values.empty() ? condition_entries.front()
                                  : body_values.front(),
              predicate.value_or(CallableConditionDescriptor::always()));
          for (const auto target : current)
            append_edge(condition_value, target,
                        predicate ? conditionNot(*predicate)
                                  : CallableConditionDescriptor::always());
        }
      }
      for (const auto target : next)
        append_edge(instruction, target);
      current = {instruction};
    }
    return current;
  };
  graph.entries = build(entry, {}, {}, {});

  std::unordered_set<std::uint32_t> reachable;
  DenseWorklist<InstId> pending(sem_ir.instCount(),
                                DenseWorklistOrder::LIFO);
  for (const auto graph_entry : graph.entries)
    pending.push(graph_entry);
  while (const auto next = pending.pop()) {
    const auto instruction = *next;
    if (!reachable.insert(instruction.index).second)
      continue;
    graph.instructions.push_back(instruction);
    for (const auto &edge : graph.successors[instruction.index])
      pending.push(edge.instruction);
  }
  std::sort(graph.instructions.begin(), graph.instructions.end(),
            [](InstId lhs, InstId rhs) { return lhs.index < rhs.index; });
  for (const auto instruction : graph.instructions)
    for (const auto &successor : graph.successors[instruction.index])
      graph.predecessors[successor.instruction.index].push_back(
          {instruction, successor.condition});
  return graph;
}
