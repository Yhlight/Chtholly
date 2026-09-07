// Included by PlaceState.cpp; this fragment retains the shared place-state
// stores and worklists without introducing a second analysis context.
SemPlaceId PlaceStateQuery::movePlace(InstId instruction) const {
  const auto found = move_places_.find(instruction.index);
  return found == move_places_.end() ? SemPlaceId::invalid() : found->second;
}

SemPlaceId PlaceStateQuery::copyPlace(InstId instruction) const {
  const auto found = copy_places_.find(instruction.index);
  return found == copy_places_.end() ? SemPlaceId::invalid() : found->second;
}

const PlaceCleanupPlan &
PlaceStateQuery::returnCleanup(InstId instruction) const {
  const auto found = return_cleanups_.find(instruction.index);
  return found == return_cleanups_.end() ? EmptyCleanupPlan : found->second;
}

const PlaceCleanupPlan &PlaceStateQuery::edgeCleanup(InstId instruction) const {
  const auto found = edge_cleanups_.find(instruction.index);
  return found == edge_cleanups_.end() ? EmptyCleanupPlan : found->second;
}

const PlaceCleanupPlan &
PlaceStateQuery::fullExpressionCleanup(InstId instruction) const {
  const auto found = full_expression_cleanups_.find(instruction.index);
  return found == full_expression_cleanups_.end() ? EmptyCleanupPlan
                                                  : found->second;
}

const PlaceCleanupPlan &PlaceStateQuery::blockCleanup(InstBlockId block) const {
  const auto found = block_cleanups_.find(block.index);
  return found == block_cleanups_.end() ? EmptyCleanupPlan : found->second;
}

const SuspensionCleanupPartition &
PlaceStateQuery::suspensionCleanup(InstId instruction) const {
  const auto found = suspension_cleanups_.find(instruction.index);
  return found == suspension_cleanups_.end() ? EmptySuspensionCleanupPartition
                                             : found->second;
}

const PlaceCleanupPlan &
PlaceStateQuery::cancellationCleanup(InstId instruction) const {
  const auto found = cancellation_cleanups_.find(instruction.index);
  return found == cancellation_cleanups_.end() ? EmptyCleanupPlan
                                               : found->second;
}

const PlaceReinitializationPlan &
PlaceStateQuery::reinitialization(InstId instruction) const {
  const auto found = reinitializations_.find(instruction.index);
  return found == reinitializations_.end() ? EmptyReinitializationPlan
                                           : found->second;
}

bool PlaceStateQuery::verify(const SemIR &sem_ir, std::string &error) const {
  const auto valid_place = [&](SemPlaceId id) {
    return id.hasValue() && id.index < places_.size() &&
           places_[id.index].root.index < sem_ir.localCount() &&
           places_[id.index].type.index < sem_ir.typeCount();
  };
  for (const auto &[instruction, place] : move_places_)
    if (instruction >= sem_ir.instCount() ||
        sem_ir.inst(InstId(instruction)).kind != SemInstKind::Move ||
        !valid_place(place)) {
      error = "place-state query has an invalid move mapping";
      return false;
    }
  for (const auto &[instruction, place] : copy_places_)
    if (instruction >= sem_ir.instCount() ||
        sem_ir.inst(InstId(instruction)).kind != SemInstKind::Copy ||
        !valid_place(place)) {
      error = "place-state query has an invalid copy mapping";
      return false;
    }
  std::unordered_set<std::uint32_t> executable_moves;
  std::unordered_set<std::uint32_t> executable_copies;
  std::unordered_set<std::uint32_t> executable_returns;
  std::unordered_set<std::uint32_t> executable_edges;
  std::unordered_set<std::uint32_t> executable_full_expressions;
  std::unordered_set<std::uint32_t> executable_materializations;
  std::unordered_set<std::uint32_t> executable_extensions;
  std::unordered_set<std::uint32_t> executable_assignments;
  std::unordered_set<std::uint32_t> executable_suspensions;
  std::unordered_set<std::uint32_t> executable_cancellations;
  std::unordered_set<std::uint32_t> visited_blocks;
  const auto collect_block = [&](auto &&self, InstBlockId block) -> void {
    if (!visited_blocks.insert(block.index).second)
      return;
    for (const auto instruction : sem_ir.instBlock(block)) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind == SemInstKind::Move)
        executable_moves.insert(instruction.index);
      else if (inst.kind == SemInstKind::Copy)
        executable_copies.insert(instruction.index);
      else if (inst.kind == SemInstKind::Return ||
               inst.kind == SemInstKind::CoroutineReturnSuccess ||
               inst.kind == SemInstKind::CoroutineReturnError ||
               inst.kind == SemInstKind::CoroutineReturnCancelled)
        executable_returns.insert(instruction.index);
      else if (inst.kind == SemInstKind::Break ||
               inst.kind == SemInstKind::Continue)
        executable_edges.insert(instruction.index);
      else if (inst.kind == SemInstKind::EndFullExpression)
        executable_full_expressions.insert(instruction.index);
      else if (inst.kind == SemInstKind::MaterializeTemporary)
        executable_materializations.insert(instruction.index);
      else if (inst.kind == SemInstKind::ExtendTemporary)
        executable_extensions.insert(instruction.index);
      else if (inst.kind == SemInstKind::Assign ||
               inst.kind == SemInstKind::Placement)
        executable_assignments.insert(instruction.index);
      else if (isCoroutineSuspension(inst.kind))
        executable_suspensions.insert(instruction.index);
      else if (inst.kind == SemInstKind::CoroutineCancellationCheck ||
               inst.kind == SemInstKind::CoroutineExecutorSwitch)
        executable_cancellations.insert(instruction.index);
      else if (inst.kind == SemInstKind::ScopedBlock)
        self(self, InstBlockId(inst.arg0));
      else if (inst.kind == SemInstKind::If) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          const auto &arm = sem_ir.inst(arm_id);
          if (arm.kind == SemInstKind::IfArm)
            self(self, InstBlockId(arm.arg0));
        }
      } else if (inst.kind == SemInstKind::While) {
        self(self, InstBlockId(inst.arg0));
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::For) {
        for (const auto clause_id : sem_ir.instBlock(InstBlockId(inst.arg0))) {
          const auto &clause = sem_ir.inst(clause_id);
          if (clause.kind == SemInstKind::ForClause)
            self(self, InstBlockId(clause.arg1));
        }
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::DoWhile) {
        self(self, InstBlockId(inst.arg0));
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::Switch) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          const auto &arm = sem_ir.inst(arm_id);
          if (arm.kind == SemInstKind::SwitchArm)
            self(self, InstBlockId(arm.arg1));
        }
      } else if (inst.kind == SemInstKind::Defer) {
        self(self, InstBlockId(inst.arg0));
      } else if (inst.kind == SemInstKind::CoroutineTaskScope) {
        self(self, InstBlockId(inst.arg0));
      }
    }
  };
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    const auto &function = sem_ir.function(FunctionId(index));
    if ((function.flags & SemFunctionTemplate) == 0)
      collect_block(collect_block, function.body);
  }
  if (executable_moves.size() != move_places_.size() ||
      std::ranges::any_of(executable_moves, [&](std::uint32_t instruction) {
        return !move_places_.contains(instruction);
      })) {
    error = "place-state query omits an executable move";
    return false;
  }
  if (executable_copies.size() != copy_places_.size() ||
      std::ranges::any_of(executable_copies, [&](std::uint32_t instruction) {
        return !copy_places_.contains(instruction);
      })) {
    error = "place-state query omits an executable copy";
    return false;
  }
  const auto valid_plan = [&](const PlaceCleanupPlan &plan) {
    return std::ranges::all_of(plan.actions, [&](const auto &action) {
      switch (action.kind) {
      case PlaceCleanupKind::Destroy:
      case PlaceCleanupKind::DestroyIfInitialized:
        return valid_place(action.place);
      case PlaceCleanupKind::RunDefer:
        return action.block.hasValue() &&
               action.block.index < sem_ir.instBlockCount();
      case PlaceCleanupKind::EndLifetime:
        return action.local.hasValue() &&
               action.local.index < sem_ir.localCount();
      }
      return false;
    });
  };
  for (const auto &[instruction, plan] : return_cleanups_)
    if (instruction >= sem_ir.instCount() ||
        (sem_ir.inst(InstId(instruction)).kind != SemInstKind::Return &&
         sem_ir.inst(InstId(instruction)).kind !=
             SemInstKind::CoroutineReturnSuccess &&
         sem_ir.inst(InstId(instruction)).kind !=
             SemInstKind::CoroutineReturnError &&
         sem_ir.inst(InstId(instruction)).kind !=
             SemInstKind::CoroutineReturnCancelled) ||
        !valid_plan(plan)) {
      error = "place-state query has an invalid return cleanup plan";
      return false;
    }
  if (executable_returns.size() != return_cleanups_.size() ||
      std::ranges::any_of(executable_returns, [&](std::uint32_t instruction) {
        return !return_cleanups_.contains(instruction);
      })) {
    error = "place-state query omits an executable return cleanup";
    return false;
  }
  for (const auto &[instruction, plan] : edge_cleanups_)
    if (!executable_edges.contains(instruction) || !valid_plan(plan)) {
      error = "place-state query has an invalid control-flow edge cleanup";
      return false;
    }
  if (executable_edges.size() != edge_cleanups_.size() ||
      std::ranges::any_of(executable_edges, [&](std::uint32_t instruction) {
        return !edge_cleanups_.contains(instruction);
      })) {
    error = "place-state query omits an executable control-flow edge cleanup";
    return false;
  }
  for (const auto &[instruction, plan] : full_expression_cleanups_)
    if (!executable_full_expressions.contains(instruction) ||
        !valid_plan(plan)) {
      error = "place-state query has an invalid full-expression cleanup";
      return false;
    }
  if (executable_full_expressions.size() != full_expression_cleanups_.size() ||
      std::ranges::any_of(
          executable_full_expressions, [&](std::uint32_t instruction) {
            return !full_expression_cleanups_.contains(instruction);
          })) {
    error = "place-state query omits an executable full-expression cleanup";
    return false;
  }
  std::unordered_map<std::uint32_t, std::uint32_t> boundary_owners;
  for (const auto boundary : executable_full_expressions)
    for (const auto temporary :
         sem_ir.instBlock(InstBlockId(sem_ir.inst(InstId(boundary)).arg0))) {
      const auto [owner, inserted] =
          boundary_owners.emplace(temporary.index, boundary);
      if (!executable_materializations.contains(temporary.index) || !inserted) {
        error = "full-expression boundary " + std::to_string(boundary) +
                " does not uniquely own temporary " +
                std::to_string(temporary.index) + " already owned by " +
                std::to_string(owner->second);
        return false;
      }
    }
  if (boundary_owners.size() != executable_materializations.size()) {
    error = "an executable temporary has no full-expression boundary";
    return false;
  }
  std::unordered_set<std::uint32_t> extended_temporaries;
  for (const auto extension : executable_extensions) {
    const auto temporary = sem_ir.inst(InstId(extension)).arg1;
    if (!executable_materializations.contains(temporary) ||
        !extended_temporaries.insert(temporary).second) {
      error = "temporary lifetime extension has invalid ownership transfer";
      return false;
    }
  }
  for (const auto &[block, plan] : block_cleanups_)
    if (!valid_plan(plan)) {
      (void)block;
      error = "place-state query has an invalid block cleanup plan";
      return false;
    }
  for (const auto &[instruction, partition] : suspension_cleanups_)
    if (!executable_suspensions.contains(instruction) ||
        !valid_plan(partition.pre_commit) ||
        !valid_plan(partition.transferred)) {
      error = "place-state query has an invalid suspension cleanup partition";
      return false;
    }
  if (executable_suspensions.size() != suspension_cleanups_.size() ||
      std::ranges::any_of(executable_suspensions,
                          [&](std::uint32_t instruction) {
                            return !suspension_cleanups_.contains(instruction);
                          })) {
    error = "place-state query omits an executable suspension cleanup";
    return false;
  }
  for (const auto &[instruction, plan] : cancellation_cleanups_)
    if (!executable_cancellations.contains(instruction) || !valid_plan(plan)) {
      error = "place-state query has an invalid cancellation cleanup";
      return false;
    }
  if (executable_cancellations.size() != cancellation_cleanups_.size() ||
      std::ranges::any_of(
          executable_cancellations, [&](std::uint32_t instruction) {
            return !cancellation_cleanups_.contains(instruction);
          })) {
    error = "place-state query omits an executable cancellation cleanup";
    return false;
  }
  for (const auto &[instruction, plan] : reinitializations_)
    if (!executable_assignments.contains(instruction) ||
        !valid_place(plan.target) ||
        !std::ranges::all_of(plan.old_value_cleanups, [&](const auto &action) {
          return valid_place(action.place);
        })) {
      error = "place-state query has an invalid reinitialization plan";
      return false;
    }
  if (executable_assignments.size() != reinitializations_.size()) {
    error = "place-state query omits an executable assignment";
    return false;
  }
  for (const auto &observation : observations_)
    if (observation.instruction.index >= sem_ir.instCount() ||
        !valid_place(observation.place)) {
      error = "place-state query has an invalid observation";
      return false;
    }
  return true;
}
