// Included inside PlaceStateAnalyzer; this fragment aliases its existing
// place-state maps, cleanup plans, and worklists.
  void analyzeWhile(InstBlockId condition, InstBlockId body) {
    const auto preheader = states_;
    const auto active_before = active_locals_;
    const auto cleanups_before = cleanup_registrations_;
    auto header = preheader;
    for (;;) {
      AnalysisCheckpoint checkpoint(*this);
      states_ = header;
      active_locals_ = active_before;
      cleanup_registrations_ = cleanups_before;
      (void)analyzeBlock(condition, false);
      const auto exit_state = states_;
      loop_flows_.push_back({active_before.size(), active_before.size(),
                             cleanups_before.size(), cleanups_before.size()});
      const auto body_falls_through = analyzeBlock(body, true);
      const auto backedge = states_;
      auto flow = std::move(loop_flows_.back());
      loop_flows_.pop_back();
      states_ = preheader;
      if (body_falls_through)
        mergeStates(preheader, backedge);
      if (flow.continues) {
        const auto current = states_;
        mergeStates(current, *flow.continues);
      }
      const auto next_header = states_;
      if (sameStates(header, next_header)) {
        states_ = exit_state;
        if (flow.breaks) {
          const auto natural_exit = states_;
          mergeStates(natural_exit, *flow.breaks);
        }
        active_locals_ = active_before;
        cleanup_registrations_ = cleanups_before;
        checkpoint.commit();
        break;
      }
      header = next_header;
    }
  }

  bool analyzeDoWhile(InstBlockId condition, InstBlockId body) {
    const auto preheader = states_;
    const auto active_before = active_locals_;
    const auto cleanups_before = cleanup_registrations_;
    auto header = preheader;
    for (;;) {
      AnalysisCheckpoint checkpoint(*this);
      states_ = header;
      active_locals_ = active_before;
      cleanup_registrations_ = cleanups_before;
      loop_flows_.push_back({active_before.size(), active_before.size(),
                             cleanups_before.size(), cleanups_before.size()});
      const auto body_falls_through = analyzeBlock(body, true);
      auto flow = std::move(loop_flows_.back());
      loop_flows_.pop_back();
      std::optional<StateMap> condition_entry;
      if (body_falls_through)
        condition_entry = states_;
      if (flow.continues)
        mergeOptionalState(condition_entry, std::move(*flow.continues), *this);
      std::optional<StateMap> exit;
      std::optional<StateMap> backedge;
      if (condition_entry) {
        states_ = *condition_entry;
        active_locals_ = active_before;
        cleanup_registrations_ = cleanups_before;
        (void)analyzeBlock(condition, false);
        exit = states_;
        backedge = states_;
      }
      states_ = preheader;
      if (backedge)
        mergeStates(preheader, *backedge);
      const auto next_header = states_;
      if (sameStates(header, next_header)) {
        if (flow.breaks)
          mergeOptionalState(exit, std::move(*flow.breaks), *this);
        states_ = exit.value_or(preheader);
        active_locals_ = active_before;
        cleanup_registrations_ = cleanups_before;
        checkpoint.commit();
        return exit.has_value();
      }
      header = next_header;
    }
  }

  void analyzeFor(const SemInst &for_inst) {
    const auto clauses = sem_ir_.instBlock(InstBlockId(for_inst.arg0));
    assert(clauses.size() == 3);
    const auto init = InstBlockId(sem_ir_.inst(clauses[0]).arg1);
    const auto condition = InstBlockId(sem_ir_.inst(clauses[1]).arg1);
    const auto step = InstBlockId(sem_ir_.inst(clauses[2]).arg1);
    const auto body = InstBlockId(for_inst.arg1);
    const auto outer_depth = active_locals_.size();
    const auto outer_cleanup_depth = cleanup_registrations_.size();
    (void)analyzeBlock(init, false);
    const auto loop_depth = active_locals_.size();
    const auto loop_cleanup_depth = cleanup_registrations_.size();
    const auto preheader = states_;
    const auto active_loop = active_locals_;
    const auto cleanup_loop = cleanup_registrations_;
    auto header = preheader;
    std::optional<StateMap> final_breaks;
    StateMap final_exit;
    for (;;) {
      AnalysisCheckpoint checkpoint(*this);
      states_ = header;
      active_locals_ = active_loop;
      cleanup_registrations_ = cleanup_loop;
      (void)analyzeBlock(condition, false);
      const auto exit_state = states_;
      loop_flows_.push_back(
          {outer_depth, loop_depth, outer_cleanup_depth, loop_cleanup_depth});
      const auto body_falls_through = analyzeBlock(body, true);
      auto flow = std::move(loop_flows_.back());
      loop_flows_.pop_back();
      std::optional<StateMap> step_entry;
      if (body_falls_through)
        step_entry = states_;
      if (flow.continues)
        mergeOptionalState(step_entry, std::move(*flow.continues), *this);
      std::optional<StateMap> backedge;
      if (step_entry) {
        states_ = *step_entry;
        active_locals_ = active_loop;
        cleanup_registrations_ = cleanup_loop;
        (void)analyzeBlock(step, false);
        backedge = states_;
      }
      states_ = preheader;
      if (backedge)
        mergeStates(preheader, *backedge);
      const auto next_header = states_;
      if (sameStates(header, next_header)) {
        final_exit = exit_state;
        final_breaks = std::move(flow.breaks);
        checkpoint.commit();
        break;
      }
      header = next_header;
    }
    states_ = std::move(final_exit);
    active_locals_ = active_loop;
    cleanup_registrations_ = cleanup_loop;
    const auto scoped =
        std::span<const LocalId>(active_locals_).subspan(outer_depth);
    const auto scoped_cleanups =
        std::span<const CleanupRegistration>(cleanup_registrations_)
            .subspan(outer_cleanup_depth);
    block_cleanups_[init.index] = cleanupPlan(scoped_cleanups);
    eraseLocals(scoped);
    active_locals_.resize(outer_depth);
    cleanup_registrations_.resize(outer_cleanup_depth);
    if (final_breaks) {
      const auto natural_exit = states_;
      mergeStates(natural_exit, *final_breaks);
    }
  }

  bool analyzeSwitch(const SemInst &switch_inst) {
    const auto before = states_;
    const auto active_before = active_locals_;
    const auto cleanups_before = cleanup_registrations_;
    const auto scrutinee = resolvePlace(InstId(switch_inst.arg0));
    const auto scrutinee_value = boundValue(InstId(switch_inst.arg0));
    const auto &scrutinee_inst = sem_ir_.inst(scrutinee_value);
    const auto placement = scrutinee_inst.kind == SemInstKind::Placement
                               ? resolvePlace(InstId(scrutinee_inst.arg0))
                               : std::optional<ResolvedPlace>{};
    const auto ok_variant = placement
                                ? resultOkVariant(TypeId(scrutinee_inst.type))
                                : std::optional<std::uint32_t>{};
    std::optional<std::unordered_map<std::uint32_t, RootState>> merged;
    bool any_falls_through = false;
    for (const auto arm_id : sem_ir_.instBlock(InstBlockId(switch_inst.arg1))) {
      const auto &arm = sem_ir_.inst(arm_id);
      states_ = before;
      active_locals_ = active_before;
      cleanup_registrations_ = cleanups_before;
      const auto variant = sem_ir_.integer(IntegerId(arm.arg0));
      if (variant >= 0) {
        const auto facts = outcome_facts_.find(scrutinee_value.index);
        if (facts != outcome_facts_.end())
          for (const auto &fact : facts->second)
            if (fact.variant == static_cast<std::uint32_t>(variant) &&
                fact.revision == place_revisions_[fact.place.root.index]) {
              if (fact.outcomes == CallableOutcomeInitialize) {
                setPlacePossibilities(fact.place, InitializedBit);
                ensureRoot(fact.place.root).ever_initialized = true;
              } else if (fact.outcomes == CallableOutcomeInvalidate)
                setPlacePossibilities(fact.place, MovedBit);
              else if (fact.outcomes == CallableOutcomePreserve)
                setPlacePossibilities(fact.place, fact.before);
            }
      }
      if (placement && ok_variant && variant >= 0)
        setPlacePossibilities(*placement,
                              static_cast<std::uint32_t>(variant) == *ok_variant
                                  ? InitializedBit
                                  : MovedBit);
      if (scrutinee && variant >= 0) {
        auto &root = ensureRoot(scrutinee->root);
        const auto state =
            std::ranges::find_if(root.enums, [&](const EnumState &candidate) {
              return candidate.path == scrutinee->path;
            });
        if (state != root.enums.end() && static_cast<std::size_t>(variant) <
                                             state->possible_variants.size()) {
          std::ranges::fill(state->possible_variants, 0U);
          state->possible_variants[static_cast<std::size_t>(variant)] = 1U;
          state->unknown = false;
        }
      }
      const auto falls_through = analyzeBlock(InstBlockId(arm.arg1), true);
      if (!falls_through)
        continue;
      any_falls_through = true;
      if (!merged)
        merged = states_;
      else {
        const auto branch = states_;
        states_ = *merged;
        mergeStates(*merged, branch);
        *merged = states_;
      }
    }
    states_ = merged ? std::move(*merged) : before;
    active_locals_ = active_before;
    cleanup_registrations_ = cleanups_before;
    return any_falls_through;
  }

  bool analyzeBlock(InstBlockId block, bool lexical_scope) {
    const auto active_begin = active_locals_.size();
    const auto cleanup_begin = cleanup_registrations_.size();
    bool falls_through = true;
    for (const auto instruction : sem_ir_.instBlock(block)) {
      if (!falls_through)
        break;
      const auto &inst = sem_ir_.inst(instruction);
      switch (inst.kind) {
      case SemInstKind::DeclareUninitialized: {
        const auto local = LocalId(inst.arg0);
        ensureRoot(local).uninitialized_declaration = instruction;
        if (std::ranges::find(active_locals_, local) == active_locals_.end())
          active_locals_.push_back(local);
        break;
      }
      case SemInstKind::OutPlace:
      case SemInstKind::InitializePlace: {
        // Taking an initialization place grants addressability but does not
        // initialize storage. The call postcondition performs that transition.
        (void)resolvePlace(instruction);
        break;
      }
      case SemInstKind::Parameter:
        initialize(LocalId(inst.arg0));
        break;
      case SemInstKind::BindName:
        if (sem_ir_.type(TypeId(sem_ir_.inst(InstId(inst.arg1)).type)).kind ==
            SemTypeKind::Reference) {
          if (const auto place = resolveCallEffectPlace(InstId(inst.arg1),
                                                        OwnershipRegion{})) {
            if (isExternal(*place))
              observeExternal(instruction, *place, PlaceObservationKind::Copy);
            else
              observe(instruction, *place, PlaceObservationKind::Copy);
          }
        }
        bound_values_.insert_or_assign(inst.arg0, InstId(inst.arg1));
        initialize(LocalId(inst.arg0), InstId(inst.arg1));
        break;
      case SemInstKind::MaterializeTemporary:
        initialize(LocalId(inst.arg0), InstId(inst.arg1));
        break;
      case SemInstKind::ExtendTemporary: {
        const auto &materialized_inst = sem_ir_.inst(InstId(inst.arg1));
        if (materialized_inst.kind != SemInstKind::MaterializeTemporary)
          break;
        const auto materialized = LocalId(materialized_inst.arg0);
        const auto registration = std::ranges::find_if(
            cleanup_registrations_, [&](const CleanupRegistration &candidate) {
              return candidate.kind == CleanupRegistration::Kind::Temporary &&
                     candidate.local == materialized;
            });
        if (registration != cleanup_registrations_.end())
          registration->kind = CleanupRegistration::Kind::Extended;
        break;
      }
      case SemInstKind::EndFullExpression: {
        std::vector<CleanupRegistration> registrations;
        std::vector<LocalId> locals;
        for (const auto temporary : sem_ir_.instBlock(InstBlockId(inst.arg0))) {
          const auto materialized =
              sem_ir_.getAs<SemMaterializeTemporary>(temporary);
          const auto registration = std::ranges::find_if(
              cleanup_registrations_,
              [&](const CleanupRegistration &candidate) {
                return candidate.kind == CleanupRegistration::Kind::Temporary &&
                       candidate.local == materialized.arg0;
              });
          if (registration == cleanup_registrations_.end())
            continue;
          registrations.push_back(*registration);
          cleanup_registrations_.erase(registration);
          std::erase(active_locals_, materialized.arg0);
          locals.push_back(materialized.arg0);
        }
        full_expression_cleanups_[instruction.index] =
            cleanupPlan(registrations);
        eraseLocals(locals);
        break;
      }
      case SemInstKind::NameRef:
      case SemInstKind::StructFieldAccess:
      case SemInstKind::UnionFieldAccess:
      case SemInstKind::EnumPayloadAccess:
      case SemInstKind::Index:
        observeDirectPlace(instruction);
        break;
      case SemInstKind::BorrowLocal: {
        const auto local = LocalId(inst.arg0);
        observe(instruction,
                ResolvedPlace{local, sem_ir_.local(local).type, {}},
                PlaceObservationKind::Borrow);
        break;
      }
      case SemInstKind::BorrowPlace: {
        const auto place = resolvePlace(InstId(inst.arg0));
        const auto external = place && isExternal(*place);
        if (!place || (!external && !hasObservableLeaf(*place)))
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidMoveOperand});
        else if (external)
          observeExternal(instruction, *place, PlaceObservationKind::Borrow);
        else
          observe(instruction, *place, PlaceObservationKind::Borrow);
        break;
      }
      case SemInstKind::Move: {
        const auto place = resolvePlace(InstId(inst.arg0));
        const auto external = place && isExternal(*place);
        if (!place || (!external && !hasTrackedLeaf(*place))) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidMoveOperand});
        } else if (place->borrowed_view) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::MoveUnavailable});
        } else if (sem_ir_.typeRepresentation(place->type).move ==
                   MoveReprKind::Unavailable) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::MoveUnavailable});
        } else if (external) {
          observeExternal(instruction, *place, PlaceObservationKind::Move);
        } else {
          if (sem_ir_.type(place->type).kind == SemTypeKind::CoroutineTask &&
              place->path.empty()) {
            auto &root = ensureRoot(place->root);
            task_value_obligations_.insert_or_assign(instruction.index,
                                                     root.task_obligation);
            task_value_scopes_.insert_or_assign(instruction.index,
                                                root.task_scope);
            root.task_obligation = 0;
            root.task_scope = 0;
          }
          observe(instruction, *place, PlaceObservationKind::Move);
        }
        break;
      }
      case SemInstKind::Copy: {
        const auto place = resolvePlace(InstId(inst.arg0));
        const auto external = place && isExternal(*place);
        if (!place || (!external && !hasTrackedLeaf(*place))) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidCopyOperand});
        } else if (sem_ir_.typeRepresentation(place->type).copy ==
                   CopyReprKind::Unavailable) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::CopyUnavailable});
        } else if (external) {
          copy_places_[instruction.index] = internPlace(*place);
          observeExternal(instruction, *place, PlaceObservationKind::Copy);
        } else {
          copy_places_[instruction.index] = internPlace(*place);
          observe(instruction, *place, PlaceObservationKind::Copy);
        }
        break;
      }
      case SemInstKind::Assign: {
        const auto place = resolvePlace(InstId(inst.arg0));
        if (place) {
          bound_values_.erase(place->root.index);
          ++place_revisions_[place->root.index];
        }
        const auto external_destination = place && isExternal(*place);
        if (external_destination) {
          if (taskScopeForValue(InstId(inst.arg1)) != 0)
            diagnostics_.push_back(
                {instruction, PlaceStateDiagnosticKind::TaskScopeEscape});
          reinitializations_[instruction.index] = {internPlace(*place), {}};
          observeExternal(instruction, *place,
                          PlaceObservationKind::Reinitialize);
        } else if (!place || !hasTrackedLeaf(*place)) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidAssignmentTarget});
        } else if (place->borrowed_view) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidAssignmentTarget});
        } else if ((sem_ir_.local(place->root).flags & SemLocalMutable) == 0) {
          diagnostics_.push_back(
              {instruction,
               PlaceStateDiagnosticKind::AssignmentToImmutablePlace});
        } else if (std::ranges::any_of(place->path, [](const auto &projection) {
                     return projection.kind == PlaceProjectionKind::AnyElement;
                   }) && stateFor(*place) != PlaceStateKind::Initialized) {
          // Writing one unknown element cannot reinitialize all array cells.
          diagnostics_.push_back({instruction, PlaceStateDiagnosticKind::UninitializedStorage});
        } else {
          reinitialize(instruction, *place);
        }
        break;
      }
      case SemInstKind::Placement: {
        const auto place = resolvePlace(InstId(inst.arg0));
        const auto fallible = TypeId(inst.type) != sem_ir_.voidType();
        const auto external_destination = place && isExternal(*place);
        if (!place || external_destination || !hasTrackedLeaf(*place)) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidAssignmentTarget});
        } else if ((sem_ir_.local(place->root).flags & SemLocalMutable) == 0) {
          diagnostics_.push_back(
              {instruction,
               PlaceStateDiagnosticKind::AssignmentToImmutablePlace});
        } else if (!fallible) {
          reinitialize(instruction, *place);
        } else if (stateFor(*place) != PlaceStateKind::Moved) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidAssignmentTarget});
        } else {
          const auto target = internPlace(*place);
          reinitializations_[instruction.index] = {target, {}};
          const auto before = stateFor(*place);
          setPlacePossibilities(*place, InitializedBit | MovedBit);
          observations_.push_back({instruction, target,
                                   PlaceObservationKind::Reinitialize, before,
                                   stateFor(*place)});
        }
        break;
      }
      case SemInstKind::Call:
      case SemInstKind::ForeignOperationCall:
      case SemInstKind::CompilerIntrinsicCall:
      case SemInstKind::IndirectCall:
      case SemInstKind::IndirectForeignCall:
      case SemInstKind::CallbackAdapterCall:
      case SemInstKind::MakeCallbackRegistration:
        applyCallEffects(instruction, inst);
        break;
      case SemInstKind::TypedChannelSendPrepare:
        // Outcome facts define prepare as a non-moving transition. Ownership
        // remains with the source until the paired commit transition.
        if (!channelOutcomeFacts()
                 .transitionEffect(OutcomeState::Pending,
                                   OutcomeState::Prepared,
                                   OutcomePhase::Prepare)
                 .has_value())
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
        break;
      case SemInstKind::TypedChannelSendCommit: {
        const auto effect = channelOutcomeFacts().transitionEffect(
            OutcomeState::Prepared, OutcomeState::Committed,
            OutcomePhase::Commit);
        if (!effect || effect->source != OutcomeEffect::MoveSource) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
          break;
        }
        const auto place = resolvePlace(InstId(inst.arg1));
        if (!place || isExternal(*place) || place->borrowed_view)
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidMoveOperand});
        else
          observe(instruction, *place, PlaceObservationKind::Move);
        break;
      }
      case SemInstKind::TypedChannelSendCancel:
        if (!channelOutcomeFacts()
                 .transitionEffect(OutcomeState::Prepared,
                                   OutcomeState::Cancelled,
                                   OutcomePhase::Cancel)
                 .has_value())
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
        // A cancelled reservation has not acquired the payload owner.
        break;
      case SemInstKind::TypedChannelReceiveAcquire:
      case SemInstKind::TypedChannelReceiveCancel:
      case SemInstKind::TypedChannelClose:
        if (!channelOutcomeFacts()
                 .transitionEffect(
                     inst.kind == SemInstKind::TypedChannelReceiveCancel
                         ? OutcomeState::Prepared
                         : OutcomeState::Pending,
                     inst.kind == SemInstKind::TypedChannelReceiveCancel
                         ? OutcomeState::Cancelled
                         : OutcomeState::Prepared,
                     inst.kind == SemInstKind::TypedChannelReceiveCancel
                         ? OutcomePhase::Cancel
                         : OutcomePhase::Prepare)
                 .has_value())
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
        break;
      case SemInstKind::TypedChannelReceiveCommit: {
        const auto effect = channelOutcomeFacts().transitionEffect(
            OutcomeState::Prepared, OutcomeState::Committed,
            OutcomePhase::Commit);
        if (!effect || effect->source != OutcomeEffect::MoveSource) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
          break;
        }
        const auto place = resolvePlace(InstId(inst.arg1));
        if (!place || isExternal(*place) || place->borrowed_view)
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidAssignmentTarget});
        else
          reinitialize(instruction, *place);
        break;
      }
      case SemInstKind::Defer:
        cleanup_registrations_.push_back({CleanupRegistration::Kind::Defer,
                                          LocalId::invalid(),
                                          InstBlockId(inst.arg0)});
        break;
      case SemInstKind::CoroutineChildTaskCreate:
        task_value_scopes_.insert_or_assign(
            instruction.index,
            active_task_scopes_.empty() ? 0U : active_task_scopes_.back().id);
        break;
      case SemInstKind::CoroutineTaskScope:
        active_task_scopes_.push_back(
            {next_task_scope_++, active_locals_.size()});
        falls_through = analyzeBlock(InstBlockId(inst.arg0), true);
        active_task_scopes_.pop_back();
        break;
      case SemInstKind::CoroutineTaskCompletionArm: {
        const auto consume = taskOutcomeFacts().transitionEffect(
            OutcomeState::Ready, OutcomeState::Consumed,
            OutcomePhase::Consume);
        if (!consume || consume->source != OutcomeEffect::ConsumePayload) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
          break;
        }
        const auto place = resolvePlace(InstId(inst.arg0));
        if (place && !isExternal(*place) && place->path.empty()) {
          auto &root = ensureRoot(place->root);
          if (root.task_obligation != TaskOutstandingBit) {
            diagnostics_.push_back(
                {instruction, PlaceStateDiagnosticKind::TaskAlreadyConsumed});
          } else {
            root.task_obligation = TaskFulfilledBit;
          }
        }
        break;
      }
      case SemInstKind::CoroutineTaskTakeResult:
      case SemInstKind::CoroutineTaskTakeError: {
        // Payload extraction is the one-shot consume edge. The task place
        // itself remains live for its ordinary lifecycle cleanup; only the
        // terminal payload is consumed here.
        const auto effect = taskOutcomeFacts(
                                inst.kind == SemInstKind::CoroutineTaskTakeError)
                                .transitionEffect(
                                    OutcomeState::Ready,
                                    OutcomeState::Consumed,
                                    OutcomePhase::Consume);
        if (!effect || effect->source != OutcomeEffect::ConsumePayload)
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
        break;
      }
      case SemInstKind::CoroutineSuspend:
      case SemInstKind::CoroutineTaskCompletionWaitAll:
      case SemInstKind::CoroutineTaskCompletionSelect:
      case SemInstKind::CoroutineTaskCompletionRace: {
        std::vector<CleanupRegistration> pre_commit;
        std::vector<LocalId> consumed_temporaries;
        for (auto registration = cleanup_registrations_.begin();
             registration != cleanup_registrations_.end();) {
          if (registration->kind != CleanupRegistration::Kind::Temporary) {
            ++registration;
            continue;
          }
          pre_commit.push_back(*registration);
          consumed_temporaries.push_back(registration->local);
          std::erase(active_locals_, registration->local);
          registration = cleanup_registrations_.erase(registration);
        }
        SuspensionCleanupPartition partition;
        partition.pre_commit = cleanupPlan(pre_commit);
        eraseLocals(consumed_temporaries);
        partition.transferred = snapshotCleanupPlan(cleanup_registrations_);
        suspension_cleanups_[instruction.index] = std::move(partition);
        break;
      }
      case SemInstKind::CoroutineCancellationCheck:
      case SemInstKind::CoroutineExecutorSwitch:
        cancellation_cleanups_[instruction.index] =
            snapshotCleanupPlan(cleanup_registrations_);
        break;
      case SemInstKind::CoroutineRuntimeFault:
        falls_through = false;
        break;
      case SemInstKind::If: {
        const auto before = states_;
        const auto active_before = active_locals_;
        const auto cleanups_before = cleanup_registrations_;
        const auto arms = sem_ir_.instBlock(InstBlockId(inst.arg1));
        std::optional<StateMap> merged;
        if (arms.size() == 1)
          merged = before;
        for (const auto arm_id : arms) {
          const auto &arm = sem_ir_.inst(arm_id);
          states_ = before;
          active_locals_ = active_before;
          cleanup_registrations_ = cleanups_before;
          if (analyzeBlock(InstBlockId(arm.arg0), true))
            mergeOptionalState(merged, states_, *this);
        }
        states_ = merged.value_or(before);
        active_locals_ = active_before;
        cleanup_registrations_ = cleanups_before;
        falls_through = merged.has_value();
        break;
      }
      case SemInstKind::ScopedBlock:
        falls_through = analyzeBlock(InstBlockId(inst.arg0), true);
        break;
      case SemInstKind::While:
        analyzeWhile(InstBlockId(inst.arg0), InstBlockId(inst.arg1));
        break;
      case SemInstKind::For:
        analyzeFor(inst);
        break;
      case SemInstKind::DoWhile:
        falls_through =
            analyzeDoWhile(InstBlockId(inst.arg0), InstBlockId(inst.arg1));
        break;
      case SemInstKind::Break:
      case SemInstKind::Continue: {
        assert(!loop_flows_.empty());
        const auto distance =
            static_cast<std::size_t>(sem_ir_.integer(IntegerId(inst.arg0)));
        assert(distance < loop_flows_.size());
        auto &flow = loop_flows_[loop_flows_.size() - 1 - distance];
        const auto local_depth = inst.kind == SemInstKind::Break
                                     ? flow.break_local_depth
                                     : flow.continue_local_depth;
        const auto cleanup_depth = inst.kind == SemInstKind::Break
                                       ? flow.break_cleanup_depth
                                       : flow.continue_cleanup_depth;
        const auto scoped_cleanups =
            std::span<const CleanupRegistration>(cleanup_registrations_)
                .subspan(cleanup_depth);
        edge_cleanups_[instruction.index] = cleanupPlan(scoped_cleanups);
        diagnoseOutstandingTasks(
            instruction,
            std::span<const LocalId>(active_locals_).subspan(local_depth));
        auto edge_state = statesAtDepth(local_depth);
        if (inst.kind == SemInstKind::Break)
          mergeOptionalState(flow.breaks, std::move(edge_state), *this);
        else
          mergeOptionalState(flow.continues, std::move(edge_state), *this);
        falls_through = false;
        break;
      }
      case SemInstKind::Switch:
        falls_through = analyzeSwitch(inst);
        break;
      case SemInstKind::Return:
      case SemInstKind::CoroutineReturnSuccess:
      case SemInstKind::CoroutineReturnError:
        if (taskScopeForValue(InstId(inst.arg0)) != 0)
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::TaskScopeEscape});
        diagnoseOutstandingTasks(instruction, active_locals_);
        diagnoseUninitializedStorage(active_locals_);
        return_cleanups_[instruction.index] =
            cleanupPlan(cleanup_registrations_);
        falls_through = false;
        break;
      case SemInstKind::CoroutineReturnCancelled:
        return_cleanups_[instruction.index] =
            cleanupPlan(cleanup_registrations_);
        falls_through = false;
        break;
      default:
        break;
      }
    }
    if (lexical_scope) {
      const auto scoped =
          std::span<const LocalId>(active_locals_).subspan(active_begin);
      const auto scoped_cleanups =
          std::span<const CleanupRegistration>(cleanup_registrations_)
              .subspan(cleanup_begin);
      if (falls_through)
        block_cleanups_[block.index] = cleanupPlan(scoped_cleanups);
      if (falls_through && !scoped.empty()) {
        const auto instructions = sem_ir_.instBlock(block);
        diagnoseOutstandingTasks(instructions.back(), scoped);
        diagnoseUninitializedStorage(scoped);
      }
      eraseLocals(scoped);
      active_locals_.resize(active_begin);
      cleanup_registrations_.resize(cleanup_begin);
    }
    return falls_through;
  }

  void analyzeFunction(FunctionId function) {
    current_function_ = function;
    states_.clear();
    active_locals_.clear();
    cleanup_registrations_.clear();
    bound_values_.clear();
    outcome_facts_.clear();
    initialization_attempts_.clear();
    place_revisions_.clear();
    task_value_obligations_.clear();
    task_value_scopes_.clear();
    active_task_scopes_.clear();
    next_task_scope_ = 1;
    initialized_forwarded_parameters_.clear();
    const auto &ownership = sem_ir_.functionOwnership(function);
    const auto parameters =
        sem_ir_.localBlock(sem_ir_.function(function).parameters);
    for (const auto &effect : ownership.effects)
      if (effect.kind == CallableEffectKind::Initialize &&
          effect.region.path.empty() &&
          effect.region.parameter_index < parameters.size())
        initialized_forwarded_parameters_.insert(
            parameters[effect.region.parameter_index].index);
    const auto body = sem_ir_.function(function).body;
    const auto falls_through = analyzeBlock(body, false);
    const auto instructions = sem_ir_.instBlock(body);
    if (falls_through && !instructions.empty())
      diagnoseOutstandingTasks(instructions.back(), active_locals_);
    if (falls_through && !instructions.empty())
      diagnoseUninitializedStorage(active_locals_);
  }
