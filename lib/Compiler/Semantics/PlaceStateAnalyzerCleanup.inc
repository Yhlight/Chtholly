// Included inside PlaceStateAnalyzer; this fragment aliases its existing
// place-state maps, cleanup plans, and worklists.
  void diagnoseUse(InstId instruction, const ResolvedPlace &place,
                   PlaceStateKind state) {
    auto kind = PlaceStateDiagnosticKind::UseAfterMove;
    switch (state) {
    case PlaceStateKind::Initialized:
      return;
    case PlaceStateKind::Moved:
      kind = (sem_ir_.local(place.root).flags & SemLocalUninitialized) != 0 &&
                     !ensureRoot(place.root).ever_initialized
                 ? PlaceStateDiagnosticKind::UninitializedStorage
                 : PlaceStateDiagnosticKind::UseAfterMove;
      break;
    case PlaceStateKind::MaybeMoved:
      kind = PlaceStateDiagnosticKind::UseOfMaybeMovedPlace;
      break;
    case PlaceStateKind::PartiallyMoved:
      kind = PlaceStateDiagnosticKind::UseOfPartiallyMovedPlace;
      break;
    }
    diagnostics_.push_back({instruction, kind});
  }

  void observe(InstId instruction, const ResolvedPlace &place,
               PlaceObservationKind kind) {
    if (kind == PlaceObservationKind::Move || kind == PlaceObservationKind::Reinitialize)
      ++place_revisions_[place.root.index];
    const auto id = internPlace(place);
    const auto before = stateFor(place);
    diagnoseUse(instruction, place, before);
    if (before == PlaceStateKind::Initialized)
      (void)(checkUnionAccess(instruction, place) &&
             checkEnumAccess(instruction, place));
    auto after = before;
    if (kind == PlaceObservationKind::Move &&
        before == PlaceStateKind::Initialized) {
      auto &root = ensureRoot(place.root);
      for (auto &leaf : root.leaves)
        if (isPrefix(place.path, leaf.path) ||
            (sem_ir_.type(leaf.type).kind == SemTypeKind::Nominal &&
             sem_ir_.nominalType(NominalTypeId(sem_ir_.type(leaf.type).arg0))
                     .kind == NominalKind::Union &&
             isPrefix(leaf.path, place.path)))
          leaf.possibilities = MovedBit;
      for (auto &union_state : root.unions)
        if (isPrefix(place.path, union_state.path) ||
            isPrefix(union_state.path, place.path)) {
          std::ranges::fill(union_state.possible_members, 0U);
          union_state.unknown = false;
        }
      after = stateFor(place);
      move_places_.emplace(instruction.index, id);
    }
    observations_.push_back({instruction, id, kind, before, after});
  }

  void observeExternal(InstId instruction, const ResolvedPlace &place,
                       PlaceObservationKind kind) {
    const auto id = internPlace(place);
    if (kind == PlaceObservationKind::Move)
      move_places_.emplace(instruction.index, id);
    if (kind != PlaceObservationKind::Reinitialize)
      diagnoseUse(instruction, place, stateFor(place));
    observations_.push_back({instruction, id, kind, PlaceStateKind::Initialized,
                             PlaceStateKind::Initialized});
  }

  void observeCallEffect(InstId instruction, const ResolvedPlace &place,
                         PlaceObservationKind kind) {
    if (kind == PlaceObservationKind::Move || kind == PlaceObservationKind::Reinitialize)
      ++place_revisions_[place.root.index];
    if (isExternal(place)) {
      observeExternal(instruction, place, kind);
      return;
    }
    const auto before = stateFor(place);
    if (kind != PlaceObservationKind::Reinitialize)
      diagnoseUse(instruction, place, before);
    observations_.push_back(
        {instruction, internPlace(place), kind, before, before});
  }

  static bool isExternal(const ResolvedPlace &place) {
    return std::ranges::find(place.path, PlaceProjectionKind::Dereference,
                             &PlaceProjection::kind) != place.path.end();
  }

  void observeDirectPlace(InstId instruction) {
    if (projection_inputs_.contains(instruction.index))
      return;
    if (const auto place = resolvePlace(instruction); place) {
      const auto external = isExternal(*place);
      if (external)
        observeExternal(instruction, *place, PlaceObservationKind::Read);
      else
        observe(instruction, *place, PlaceObservationKind::Read);
    }
  }

  void appendLocalCleanup(PlaceCleanupPlan &plan, LocalId local) {
    const auto representation =
        sem_ir_.typeRepresentation(sem_ir_.local(local).type);
    if (representation.ownership == OwnershipReprKind::Owned) {
      auto &root = ensureRoot(local);
      const ResolvedPlace root_place{local, root.type, {}};
      const auto root_state = stateFor(root_place);
      if (root_state == PlaceStateKind::Initialized) {
        if (representation.destroy != DestroyReprKind::None)
          plan.actions.push_back(
              {internPlace(root_place), PlaceCleanupKind::Destroy});
      } else if (root_state == PlaceStateKind::MaybeMoved) {
        if (representation.destroy != DestroyReprKind::None)
          plan.actions.push_back({internPlace(root_place),
                                  PlaceCleanupKind::DestroyIfInitialized});
      } else if (root_state == PlaceStateKind::PartiallyMoved) {
        for (const auto &leaf : root.leaves) {
          if ((leaf.possibilities & InitializedBit) == 0)
            continue;
          bool active_payload = true;
          for (const auto &enum_state : root.enums) {
            if (!isPrefix(enum_state.path, leaf.path) ||
                enum_state.path.size() >= leaf.path.size())
              continue;
            const auto &step = leaf.path[enum_state.path.size()];
            if (step.kind == PlaceProjectionKind::EnumPayload &&
                (step.variant >= enum_state.possible_variants.size() ||
                 !enum_state.possible_variants[step.variant])) {
              active_payload = false;
              break;
            }
          }
          if (!active_payload)
            continue;
          const auto leaf_representation =
              sem_ir_.typeRepresentation(leaf.type);
          if (leaf_representation.ownership != OwnershipReprKind::Owned ||
              leaf_representation.destroy == DestroyReprKind::None)
            continue;
          const ResolvedPlace leaf_place{local, leaf.type, leaf.path};
          plan.actions.push_back(
              {internPlace(leaf_place),
               leaf.possibilities == (InitializedBit | MovedBit)
                   ? PlaceCleanupKind::DestroyIfInitialized
                   : PlaceCleanupKind::Destroy});
        }
      }
      plan.actions.push_back({SemPlaceId::invalid(),
                              PlaceCleanupKind::EndLifetime,
                              InstBlockId::invalid(), local});
    }
  }

  PlaceCleanupPlan
  cleanupPlan(std::span<const CleanupRegistration> registrations) {
    PlaceCleanupPlan plan;
    const std::vector pending(registrations.begin(), registrations.end());
    for (auto position = pending.rbegin(); position != pending.rend();
         ++position) {
      if (position->kind != CleanupRegistration::Kind::Defer) {
        appendLocalCleanup(plan, position->local);
      } else {
        plan.actions.push_back({SemPlaceId::invalid(),
                                PlaceCleanupKind::RunDefer, position->block});
        (void)analyzeBlock(position->block, true);
      }
    }
    return plan;
  }

  PlaceCleanupPlan
  snapshotCleanupPlan(std::span<const CleanupRegistration> registrations) {
    const auto states = states_;
    const auto active_locals = active_locals_;
    const auto cleanup_registrations = cleanup_registrations_;
    const auto bound_values = bound_values_;
    const auto task_value_obligations = task_value_obligations_;
    const auto task_value_scopes = task_value_scopes_;
    const auto active_task_scopes = active_task_scopes_;
    auto plan = cleanupPlan(registrations);
    states_ = states;
    active_locals_ = active_locals;
    cleanup_registrations_ = cleanup_registrations;
    bound_values_ = bound_values;
    task_value_obligations_ = task_value_obligations;
    task_value_scopes_ = task_value_scopes;
    active_task_scopes_ = active_task_scopes;
    return plan;
  }

  void eraseLocals(std::span<const LocalId> locals) {
    for (const auto local : locals)
      states_.erase(local.index);
  }

  void diagnoseOutstandingTasks(InstId instruction,
                                std::span<const LocalId> locals) {
    for (const auto local : locals) {
      const auto found = states_.find(local.index);
      if (found != states_.end() &&
          (found->second.task_obligation & TaskOutstandingBit) != 0 &&
          !(found->second.task_scope != 0 &&
            !sem_ir_.coroutineTaskErrorType(found->second.type)))
        diagnostics_.push_back(
            {instruction, PlaceStateDiagnosticKind::TaskNotConsumed});
    }
  }

  void diagnoseUninitializedStorage(std::span<const LocalId> locals) {
    for (const auto local : locals) {
      const auto found = states_.find(local.index);
      if (found != states_.end() &&
          (sem_ir_.local(local).flags & SemLocalUninitialized) != 0 &&
          !found->second.ever_initialized &&
          !(initialization_attempts_.contains(local.index) &&
            stateFor(ResolvedPlace{local, found->second.type, {}}) == PlaceStateKind::Moved) &&
          found->second.uninitialized_declaration.hasValue())
        diagnostics_.push_back(
            {found->second.uninitialized_declaration,
             PlaceStateDiagnosticKind::UninitializedStorage});
    }
  }

  void mergeStates(const std::unordered_map<std::uint32_t, RootState> &before,
                   const std::unordered_map<std::uint32_t, RootState> &after) {
    states_ = before;
    for (auto &[local, state] : states_) {
      const auto found = after.find(local);
      if (found == after.end())
        continue;
      state.task_obligation |= found->second.task_obligation;
      state.ever_initialized =
          state.ever_initialized && found->second.ever_initialized;
      if (state.task_scope != found->second.task_scope)
        state.task_scope = 0;
      assert(state.leaves.size() == found->second.leaves.size());
      for (std::size_t index = 0; index < state.leaves.size(); ++index)
        state.leaves[index].possibilities |=
            found->second.leaves[index].possibilities;
      assert(state.unions.size() == found->second.unions.size());
      for (std::size_t index = 0; index < state.unions.size(); ++index) {
        auto &target = state.unions[index];
        const auto &source = found->second.unions[index];
        target.unknown |= source.unknown;
        assert(target.possible_members.size() ==
               source.possible_members.size());
        for (std::size_t member = 0; member < target.possible_members.size();
             ++member)
          target.possible_members[member] |= source.possible_members[member];
      }
      assert(state.enums.size() == found->second.enums.size());
      for (std::size_t index = 0; index < state.enums.size(); ++index) {
        auto &target = state.enums[index];
        const auto &source = found->second.enums[index];
        target.unknown |= source.unknown;
        assert(target.possible_variants.size() ==
               source.possible_variants.size());
        for (std::size_t variant = 0; variant < target.possible_variants.size();
             ++variant)
          target.possible_variants[variant] |=
              source.possible_variants[variant];
      }
    }
  }

  using StateMap = std::unordered_map<std::uint32_t, RootState>;

  StateMap statesAtDepth(std::size_t depth) const {
    auto result = states_;
    for (std::size_t index = depth; index < active_locals_.size(); ++index)
      result.erase(active_locals_[index].index);
    return result;
  }

  static void mergeOptionalState(std::optional<StateMap> &target,
                                 StateMap source,
                                 PlaceStateAnalyzer &analyzer) {
    if (!target) {
      target = std::move(source);
      return;
    }
    const auto current = std::move(*target);
    analyzer.states_ = current;
    analyzer.mergeStates(current, source);
    *target = analyzer.states_;
  }

  struct LoopFlow {
    std::size_t break_local_depth = 0;
    std::size_t continue_local_depth = 0;
    std::size_t break_cleanup_depth = 0;
    std::size_t continue_cleanup_depth = 0;
    std::optional<StateMap> breaks;
    std::optional<StateMap> continues;
  };

  static bool
  sameStates(const std::unordered_map<std::uint32_t, RootState> &lhs,
             const std::unordered_map<std::uint32_t, RootState> &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &[local, state] : lhs) {
      const auto found = rhs.find(local);
      if (found == rhs.end() ||
          state.leaves.size() != found->second.leaves.size() ||
          state.ever_initialized != found->second.ever_initialized ||
          state.task_obligation != found->second.task_obligation ||
          state.task_scope != found->second.task_scope)
        return false;
      for (std::size_t index = 0; index < state.leaves.size(); ++index)
        if (state.leaves[index].possibilities !=
            found->second.leaves[index].possibilities)
          return false;
      if (state.unions.size() != found->second.unions.size())
        return false;
      for (std::size_t index = 0; index < state.unions.size(); ++index)
        if (state.unions[index].unknown !=
                found->second.unions[index].unknown ||
            state.unions[index].possible_members !=
                found->second.unions[index].possible_members)
          return false;
      if (state.enums.size() != found->second.enums.size())
        return false;
      for (std::size_t index = 0; index < state.enums.size(); ++index)
        if (state.enums[index].unknown != found->second.enums[index].unknown ||
            state.enums[index].possible_variants !=
                found->second.enums[index].possible_variants)
          return false;
    }
    return true;
  }

  void reinitialize(InstId instruction, const ResolvedPlace &place) {
    ++place_revisions_[place.root.index];
    auto &root = ensureRoot(place.root);
    PlaceReinitializationPlan plan{internPlace(place), {}};
    const auto state = stateFor(place);
    const auto representation = sem_ir_.typeRepresentation(place.type);
    if (state == PlaceStateKind::Initialized &&
        representation.destroy != DestroyReprKind::None) {
      plan.old_value_cleanups.push_back(
          {plan.target, PlaceCleanupKind::Destroy});
    } else if (state == PlaceStateKind::MaybeMoved &&
               representation.destroy != DestroyReprKind::None) {
      plan.old_value_cleanups.push_back(
          {plan.target, PlaceCleanupKind::DestroyIfInitialized});
    } else if (state == PlaceStateKind::PartiallyMoved) {
      for (const auto &leaf : root.leaves) {
        if (!isPrefix(place.path, leaf.path) ||
            (leaf.possibilities & InitializedBit) == 0)
          continue;
        const auto leaf_representation = sem_ir_.typeRepresentation(leaf.type);
        if (leaf_representation.destroy == DestroyReprKind::None)
          continue;
        plan.old_value_cleanups.push_back(
            {internPlace({place.root, leaf.type, leaf.path}),
             leaf.possibilities == (InitializedBit | MovedBit)
                 ? PlaceCleanupKind::DestroyIfInitialized
                 : PlaceCleanupKind::Destroy});
      }
    }
    const auto before = stateFor(place);
    for (auto &leaf : root.leaves)
      if (isPrefix(place.path, leaf.path) ||
          (sem_ir_.type(leaf.type).kind == SemTypeKind::Nominal &&
           sem_ir_.nominalType(NominalTypeId(sem_ir_.type(leaf.type).arg0))
                   .kind == NominalKind::Union &&
           isPrefix(leaf.path, place.path)))
        leaf.possibilities = InitializedBit;
    for (auto &union_state : root.unions) {
      const auto contains_target = isPrefix(union_state.path, place.path);
      const auto inside_target = isPrefix(place.path, union_state.path);
      if (!contains_target && !inside_target)
        continue;
      if (inside_target) {
        std::ranges::fill(union_state.possible_members, 0U);
        union_state.unknown = true;
      }
      if (contains_target && union_state.path.size() < place.path.size() &&
          place.path[union_state.path.size()].kind ==
              PlaceProjectionKind::Field) {
        const auto member = place.path[union_state.path.size()].index;
        if (place.path.size() > union_state.path.size() + 1)
          (void)checkUnionAccess(instruction, place);
        else if (member < union_state.possible_members.size()) {
          std::ranges::fill(union_state.possible_members, 0U);
          union_state.possible_members[member] = 1;
          union_state.unknown = false;
        }
      }
    }
    const auto &assign = sem_ir_.inst(instruction);
    setUnionStatesFromValue(root, place.path, InstId(assign.arg1));
    if (place.path.empty() &&
        sem_ir_.type(root.type).kind == SemTypeKind::CoroutineTask) {
      const auto obligation = task_value_obligations_.find(assign.arg1);
      root.task_obligation = obligation == task_value_obligations_.end()
                                 ? TaskOutstandingBit
                                 : obligation->second;
      root.task_scope = taskScopeForValue(InstId(assign.arg1));
      if (root.task_scope != 0) {
        const auto scope = std::ranges::find(
            active_task_scopes_, root.task_scope, &ActiveTaskScope::id);
        const auto local = std::ranges::find(active_locals_, place.root);
        const auto target_is_outside =
            scope == active_task_scopes_.end() ||
            local == active_locals_.end() ||
            static_cast<std::size_t>(local - active_locals_.begin()) <
                scope->local_depth;
        if (target_is_outside)
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::TaskScopeEscape});
      }
    }
    reinitializations_[instruction.index] = std::move(plan);
    observations_.push_back({instruction, internPlace(place),
                             PlaceObservationKind::Reinitialize, before,
                             stateFor(place)});
  }

  void setPlacePossibilities(const ResolvedPlace &place,
                             std::uint8_t possibilities) {
    auto &root = ensureRoot(place.root);
    for (auto &leaf : root.leaves)
      if (isPrefix(place.path, leaf.path) ||
          (sem_ir_.type(leaf.type).kind == SemTypeKind::Nominal &&
           sem_ir_.nominalType(NominalTypeId(sem_ir_.type(leaf.type).arg0))
                   .kind == NominalKind::Union &&
           isPrefix(leaf.path, place.path)))
        leaf.possibilities = possibilities;
    for (auto &union_state : root.unions)
      if (isPrefix(place.path, union_state.path) ||
          isPrefix(union_state.path, place.path)) {
        std::ranges::fill(union_state.possible_members, 0U);
        union_state.unknown = (possibilities & InitializedBit) != 0;
      }
    for (auto &enum_state : root.enums)
      if (isPrefix(place.path, enum_state.path) ||
          isPrefix(enum_state.path, place.path)) {
        std::ranges::fill(enum_state.possible_variants,
                          (possibilities & InitializedBit) != 0 ? 1U : 0U);
        enum_state.unknown = (possibilities & InitializedBit) != 0;
      }
  }

