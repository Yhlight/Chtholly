#include "LowIRVerificationContext.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool LowIRVerificationContext::verifyCoroutinePlans(std::string &error) const {
  const auto sem_ir_ = low_ir_.sem_ir_;
  const auto &block_lists_ = low_ir_.block_lists_;
  const auto &construct_plans_ = low_ir_.construct_plans_;
  const auto &coroutine_cleanup_graphs_ = low_ir_.coroutine_cleanup_graphs_;
  const auto &coroutine_frame_plans_ = low_ir_.coroutine_frame_plans_;
  const auto &coroutine_task_completion_arm_plans_ = low_ir_.coroutine_task_completion_arm_plans_;
  const auto &coroutine_task_completion_combine_plans_ = low_ir_.coroutine_task_completion_combine_plans_;
  const auto &coroutine_task_completion_set_plans_ = low_ir_.coroutine_task_completion_set_plans_;
  const auto &coroutine_task_create_plans_ = low_ir_.coroutine_task_create_plans_;
  const auto &insts_ = low_ir_.insts_;
  const auto block = [&](LowBlockId id) { return low_ir_.block(id); };
  const auto blockCount = [&] { return low_ir_.blockCount(); };
  const auto blockList = [&](LowBlockListId id) { return low_ir_.blockList(id); };
  const auto function = [&](LowFunctionId id) -> const LowFunction & { return low_ir_.function(id); };
  const auto inst = [&](LowInstId id) -> const LowInst & { return low_ir_.inst(id); };
  const auto origin = [&](LowInstId id) { return low_ir_.origin(id); };
  const auto functionCount = [&] { return low_ir_.functionCount(); };
  const auto placeCount = [&] { return low_ir_.placeCount(); };
  const auto instCount = [&] { return low_ir_.instCount(); };
  const auto place = [&](LowPlaceId id) -> const LowPlace & { return low_ir_.place(id); };
  const auto logicalPlaceProjections = [&](LowPlaceId id) { return low_ir_.logicalPlaceProjections(id); };
  const auto slot = [&](SlotId id) -> const LowSlot & { return low_ir_.slot(id); };
  const auto slotBlock = [&](SlotBlockId id) { return low_ir_.slotBlock(id); };
  const auto targets = [&](TargetPairId id) -> const TargetPair & { return low_ir_.targets(id); };
  const auto valueBlock = [&](LowValueBlockId id) { return low_ir_.valueBlock(id); };
  const auto containsArg = [&](LowArgKind kind, std::uint32_t raw) {
    return low_ir_.containsArg(kind, raw);
  };
  const auto coroutineCleanupGraph = [&](CoroutineCleanupGraphId id) -> const CoroutineCleanupGraph & {
    return low_ir_.coroutineCleanupGraph(id);
  };
  const auto coroutineTaskCompletionCombinePlan = [&](CoroutineTaskCompletionCombinePlanId id) -> const CoroutineTaskCompletionCombinePlan & {
    return low_ir_.coroutineTaskCompletionCombinePlan(id);
  };
  const auto build = [&](auto fn, auto... args) { return (low_ir_.*fn)(args...); };

  std::vector<std::uint32_t> task_plan_uses(
      coroutine_task_create_plans_.size());
  std::vector<std::uint32_t> completion_plan_uses(
      coroutine_task_completion_arm_plans_.size());
  std::vector<std::uint32_t> completion_set_plan_uses(
      coroutine_task_completion_set_plans_.size());
  std::vector<std::uint32_t> completion_combine_plan_uses(
      coroutine_task_completion_combine_plans_.size());
  std::vector<std::uint32_t> construct_plan_uses(construct_plans_.size());
  for (const auto &instruction : insts_.values())
    if (instruction.kind == LowInstKind::CoroutineTaskCreate &&
        instruction.arg0 < task_plan_uses.size())
      ++task_plan_uses[instruction.arg0];
    else if (instruction.kind == LowInstKind::CoroutineTaskCompletionArm &&
             instruction.arg0 < completion_plan_uses.size())
      ++completion_plan_uses[instruction.arg0];
    else if (instruction.kind ==
                 LowInstKind::CoroutineTaskCompletionSetCreate &&
             instruction.arg0 < completion_set_plan_uses.size())
      ++completion_set_plan_uses[instruction.arg0];
    else if (instruction.kind == LowInstKind::CoroutineTaskCompletionCombine &&
             instruction.arg0 < completion_combine_plan_uses.size())
      ++completion_combine_plan_uses[instruction.arg0];
    else if (instruction.kind == LowInstKind::Construct &&
             instruction.arg0 < construct_plan_uses.size())
      ++construct_plan_uses[instruction.arg0];
  if (std::ranges::any_of(task_plan_uses,
                          [](auto uses) { return uses != 1; })) {
    error = "LowIR coroutine task-create plan is not uniquely owned";
    return false;
  }
  if (std::ranges::any_of(completion_plan_uses,
                          [](auto uses) { return uses != 1; })) {
    error = "LowIR coroutine completion-arm plan is not uniquely owned";
    return false;
  }
  if (std::ranges::any_of(completion_set_plan_uses,
                          [](auto uses) { return uses != 1; }) ||
      std::ranges::any_of(completion_combine_plan_uses,
                          [](auto uses) { return uses != 1; })) {
    error = "LowIR coroutine completion-combination plan is not uniquely owned";
    return false;
  }
  if (std::ranges::any_of(construct_plan_uses,
                          [](auto uses) { return uses != 1; })) {
    error = "LowIR construct plan is not uniquely owned";
    return false;
  }
  std::unordered_set<std::uint32_t> coroutine_functions;
  std::vector<std::uint32_t> coroutine_cleanup_graph_uses(
      coroutine_cleanup_graphs_.size());
  bool has_coroutine_execution_entry = false;
  for (const auto &plan : coroutine_frame_plans_.values()) {
    if (plan.abi_version != 11 || plan.constructor_abi_epoch != 1 ||
        !plan.function.hasValue() || !plan.constructor_entity.hasValue() ||
        plan.function.index >= sem_ir_->functionCount() ||
        !coroutine_functions.insert(plan.function.index).second ||
        plan.start_policy != CoroutineStartPolicy::Eager ||
        plan.evaluation_policy !=
            CoroutineEvaluationPolicy::LeftToRightExactlyOnce ||
        plan.executor_binding_policy !=
            CoroutineExecutorBindingPolicy::InheritAtCreation ||
        plan.executor_switch_policy !=
            CoroutineExecutorSwitchPolicy::Persistent ||
        plan.deadline_clock_policy !=
            CoroutineDeadlineClockPolicy::RuntimeMonotonic ||
        plan.deadline_representation !=
            CoroutineDeadlineRepresentation::AbsoluteNormalized ||
        plan.deadline_inheritance_policy !=
            CoroutineDeadlineInheritancePolicy::EarliestActive ||
        plan.deadline_outcome_policy !=
            CoroutineDeadlineOutcomePolicy::StickyCancellation ||
        plan.deadline_cause_precedence !=
            CoroutineDeadlineCausePrecedence::FirstLinearized ||
        plan.terminal_precedence !=
            CoroutineTerminalPrecedence::CancellationBeforeTerminalCommit ||
        plan.cancellation_points != FrozenCoroutineCancellationPoints) {
      error = "LowIR has an invalid or duplicate coroutine frame plan";
      return false;
    }
    const auto &semantic_function = sem_ir_->function(plan.function);
    if (plan.execution_entry &&
        (has_coroutine_execution_entry ||
         !sem_ir_
              ->typeBlock(
                  TypeBlockId(sem_ir_->type(semantic_function.type).arg0))
              .empty())) {
      error = "LowIR has an invalid coroutine execution entry";
      return false;
    }
    has_coroutine_execution_entry |= plan.execution_entry;
    if ((semantic_function.flags &
         (SemFunctionCoroutineScaffold | SemFunctionAsync)) !=
            (SemFunctionCoroutineScaffold | SemFunctionAsync) ||
        (semantic_function.flags &
         (SemFunctionTemplate | SemFunctionSpecific)) != 0 ||
        sem_ir_->type(semantic_function.type).kind !=
            SemTypeKind::AsyncFunction ||
        plan.result_type != sem_ir_->asyncSuccessType(semantic_function.type) ||
        plan.error_type != sem_ir_->asyncErrorType(semantic_function.type) ||
        plan.constructor_entity !=
            sem_ir_->coroutineConstructorEntity(plan.function)) {
      error = "LowIR coroutine plan does not own an internal scaffold";
      return false;
    }
    const auto *constructor =
        sem_ir_->importIRs().tryGetEntity(plan.constructor_entity);
    if (!constructor || constructor->kind != PublicEntityKind::Function ||
        constructor->execution_kind != PublicFunctionExecutionKind::Async ||
        constructor->coroutine_constructor !=
            PublicCoroutineConstructorABI{1, true, true, true, true}) {
      error = "LowIR coroutine frame has an invalid constructor ABI";
      return false;
    }
    const LowFunction *low_function = nullptr;
    for (std::uint32_t index = 0; index < functionCount(); ++index) {
      const auto &candidate = function(LowFunctionId(index));
      if (candidate.semantic_function == plan.function) {
        low_function = &candidate;
        break;
      }
    }
    if (low_function == nullptr) {
      error = "LowIR coroutine plan has no lowered function";
      return false;
    }
    const auto slots = slotBlock(low_function->slots);
    if (plan.cleanup_order.size() != plan.frame_places.size()) {
      error = "LowIR coroutine frame has an invalid lifted-slot set";
      return false;
    }
    std::unordered_set<std::uint32_t> lifted;
    for (std::size_t index = 0; index < plan.lifted_slots.size(); ++index) {
      if (std::ranges::find(slots, plan.lifted_slots[index]) == slots.end() ||
          !lifted.insert(plan.lifted_slots[index].index).second ||
          sem_ir_->type(slot(plan.lifted_slots[index]).type).kind ==
              SemTypeKind::Reference) {
        error = "LowIR coroutine frame has an invalid cleanup contract";
        return false;
      }
    }
    std::unordered_set<std::uint32_t> frame_place_values;
    for (std::size_t index = 0; index < plan.frame_places.size(); ++index) {
      const auto &frame_place = plan.frame_places[index];
      if (!frame_place.place.hasValue() ||
          frame_place.place.index >= placeCount() ||
          frame_place.initialization_bit != index ||
          !frame_place_values.insert(frame_place.place.index).second ||
          !lifted.contains(place(frame_place.place).root.index) ||
          (place(frame_place.place).flags & LowPlaceAddressable) == 0 ||
          std::ranges::find(logicalPlaceProjections(frame_place.place),
                            LowPlaceProjectionKind::Dereference,
                            &LowPlaceProjection::kind) !=
              logicalPlaceProjections(frame_place.place).end() ||
          plan.cleanup_order[index].index != index) {
        error = "LowIR coroutine frame has an invalid projected-place set";
        return false;
      }
    }
    std::unordered_set<std::uint32_t> function_instructions;
    for (const auto block_id : blockList(low_function->blocks))
      for (const auto instruction : block(block_id))
        function_instructions.insert(instruction.index);
    if (!plan.initial_region.hasValue() ||
        plan.initial_region.index >= plan.regions.size() ||
        plan.regions[plan.initial_region.index].entry_kind !=
            CoroutineRegionEntryKind::Initial ||
        plan.segments.empty() || plan.regions.size() != plan.segments.size()) {
      error = "LowIR coroutine plan has an invalid initial region";
      return false;
    }
    std::unordered_set<std::uint32_t> partitioned_instructions;
    std::vector<CoroutineRegionId> instruction_regions(
        instCount(), CoroutineRegionId::invalid());
    for (std::size_t segment_index = 0; segment_index < plan.segments.size();
         ++segment_index) {
      const auto segment_id =
          CoroutineSegmentId(static_cast<std::uint32_t>(segment_index));
      const auto &segment = plan.segments[segment_index];
      if (!segment.block.hasValue() || segment.block.index >= blockCount() ||
          !segment.region.hasValue() ||
          segment.region.index >= plan.regions.size() ||
          segment.begin >= segment.end ||
          segment.end > block(segment.block).size()) {
        error = "LowIR coroutine plan has an invalid segment";
        return false;
      }
      const auto &region = plan.regions[segment.region.index];
      if (region.entry != segment_id || region.segments.size() != 1 ||
          region.segments.front() != segment_id ||
          region.entry_kind == CoroutineRegionEntryKind::Count) {
        error = "LowIR coroutine plan has an invalid region ownership";
        return false;
      }
      const auto instructions = block(segment.block);
      for (std::size_t position = segment.begin; position < segment.end;
           ++position) {
        const auto instruction = instructions[position];
        if (!function_instructions.contains(instruction.index) ||
            !partitioned_instructions.insert(instruction.index).second) {
          error = "LowIR coroutine plan has duplicate segment ownership";
          return false;
        }
        instruction_regions[instruction.index] = segment.region;
      }
    }
    if (partitioned_instructions != function_instructions) {
      error = "LowIR coroutine plan does not partition its function";
      return false;
    }
    std::unordered_set<std::uint32_t> edge_states;
    std::unordered_set<std::uint32_t> resume_regions;
    std::vector<std::vector<CoroutineRegionId>> region_successors(
        plan.regions.size());
    for (std::size_t region_index = 0; region_index < plan.regions.size();
         ++region_index) {
      const auto region_id =
          CoroutineRegionId(static_cast<std::uint32_t>(region_index));
      const auto &region = plan.regions[region_index];
      const auto &segment = plan.segments[region.entry.index];
      const auto terminal = block(segment.block)[segment.end - 1];
      std::vector<CoroutineRegionId> actual_targets;
      for (const auto &edge : region.exits) {
        const auto terminal_edge =
            edge.kind == CoroutineRegionEdgeKind::Success ||
            edge.kind == CoroutineRegionEdgeKind::Error ||
            edge.kind == CoroutineRegionEdgeKind::Cancelled;
        if (edge.source != region_id ||
            edge.kind == CoroutineRegionEdgeKind::Count ||
            (!terminal_edge && (!edge.target.hasValue() ||
                                edge.target.index >= plan.regions.size()))) {
          error = "LowIR coroutine plan has an invalid region edge";
          return false;
        }
        if (edge.kind == CoroutineRegionEdgeKind::Suspend) {
          if (edge.suspension != terminal ||
              (inst(terminal).kind != LowInstKind::CallbackWakeWait &&
               inst(terminal).kind !=
                   LowInstKind::CoroutineTaskCompletionWait &&
               inst(terminal).kind !=
                   LowInstKind::CoroutineTaskCompletionCombine &&
               inst(terminal).kind != LowInstKind::CoroutineTaskGroupDrain &&
               inst(terminal).kind !=
                   LowInstKind::CoroutineTaskGroupErrorDrain &&
               inst(terminal).kind !=
                   LowInstKind::CoroutineTaskGroupCancelDrain) ||
              edge.resume_state == 0 ||
              !edge_states.insert(edge.resume_state).second ||
              plan.regions[edge.target.index].entry_kind !=
                  CoroutineRegionEntryKind::Resume) {
            error = "LowIR coroutine plan has an invalid suspension edge";
            return false;
          }
          if (segment.end >= block(segment.block).size() ||
              instruction_regions[block(segment.block)[segment.end].index] !=
                  edge.target) {
            error = "LowIR coroutine suspension edge misses its continuation";
            return false;
          }
          actual_targets.push_back(edge.target);
          resume_regions.insert(edge.target.index);
        } else if (terminal_edge) {
          const auto expected_terminal =
              edge.kind == CoroutineRegionEdgeKind::Success
                  ? LowInstKind::CoroutineReturnSuccess
              : edge.kind == CoroutineRegionEdgeKind::Error
                  ? LowInstKind::CoroutineReturnError
                  : LowInstKind::CoroutineReturnCancelled;
          if (edge.target.hasValue() || edge.suspension.hasValue() ||
              edge.resume_state != 0 ||
              inst(terminal).kind != expected_terminal ||
              (edge.kind == CoroutineRegionEdgeKind::Success &&
               TypeId(inst(LowInstId(inst(terminal).arg0)).type) !=
                   plan.result_type) ||
              (edge.kind == CoroutineRegionEdgeKind::Error &&
               (!plan.error_type ||
                TypeId(inst(LowInstId(inst(terminal).arg0)).type) !=
                    *plan.error_type))) {
            error = "LowIR coroutine plan has an invalid terminal edge";
            return false;
          }
        } else if (edge.suspension.hasValue() || edge.resume_state != 0) {
          error = "LowIR coroutine plan has an invalid control-flow edge";
          return false;
        } else {
          actual_targets.push_back(edge.target);
        }
      }
      const auto low_terminal_kind = inst(terminal).kind;
      if ((low_terminal_kind == LowInstKind::CoroutineReturnSuccess ||
           low_terminal_kind == LowInstKind::CoroutineReturnError ||
           low_terminal_kind == LowInstKind::CoroutineReturnCancelled) &&
          region.exits.size() != 1) {
        error = "LowIR coroutine terminal region does not own one edge";
        return false;
      }
      std::vector<CoroutineRegionId> expected_targets;
      const auto append_target = [&](LowBlockId target_block) {
        const auto target_instructions = block(target_block);
        if (!target_instructions.empty())
          expected_targets.push_back(
              instruction_regions[target_instructions.front().index]);
      };
      if (inst(terminal).kind == LowInstKind::Branch) {
        append_target(LowBlockId(inst(terminal).arg0));
      } else if (inst(terminal).kind == LowInstKind::BranchIf) {
        const auto branch_targets = targets(TargetPairId(inst(terminal).arg1));
        append_target(branch_targets.true_block);
        append_target(branch_targets.false_block);
      }
      const auto normalize_regions = [](auto &values) {
        std::ranges::sort(values, {}, &CoroutineRegionId::index);
        values.erase(std::unique(values.begin(), values.end()), values.end());
      };
      normalize_regions(actual_targets);
      normalize_regions(expected_targets);
      if (inst(terminal).kind != LowInstKind::CallbackWakeWait &&
          inst(terminal).kind != LowInstKind::CoroutineTaskCompletionWait &&
          inst(terminal).kind != LowInstKind::CoroutineTaskCompletionCombine &&
          inst(terminal).kind != LowInstKind::CoroutineTaskGroupDrain &&
          inst(terminal).kind != LowInstKind::CoroutineTaskGroupErrorDrain &&
          inst(terminal).kind != LowInstKind::CoroutineTaskGroupCancelDrain &&
          actual_targets != expected_targets) {
        error = "LowIR coroutine region edges disagree with LowIR control flow";
        return false;
      }
      region_successors[region_index] = std::move(actual_targets);
    }
    for (std::size_t index = 0; index < plan.regions.size(); ++index) {
      const auto expected_entry =
          index == plan.initial_region.index ? CoroutineRegionEntryKind::Initial
          : resume_regions.contains(static_cast<std::uint32_t>(index))
              ? CoroutineRegionEntryKind::Resume
              : CoroutineRegionEntryKind::ControlFlow;
      if (plan.regions[index].entry_kind != expected_entry) {
        error = "LowIR coroutine plan has an invalid region entry kind";
        return false;
      }
    }

    struct RegionLiveness {
      std::vector<LowInstId> value_uses;
      std::vector<LowInstId> value_defs;
      std::vector<SlotId> place_uses;
      std::vector<SlotId> place_defs;
      std::vector<LowInstId> value_in;
      std::vector<LowInstId> value_out;
      std::vector<SlotId> place_in;
      std::vector<SlotId> place_out;
    };
    std::vector<RegionLiveness> region_liveness(plan.regions.size());
    const auto append_unique = [](auto &values, auto value) {
      if (std::ranges::find(values, value) == values.end())
        values.push_back(value);
    };
    const auto normalize_ids = [](auto &values) {
      std::ranges::sort(values, {}, &core::AnyId::index);
      values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    for (std::size_t region_index = 0; region_index < plan.regions.size();
         ++region_index) {
      auto &facts = region_liveness[region_index];
      const auto &segment =
          plan.segments[plan.regions[region_index].entry.index];
      const auto instructions = block(segment.block);
      for (std::size_t position = segment.begin; position < segment.end;
           ++position) {
        const auto id = instructions[position];
        const auto &instruction = inst(id);
        for (std::size_t argument = 0; argument != 2; ++argument) {
          const auto raw = argument == 0 ? instruction.arg0 : instruction.arg1;
          switch (lowInstArgKind(instruction.kind, argument)) {
          case LowArgKind::Value: {
            const auto value = LowInstId(raw);
            if (std::ranges::find(facts.value_defs, value) ==
                facts.value_defs.end())
              append_unique(facts.value_uses, value);
            break;
          }
          case LowArgKind::ValueBlock:
            for (const auto value : valueBlock(LowValueBlockId(raw)))
              if (std::ranges::find(facts.value_defs, value) ==
                  facts.value_defs.end())
                append_unique(facts.value_uses, value);
            break;
          case LowArgKind::Slot: {
            const auto slot_id = SlotId(raw);
            if (instruction.kind == LowInstKind::Initialize ||
                instruction.kind == LowInstKind::Transfer ||
                instruction.kind == LowInstKind::InitializeFromValue ||
                instruction.kind == LowInstKind::Parameter ||
                instruction.kind == LowInstKind::EndLifetime)
              append_unique(facts.place_defs, slot_id);
            else if (std::ranges::find(facts.place_defs, slot_id) ==
                     facts.place_defs.end())
              append_unique(facts.place_uses, slot_id);
            break;
          }
          case LowArgKind::Place: {
            const auto slot_id = place(LowPlaceId(raw)).root;
            if (instruction.kind == LowInstKind::InitializePlace ||
                instruction.kind == LowInstKind::InitializePlaceFromValue ||
                instruction.kind == LowInstKind::MarkMoved)
              append_unique(facts.place_defs, slot_id);
            else if (std::ranges::find(facts.place_defs, slot_id) ==
                     facts.place_defs.end())
              append_unique(facts.place_uses, slot_id);
            break;
          }
          default:
            break;
          }
        }
        append_unique(facts.value_defs, id);
      }
      normalize_ids(facts.value_uses);
      normalize_ids(facts.value_defs);
      normalize_ids(facts.place_uses);
      normalize_ids(facts.place_defs);
    }
    std::deque<CoroutineRegionId> liveness_worklist;
    std::vector<bool> liveness_queued(plan.regions.size(), true);
    for (std::size_t index = plan.regions.size(); index != 0; --index)
      liveness_worklist.emplace_back(static_cast<std::uint32_t>(index - 1));
    const auto merge_ids = [&](auto &result, const auto &values) {
      result.insert(result.end(), values.begin(), values.end());
      normalize_ids(result);
    };
    while (!liveness_worklist.empty()) {
      const auto region_id = liveness_worklist.front();
      liveness_worklist.pop_front();
      liveness_queued[region_id.index] = false;
      auto &facts = region_liveness[region_id.index];
      std::vector<LowInstId> value_out;
      std::vector<SlotId> place_out;
      for (const auto successor : region_successors[region_id.index]) {
        merge_ids(value_out, region_liveness[successor.index].value_in);
        merge_ids(place_out, region_liveness[successor.index].place_in);
      }
      auto value_in = value_out;
      for (const auto definition : facts.value_defs)
        std::erase(value_in, definition);
      merge_ids(value_in, facts.value_uses);
      auto place_in = place_out;
      for (const auto definition : facts.place_defs)
        std::erase(place_in, definition);
      merge_ids(place_in, facts.place_uses);
      if (facts.value_in != value_in || facts.value_out != value_out ||
          facts.place_in != place_in || facts.place_out != place_out) {
        facts.value_in = std::move(value_in);
        facts.value_out = std::move(value_out);
        facts.place_in = std::move(place_in);
        facts.place_out = std::move(place_out);
        for (std::size_t predecessor = 0;
             predecessor < region_successors.size(); ++predecessor)
          if (std::ranges::find(region_successors[predecessor], region_id) !=
                  region_successors[predecessor].end() &&
              !liveness_queued[predecessor]) {
            liveness_worklist.emplace_back(
                static_cast<std::uint32_t>(predecessor));
            liveness_queued[predecessor] = true;
          }
      }
    }
    for (std::size_t index = 0; index < plan.regions.size(); ++index) {
      const auto &region = plan.regions[index];
      const auto &facts = region_liveness[index];
      if (region.live_values_in != facts.value_in ||
          region.live_values_out != facts.value_out ||
          region.live_places_in != facts.place_in ||
          region.live_places_out != facts.place_out) {
        error = "LowIR coroutine region has invalid liveness facts";
        return false;
      }
    }
    for (const auto value : plan.frame_values)
      if (!function_instructions.contains(value.index) ||
          sem_ir_->type(TypeId(inst(value).type)).kind ==
              SemTypeKind::Reference ||
          sem_ir_->type(TypeId(inst(value).type)).kind == SemTypeKind::Void) {
        error = "LowIR coroutine frame has an invalid lifted value";
        return false;
      }
    std::unordered_set<std::uint32_t> semantic_states;
    std::vector<LowInstId> expected_frame_values;
    std::vector<SlotId> expected_lifted_slots;
    const auto validate_cleanup_graph = [&](CoroutineCleanupGraphId graph_id,
                                            InstId semantic_origin) {
      if (!graph_id.hasValue())
        return true;
      if (graph_id.index >= coroutine_cleanup_graphs_.size())
        return false;
      const auto &graph = coroutineCleanupGraph(graph_id);
      if (graph.function != plan.function ||
          graph.semantic_origin != semantic_origin ||
          std::ranges::any_of(graph.local_slots, [&](SlotId local) {
            return std::ranges::find(plan.lifted_slots, local) !=
                   plan.lifted_slots.end();
          }))
        return false;
      ++coroutine_cleanup_graph_uses[graph_id.index];
      return true;
    };
    const auto append_cleanup_slots = [&](CoroutineCleanupGraphId graph_id,
                                          std::vector<SlotId> &slots) {
      if (!graph_id.hasValue())
        return true;
      if (graph_id.index >= coroutine_cleanup_graphs_.size())
        return false;
      const auto &graph = coroutineCleanupGraph(graph_id);
      if (graph.function != plan.function || !graph.blocks.hasValue() ||
          graph.blocks.index >= block_lists_.size())
        return false;
      for (const auto block_id : blockList(graph.blocks)) {
        if (!block_id.hasValue() || block_id.index >= blockCount())
          return false;
        for (const auto instruction : block(block_id)) {
          if (!instruction.hasValue() || instruction.index >= instCount())
            return false;
          const auto &cleanup_inst = inst(instruction);
          if (cleanup_inst.kind == LowInstKind::Invalid ||
              cleanup_inst.kind == LowInstKind::Count)
            return false;
          for (std::size_t argument = 0; argument != 2; ++argument) {
            const auto raw =
                argument == 0 ? cleanup_inst.arg0 : cleanup_inst.arg1;
            const auto kind = lowInstArgKind(cleanup_inst.kind, argument);
            if (!containsArg(kind, raw))
              return false;
            const auto slot_id = kind == LowArgKind::Slot ? SlotId(raw)
                                 : kind == LowArgKind::Place
                                     ? place(LowPlaceId(raw)).root
                                     : SlotId::invalid();
            if (slot_id.hasValue())
              append_unique(slots, slot_id);
          }
        }
      }
      for (const auto local : graph.local_slots)
        std::erase(slots, local);
      return true;
    };
    for (const auto block_id : blockList(low_function->blocks))
      for (const auto instruction : block(block_id)) {
        if (inst(instruction).kind == LowInstKind::ParameterValue)
          expected_frame_values.push_back(instruction);
        else if (inst(instruction).kind == LowInstKind::Parameter)
          expected_lifted_slots.push_back(
              low_ir_.getAs<LowParameter>(instruction).arg0);
      }
    for (std::size_t index = 0; index < plan.resume_states.size(); ++index) {
      const auto &state = plan.resume_states[index];
      if (!validate_cleanup_graph(state.pre_commit_cleanup,
                                  state.semantic_suspension) ||
          !validate_cleanup_graph(state.transferred_cleanup,
                                  state.semantic_suspension) ||
          !append_cleanup_slots(state.transferred_cleanup,
                                expected_lifted_slots)) {
        error = "LowIR coroutine state has an invalid cleanup graph";
        return false;
      }
      const auto callback_wait =
          inst(state.suspension).kind == LowInstKind::CallbackWakeWait;
      const auto completion_wait = inst(state.suspension).kind ==
                                   LowInstKind::CoroutineTaskCompletionWait;
      const auto completion_set_wait =
          inst(state.suspension).kind ==
          LowInstKind::CoroutineTaskCompletionCombine;
      const auto task_group_drain =
          inst(state.suspension).kind == LowInstKind::CoroutineTaskGroupDrain ||
          inst(state.suspension).kind ==
              LowInstKind::CoroutineTaskGroupErrorDrain ||
          inst(state.suspension).kind ==
              LowInstKind::CoroutineTaskGroupCancelDrain;
      const auto expected_slot_kind =
          state.suspension_kind ==
                  CoroutineResumeState::SuspensionKind::CallbackWake
              ? SemTypeKind::CallbackWake
          : state.suspension_kind ==
                      CoroutineResumeState::SuspensionKind::TaskCompletion ||
                  state.suspension_kind ==
                      CoroutineResumeState::SuspensionKind::TaskGroupDrain
              ? SemTypeKind::CoroutineTaskCompletion
              : SemTypeKind::CoroutineTaskCompletionSet;
      const auto wake_slot_type = slot(state.wake_slot).type;
      const auto wake_slot_kind = sem_ir_->type(wake_slot_type).kind;
      const auto valid_wake_slot =
          wake_slot_kind == expected_slot_kind ||
          (state.suspension_kind ==
               CoroutineResumeState::SuspensionKind::CallbackWake &&
           wake_slot_kind == SemTypeKind::ForeignWake &&
           sem_ir_->type(sem_ir_
                             ->nominalType(NominalTypeId(
                                 sem_ir_->type(wake_slot_type).arg0))
                             .foreign_wake_storage_type)
                   .kind == SemTypeKind::CallbackWake);
      if (state.state != index + 1 ||
          (!task_group_drain &&
           !semantic_states.insert(state.semantic_suspension.index).second) ||
          !function_instructions.contains(state.suspension.index) ||
          (!callback_wait && !completion_wait && !completion_set_wait &&
           !task_group_drain) ||
          callback_wait !=
              (state.suspension_kind ==
               CoroutineResumeState::SuspensionKind::CallbackWake) ||
          completion_set_wait !=
              (state.suspension_kind ==
               CoroutineResumeState::SuspensionKind::TaskCompletionSet) ||
          task_group_drain !=
              (state.suspension_kind ==
               CoroutineResumeState::SuspensionKind::TaskGroupDrain) ||
          origin(state.suspension) != state.semantic_suspension ||
          (callback_wait && low_ir_.getAs<LowCallbackWakeWait>(state.suspension).arg0 !=
                                state.wake_plan) ||
          !state.suspension_region.hasValue() ||
          state.suspension_region.index >= plan.regions.size() ||
          !state.continuation_region.hasValue() ||
          state.continuation_region.index >= plan.regions.size() ||
          instruction_regions[state.suspension.index] !=
              state.suspension_region ||
          !edge_states.contains(state.state) ||
          std::ranges::find(plan.lifted_slots, state.wake_slot) ==
              plan.lifted_slots.end() ||
          !valid_wake_slot) {
        error = "LowIR coroutine frame has a duplicate or unreachable state";
        return false;
      }
      if (task_group_drain) {
        const auto drain_kind = inst(state.suspension).kind;
        const auto expected_exit_intent =
            drain_kind == LowInstKind::CoroutineTaskGroupDrain
                ? CoroutineTaskGroupExitIntent::Normal
            : drain_kind == LowInstKind::CoroutineTaskGroupErrorDrain
                ? CoroutineTaskGroupExitIntent::SelectedError
                : CoroutineTaskGroupExitIntent::SelectedCancellation;
        const auto normal_exit =
            expected_exit_intent == CoroutineTaskGroupExitIntent::Normal;
        const auto expected_cause_policy =
            normal_exit ? CoroutineCancellationCausePolicy::
                              OwnerRequestOrUnexpectedChild
                        : CoroutineCancellationCausePolicy::OwnerRequestOnly;
        const auto expected_child_policy =
            normal_exit
                ? CoroutineChildCancellationPolicy::EscalateUnexpected
                : CoroutineChildCancellationPolicy::PreserveSelectedExit;
        const auto operands = valueBlock(
            drain_kind == LowInstKind::CoroutineTaskGroupDrain
                ? low_ir_.getAs<LowCoroutineTaskGroupDrain>(state.suspension).arg0
            : drain_kind == LowInstKind::CoroutineTaskGroupErrorDrain
                ? low_ir_.getAs<LowCoroutineTaskGroupErrorDrain>(state.suspension).arg0
                : low_ir_.getAs<LowCoroutineTaskGroupCancelDrain>(state.suspension)
                      .arg0);
        if (!state.task_group.hasValue() || operands.size() != 2 ||
            operands[1] != state.task_group ||
            state.task_group_exit_intent != expected_exit_intent ||
            state.cancellation_cause_policy != expected_cause_policy ||
            state.child_cancellation_policy != expected_child_policy ||
            TypeId(inst(state.task_group).type) !=
                sem_ir_->coroutineScopeType() ||
            std::ranges::find(plan.frame_values, state.task_group) ==
                plan.frame_values.end()) {
          error = "LowIR coroutine task-group state has invalid ownership";
          return false;
        }
        expected_frame_values.push_back(state.task_group);
      } else if (state.task_group.hasValue() ||
                 state.task_group_exit_intent !=
                     CoroutineTaskGroupExitIntent::Normal ||
                 state.cancellation_cause_policy !=
                     CoroutineCancellationCausePolicy::OwnerRequestOnly ||
                 state.child_cancellation_policy !=
                     CoroutineChildCancellationPolicy::PreserveSelectedExit) {
        error = "LowIR ordinary coroutine state owns a task group";
        return false;
      }
      const auto has_live_task_group =
          std::ranges::any_of(state.live_values, [&](LowInstId live) {
            return live != state.task_group &&
                   TypeId(inst(live).type) == sem_ir_->coroutineScopeType();
          });
      const auto expected_acknowledgement =
          has_live_task_group
              ? CoroutineCancellationAcknowledgement::EnclosingTaskScope
              : CoroutineCancellationAcknowledgement::AtThisDrain;
      if (state.cancellation_acknowledgement != expected_acknowledgement) {
        error = "LowIR coroutine state has invalid deferred cancellation";
        return false;
      }
      if (state.result_value.hasValue() &&
          (state.result_value != state.suspension ||
           TypeId(inst(state.result_value).type) == sem_ir_->voidType() ||
           std::ranges::find(plan.frame_values, state.result_value) ==
               plan.frame_values.end())) {
        error = "LowIR coroutine state has an invalid suspension result";
        return false;
      }
      for (const auto value : state.live_values) {
        if (!function_instructions.contains(value.index) ||
            sem_ir_->type(TypeId(inst(value).type)).kind ==
                SemTypeKind::Reference ||
            sem_ir_->type(TypeId(inst(value).type)).kind == SemTypeKind::Void) {
          error = "LowIR coroutine state has an invalid live value";
          return false;
        }
        expected_frame_values.push_back(value);
      }
      if (state.result_value.hasValue())
        expected_frame_values.push_back(state.result_value);
      for (const auto slot_id : state.live_places) {
        if (!lifted.contains(slot_id.index) ||
            sem_ir_->type(slot(slot_id).type).kind == SemTypeKind::Reference) {
          error = "LowIR coroutine state has an invalid live slot";
          return false;
        }
        expected_lifted_slots.push_back(slot_id);
      }
      expected_lifted_slots.push_back(state.wake_slot);
      if (std::ranges::any_of(state.cleanup_order,
                              [&](CoroutineFramePlaceId frame_place) {
                                return frame_place.index >=
                                       plan.frame_places.size();
                              }) ||
          !std::ranges::is_sorted(state.cleanup_order, {},
                                  &CoroutineFramePlaceId::index)) {
        error = "LowIR coroutine state has an invalid cleanup order";
        return false;
      }
      std::vector<SlotId> expected_cleanup;
      if (!append_cleanup_slots(state.transferred_cleanup, expected_cleanup)) {
        error = "LowIR coroutine state has invalid cleanup ownership facts";
        return false;
      }
      append_unique(expected_cleanup, state.wake_slot);
      normalize_ids(expected_cleanup);
      std::vector<CoroutineFramePlaceId> expected_place_cleanup;
      for (std::size_t frame_place_index = 0;
           frame_place_index < plan.frame_places.size(); ++frame_place_index)
        if (std::ranges::find(
                expected_cleanup,
                place(plan.frame_places[frame_place_index].place).root) !=
            expected_cleanup.end())
          expected_place_cleanup.emplace_back(
              static_cast<std::uint32_t>(frame_place_index));
      if (state.cleanup_order != expected_place_cleanup) {
        error = "LowIR coroutine state has invalid cleanup ownership facts";
        return false;
      }
      const auto &suspension_region =
          plan.regions[state.suspension_region.index];
      auto persisted_region_values = suspension_region.live_values_out;
      std::erase_if(persisted_region_values, [&](LowInstId value) {
        return sem_ir_->type(TypeId(inst(value).type)).kind ==
                   SemTypeKind::Void ||
               value == state.result_value;
      });
      if (suspension_region.exits.size() != 1 ||
          suspension_region.exits.front().kind !=
              CoroutineRegionEdgeKind::Suspend ||
          suspension_region.exits.front().target != state.continuation_region ||
          suspension_region.exits.front().resume_state != state.state ||
          persisted_region_values != state.live_values ||
          suspension_region.live_places_out != state.live_places) {
        error = "LowIR coroutine state disagrees with its suspension region";
        return false;
      }
      const auto &semantic = sem_ir_->inst(state.semantic_suspension);
      const auto &continuation_region =
          plan.regions[state.continuation_region.index];
      const auto continuation_segment =
          plan.segments[continuation_region.entry.index];
      const auto continuation_instructions = block(continuation_segment.block);
      const auto semantic_kind_matches =
          state.suspension_kind ==
                  CoroutineResumeState::SuspensionKind::TaskGroupDrain
              ? semantic.kind == SemInstKind::CoroutineTaskScope
          : state.suspension_kind !=
                  CoroutineResumeState::SuspensionKind::TaskCompletionSet
              ? semantic.kind == SemInstKind::CoroutineSuspend
              : semantic.kind == SemInstKind::CoroutineTaskCompletionWaitAll ||
                    semantic.kind ==
                        SemInstKind::CoroutineTaskCompletionSelect ||
                    semantic.kind == SemInstKind::CoroutineTaskCompletionRace;
      if (!semantic_kind_matches || continuation_region.segments.empty() ||
          continuation_region.segments.front() != continuation_region.entry ||
          continuation_segment.begin >= continuation_segment.end ||
          continuation_segment.end > continuation_instructions.size() ||
          (completion_set_wait &&
           coroutineTaskCompletionCombinePlan(
               low_ir_.getAs<LowCoroutineTaskCompletionCombine>(state.suspension).arg0)
                   .continuation !=
               continuation_instructions[continuation_segment.begin]) ||
          (!task_group_drain &&
           sem_ir_->type(TypeId(sem_ir_->inst(InstId(semantic.arg0)).type))
                   .kind != expected_slot_kind)) {
        error = "LowIR coroutine state is not its planned checkpoint kind";
        return false;
      }
    }
    std::unordered_set<std::uint32_t> cancellation_instructions;
    if (!std::ranges::is_sorted(
            plan.cancellation_cleanups, {},
            [](const auto &cleanup) { return cleanup.instruction.index; })) {
      error = "LowIR coroutine cancellation cleanups are not sorted";
      return false;
    }
    for (const auto &cleanup : plan.cancellation_cleanups)
      if (!cleanup.instruction.hasValue() ||
          cleanup.instruction.index >= instCount() ||
          !function_instructions.contains(cleanup.instruction.index) ||
          !cancellation_instructions.insert(cleanup.instruction.index).second ||
          (inst(cleanup.instruction).kind !=
               LowInstKind::CoroutineCancellationCheck &&
           inst(cleanup.instruction).kind !=
               LowInstKind::CoroutineExecutorSwitch) ||
          !validate_cleanup_graph(cleanup.cleanup,
                                  origin(cleanup.instruction))) {
        error = "LowIR coroutine plan has an invalid cancellation cleanup";
        return false;
      }
    const auto normalize_expected = [](auto &values) {
      std::ranges::sort(values, {}, &core::AnyId::index);
      values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    normalize_expected(expected_frame_values);
    normalize_expected(expected_lifted_slots);
    if (plan.frame_values != expected_frame_values ||
        plan.lifted_slots != expected_lifted_slots) {
      const auto format_ids = [](const auto &values) {
        std::string text;
        for (const auto value : values) {
          if (!text.empty())
            text += ',';
          text += std::to_string(value.index);
        }
        return text;
      };
      error = "LowIR coroutine frame does not match cross-edge liveness "
              "(values actual=[" +
              format_ids(plan.frame_values) + "] expected=[" +
              format_ids(expected_frame_values) + "]; slots actual=[" +
              format_ids(plan.lifted_slots) + "] expected=[" +
              format_ids(expected_lifted_slots) + "])";
      return false;
    }
    for (std::uint32_t index = 0; index < sem_ir_->instCount(); ++index) {
      const auto &instruction = sem_ir_->inst(InstId(index));
      if (instruction.kind == SemInstKind::Call &&
          sem_ir_->functionRef(FunctionRefId(instruction.arg0))
                  .local_function == plan.function) {
        error = "LowIR coroutine scaffold remains ordinarily callable";
        return false;
      }
    }
  }
  for (std::size_t index = 0; index < coroutine_cleanup_graph_uses.size();
       ++index)
    if (coroutine_cleanup_graph_uses[index] != 1) {
      const auto &graph = coroutineCleanupGraph(
          CoroutineCleanupGraphId(static_cast<std::uint32_t>(index)));
      error = "LowIR coroutine cleanup graph " + std::to_string(index) +
              " has " + std::to_string(coroutine_cleanup_graph_uses[index]) +
              " owners instead of one (function " +
              std::to_string(graph.function.index) + ", origin " +
              std::to_string(graph.semantic_origin.index) + ")";
      return false;
    }
  return true;
}


} // namespace chtholly::compiler::internal
