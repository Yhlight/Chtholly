// Included inside PlaceStateAnalyzer; this fragment aliases its existing
// place-state maps, cleanup plans, and worklists.
  [[nodiscard]] InstId boundValue(InstId value) const {
    std::unordered_set<std::uint32_t> seen;
    while (value.hasValue() && value.index < sem_ir_.instCount() && seen.insert(value.index).second) {
      const auto &inst = sem_ir_.inst(value);
      if (inst.kind == SemInstKind::Move || inst.kind == SemInstKind::Copy) { value = InstId(inst.arg0); continue; }
      if (inst.kind != SemInstKind::NameRef) return value;
      const auto found = bound_values_.find(inst.arg0);
      if (found == bound_values_.end()) return value;
      value = found->second;
    }
    return value;
  }

  [[nodiscard]] std::optional<std::uint32_t>
  resultOkVariant(TypeId type) const {
    const auto shape = sem_ir_.canonicalResultShape(type);
    return shape ? std::optional(shape->ok_variant) : std::nullopt;
  }

  std::optional<ResolvedPlace>
  resolveCallEffectPlace(InstId argument, const OwnershipRegion &region) {
    if (sem_ir_.inst(argument).kind == SemInstKind::CallbackRegistrationBinding)
      argument = InstId(sem_ir_.inst(argument).arg1);
    const auto &argument_inst = sem_ir_.inst(argument);
    std::optional<ResolvedPlace> place;
    if (argument_inst.kind == SemInstKind::BorrowLocal) {
      const auto local = LocalId(argument_inst.arg0);
      place = ResolvedPlace{local, sem_ir_.local(local).type, {}};
    } else {
      if (argument_inst.kind == SemInstKind::BorrowPlace ||
          argument_inst.kind == SemInstKind::Move ||
          argument_inst.kind == SemInstKind::Copy)
        argument = InstId(argument_inst.arg0);
      place = resolvePlace(argument);
    }
    if (!place)
      return std::nullopt;
    if (sem_ir_.type(place->type).kind == SemTypeKind::Reference) {
      place->path.push_back({PlaceProjectionKind::Dereference, 0});
      place->type = sem_ir_.referencePointee(place->type);
    }
    for (const auto &step : region.path) {
      if (step.kind == OwnershipRegionStepKind::Dereference) {
        const auto kind = sem_ir_.type(place->type).kind;
        if (step.index != 0 ||
            (kind != SemTypeKind::Reference && kind != SemTypeKind::RawPointer))
          return std::nullopt;
        place->path.push_back({PlaceProjectionKind::Dereference, 0});
        place->type = kind == SemTypeKind::Reference
                          ? sem_ir_.referencePointee(place->type)
                          : sem_ir_.rawPointerPointee(place->type);
        continue;
      }
      if (step.kind == OwnershipRegionStepKind::AnyElement) {
        const auto kind = sem_ir_.type(place->type).kind;
        if (kind != SemTypeKind::Array && kind != SemTypeKind::Slice)
          return std::nullopt;
        place->path.push_back({PlaceProjectionKind::AnyElement, 0});
        place->borrowed_view |= kind == SemTypeKind::Slice;
        place->type = TypeId(sem_ir_.type(place->type).arg0);
        return place;
      }
      if (step.kind == OwnershipRegionStepKind::StaticElement) {
        if (sem_ir_.type(place->type).kind != SemTypeKind::Array ||
            step.index >= sem_ir_.type(place->type).arg1)
          return std::nullopt;
        place->path.push_back({PlaceProjectionKind::Element, step.index});
        place->type = TypeId(sem_ir_.type(place->type).arg0);
        continue;
      }
      if (step.kind != OwnershipRegionStepKind::Field ||
          sem_ir_.type(place->type).kind != SemTypeKind::Nominal)
        return std::nullopt;
      const auto &nominal =
          sem_ir_.nominalType(NominalTypeId(sem_ir_.type(place->type).arg0));
      if (step.index >= nominal.fields.size())
        return std::nullopt;
      place->path.push_back({PlaceProjectionKind::Field, step.index});
      place->type = sem_ir_.nominalFieldType(place->type, step.index);
    }
    return place;
  }

  void applyCallEffects(InstId instruction, const SemInst &inst) {
    std::optional<CallableOwnershipSummary> adapter_summary;
    const auto *summary = [&]() -> const CallableOwnershipSummary * {
      if (inst.kind == SemInstKind::ForeignOperationCall) {
        adapter_summary = foreignOutcomeCallOwnershipSummary(
            sem_ir_, FunctionRefId(inst.arg0), TypeId(inst.type));
        if (adapter_summary)
          return &*adapter_summary;
      }
      if (inst.kind == SemInstKind::Call ||
          inst.kind == SemInstKind::ForeignOperationCall ||
          inst.kind == SemInstKind::CompilerIntrinsicCall)
        return callableOwnershipSummary(sem_ir_, FunctionRefId(inst.arg0));
      if (inst.kind == SemInstKind::IndirectCall) {
        adapter_summary = indirectCallOwnershipSummary(sem_ir_, TypeId(sem_ir_.inst(InstId(inst.arg0)).type));
        return &*adapter_summary;
      }
      if (inst.kind == SemInstKind::IndirectForeignCall)
        return &sem_ir_.callbackContract(
            TypeId(sem_ir_.inst(InstId(inst.arg0)).type));
      if (inst.kind == SemInstKind::MakeCallbackRegistration) {
        adapter_summary =
            callbackRegistrationCallOwnershipSummary(sem_ir_, instruction);
        return adapter_summary ? &*adapter_summary : nullptr;
      }
      adapter_summary = callbackAdapterCallOwnershipSummary(
          sem_ir_, TypeId(sem_ir_.inst(InstId(inst.arg0)).type));
      return adapter_summary ? &*adapter_summary : nullptr;
    }();
    if (!summary)
      return;
    // Calls producing a canonical Result or Task must be backed by the same
    // verified one-shot outcome protocol used by the lowering plans. Keep
    // this check local to the semantic boundary so an invalid internal
    // descriptor fails closed instead of widening a place unconditionally.
    const auto result_type = TypeId(inst.type);
    if (sem_ir_.canonicalResultShape(result_type)) {
      if (!resultOutcomeFacts()
               .transitionEffect(OutcomeState::Pending,
                                 OutcomeState::Ready,
                                 OutcomePhase::Publish)
               .has_value()) {
        diagnostics_.push_back(
            {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
        return;
      }
    } else if (sem_ir_.type(result_type).kind == SemTypeKind::CoroutineTask) {
      const bool fallible = sem_ir_.coroutineTaskErrorType(result_type).has_value();
      const auto &facts = taskOutcomeFacts(fallible);
      if (fallible != (std::ranges::find(facts.arms, OutcomeArmKind::Failure) !=
                       facts.arms.end())) {
        diagnostics_.push_back(
            {instruction, PlaceStateDiagnosticKind::AnalysisNonConvergent});
        return;
      }
    }
    std::vector<TypeId> adapter_parameter_types;
    std::span<const TypeId> parameter_types;
    if (inst.kind == SemInstKind::CallbackAdapterCall) {
      const auto adapter_type = TypeId(sem_ir_.inst(InstId(inst.arg0)).type);
      const auto fields =
          sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(adapter_type).arg0));
      const auto entry_parameters =
          sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(fields[0]).arg0));
      const auto context = sem_ir_.callbackContextParameter(fields[0]);
      for (std::size_t index = 0; index < entry_parameters.size(); ++index)
        if (index != context)
          adapter_parameter_types.push_back(entry_parameters[index]);
      parameter_types = adapter_parameter_types;
    } else if (inst.kind == SemInstKind::MakeCallbackRegistration) {
      const auto arguments = sem_ir_.instBlock(InstBlockId(inst.arg0));
      for (const auto argument : arguments)
        adapter_parameter_types.push_back(TypeId(sem_ir_.inst(argument).type));
      parameter_types = adapter_parameter_types;
    } else {
      const auto callee_type_id =
          (inst.kind == SemInstKind::Call ||
           inst.kind == SemInstKind::ForeignOperationCall ||
           inst.kind == SemInstKind::CompilerIntrinsicCall)
              ? sem_ir_.functionRef(FunctionRefId(inst.arg0)).local_type
              : TypeId(sem_ir_.inst(InstId(inst.arg0)).type);
      parameter_types =
          sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callee_type_id).arg0));
    }
    const auto arguments = sem_ir_.instBlock(InstBlockId(
        inst.kind == SemInstKind::MakeCallbackRegistration ? inst.arg0
                                                           : inst.arg1));
    for (const auto &effect : summary->effects) {
      if (effect.region.parameter_index >= arguments.size() ||
          effect.region.parameter_index >= parameter_types.size() ||
          (sem_ir_.type(parameter_types[effect.region.parameter_index]).kind !=
               SemTypeKind::Reference &&
           std::ranges::none_of(effect.region.path, [](const auto &step) {
             return step.kind == OwnershipRegionStepKind::Dereference;
           })))
        continue;
      const auto place = resolveCallEffectPlace(
          arguments[effect.region.parameter_index], effect.region);
      if (!place)
        continue;
      if (effect.kind == CallableEffectKind::Initialize) {
        const auto is_forwarded_parameter = [&] {
          const auto argument = arguments[effect.region.parameter_index];
          const auto &argument_inst = sem_ir_.inst(argument);
          if (argument_inst.kind != SemInstKind::NameRef ||
              !current_function_.has_value())
            return false;
          const auto local = LocalId(argument_inst.arg0);
          const auto parameters = sem_ir_.localBlock(
              sem_ir_.function(*current_function_).parameters);
          return std::ranges::find(parameters, local) != parameters.end() &&
                 sem_ir_.type(sem_ir_.local(local).type).kind ==
                     SemTypeKind::Reference &&
                 sem_ir_.referenceMutability(sem_ir_.local(local).type) ==
                     SemReferenceMutability::Mutable;
        }();
        if (isExternal(*place) || is_forwarded_parameter) {
          if (is_forwarded_parameter &&
              !initialized_forwarded_parameters_.contains(place->root.index)) {
            diagnostics_.push_back(
                {instruction,
                 PlaceStateDiagnosticKind::InvalidAssignmentTarget});
            continue;
          }
          // A mutable reference parameter may be an initialization capability
          // forwarded from the caller. Its referent state is owned by the
          // caller's place-state analysis; this call consumes the capability
          // and the caller applies the initialized postcondition at return.
          observations_.push_back(
              {instruction, internPlace(*place), PlaceObservationKind::Borrow,
               PlaceStateKind::Moved, PlaceStateKind::Moved});
          continue;
        }
        if (place->borrowed_view || stateFor(*place) != PlaceStateKind::Moved) {
          diagnostics_.push_back(
              {instruction, PlaceStateDiagnosticKind::InvalidAssignmentTarget});
          continue;
        }
        const auto state = stateFor(*place);
        observations_.push_back({instruction, internPlace(*place),
                                 PlaceObservationKind::Borrow, state, state});
        continue;
      }
      auto observation = PlaceObservationKind::Read;
      switch (effect.kind) {
      case CallableEffectKind::Read:
        observation = PlaceObservationKind::Read;
        break;
      case CallableEffectKind::Take:
        observation = PlaceObservationKind::Move;
        break;
      case CallableEffectKind::Write:
        observation = PlaceObservationKind::Reinitialize;
        break;
      case CallableEffectKind::Initialize:
        continue;
      case CallableEffectKind::BorrowShared:
      case CallableEffectKind::BorrowMutable:
        observation = PlaceObservationKind::Borrow;
        break;
      case CallableEffectKind::Count:
        continue;
      }
      observeCallEffect(instruction, *place, observation);
    }
    outcome_facts_.erase(instruction.index);
    std::unordered_map<std::uint32_t, std::uint8_t> before_calls;
    for (const auto &postcondition : summary->postconditions) {
      if (postcondition.region.parameter_index >= arguments.size() ||
          postcondition.region.parameter_index >= parameter_types.size() ||
          (sem_ir_.type(parameter_types[postcondition.region.parameter_index])
                   .kind != SemTypeKind::Reference &&
           std::ranges::none_of(
               postcondition.region.path, [](const auto &step) {
                 return step.kind == OwnershipRegionStepKind::Dereference;
               })))
        continue;
      const auto place = resolveCallEffectPlace(
          arguments[postcondition.region.parameter_index],
          postcondition.region);
      if (!place)
        continue;
      // PlaceState is path-sensitive for local CFG branches but does not
      // retain a callable's parameter predicate at an opaque call boundary.
      // Preserve the old state whenever a postcondition is conditional; this
      // is conservative and prevents a guarded initialize/invalidate from
      // becoming an unconditional transition. The precise predicate is still
      // replayed by CallableOwnership and artifact verification.
      const auto before_state = stateFor(*place);
      before_calls.try_emplace(place->root.index, before_state == PlaceStateKind::Initialized ? InitializedBit
           : before_state == PlaceStateKind::Moved ? MovedBit : static_cast<std::uint8_t>(InitializedBit | MovedBit));
      auto outcomes = postcondition.outcomes;
      if (!postcondition.condition.isAlways() || postcondition.result_variant != core::AnyId::InvalidIndex)
        outcomes |= CallableOutcomePreserve;
      if (!isExternal(*place) && postcondition.condition.isAlways() &&
          postcondition.result_variant != core::AnyId::InvalidIndex)
        outcome_facts_[instruction.index].push_back({*place, postcondition.result_variant,
            postcondition.outcomes, place_revisions_[place->root.index], before_calls.at(place->root.index)});
      if (!isExternal(*place) && postcondition.result_variant != core::AnyId::InvalidIndex &&
          (postcondition.outcomes & CallableOutcomeInitialize) != 0) {
        initialization_attempts_.insert(place->root.index);
        if (std::ranges::none_of(cleanup_registrations_, [&](const auto &entry) { return entry.local == place->root; }))
          cleanup_registrations_.push_back({CleanupRegistration::Kind::Local, place->root, InstBlockId::invalid()});
      }
      if (isExternal(*place)) {
        if (outcomes == CallableOutcomeInitialize)
          initialized_forwarded_parameters_.erase(place->root.index);
        continue;
      }
      if (place->path.empty() &&
          outcomes == CallableOutcomeInitialize) {
        initialize(place->root);
        continue;
      }
      auto &root = ensureRoot(place->root);
      for (auto &leaf : root.leaves) {
        const auto union_leaf =
            sem_ir_.type(leaf.type).kind == SemTypeKind::Nominal &&
            sem_ir_.nominalType(NominalTypeId(sem_ir_.type(leaf.type).arg0))
                    .kind == NominalKind::Union;
        if (!isPrefix(place->path, leaf.path) &&
            !(union_leaf && isPrefix(leaf.path, place->path)))
          continue;
        std::uint8_t after = 0;
        if ((outcomes & CallableOutcomePreserve) != 0)
          after |= leaf.possibilities;
        if ((outcomes & CallableOutcomeInitialize) != 0)
          after |= InitializedBit;
        if ((outcomes & CallableOutcomeInvalidate) != 0)
          after |= MovedBit;
        leaf.possibilities = after;
      }
      for (auto &union_state : root.unions) {
        if (!isPrefix(place->path, union_state.path) &&
            !isPrefix(union_state.path, place->path))
          continue;
        if ((outcomes & CallableOutcomeInitialize) != 0) {
          std::ranges::fill(union_state.possible_members, 0U);
          union_state.unknown = true;
        }
        if ((outcomes & CallableOutcomeInvalidate) != 0 &&
            (outcomes & CallableOutcomePreserve) == 0 &&
            (outcomes & CallableOutcomeInitialize) == 0) {
          std::ranges::fill(union_state.possible_members, 0U);
          union_state.unknown = false;
        }
      }
    }
  }
