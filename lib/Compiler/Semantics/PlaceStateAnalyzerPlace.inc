// Included inside PlaceStateAnalyzer; this fragment aliases its existing
// place-state maps, cleanup plans, and worklists.
  RootState &ensureRoot(LocalId local) {
    if (const auto found = states_.find(local.index); found != states_.end())
      return found->second;
    RootState state;
    state.type = sem_ir_.local(local).type;
    std::vector<PlaceProjection> path;
    collectLeaves(state.type, path, state.leaves, state.unions, state.enums);
    return states_.emplace(local.index, std::move(state)).first->second;
  }

  [[nodiscard]] std::uint32_t taskScopeForValue(InstId value) const {
    if (!value.hasValue() || value.index >= sem_ir_.instCount())
      return 0;
    if (const auto found = task_value_scopes_.find(value.index);
        found != task_value_scopes_.end())
      return found->second;
    const auto &instruction = sem_ir_.inst(value);
    if (instruction.kind == SemInstKind::CoroutineCheckedTake ||
        instruction.kind == SemInstKind::Yield)
      return taskScopeForValue(InstId(instruction.arg0));
    if (instruction.kind != SemInstKind::If)
      return 0;
    std::optional<std::uint32_t> result;
    for (const auto arm_id : sem_ir_.instBlock(InstBlockId(instruction.arg1))) {
      const auto body =
          sem_ir_.instBlock(InstBlockId(sem_ir_.getAs<SemIfArm>(arm_id).arg0));
      const auto yielded = std::ranges::find_if(body, [&](InstId candidate) {
        return sem_ir_.inst(candidate).kind == SemInstKind::Yield;
      });
      if (yielded == body.end())
        continue;
      const auto arm_scope = taskScopeForValue(*yielded);
      if (result && *result != arm_scope)
        return 0;
      result = arm_scope;
    }
    return result.value_or(0);
  }

  void initialize(LocalId local, InstId value = InstId::invalid()) {
    ++place_revisions_[local.index];
    auto &state = ensureRoot(local);
    state.ever_initialized = true;
    for (auto &leaf : state.leaves)
      leaf.possibilities = InitializedBit;
    for (auto &union_state : state.unions) {
      std::ranges::fill(union_state.possible_members, 0U);
      union_state.unknown = true;
    }
    for (auto &enum_state : state.enums) {
      std::ranges::fill(enum_state.possible_variants, 1U);
      enum_state.unknown = true;
    }
    if (value.hasValue() && !state.unions.empty())
      setUnionStatesFromValue(state, {}, value);
    if (value.hasValue() && !state.enums.empty())
      setEnumStatesFromValue(state, {}, value);
    if (sem_ir_.type(state.type).kind == SemTypeKind::CoroutineTask) {
      const auto transferred = value.hasValue()
                                   ? task_value_obligations_.find(value.index)
                                   : task_value_obligations_.end();
      state.task_obligation = transferred == task_value_obligations_.end()
                                  ? TaskOutstandingBit
                                  : transferred->second;
      state.task_scope = taskScopeForValue(value);
    }
    if (std::ranges::find(active_locals_, local) == active_locals_.end()) {
      active_locals_.push_back(local);
    }
    if (std::ranges::none_of(
            cleanup_registrations_, [&](const CleanupRegistration &candidate) {
              return candidate.kind != CleanupRegistration::Kind::Defer &&
                     candidate.local == local;
            })) {
      cleanup_registrations_.push_back(
          {(sem_ir_.local(local).flags & SemLocalTemporary) != 0
               ? CleanupRegistration::Kind::Temporary
               : CleanupRegistration::Kind::Local,
           local, InstBlockId::invalid()});
    }
  }

  std::optional<ResolvedPlace> resolvePlace(InstId instruction) {
    if (!instruction.hasValue() || instruction.index >= sem_ir_.instCount())
      return std::nullopt;
    const auto &inst = sem_ir_.inst(instruction);
    if (inst.kind == SemInstKind::OutPlace ||
        inst.kind == SemInstKind::InitializePlace) {
      const auto local = LocalId(inst.arg0);
      return ResolvedPlace{local, sem_ir_.local(local).type, {}};
    }
    if (inst.kind == SemInstKind::CallbackRegistrationBinding)
      return resolvePlace(InstId(inst.arg1));
    if (inst.kind == SemInstKind::MaterializeTemporary) {
      const auto local = LocalId(inst.arg0);
      return ResolvedPlace{local, sem_ir_.local(local).type, {}};
    }
    if (inst.kind == SemInstKind::NameRef) {
      const auto local = LocalId(inst.arg0);
      return ResolvedPlace{local, sem_ir_.local(local).type, {}};
    }
    if (inst.kind == SemInstKind::CarrierView) {
      auto base = resolvePlace(InstId(inst.arg0));
      if (!base)
        return std::nullopt;
      base->path.push_back({PlaceProjectionKind::CarrierView, inst.type});
      base->type = TypeId(inst.type);
      return base;
    }
    if (inst.kind == SemInstKind::Dereference) {
      auto base = resolvePlace(InstId(inst.arg0));
      if (!base)
        return std::nullopt;
      const auto kind = sem_ir_.type(base->type).kind;
      if (kind != SemTypeKind::Reference && kind != SemTypeKind::RawPointer)
        return std::nullopt;
      base->path.push_back({PlaceProjectionKind::Dereference, 0});
      base->type = kind == SemTypeKind::Reference
                       ? sem_ir_.referencePointee(base->type)
                       : sem_ir_.rawPointerPointee(base->type);
      return base;
    }
    if (inst.kind != SemInstKind::StructFieldAccess &&
        inst.kind != SemInstKind::UnionFieldAccess &&
        inst.kind != SemInstKind::EnumPayloadAccess &&
        inst.kind != SemInstKind::Index)
      return std::nullopt;
    auto base = resolvePlace(InstId(inst.arg0));
    if (!base)
      return std::nullopt;
    if (inst.kind == SemInstKind::EnumPayloadAccess) {
      const auto encoded =
          static_cast<std::uint64_t>(sem_ir_.integer(IntegerId(inst.arg1)));
      base->path.push_back({PlaceProjectionKind::EnumPayload,
                            static_cast<std::uint32_t>(encoded),
                            static_cast<std::uint32_t>(encoded >> 32U)});
    } else if (inst.kind == SemInstKind::StructFieldAccess ||
               inst.kind == SemInstKind::UnionFieldAccess) {
      auto field = sem_ir_.integer(IntegerId(inst.arg1));
      if (inst.kind == SemInstKind::UnionFieldAccess && field < 0) {
        if (field == std::numeric_limits<std::int64_t>::min())
          return std::nullopt;
        base->unsafe_union_access = true;
        field = -field - 1;
      }
      if (field < 0 || static_cast<std::uint64_t>(field) >
                           std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
      base->path.push_back(
          {PlaceProjectionKind::Field, static_cast<std::uint32_t>(field)});
    } else {
      const auto &base_type = sem_ir_.type(base->type);
      if ((base_type.kind != SemTypeKind::Array &&
           base_type.kind != SemTypeKind::Slice) ||
          (base_type.kind == SemTypeKind::Array &&
           (base_type.arg1 == 0 || base_type.arg1 > MaxTrackedElements)))
        return std::nullopt;
      const auto &index_inst = sem_ir_.inst(InstId(inst.arg1));
      if (base_type.kind == SemTypeKind::Array &&
          index_inst.kind == SemInstKind::IntegerLiteral) {
        const auto index = sem_ir_.integer(IntegerId(index_inst.arg0));
        if (index < 0 ||
            static_cast<std::uint64_t>(index) >
                std::numeric_limits<std::uint32_t>::max() ||
            static_cast<std::uint64_t>(index) >= base_type.arg1)
          return std::nullopt;
        base->path.push_back(
            {PlaceProjectionKind::Element, static_cast<std::uint32_t>(index)});
      } else {
        base->path.push_back({PlaceProjectionKind::AnyElement, 0});
        base->borrowed_view |= base_type.kind == SemTypeKind::Slice;
      }
    }
    base->type = TypeId(inst.type);
    return base;
  }

  void setEnumStatesFromValue(RootState &target,
                              std::span<const PlaceProjection> target_path,
                              InstId value) {
    if (!value.hasValue() || value.index >= sem_ir_.instCount())
      return;
    const auto &source = sem_ir_.inst(value);
    if (source.kind == SemInstKind::Move || source.kind == SemInstKind::Copy) {
      setEnumStatesFromValue(target, target_path, InstId(source.arg0));
      return;
    }
    if (source.kind == SemInstKind::EnumInit) {
      const auto variant = sem_ir_.integer(IntegerId(source.arg1));
      const auto state =
          std::ranges::find_if(target.enums, [&](const EnumState &candidate) {
            return candidate.path.size() == target_path.size() &&
                   std::ranges::equal(candidate.path, target_path);
          });
      if (state == target.enums.end() || variant < 0 ||
          static_cast<std::size_t>(variant) >= state->possible_variants.size())
        return;
      std::ranges::fill(state->possible_variants, 0U);
      state->possible_variants[static_cast<std::size_t>(variant)] = 1U;
      state->unknown = false;
      return;
    }
    const auto source_place = resolvePlace(value);
    if (!source_place)
      return;
    const auto &source_root = ensureRoot(source_place->root);
    for (const auto &source_state : source_root.enums) {
      if (!isPrefix(source_place->path, source_state.path))
        continue;
      auto relative_path =
          std::vector<PlaceProjection>(target_path.begin(), target_path.end());
      relative_path.insert(relative_path.end(),
                           source_state.path.begin() +
                               source_place->path.size(),
                           source_state.path.end());
      const auto destination =
          std::ranges::find_if(target.enums, [&](const EnumState &candidate) {
            return candidate.path == relative_path;
          });
      if (destination != target.enums.end()) {
        destination->possible_variants = source_state.possible_variants;
        destination->unknown = source_state.unknown;
      }
    }
  }

  void setUnionStatesFromValue(RootState &target,
                               std::span<const PlaceProjection> target_path,
                               InstId value) {
    if (!value.hasValue() || value.index >= sem_ir_.instCount())
      return;
    const auto &source = sem_ir_.inst(value);
    if (source.kind == SemInstKind::Move || source.kind == SemInstKind::Copy) {
      setUnionStatesFromValue(target, target_path, InstId(source.arg0));
      return;
    }
    if (source.kind == SemInstKind::UnionInit) {
      const auto member = sem_ir_.integer(IntegerId(source.arg1));
      const auto state =
          std::ranges::find_if(target.unions, [&](const UnionState &candidate) {
            return candidate.path.size() == target_path.size() &&
                   std::ranges::equal(candidate.path, target_path);
          });
      if (state == target.unions.end() || member < 0 ||
          static_cast<std::size_t>(member) >= state->possible_members.size())
        return;
      std::ranges::fill(state->possible_members, 0U);
      state->possible_members[static_cast<std::size_t>(member)] = 1;
      state->unknown = false;
      auto member_path =
          std::vector<PlaceProjection>(target_path.begin(), target_path.end());
      member_path.push_back(
          {PlaceProjectionKind::Field, static_cast<std::uint32_t>(member)});
      setUnionStatesFromValue(target, member_path, InstId(source.arg0));
      return;
    }
    if (source.kind == SemInstKind::AggregateInit ||
        source.kind == SemInstKind::Closure) {
      const auto fields = sem_ir_.instBlock(InstBlockId(
          source.kind == SemInstKind::Closure ? source.arg1 : source.arg0));
      for (std::uint32_t index = 0; index < fields.size(); ++index) {
        auto field_path = std::vector<PlaceProjection>(target_path.begin(),
                                                       target_path.end());
        field_path.push_back({PlaceProjectionKind::Field, index});
        setUnionStatesFromValue(target, field_path, fields[index]);
      }
      return;
    }
    if (source.kind == SemInstKind::ArrayLiteral) {
      const auto elements = sem_ir_.instBlock(InstBlockId(source.arg0));
      for (std::uint32_t index = 0; index < elements.size(); ++index) {
        auto element_path = std::vector<PlaceProjection>(target_path.begin(),
                                                         target_path.end());
        element_path.push_back({PlaceProjectionKind::Element, index});
        setUnionStatesFromValue(target, element_path, elements[index]);
      }
      return;
    }
    const auto source_place = resolvePlace(value);
    if (!source_place)
      return;
    const auto &source_root = ensureRoot(source_place->root);
    std::vector<UnionState> copied;
    for (const auto &source_state : source_root.unions) {
      if (!isPrefix(source_place->path, source_state.path))
        continue;
      auto relative_path =
          std::vector<PlaceProjection>(target_path.begin(), target_path.end());
      relative_path.insert(relative_path.end(),
                           source_state.path.begin() +
                               source_place->path.size(),
                           source_state.path.end());
      auto copy = source_state;
      copy.path = std::move(relative_path);
      copied.push_back(std::move(copy));
    }
    for (const auto &copy : copied) {
      const auto destination =
          std::ranges::find_if(target.unions, [&](const UnionState &candidate) {
            return candidate.path == copy.path;
          });
      if (destination != target.unions.end()) {
        destination->possible_members = copy.possible_members;
        destination->unknown = copy.unknown;
      }
    }
  }

  SemPlaceId internPlace(const ResolvedPlace &place) {
    const auto key = placeKey(place.root, place.path);
    if (const auto found = place_ids_.find(key); found != place_ids_.end())
      return found->second;
    const auto id = SemPlaceId(static_cast<std::uint32_t>(places_.size()));
    places_.push_back({place.root, place.type, place.path});
    place_ids_.emplace(key, id);
    return id;
  }

  PlaceStateKind stateFor(const ResolvedPlace &place) {
    if (!place.path.empty() &&
        place.path.front().kind == PlaceProjectionKind::Dereference &&
        initialized_forwarded_parameters_.contains(place.root.index))
      return PlaceStateKind::Moved;
    const auto &root = ensureRoot(place.root);
    std::optional<std::uint8_t> common;
    for (const auto &leaf : root.leaves) {
      if (!isPrefix(place.path, leaf.path) && !isPrefix(leaf.path, place.path))
        continue;
      if (!common)
        common = leaf.possibilities;
      else if (*common != leaf.possibilities)
        return PlaceStateKind::PartiallyMoved;
    }
    if (common == (InitializedBit | MovedBit))
      return PlaceStateKind::MaybeMoved;
    return common == InitializedBit ? PlaceStateKind::Initialized
                                    : PlaceStateKind::Moved;
  }

  bool hasTrackedLeaf(const ResolvedPlace &place) {
    const auto &root = ensureRoot(place.root);
    return std::ranges::any_of(root.leaves, [&](const auto &leaf) {
      const auto union_leaf =
          sem_ir_.type(leaf.type).kind == SemTypeKind::Nominal &&
          sem_ir_.nominalType(NominalTypeId(sem_ir_.type(leaf.type).arg0))
                  .kind == NominalKind::Union;
      return isPrefix(place.path, leaf.path) ||
             (union_leaf && isPrefix(leaf.path, place.path));
    });
  }

  bool hasObservableLeaf(const ResolvedPlace &place) {
    const auto &root = ensureRoot(place.root);
    return std::ranges::any_of(root.leaves, [&](const auto &leaf) {
      return isPrefix(place.path, leaf.path) || isPrefix(leaf.path, place.path);
    });
  }

  bool checkUnionAccess(InstId instruction, const ResolvedPlace &place) {
    if (place.unsafe_union_access)
      return true;
    const auto &root = ensureRoot(place.root);
    for (const auto &state : root.unions) {
      if (!isPrefix(state.path, place.path) ||
          state.path.size() >= place.path.size())
        continue;
      const auto &projection = place.path[state.path.size()];
      if (projection.kind != PlaceProjectionKind::Field ||
          projection.index >= state.possible_members.size())
        continue;
      if (state.unknown) {
        diagnostics_.push_back(
            {instruction, PlaceStateDiagnosticKind::UnknownActiveUnionMember});
        return false;
      }
      const auto active_count = static_cast<std::size_t>(std::ranges::count(
          state.possible_members, static_cast<std::uint8_t>(1)));
      if (active_count != 1 || !state.possible_members[projection.index]) {
        diagnostics_.push_back(
            {instruction, PlaceStateDiagnosticKind::InactiveUnionMember});
        return false;
      }
    }
    return true;
  }

  bool checkEnumAccess(InstId instruction, const ResolvedPlace &place) {
    const auto &root = ensureRoot(place.root);
    for (const auto &state : root.enums) {
      if (!isPrefix(state.path, place.path) ||
          state.path.size() >= place.path.size())
        continue;
      const auto &projection = place.path[state.path.size()];
      if (projection.kind != PlaceProjectionKind::EnumPayload)
        continue;
      const auto active_count = static_cast<std::size_t>(std::ranges::count(
          state.possible_variants, static_cast<std::uint8_t>(1)));
      if (state.unknown || active_count != 1 ||
          projection.variant >= state.possible_variants.size() ||
          !state.possible_variants[projection.variant]) {
        diagnostics_.push_back(
            {instruction, PlaceStateDiagnosticKind::InvalidMoveOperand});
        return false;
      }
    }
    return true;
  }

