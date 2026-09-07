#include "LowerToLowIRInternal.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>

namespace chtholly::compiler::internal {
namespace {

class CoroutineFramePlanner {
public:
  explicit CoroutineFramePlanner(LoweringCoroutineFrameState &state)
      : state_(state) {}
  struct CoroutineFlowNode {
    bool in_function = false;
    std::size_t ordinal = std::numeric_limits<std::size_t>::max();
    LowBlockId block;
    LowInstId continuation;
    std::vector<LowInstId> successors;
    std::vector<LowInstId> predecessors;
    std::vector<LowInstId> value_uses;
    std::vector<SlotId> slot_uses;
    std::vector<SlotId> slot_defs;
    std::vector<LowInstId> live_values_in;
    std::vector<LowInstId> live_values_out;
    std::vector<SlotId> live_slots_in;
    std::vector<SlotId> live_slots_out;
  };

  struct CoroutineFlowFacts {
    std::vector<CoroutineFlowNode> nodes;
    std::vector<LowInstId> order;

    [[nodiscard]] const CoroutineFlowNode &node(LowInstId id) const {
      return nodes.at(id.index);
    }
  };

  [[nodiscard]] CoroutineFlowFacts
  buildCoroutineFlowFacts(const LowFunction &function) const {
    CoroutineFlowFacts facts;
    facts.nodes.resize(state_.session.low_ir.instCount());
    const auto append_unique = [](auto &values, auto value) {
      if (std::ranges::find(values, value) == values.end())
        values.push_back(value);
    };
    for (const auto block : state_.session.low_ir.blockList(function.blocks)) {
      const auto instructions = state_.session.low_ir.block(block);
      for (std::size_t position = 0; position < instructions.size();
           ++position) {
        const auto id = instructions[position];
        auto &node = facts.nodes[id.index];
        node.in_function = true;
        node.ordinal = facts.order.size();
        node.block = block;
        facts.order.push_back(id);
        if (position + 1 < instructions.size()) {
          node.successors.push_back(instructions[position + 1]);
          node.continuation = instructions[position + 1];
        }
        const auto &inst = state_.session.low_ir.inst(id);
        for (std::size_t argument = 0; argument != 2; ++argument) {
          const auto raw = argument == 0 ? inst.arg0 : inst.arg1;
          switch (lowInstArgKind(inst.kind, argument)) {
          case LowArgKind::Value:
            append_unique(node.value_uses, LowInstId(raw));
            break;
          case LowArgKind::ValueBlock:
            for (const auto value : state_.session.low_ir.valueBlock(LowValueBlockId(raw)))
              append_unique(node.value_uses, value);
            break;
          case LowArgKind::Slot: {
            const auto slot = SlotId(raw);
            if (inst.kind == LowInstKind::Initialize ||
                inst.kind == LowInstKind::Transfer ||
                inst.kind == LowInstKind::InitializeFromValue ||
                inst.kind == LowInstKind::Parameter ||
                inst.kind == LowInstKind::EndLifetime)
              append_unique(node.slot_defs, slot);
            else
              append_unique(node.slot_uses, slot);
            break;
          }
          case LowArgKind::Place: {
            const auto slot = state_.session.low_ir.place(LowPlaceId(raw)).root;
            if (inst.kind == LowInstKind::InitializePlace ||
                inst.kind == LowInstKind::InitializePlaceFromValue ||
                inst.kind == LowInstKind::MarkMoved)
              append_unique(node.slot_defs, slot);
            else
              append_unique(node.slot_uses, slot);
            break;
          }
          default:
            break;
          }
        }
      }
      if (instructions.empty())
        continue;
      const auto terminal = instructions.back();
      auto &successors = facts.nodes[terminal.index].successors;
      const auto &inst = state_.session.low_ir.inst(terminal);
      if (inst.kind == LowInstKind::Branch) {
        const auto target = state_.session.low_ir.block(LowBlockId(inst.arg0));
        if (!target.empty())
          append_unique(successors, target.front());
      } else if (inst.kind == LowInstKind::BranchIf) {
        const auto targets = state_.session.low_ir.targets(TargetPairId(inst.arg1));
        for (const auto target_id : {targets.true_block, targets.false_block}) {
          const auto target = state_.session.low_ir.block(target_id);
          if (!target.empty())
            append_unique(successors, target.front());
        }
      }
    }

    for (const auto id : facts.order)
      for (const auto successor : facts.nodes[id.index].successors)
        append_unique(facts.nodes[successor.index].predecessors, id);

    const auto normalize = [](auto &values) {
      std::ranges::sort(values, {}, &core::AnyId::index);
      values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    const auto merge = [&](auto &result, const auto &values) {
      result.insert(result.end(), values.begin(), values.end());
      normalize(result);
    };
    std::deque<LowInstId> worklist;
    std::vector<bool> queued(facts.nodes.size(), false);
    for (auto position = facts.order.rbegin(); position != facts.order.rend();
         ++position) {
      worklist.push_back(*position);
      queued[position->index] = true;
    }
    while (!worklist.empty()) {
      const auto id = worklist.front();
      worklist.pop_front();
      queued[id.index] = false;
      auto &node = facts.nodes[id.index];
      std::vector<LowInstId> value_out;
      std::vector<SlotId> slot_out;
      for (const auto successor : node.successors) {
        merge(value_out, facts.nodes[successor.index].live_values_in);
        merge(slot_out, facts.nodes[successor.index].live_slots_in);
      }
      auto value_in = value_out;
      std::erase(value_in, id);
      merge(value_in, node.value_uses);
      auto slot_in = slot_out;
      for (const auto def : node.slot_defs)
        std::erase(slot_in, def);
      merge(slot_in, node.slot_uses);
      if (node.live_values_out != value_out ||
          node.live_slots_out != slot_out || node.live_values_in != value_in ||
          node.live_slots_in != slot_in) {
        node.live_values_out = std::move(value_out);
        node.live_slots_out = std::move(slot_out);
        node.live_values_in = std::move(value_in);
        node.live_slots_in = std::move(slot_in);
        for (const auto predecessor : node.predecessors) {
          if (!queued[predecessor.index]) {
            worklist.push_back(predecessor);
            queued[predecessor.index] = true;
          }
        }
      }
    }

    return facts;
  }

  [[nodiscard]] InstId firstCoroutineUse(const CoroutineFlowFacts &facts,
                                         LowInstId suspension,
                                         std::uint32_t raw, bool slot) const {
    std::deque<LowInstId> pending(facts.node(suspension).successors.begin(),
                                  facts.node(suspension).successors.end());
    std::vector<bool> seen(facts.nodes.size(), false);
    while (!pending.empty()) {
      const auto id = pending.front();
      pending.pop_front();
      if (seen[id.index])
        continue;
      seen[id.index] = true;
      const auto &node = facts.node(id);
      const auto used = [&] {
        if (slot) {
          return std::ranges::any_of(
              node.slot_uses, [&](SlotId value) { return value.index == raw; });
        }
        return std::ranges::any_of(node.value_uses, [&](LowInstId value) {
          return value.index == raw;
        });
      }();
      if (used)
        return state_.session.low_ir.origin(id);
      pending.insert(pending.end(), node.successors.begin(),
                     node.successors.end());
    }
    return state_.session.low_ir.origin(suspension);
  }

  [[nodiscard]] bool coroutineReachable(const CoroutineFlowFacts &facts,
                                        LowInstId from, LowInstId to) const {
    std::deque<LowInstId> pending{from};
    std::vector<bool> seen(facts.nodes.size(), false);
    while (!pending.empty()) {
      const auto id = pending.front();
      pending.pop_front();
      if (id == to)
        return true;
      if (seen[id.index])
        continue;
      seen[id.index] = true;
      const auto &successors = facts.node(id).successors;
      pending.insert(pending.end(), successors.begin(), successors.end());
    }
    return false;
  }

  [[nodiscard]] std::vector<SlotId>
  cleanupGraphSlots(CoroutineCleanupGraphId graph_id) const {
    std::vector<SlotId> slots;
    if (!graph_id.hasValue())
      return slots;
    const auto append = [&](SlotId slot) {
      if (std::ranges::find(slots, slot) == slots.end())
        slots.push_back(slot);
    };
    const auto &graph = state_.session.low_ir.coroutineCleanupGraph(graph_id);
    for (const auto block : state_.session.low_ir.blockList(graph.blocks))
      for (const auto instruction : state_.session.low_ir.block(block)) {
        const auto &inst = state_.session.low_ir.inst(instruction);
        for (std::size_t argument = 0; argument != 2; ++argument) {
          const auto raw = argument == 0 ? inst.arg0 : inst.arg1;
          const auto kind = lowInstArgKind(inst.kind, argument);
          if (kind == LowArgKind::Slot)
            append(SlotId(raw));
          else if (kind == LowArgKind::Place)
            append(state_.session.low_ir.place(LowPlaceId(raw)).root);
        }
      }
    for (const auto local : graph.local_slots)
      std::erase(slots, local);
    std::ranges::sort(slots, {}, &SlotId::index);
    return slots;
  }

  void buildCoroutineFramePlan(FunctionId function,
                               LowFunctionId low_function_id) {
    const auto &semantic_function = state_.session.sem_ir.function(function);
    if ((semantic_function.flags &
         (SemFunctionCoroutineScaffold | SemFunctionAsync)) !=
            (SemFunctionCoroutineScaffold | SemFunctionAsync) ||
        (semantic_function.flags &
         (SemFunctionTemplate | SemFunctionSpecific)) != 0 ||
        state_.session.sem_ir.type(semantic_function.type).kind !=
            SemTypeKind::AsyncFunction) {
      state_.session.low_ir.setCoroutineLoweringError(
          "coroutine lowering requires an internal compiler scaffold function");
      return;
    }
    for (std::uint32_t index = 0; index < state_.session.sem_ir.instCount(); ++index) {
      const auto &inst = state_.session.sem_ir.inst(InstId(index));
      if (inst.kind != SemInstKind::Call &&
          inst.kind != SemInstKind::ForeignOperationCall)
        continue;
      const auto &target = state_.session.sem_ir.functionRef(FunctionRefId(inst.arg0));
      if (target.local_function == function) {
        state_.session.low_ir.setCoroutineLoweringError(
            "coroutine scaffold function is called as an ordinary function");
        return;
      }
    }

    const auto &low_function = state_.session.low_ir.function(low_function_id);
    const auto flow = buildCoroutineFlowFacts(low_function);
    CoroutineFramePlan plan;
    plan.function = function;
    plan.constructor_entity = state_.session.sem_ir.coroutineConstructorEntity(function);
    if (!plan.constructor_entity.hasValue()) {
      state_.session.low_ir.setCoroutineLoweringError(
          "coroutine scaffold has no canonical constructor entity");
      return;
    }
    plan.execution_entry =
        (semantic_function.flags & SemFunctionCoroutineExecutionEntry) != 0;
    plan.result_type = state_.session.sem_ir.asyncSuccessType(semantic_function.type);
    plan.error_type = state_.session.sem_ir.asyncErrorType(semantic_function.type);
    if (state_.session.sem_ir.type(plan.result_type).kind == SemTypeKind::Reference) {
      state_.session.low_ir.setCoroutineLoweringError(
          "coroutine scaffold cannot return a reference across suspension");
      return;
    }
    if (plan.error_type &&
        state_.session.sem_ir.type(*plan.error_type).kind == SemTypeKind::Reference) {
      state_.session.low_ir.setCoroutineLoweringError(
          "coroutine scaffold cannot return a reference error across "
          "suspension");
      return;
    }
    struct SuspensionMarker {
      InstId semantic;
      LowInstId lowered;
    };
    std::vector<SuspensionMarker> suspension_markers;
    for (const auto candidate : flow.order) {
      const auto semantic_suspension = state_.session.low_ir.origin(candidate);
      const auto empty_combination =
          state_.session.low_ir.inst(candidate).kind ==
              LowInstKind::CoroutineTaskCompletionCombine &&
          state_.session.low_ir.coroutineTaskCompletionCombinePlan(
                     state_.session.low_ir.getAs<LowCoroutineTaskCompletionCombine>(candidate)
                         .arg0)
                  .operand_count == 0;
      if ((state_.session.low_ir.inst(candidate).kind == LowInstKind::CallbackWakeWait ||
           state_.session.low_ir.inst(candidate).kind ==
               LowInstKind::CoroutineTaskCompletionWait ||
           state_.session.low_ir.inst(candidate).kind ==
               LowInstKind::CoroutineTaskCompletionCombine ||
           state_.session.low_ir.inst(candidate).kind ==
               LowInstKind::CoroutineTaskGroupDrain ||
           state_.session.low_ir.inst(candidate).kind ==
               LowInstKind::CoroutineTaskGroupErrorDrain ||
           state_.session.low_ir.inst(candidate).kind ==
               LowInstKind::CoroutineTaskGroupCancelDrain) &&
          !empty_combination && semantic_suspension.hasValue() &&
          (state_.session.sem_ir.inst(semantic_suspension).kind ==
               SemInstKind::CoroutineSuspend ||
           state_.session.sem_ir.inst(semantic_suspension).kind ==
               SemInstKind::CoroutineTaskCompletionWaitAll ||
           state_.session.sem_ir.inst(semantic_suspension).kind ==
               SemInstKind::CoroutineTaskCompletionSelect ||
           state_.session.sem_ir.inst(semantic_suspension).kind ==
               SemInstKind::CoroutineTaskCompletionRace ||
           state_.session.sem_ir.inst(semantic_suspension).kind ==
               SemInstKind::CoroutineTaskScope))
        suspension_markers.push_back({semantic_suspension, candidate});
    }
    std::ranges::sort(suspension_markers, [&](const auto &left,
                                              const auto &right) {
      return flow.node(left.lowered).ordinal < flow.node(right.lowered).ordinal;
    });
    std::unordered_map<std::uint32_t, std::uint32_t> suspension_states;
    for (std::size_t index = 0; index < suspension_markers.size(); ++index)
      suspension_states.emplace(suspension_markers[index].lowered.index,
                                static_cast<std::uint32_t>(index + 1));
    std::vector<std::vector<SlotId>> state_cleanup_roots;

    std::vector<CoroutineRegionId> instruction_regions(
        state_.session.low_ir.instCount(), CoroutineRegionId::invalid());
    for (const auto block : state_.session.low_ir.blockList(low_function.blocks)) {
      const auto instructions = state_.session.low_ir.block(block);
      std::size_t begin = 0;
      while (begin < instructions.size()) {
        std::size_t end = begin + 1;
        while (end < instructions.size() &&
               !suspension_states.contains(instructions[end - 1].index))
          ++end;
        const auto segment_id = CoroutineSegmentId(
            static_cast<std::uint32_t>(plan.segments.size()));
        const auto region_id =
            CoroutineRegionId(static_cast<std::uint32_t>(plan.regions.size()));
        CoroutineRegionEntryKind entry_kind =
            CoroutineRegionEntryKind::ControlFlow;
        if (block == low_function.entry && begin == 0) {
          entry_kind = CoroutineRegionEntryKind::Initial;
          plan.initial_region = region_id;
        } else if (begin != 0 &&
                   suspension_states.contains(instructions[begin - 1].index)) {
          entry_kind = CoroutineRegionEntryKind::Resume;
        }
        plan.segments.push_back({block, static_cast<std::uint32_t>(begin),
                                 static_cast<std::uint32_t>(end), region_id});
        CoroutineRegion region;
        region.entry_kind = entry_kind;
        region.entry = segment_id;
        region.segments.push_back(segment_id);
        const auto first = instructions[begin];
        const auto last = instructions[end - 1];
        region.live_values_in = flow.node(first).live_values_in;
        region.live_values_out = flow.node(last).live_values_out;
        region.live_places_in = flow.node(first).live_slots_in;
        region.live_places_out = flow.node(last).live_slots_out;
        plan.regions.push_back(std::move(region));
        for (std::size_t position = begin; position < end; ++position)
          instruction_regions[instructions[position].index] = region_id;
        begin = end;
      }
    }
    if (!plan.initial_region.hasValue()) {
      state_.session.low_ir.setCoroutineLoweringError(
          "coroutine lowering has no reachable initial region");
      return;
    }
    for (const auto &segment : plan.segments) {
      const auto instructions = state_.session.low_ir.block(segment.block);
      const auto terminal = instructions[segment.end - 1];
      auto &region = plan.regions[segment.region.index];
      if (const auto state = suspension_states.find(terminal.index);
          state != suspension_states.end()) {
        const auto continuation = flow.node(terminal).continuation;
        if (!continuation.hasValue() ||
            !instruction_regions[continuation.index].hasValue()) {
          state_.session.low_ir.setCoroutineLoweringError(
              "coroutine suspension has no resume continuation");
          return;
        }
        region.exits.push_back(
            {CoroutineRegionEdgeKind::Suspend, segment.region,
             instruction_regions[continuation.index], terminal, state->second});
      } else if (state_.session.low_ir.inst(terminal).kind ==
                     LowInstKind::CoroutineReturnSuccess ||
                 state_.session.low_ir.inst(terminal).kind ==
                     LowInstKind::CoroutineReturnError ||
                 state_.session.low_ir.inst(terminal).kind ==
                     LowInstKind::CoroutineReturnCancelled) {
        const auto edge_kind =
            state_.session.low_ir.inst(terminal).kind == LowInstKind::CoroutineReturnSuccess
                ? CoroutineRegionEdgeKind::Success
            : state_.session.low_ir.inst(terminal).kind == LowInstKind::CoroutineReturnError
                ? CoroutineRegionEdgeKind::Error
                : CoroutineRegionEdgeKind::Cancelled;
        region.exits.push_back({edge_kind, segment.region,
                                CoroutineRegionId::invalid(),
                                LowInstId::invalid(), 0});
      } else if (state_.session.low_ir.inst(terminal).kind == LowInstKind::Return ||
                 state_.session.low_ir.inst(terminal).kind == LowInstKind::ReturnInPlace) {
        state_.session.low_ir.setCoroutineLoweringError(
            "coroutine scaffold retained an ordinary return terminal");
        return;
      } else {
        for (const auto successor : flow.node(terminal).successors) {
          const auto target = instruction_regions[successor.index];
          if (target.hasValue())
            region.exits.push_back({CoroutineRegionEdgeKind::ControlFlow,
                                    segment.region, target,
                                    LowInstId::invalid(), 0});
        }
      }
    }
    const auto nontransferable_closure = [&](TypeId type) {
      if (state_.session.sem_ir.type(type).kind != SemTypeKind::Nominal)
        return false;
      const auto &nominal =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      const auto closure_environment =
          (nominal.flags & (SemNominalTypeClosureEnvironment |
                            SemNominalTypeBoundMethodEnvironment)) != 0;
      if (!closure_environment)
        return false;
      const auto *witness = state_.session.sem_ir.nominalSemanticWitness(type);
      return !witness || !witness->concurrency.transferable;
    };
    const auto reject_nontransferable_closure =
        [&](TypeId type, std::string_view description) {
          if (!nontransferable_closure(type))
            return false;
          state_.session.low_ir.setCoroutineLoweringError(
              "chtholly.next.sem.async.non-transferable-closure: " +
              std::string(description) + " crosses a suspension");
          return true;
        };
    for (const auto &marker : suspension_markers) {
      const auto semantic_suspension = marker.semantic;
      const auto low_suspension = marker.lowered;
      const auto &flow_node = flow.node(low_suspension);
      const auto continuation = flow_node.continuation;
      if (!continuation.hasValue()) {
        state_.session.low_ir.setCoroutineLoweringError(
            "coroutine suspension has no resume continuation");
        return;
      }
      const auto suspension_kind = state_.session.low_ir.inst(low_suspension).kind;
      const auto task_scope_suspension =
          suspension_kind == LowInstKind::CoroutineTaskGroupDrain ||
          suspension_kind == LowInstKind::CoroutineTaskGroupErrorDrain ||
          suspension_kind == LowInstKind::CoroutineTaskGroupCancelDrain;
      const auto operands =
          suspension_kind == LowInstKind::CallbackWakeWait
              ? state_.session.low_ir.valueBlock(
                    state_.session.low_ir.getAs<LowCallbackWakeWait>(low_suspension).arg1)
          : suspension_kind == LowInstKind::CoroutineTaskCompletionWait
              ? state_.session.low_ir.valueBlock(
                    state_.session.low_ir
                        .getAs<LowCoroutineTaskCompletionWait>(low_suspension)
                        .arg0)
          : task_scope_suspension
              ? state_.session.low_ir.valueBlock(
                    suspension_kind == LowInstKind::CoroutineTaskGroupDrain
                        ? state_.session.low_ir
                              .getAs<LowCoroutineTaskGroupDrain>(low_suspension)
                              .arg0
                    : suspension_kind ==
                            LowInstKind::CoroutineTaskGroupErrorDrain
                        ? state_.session.low_ir
                              .getAs<LowCoroutineTaskGroupErrorDrain>(
                                  low_suspension)
                              .arg0
                        : state_.session.low_ir
                              .getAs<LowCoroutineTaskGroupCancelDrain>(
                                  low_suspension)
                              .arg0)
              : std::span<const LowInstId>{};
      const auto wake_value =
          suspension_kind == LowInstKind::CoroutineTaskCompletionCombine
              ? state_.session.low_ir.getAs<LowCoroutineTaskCompletionCombine>(low_suspension)
                    .arg1
          : task_scope_suspension ? operands.front()
          : operands.size() == 1  ? operands.front()
                                  : LowInstId::invalid();
      const auto wake_slot = wakeSlotFor(wake_value);
      if (!wake_slot.hasValue()) {
        state_.session.low_ir.setCoroutineLoweringError(
            "coroutine suspension has no lifted active wake slot");
        return;
      }
      CoroutineResumeState state;
      state.state = static_cast<std::uint32_t>(plan.resume_states.size() + 1);
      state.semantic_suspension = semantic_suspension;
      state.suspension = low_suspension;
      state.suspension_region = instruction_regions[low_suspension.index];
      state.continuation_region = instruction_regions[continuation.index];
      state.wake_slot = wake_slot;
      if (TypeId(state_.session.low_ir.inst(low_suspension).type) != state_.session.sem_ir.voidType())
        state.result_value = low_suspension;
      state.suspension_kind =
          suspension_kind == LowInstKind::CallbackWakeWait
              ? CoroutineResumeState::SuspensionKind::CallbackWake
          : suspension_kind == LowInstKind::CoroutineTaskCompletionWait
              ? CoroutineResumeState::SuspensionKind::TaskCompletion
          : task_scope_suspension
              ? CoroutineResumeState::SuspensionKind::TaskGroupDrain
              : CoroutineResumeState::SuspensionKind::TaskCompletionSet;
      if (state.suspension_kind ==
          CoroutineResumeState::SuspensionKind::TaskGroupDrain) {
        assert(operands.size() == 2);
        state.task_group = operands[1];
        state.task_group_exit_intent =
            suspension_kind == LowInstKind::CoroutineTaskGroupDrain
                ? CoroutineTaskGroupExitIntent::Normal
            : suspension_kind == LowInstKind::CoroutineTaskGroupErrorDrain
                ? CoroutineTaskGroupExitIntent::SelectedError
                : CoroutineTaskGroupExitIntent::SelectedCancellation;
        const auto normal_exit = state.task_group_exit_intent ==
                                 CoroutineTaskGroupExitIntent::Normal;
        state.cancellation_cause_policy =
            normal_exit ? CoroutineCancellationCausePolicy::
                              OwnerRequestOrUnexpectedChild
                        : CoroutineCancellationCausePolicy::OwnerRequestOnly;
        state.child_cancellation_policy =
            normal_exit
                ? CoroutineChildCancellationPolicy::EscalateUnexpected
                : CoroutineChildCancellationPolicy::PreserveSelectedExit;
        plan.frame_values.push_back(state.task_group);
      }
      if (state_.scope_protected_suspensions.contains(low_suspension.index))
        state.cancellation_acknowledgement =
            CoroutineCancellationAcknowledgement::EnclosingTaskScope;
      if (state.suspension_kind ==
          CoroutineResumeState::SuspensionKind::CallbackWake)
        state.wake_plan =
            state_.session.low_ir.getAs<LowCallbackWakeWait>(low_suspension).arg0;
      if (state.suspension_kind ==
          CoroutineResumeState::SuspensionKind::TaskCompletionSet)
        state_.session.low_ir.bindCoroutineTaskCompletionCombineContinuation(
            state_.session.low_ir.getAs<LowCoroutineTaskCompletionCombine>(low_suspension)
                .arg0,
            continuation);
      if (std::ranges::find(plan.lifted_slots, wake_slot) ==
          plan.lifted_slots.end())
        plan.lifted_slots.push_back(wake_slot);
      for (const auto value : flow_node.live_values_out) {
        if (value == state.result_value)
          continue;
        const auto value_kind =
            state_.session.sem_ir.type(TypeId(state_.session.low_ir.inst(value).type)).kind;
        if (value_kind == SemTypeKind::Void)
          continue;
        if (value_kind == SemTypeKind::Reference) {
          const auto use =
              firstCoroutineUse(flow, low_suspension, value.index, false);
          state_.session.low_ir.setCoroutineLoweringError(
              std::string(task_scope_suspension
                              ? "chtholly.next.sem.async."
                                "reference-across-task-scope: "
                              : "chtholly.next.sem.async."
                                "reference-across-suspension: ") +
              "coroutine borrow from instruction " +
              std::to_string(state_.session.low_ir.origin(value).index) +
              " escapes suspension " +
              std::to_string(semantic_suspension.index) + " toward use " +
              std::to_string(use.index));
          return;
        }
        if (reject_nontransferable_closure(
                TypeId(state_.session.low_ir.inst(value).type),
                "closure value from instruction " +
                    std::to_string(state_.session.low_ir.origin(value).index)))
          return;
        state.live_values.push_back(value);
      }
      for (const auto slot : flow_node.live_slots_out) {
        if (state_.session.sem_ir.type(state_.session.low_ir.slot(slot).type).kind ==
            SemTypeKind::Reference) {
          const auto use =
              firstCoroutineUse(flow, low_suspension, slot.index, true);
          state_.session.low_ir.setCoroutineLoweringError(
              std::string(task_scope_suspension
                              ? "chtholly.next.sem.async."
                                "reference-across-task-scope: "
                              : "chtholly.next.sem.async."
                                "reference-across-suspension: ") +
              "coroutine reference slot " + std::to_string(slot.index) +
              " escapes suspension " +
              std::to_string(semantic_suspension.index) + " toward use " +
              std::to_string(use.index));
          return;
        }
        if (reject_nontransferable_closure(state_.session.low_ir.slot(slot).type,
                                           "closure slot " +
                                               std::to_string(slot.index)))
          return;
        state.live_places.push_back(slot);
        if (std::ranges::find(plan.lifted_slots, slot) ==
            plan.lifted_slots.end())
          plan.lifted_slots.push_back(slot);
      }
      std::ranges::sort(state.live_values, {}, &LowInstId::index);
      std::ranges::sort(state.live_places, {}, &SlotId::index);
      if (std::ranges::any_of(state.live_values, [&](LowInstId live) {
            return live != state.task_group &&
                   TypeId(state_.session.low_ir.inst(live).type) ==
                       state_.session.sem_ir.coroutineScopeType();
          }))
        state.cancellation_acknowledgement =
            CoroutineCancellationAcknowledgement::EnclosingTaskScope;
      std::vector<SlotId> cleanup_roots{wake_slot};
      if (const auto found =
              state_.suspension_pre_cleanup.find(semantic_suspension.index);
          found != state_.suspension_pre_cleanup.end())
        state.pre_commit_cleanup = found->second;
      if (const auto found =
              state_.suspension_transferred_cleanup.find(semantic_suspension.index);
          found != state_.suspension_transferred_cleanup.end()) {
        state.transferred_cleanup = found->second;
        for (const auto slot : cleanupGraphSlots(found->second)) {
          if (state_.session.sem_ir.type(state_.session.low_ir.slot(slot).type).kind ==
              SemTypeKind::Reference) {
            state_.session.low_ir.setCoroutineLoweringError(
                std::string(task_scope_suspension
                                ? "chtholly.next.sem.async."
                                  "reference-across-task-scope: "
                                : "chtholly.next.sem.async."
                                  "reference-across-suspension: ") +
                "coroutine cleanup reference slot " +
                std::to_string(slot.index) + " escapes suspension " +
                std::to_string(semantic_suspension.index));
            return;
          }
          if (reject_nontransferable_closure(state_.session.low_ir.slot(slot).type,
                                             "closure cleanup slot " +
                                                 std::to_string(slot.index)))
            return;
          if (std::ranges::find(cleanup_roots, slot) == cleanup_roots.end())
            cleanup_roots.push_back(slot);
          if (std::ranges::find(plan.lifted_slots, slot) ==
              plan.lifted_slots.end())
            plan.lifted_slots.push_back(slot);
        }
        std::ranges::sort(cleanup_roots, {}, &SlotId::index);
      }
      state_cleanup_roots.push_back(std::move(cleanup_roots));
      if (state.result_value.hasValue())
        plan.frame_values.push_back(state.result_value);
      plan.resume_states.push_back(std::move(state));
    }
    for (const auto instruction : flow.order)
      if (state_.session.low_ir.inst(instruction).kind == LowInstKind::Parameter) {
        const auto slot = state_.session.low_ir.getAs<LowParameter>(instruction).arg0;
        if (std::ranges::find(plan.lifted_slots, slot) ==
            plan.lifted_slots.end())
          plan.lifted_slots.push_back(slot);
      }
    for (std::size_t state_index = 0; state_index < plan.resume_states.size();
         ++state_index) {
      for (const auto slot : state_cleanup_roots[state_index])
        if (std::ranges::find(plan.lifted_slots, slot) ==
            plan.lifted_slots.end())
          plan.lifted_slots.push_back(slot);
      const auto &state = plan.resume_states[state_index];
      for (const auto value : state.live_values)
        if (std::ranges::find(plan.frame_values, value) ==
            plan.frame_values.end())
          plan.frame_values.push_back(value);
    }
    for (const auto instruction : flow.order)
      if (state_.session.low_ir.inst(instruction).kind == LowInstKind::ParameterValue &&
          std::ranges::find(plan.frame_values, instruction) ==
              plan.frame_values.end())
        plan.frame_values.push_back(instruction);
    std::ranges::sort(plan.frame_values, {}, &LowInstId::index);
    plan.frame_values.erase(
        std::unique(plan.frame_values.begin(), plan.frame_values.end()),
        plan.frame_values.end());
    std::ranges::sort(plan.lifted_slots, {}, &SlotId::index);
    for (const auto slot : plan.lifted_slots)
      (void)rootPlaceFor(slot);
    for (const auto slot : plan.lifted_slots) {
      std::vector<LowPlaceId> candidates;
      for (std::uint32_t index = 0; index < state_.session.low_ir.placeCount(); ++index) {
        const auto place = LowPlaceId(index);
        const auto &value = state_.session.low_ir.place(place);
        if (value.root != slot || (value.flags & LowPlaceAddressable) == 0)
          continue;
        const auto path = state_.session.low_ir.logicalPlaceProjections(place);
        if (std::ranges::find(path, LowPlaceProjectionKind::Dereference,
                              &LowPlaceProjection::kind) != path.end())
          continue;
        const auto duplicate = std::ranges::any_of(candidates, [&](auto other) {
          const auto other_path = state_.session.low_ir.logicalPlaceProjections(other);
          return other_path.size() == path.size() &&
                 std::equal(other_path.begin(), other_path.end(), path.begin());
        });
        if (!duplicate)
          candidates.push_back(place);
      }
      std::ranges::sort(candidates, [&](auto left, auto right) {
        const auto left_size = state_.session.low_ir.logicalPlaceProjections(left).size();
        const auto right_size = state_.session.low_ir.logicalPlaceProjections(right).size();
        return left_size != right_size ? left_size < right_size
                                       : left.index < right.index;
      });
      for (const auto place : candidates)
        plan.frame_places.push_back(
            {place, static_cast<std::uint32_t>(plan.frame_places.size())});
    }
    for (std::size_t state_index = 0; state_index < plan.resume_states.size();
         ++state_index) {
      auto &cleanup = plan.resume_states[state_index].cleanup_order;
      for (std::size_t place_index = 0; place_index < plan.frame_places.size();
           ++place_index) {
        const auto root =
            state_.session.low_ir.place(plan.frame_places[place_index].place).root;
        if (std::ranges::find(state_cleanup_roots[state_index], root) !=
            state_cleanup_roots[state_index].end())
          cleanup.emplace_back(static_cast<std::uint32_t>(place_index));
      }
    }
    for (std::size_t index = 0; index < plan.frame_places.size(); ++index)
      plan.cleanup_order.emplace_back(static_cast<std::uint32_t>(index));
    for (const auto instruction : flow.order)
      if (const auto found =
              state_.cancellation_cleanup.find(state_.session.low_ir.origin(instruction).index);
          found != state_.cancellation_cleanup.end())
        plan.cancellation_cleanups.push_back({instruction, found->second});
    std::ranges::sort(plan.cancellation_cleanups, {},
                      &CoroutineCancellationCleanup::instruction);
    (void)state_.session.low_ir.addCoroutineFramePlan(std::move(plan));
  }

private:
  [[nodiscard]] SlotId wakeSlotFor(LowInstId value) const {
    return LoweringCoroutineService::wakeSlotFor(state_.session, value);
  }
  [[nodiscard]] LowPlaceId rootPlaceFor(SlotId slot) const {
    return state_.root_place_for(slot);
  }

  LoweringCoroutineFrameState &state_;
};

} // namespace

void LoweringCoroutineFrameService::build(
    FunctionId function, LowFunctionId low_function,
    LoweringCoroutineFrameState &state) {
  CoroutineFramePlanner(state).buildCoroutineFramePlan(function, low_function);
}

} // namespace chtholly::compiler::internal
