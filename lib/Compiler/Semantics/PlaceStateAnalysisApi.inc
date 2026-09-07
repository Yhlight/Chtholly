// Included by PlaceState.cpp; this fragment retains the shared place-state
// stores and worklists without introducing a second analysis context.
std::vector<PlaceStateDiagnostic> analyzePlaceStates(SemIR &sem_ir) {
  return analyzePlaceStates(sem_ir, nullptr);
}

std::vector<PlaceStateDiagnostic>
analyzePlaceStates(SemIR &sem_ir, PlaceStateAnalysisMetrics *metrics) {
  const auto started = std::chrono::steady_clock::now();
  auto result = PlaceStateAnalyzer(sem_ir).run();
  if (metrics) {
    metrics->place_work_items += sem_ir.instCount();
    metrics->elapsed_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
  }
  return result;
}

std::vector<PlaceStateDiagnostic>
analyzeInterproceduralLoans(const SemIR &sem_ir) {
  return analyzeInterproceduralLoans(sem_ir, nullptr);
}

std::vector<PlaceStateDiagnostic>
analyzeInterproceduralLoans(const SemIR &sem_ir,
                            PlaceStateAnalysisMetrics *metrics) {
  const auto started = std::chrono::steady_clock::now();
  std::vector<PlaceStateDiagnostic> diagnostics;
  bool analysis_non_convergent = false;

  struct Region {
    LocalId root;
    std::vector<std::uint64_t> path;
    bool bit_range = false;
    std::uint32_t bit_begin = 0;
    std::uint32_t bit_end = 0;
    bool widened = false;
  };
  struct Loan {
    InstId origin;
    Region region;
    bool mutable_borrow = false;
    std::vector<CallableReturnSource::CarrierStep> carrier_path;
  };

  const auto region_for = [&](const SemPlace &place) -> std::optional<Region> {
    Region region{place.root};
    auto current_type = sem_ir.local(place.root).type;
    for (const auto projection : place.projections) {
      if (projection.kind == PlaceProjectionKind::CarrierView) {
        current_type = TypeId(projection.index);
        continue;
      }
      if (projection.kind == PlaceProjectionKind::Dereference) {
        const auto &type = sem_ir.type(current_type);
        if (type.kind != SemTypeKind::Reference)
          return std::nullopt;
        current_type = sem_ir.referencePointee(current_type);
        continue;
      }
      if (projection.kind == PlaceProjectionKind::Element) {
        region.path.push_back((std::uint64_t{2} << 32U) | projection.index);
        const auto &type = sem_ir.type(current_type);
        if (type.kind != SemTypeKind::Array)
          return std::nullopt;
        current_type = TypeId(type.arg0);
        continue;
      }
      if (projection.kind == PlaceProjectionKind::AnyElement) {
        region.path.push_back(std::uint64_t{3} << 32U);
        const auto &type = sem_ir.type(current_type);
        if (type.kind != SemTypeKind::Array && type.kind != SemTypeKind::Slice)
          return std::nullopt;
        current_type = TypeId(type.arg0);
        continue;
      }
      const auto &type = sem_ir.type(current_type);
      if (type.kind != SemTypeKind::Nominal)
        return std::nullopt;
      const auto &nominal = sem_ir.nominalType(NominalTypeId(type.arg0));
      if (projection.index >= nominal.fields.size())
        return std::nullopt;
      const auto &field = nominal.fields[projection.index];
      if (nominal.kind == NominalKind::Union) {
        // All union alternatives denote the same storage region.
        region.path.push_back(std::uint64_t{4} << 32U);
      } else if (field.projection_kind ==
                 PublicObjectProjectionKind::Computed) {
        for (const auto component : field.projection_region_path)
          region.path.push_back((std::uint64_t{1} << 32U) | component.index);
      } else {
        const auto &storage = field.storage_path;
        if (storage.empty())
          region.path.push_back((std::uint64_t{1} << 32U) | field.name.index);
        else
          for (const auto component : storage)
            region.path.push_back((std::uint64_t{1} << 32U) | component.index);
        if (field.projection_kind == PublicObjectProjectionKind::BitPacked) {
          region.bit_range = true;
          region.bit_begin = field.bit_begin;
          region.bit_end = field.bit_end;
        }
      }
      current_type = sem_ir.nominalFieldType(current_type, projection.index);
    }
    return region;
  };
  const auto overlaps = [](const Region &lhs, const Region &rhs) {
    if (lhs.root != rhs.root)
      return false;
    const auto common = std::min(lhs.path.size(), rhs.path.size());
    for (std::size_t index = 0; index < common; ++index) {
      const auto lhs_kind = lhs.path[index] >> 32U;
      const auto rhs_kind = rhs.path[index] >> 32U;
      if (lhs_kind == 4 && rhs_kind == 4 && lhs.path[index] == rhs.path[index])
        return true;
      const auto element_wildcard =
          (lhs_kind == 3 && (rhs_kind == 2 || rhs_kind == 3)) ||
          (rhs_kind == 3 && (lhs_kind == 2 || lhs_kind == 3));
      if (!element_wildcard && lhs.path[index] != rhs.path[index])
        return false;
    }
    if (lhs.path.size() != rhs.path.size())
      return true;
    if (lhs.bit_range && rhs.bit_range)
      return std::max(lhs.bit_begin, rhs.bit_begin) <
             std::min(lhs.bit_end, rhs.bit_end);
    return true;
  };
  const auto project_region =
      [&](Region region, TypeId type,
          const OwnershipRegion &source) -> std::optional<Region> {
    if (region.widened)
      return region;
    const auto base_path_size = region.path.size();
    if (sem_ir.type(type).kind == SemTypeKind::Reference)
      type = sem_ir.referencePointee(type);
    for (const auto &step : source.path) {
      if (step.kind == OwnershipRegionStepKind::Dereference) {
        if (step.index != 0 || sem_ir.type(type).kind != SemTypeKind::Reference)
          return std::nullopt;
        type = sem_ir.referencePointee(type);
        continue;
      }
      if (step.kind == OwnershipRegionStepKind::StaticElement ||
          step.kind == OwnershipRegionStepKind::AnyElement) {
        if (sem_ir.type(type).kind != SemTypeKind::Array)
          return std::nullopt;
        region.path.push_back(
            (std::uint64_t{
                 step.kind == OwnershipRegionStepKind::AnyElement ? 3U : 2U}
             << 32U) |
            step.index);
        type = TypeId(sem_ir.type(type).arg0);
        continue;
      }
      if (step.kind != OwnershipRegionStepKind::Field ||
          sem_ir.type(type).kind != SemTypeKind::Nominal)
        return std::nullopt;
      const auto &nominal =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(type).arg0));
      if (step.index >= nominal.fields.size())
        return std::nullopt;
      const auto &field = nominal.fields[step.index];
      if (nominal.kind == NominalKind::Union) {
        region.path.push_back(std::uint64_t{4} << 32U);
      } else if (field.projection_kind ==
                 PublicObjectProjectionKind::Computed) {
        for (const auto component : field.projection_region_path)
          region.path.push_back((std::uint64_t{1} << 32U) | component.index);
      } else if (field.storage_path.empty()) {
        region.path.push_back((std::uint64_t{1} << 32U) | field.name.index);
      } else {
        for (const auto component : field.storage_path)
          region.path.push_back((std::uint64_t{1} << 32U) | component.index);
      }
      type = sem_ir.nominalFieldType(type, step.index);
    }
    if (source.has_bit_range) {
      region.bit_range = true;
      region.bit_begin = source.bit_begin;
      region.bit_end = source.bit_end;
    }
    const auto projected_size = region.path.size() - base_path_size;
    if (projected_size != 0 && base_path_size >= projected_size &&
        std::equal(region.path.begin() + base_path_size - projected_size,
                   region.path.begin() + base_path_size,
                   region.path.begin() + base_path_size)) {
      region.path.clear();
      region.bit_range = false;
      region.bit_begin = 0;
      region.bit_end = 0;
      region.widened = true;
    }
    return region;
  };

  for (std::uint32_t function_index = 0;
       function_index < sem_ir.functionCount(); ++function_index) {
    const auto &function = sem_ir.function(FunctionId(function_index));
    if ((function.flags & SemFunctionTemplate) != 0)
      continue;
    std::unordered_map<std::uint32_t, std::vector<InstId>> successors;
    const auto append_inst = [](std::vector<InstId> &target,
                                std::span<const InstId> source) {
      for (const auto id : source)
        if (std::ranges::find(target, id) == target.end())
          target.push_back(id);
    };
    struct LoopTargets {
      std::vector<InstId> breaks;
      std::vector<InstId> continues;
    };
    std::vector<LoopTargets> active_targets;
    const auto build_cfg =
        [&](auto &&self, InstBlockId block, std::vector<InstId> tail,
            std::vector<InstId> break_target,
            std::vector<InstId> continue_target) -> std::vector<InstId> {
      const auto cleanup_entries = [&](const PlaceCleanupPlan &plan,
                                       std::vector<InstId> successor) {
        for (auto action = plan.actions.rbegin(); action != plan.actions.rend();
             ++action)
          if (action->kind == PlaceCleanupKind::RunDefer)
            successor = self(self, action->block, std::move(successor), {}, {});
        return successor;
      };
      auto current = cleanup_entries(sem_ir.placeStates().blockCleanup(block),
                                     std::move(tail));
      const auto values = sem_ir.instBlock(block);
      for (auto position = values.rbegin(); position != values.rend();
           ++position) {
        const auto instruction = *position;
        const auto &inst = sem_ir.inst(instruction);
        auto next = current;
        if (inst.kind == SemInstKind::Return ||
            inst.kind == SemInstKind::CoroutineReturnSuccess ||
            inst.kind == SemInstKind::CoroutineReturnError ||
            inst.kind == SemInstKind::CoroutineReturnCancelled) {
          next = cleanup_entries(
              sem_ir.placeStates().returnCleanup(instruction), {});
        } else if (inst.kind == SemInstKind::Break) {
          const auto distance =
              static_cast<std::size_t>(sem_ir.integer(IntegerId(inst.arg0)));
          assert(distance < active_targets.size());
          next = cleanup_entries(
              sem_ir.placeStates().edgeCleanup(instruction),
              active_targets[active_targets.size() - 1 - distance].breaks);
        } else if (inst.kind == SemInstKind::Continue) {
          const auto distance =
              static_cast<std::size_t>(sem_ir.integer(IntegerId(inst.arg0)));
          assert(distance < active_targets.size());
          next = cleanup_entries(
              sem_ir.placeStates().edgeCleanup(instruction),
              active_targets[active_targets.size() - 1 - distance].continues);
        } else if (inst.kind == SemInstKind::If) {
          next.clear();
          const auto arms = sem_ir.instBlock(InstBlockId(inst.arg1));
          for (const auto arm_id : arms) {
            const auto &arm = sem_ir.getAs<SemIfArm>(arm_id);
            auto branch =
                self(self, arm.arg0, current, break_target, continue_target);
            append_inst(next, branch);
          }
          if (arms.size() == 1)
            append_inst(next, current);
        } else if (inst.kind == SemInstKind::Switch) {
          next.clear();
          for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
            const auto &arm = sem_ir.getAs<SemSwitchArm>(arm_id);
            auto branch =
                self(self, arm.arg1, current, break_target, continue_target);
            append_inst(next, branch);
          }
        } else if (inst.kind == SemInstKind::While) {
          const auto condition = sem_ir.instBlock(InstBlockId(inst.arg0));
          if (!condition.empty()) {
            const auto condition_entry = condition.front();
            active_targets.push_back({current, {condition_entry}});
            auto body = self(self, InstBlockId(inst.arg1), {condition_entry},
                             current, {condition_entry});
            active_targets.pop_back();
            append_inst(body, current);
            next = self(self, InstBlockId(inst.arg0), std::move(body),
                        break_target, continue_target);
          }
        } else if (inst.kind == SemInstKind::For) {
          const auto clauses = sem_ir.instBlock(InstBlockId(inst.arg0));
          const auto init =
              InstBlockId(sem_ir.getAs<SemForClause>(clauses[0]).arg1);
          const auto condition =
              InstBlockId(sem_ir.getAs<SemForClause>(clauses[1]).arg1);
          const auto step =
              InstBlockId(sem_ir.getAs<SemForClause>(clauses[2]).arg1);
          const auto condition_values = sem_ir.instBlock(condition);
          const std::vector condition_entry{condition_values.front()};
          const auto step_entries =
              self(self, step, condition_entry, break_target, continue_target);
          active_targets.push_back({current, step_entries});
          auto body_entries = self(self, InstBlockId(inst.arg1), step_entries,
                                   current, step_entries);
          active_targets.pop_back();
          append_inst(body_entries, current);
          const auto condition_entries =
              self(self, condition, std::move(body_entries), break_target,
                   continue_target);
          next = self(self, init, condition_entries, break_target,
                      continue_target);
        } else if (inst.kind == SemInstKind::DoWhile) {
          const auto body_values = sem_ir.instBlock(InstBlockId(inst.arg1));
          const auto condition_values =
              sem_ir.instBlock(InstBlockId(inst.arg0));
          if (!condition_values.empty()) {
            std::vector<InstId> condition_tail{body_values.empty()
                                                   ? condition_values.front()
                                                   : body_values.front()};
            append_inst(condition_tail, current);
            const auto condition_entries =
                self(self, InstBlockId(inst.arg0), std::move(condition_tail),
                     break_target, continue_target);
            if (body_values.empty())
              next = condition_entries;
            else {
              active_targets.push_back({current, condition_entries});
              next = self(self, InstBlockId(inst.arg1), condition_entries,
                          current, condition_entries);
              active_targets.pop_back();
            }
          }
        } else if (inst.kind == SemInstKind::CoroutineTaskScope) {
          next = self(self, InstBlockId(inst.arg0), current, break_target,
                      continue_target);
        }
        append_inst(successors[instruction.index], next);
        current = {instruction};
      }
      return current;
    };
    const auto entries = build_cfg(build_cfg, function.body, {}, {}, {});
    if (entries.empty())
      continue;

    std::vector<InstId> order;
    std::unordered_set<std::uint32_t> reachable;
    DenseWorklist<InstId> reachability_pending(
        sem_ir.instCount(), DenseWorklistOrder::LIFO);
    for (const auto entry : entries)
      reachability_pending.push(entry);
    while (const auto next = reachability_pending.pop()) {
      const auto instruction = *next;
      if (!reachable.insert(instruction.index).second)
        continue;
      order.push_back(instruction);
      for (const auto successor : successors[instruction.index])
        reachability_pending.push(successor);
    }
    std::ranges::sort(order, {}, &InstId::index);
    std::unordered_map<std::uint32_t, std::vector<InstId>> predecessors;
    for (const auto instruction : order)
      for (const auto successor : successors[instruction.index])
        predecessors[successor.index].push_back(instruction);

    using LoanSet = std::vector<std::uint32_t>;
    using LoanState = std::unordered_map<std::uint32_t, LoanSet>;
    std::vector<Loan> loans;
    std::unordered_map<std::uint32_t, LoanSet> borrow_loans;
    std::unordered_map<std::string, std::uint32_t> derived_call_loans;
    const auto append_unique = [](std::vector<std::uint32_t> &target,
                                  std::span<const std::uint32_t> source) {
      for (const auto id : source)
        if (std::ranges::find(target, id) == target.end())
          target.push_back(id);
    };

    std::unordered_map<std::uint32_t, SemPlaceId> observed_places;
    for (const auto &observation : sem_ir.placeStates().observations())
      if (reachable.contains(observation.instruction.index)) {
        observed_places.emplace(observation.instruction.index,
                                observation.place);
      }
    for (const auto instruction : order) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind != SemInstKind::BorrowLocal &&
          inst.kind != SemInstKind::BorrowPlace)
        continue;
      const auto observed = observed_places.find(instruction.index);
      if (observed == observed_places.end())
        continue;
      if (auto region =
              region_for(sem_ir.placeStates().place(observed->second))) {
        const auto loan = static_cast<std::uint32_t>(loans.size());
        loans.push_back({instruction,
                         std::move(*region),
                         sem_ir.referenceMutability(TypeId(inst.type)) ==
                             SemReferenceMutability::Mutable,
                         {}});
        borrow_loans[instruction.index] = {loan};
      }
    }

    const auto normalize_state = [](LoanState &state) {
      for (auto &[local, values] : state) {
        (void)local;
        std::ranges::sort(values);
        values.erase(std::unique(values.begin(), values.end()), values.end());
      }
    };
    std::unordered_map<std::uint32_t, LoanState> in_states;
    std::unordered_map<std::uint32_t, LoanState> out_states;
    std::unordered_map<std::uint32_t, LoanSet> inst_loans;
    std::unordered_map<std::string, std::uint32_t> projected_carrier_loans;
    const auto carries_borrow = [&](TypeId type) {
      return sem_ir.loanCarrierCapability(type) !=
             SemLoanCarrierCapability::None;
    };
    std::unordered_map<std::uint32_t, std::vector<InstId>> loan_consumers;
    const auto add_consumer = [&](InstId source, InstId consumer) {
      if (!source.hasValue())
        return;
      auto &values = loan_consumers[source.index];
      if (std::ranges::find(values, consumer) == values.end())
        values.push_back(consumer);
    };
    for (const auto instruction : order) {
      const auto &inst = sem_ir.inst(instruction);
      for (std::size_t arg = 0; arg < 2; ++arg)
        if (semInstArgKind(inst.kind, arg) == SemArgKind::Inst)
          add_consumer(InstId(arg == 0 ? inst.arg0 : inst.arg1), instruction);
      if (inst.kind == SemInstKind::Call ||
          inst.kind == SemInstKind::ForeignOperationCall ||
          inst.kind == SemInstKind::CompilerIntrinsicCall ||
          inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
          inst.kind == SemInstKind::CallbackAdapterCall ||
          inst.kind == SemInstKind::MakeCallbackRegistration ||
          inst.kind == SemInstKind::AggregateInit ||
          inst.kind == SemInstKind::Closure ||
          inst.kind == SemInstKind::ArrayLiteral) {
        const auto block =
            inst.kind == SemInstKind::Call ||
                    inst.kind == SemInstKind::ForeignOperationCall ||
                    inst.kind == SemInstKind::CompilerIntrinsicCall ||
                    inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
                    inst.kind == SemInstKind::CallbackAdapterCall
                ? inst.arg1
            : inst.kind == SemInstKind::Closure ? inst.arg1
                                                : inst.arg0;
        for (const auto value : sem_ir.instBlock(InstBlockId(block)))
          add_consumer(value, instruction);
      }
    }
    for (auto &[source, values] : loan_consumers) {
      (void)source;
      std::ranges::sort(values, {}, &InstId::index);
    }
    DenseWorklist<InstId> flow_worklist(sem_ir.instCount());
    for (const auto instruction : order)
      flow_worklist.push(instruction);
    const auto flow_limit = std::max<std::size_t>(
        4096, 8 * (MaxOwnershipRegionPath + 1) * (order.size() + 1));
    while (const auto pending = flow_worklist.pop()) {
      const auto instruction = *pending;
      if (flow_worklist.processedCount() > flow_limit) {
        analysis_non_convergent = true;
        break;
      }
      if (metrics)
        ++metrics->loan_flow_work_items;
      LoanState input_state;
      for (const auto predecessor : predecessors[instruction.index])
        for (const auto &[local, values] : out_states[predecessor.index])
          append_unique(input_state[local], values);
      normalize_state(input_state);
      if (in_states[instruction.index] != input_state) {
        in_states[instruction.index] = input_state;
        if (metrics)
          ++metrics->state_changes;
      }

      const auto &inst = sem_ir.inst(instruction);
      LoanSet inputs;
      const auto append_inst_loans = [&](InstId value) {
        if (const auto found = inst_loans.find(value.index);
            found != inst_loans.end())
          append_unique(inputs, found->second);
      };
      for (std::size_t arg = 0; arg < 2; ++arg)
        if (semInstArgKind(inst.kind, arg) == SemArgKind::Inst)
          append_inst_loans(InstId(arg == 0 ? inst.arg0 : inst.arg1));
      if (inst.kind == SemInstKind::Call ||
          inst.kind == SemInstKind::ForeignOperationCall ||
          inst.kind == SemInstKind::CompilerIntrinsicCall ||
          inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
          inst.kind == SemInstKind::CallbackAdapterCall ||
          inst.kind == SemInstKind::MakeCallbackRegistration ||
          inst.kind == SemInstKind::AggregateInit ||
          inst.kind == SemInstKind::Closure ||
          inst.kind == SemInstKind::ArrayLiteral) {
        const auto block =
            inst.kind == SemInstKind::Call ||
                    inst.kind == SemInstKind::ForeignOperationCall ||
                    inst.kind == SemInstKind::CompilerIntrinsicCall ||
                    inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
                    inst.kind == SemInstKind::CallbackAdapterCall
                ? inst.arg1
            : inst.kind == SemInstKind::Closure ? inst.arg1
                                                : inst.arg0;
        for (const auto value : sem_ir.instBlock(InstBlockId(block)))
          append_inst_loans(value);
      }

      LoanSet result;
      if (inst.kind == SemInstKind::NameRef) {
        result = input_state[inst.arg0];
      } else if (const auto found = borrow_loans.find(instruction.index);
                 found != borrow_loans.end()) {
        result = found->second;
      } else if (carries_borrow(TypeId(inst.type))) {
        if (inst.kind == SemInstKind::Call ||
            inst.kind == SemInstKind::ForeignOperationCall ||
            inst.kind == SemInstKind::CompilerIntrinsicCall ||
            inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
            inst.kind == SemInstKind::CallbackAdapterCall) {
          const auto arguments = sem_ir.instBlock(InstBlockId(inst.arg1));
          std::optional<CallableOwnershipSummary> adapter_summary;
          const auto *summary = [&]() -> const CallableOwnershipSummary * {
            if (inst.kind == SemInstKind::ForeignOperationCall) {
              adapter_summary = foreignOutcomeCallOwnershipSummary(
                  sem_ir, FunctionRefId(inst.arg0), TypeId(inst.type));
              if (adapter_summary)
                return &*adapter_summary;
            }
            if (inst.kind == SemInstKind::Call ||
                inst.kind == SemInstKind::ForeignOperationCall ||
                inst.kind == SemInstKind::CompilerIntrinsicCall)
              return callableOwnershipSummary(sem_ir, FunctionRefId(inst.arg0));
            if (inst.kind == SemInstKind::IndirectCall) {
      adapter_summary = indirectCallOwnershipSummary(sem_ir, TypeId(sem_ir.inst(InstId(inst.arg0)).type));
      return &*adapter_summary;
    }
    if (inst.kind == SemInstKind::IndirectForeignCall)
              return &sem_ir.callbackContract(
                  TypeId(sem_ir.inst(InstId(inst.arg0)).type));
            adapter_summary = callbackAdapterCallOwnershipSummary(
                sem_ir, TypeId(sem_ir.inst(InstId(inst.arg0)).type));
            return adapter_summary ? &*adapter_summary : nullptr;
          }();
          if (summary && !summary->return_provenance.empty()) {
            for (std::uint32_t source_index = 0;
                 source_index < summary->return_provenance.size();
                 ++source_index) {
              const auto &source = summary->return_provenance[source_index];
              if (source.region.parameter_index >= arguments.size() ||
                  !guardMayMatch(sem_ir, source, arguments))
                continue;
              const auto argument = arguments[source.region.parameter_index];
              for (const auto base_loan : inst_loans[argument.index]) {
                auto region = std::optional<Region>(loans[base_loan].region);
                if (!source.region.path.empty() || source.region.has_bit_range)
                  region = project_region(std::move(*region),
                                          TypeId(sem_ir.inst(argument).type),
                                          source.region);
                if (!region)
                  continue;
                if (region->path.size() > MaxOwnershipRegionPath) {
                  region->path.clear();
                  region->bit_range = false;
                  region->bit_begin = 0;
                  region->bit_end = 0;
                  region->widened = true;
                }
                std::string key;
                const auto append_u32 = [&](std::uint32_t value) {
                  for (std::uint32_t shift = 0; shift != 32; shift += 8)
                    key.push_back(static_cast<char>((value >> shift) & 0xffU));
                };
                append_u32(region->root.index);
                append_u32(static_cast<std::uint32_t>(region->path.size()));
                for (const auto component : region->path) {
                  append_u32(static_cast<std::uint32_t>(component));
                  append_u32(static_cast<std::uint32_t>(component >> 32U));
                }
                append_u32(region->bit_range ? 1U : 0U);
                append_u32(region->bit_begin);
                append_u32(region->bit_end);
                append_u32(region->widened ? 1U : 0U);
                append_u32(
                    static_cast<std::uint32_t>(source.carrier_path.size()));
                for (const auto &step : source.carrier_path) {
                  append_u32(static_cast<std::uint32_t>(step.kind));
                  append_u32(step.index);
                }
                const auto mutable_borrow =
                    sem_ir.loanCarrierCapability(TypeId(inst.type)) ==
                    SemLoanCarrierCapability::Mutable;
                append_u32(mutable_borrow ? 1U : 0U);
                auto derived_found = derived_call_loans.find(key);
                if (derived_found == derived_call_loans.end()) {
                  const auto id = static_cast<std::uint32_t>(loans.size());
                  if (metrics && region->widened)
                    ++metrics->loan_region_widenings;
                  loans.push_back({instruction, std::move(*region),
                                   mutable_borrow, source.carrier_path});
                  derived_found =
                      derived_call_loans.emplace(std::move(key), id).first;
                }
                const auto derived = derived_found->second;
                append_unique(result, std::span(&derived, 1));
              }
            }
          }
        } else if (inst.kind == SemInstKind::StructFieldAccess ||
                   inst.kind == SemInstKind::EnumPayloadAccess) {
          std::uint32_t variant = core::AnyId::InvalidIndex;
          std::uint32_t field = core::AnyId::InvalidIndex;
          if (inst.kind == SemInstKind::StructFieldAccess) {
            const auto raw = sem_ir.integer(IntegerId(inst.arg1));
            if (raw >= 0)
              field = static_cast<std::uint32_t>(raw);
          } else {
            const auto encoded = static_cast<std::uint64_t>(
                sem_ir.integer(IntegerId(inst.arg1)));
            variant = static_cast<std::uint32_t>(encoded >> 32U);
            field = static_cast<std::uint32_t>(encoded);
          }
          for (const auto input_loan : inputs) {
            const auto &loan = loans[input_loan];
            std::size_t consumed = 0;
            if (loan.carrier_path.empty()) {
              append_unique(result, std::span(&input_loan, 1));
              continue;
            }
            if (inst.kind == SemInstKind::EnumPayloadAccess &&
                loan.carrier_path.size() >= 2 &&
                loan.carrier_path[0].kind ==
                    CallableReturnSource::CarrierStepKind::EnumVariant &&
                loan.carrier_path[0].index == variant &&
                loan.carrier_path[1].kind ==
                    CallableReturnSource::CarrierStepKind::Field &&
                loan.carrier_path[1].index == field) {
              consumed = 2;
            } else if (inst.kind == SemInstKind::StructFieldAccess &&
                       loan.carrier_path[0].kind ==
                           CallableReturnSource::CarrierStepKind::Field &&
                       loan.carrier_path[0].index == field) {
              consumed = 1;
            } else {
              continue;
            }
            std::string key =
                std::to_string(input_loan) + ":" + std::to_string(consumed);
            auto projected_found = projected_carrier_loans.find(key);
            if (projected_found == projected_carrier_loans.end()) {
              auto projected = loan;
              projected.origin = instruction;
              projected.carrier_path.erase(projected.carrier_path.begin(),
                                           projected.carrier_path.begin() +
                                               consumed);
              const auto id = static_cast<std::uint32_t>(loans.size());
              loans.push_back(std::move(projected));
              projected_found =
                  projected_carrier_loans.emplace(std::move(key), id).first;
            }
            const auto projected = projected_found->second;
            append_unique(result, std::span(&projected, 1));
          }
        } else {
          result = inputs;
        }
      }
      std::ranges::sort(result);
      result.erase(std::unique(result.begin(), result.end()), result.end());
      if (inst_loans[instruction.index] != result) {
        inst_loans[instruction.index] = result;
        if (metrics)
          ++metrics->state_changes;
        if (const auto found = loan_consumers.find(instruction.index);
            found != loan_consumers.end())
          for (const auto consumer : found->second)
            flow_worklist.push(consumer);
      }

      auto output_state = input_state;
      if (inst.kind == SemInstKind::BindName &&
          carries_borrow(sem_ir.local(LocalId(inst.arg0)).type)) {
        output_state[inst.arg0] = inst_loans[inst.arg1];
      } else if (inst.kind == SemInstKind::Assign) {
        const auto &target = sem_ir.inst(InstId(inst.arg0));
        if (target.kind == SemInstKind::NameRef &&
            carries_borrow(sem_ir.local(LocalId(target.arg0)).type))
          output_state[target.arg0] = inst_loans[inst.arg1];
      }
      normalize_state(output_state);
      if (out_states[instruction.index] != output_state) {
        out_states[instruction.index] = std::move(output_state);
        if (metrics)
          ++metrics->state_changes;
        for (const auto successor : successors[instruction.index])
          flow_worklist.push(successor);
      }
    }
    if (metrics)
      metrics->max_worklist_depth = std::max<std::uint64_t>(
          metrics->max_worklist_depth, flow_worklist.peakPendingCount());
    if (analysis_non_convergent)
      break;

    using LiveSet = std::unordered_set<std::uint32_t>;
    std::unordered_set<std::uint32_t> assignment_targets;
    for (const auto instruction : order) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind == SemInstKind::Assign)
        assignment_targets.insert(inst.arg0);
    }
    std::unordered_map<std::uint32_t, LiveSet> live_inst_in;
    std::unordered_map<std::uint32_t, LiveSet> live_inst_out;
    std::unordered_map<std::uint32_t, LiveSet> live_local_in;
    std::unordered_map<std::uint32_t, LiveSet> live_local_out;
    DenseWorklist<InstId> live_worklist(sem_ir.instCount());
    for (auto position = order.rbegin(); position != order.rend(); ++position)
      live_worklist.push(*position);
    const auto live_limit =
        std::max<std::size_t>(4096, 64 * (order.size() + 1));
    while (const auto pending = live_worklist.pop()) {
      const auto instruction = *pending;
      if (live_worklist.processedCount() > live_limit) {
        analysis_non_convergent = true;
        break;
      }
      if (metrics)
        ++metrics->liveness_work_items;
      const auto &inst = sem_ir.inst(instruction);
      LiveSet inst_out;
      LiveSet local_out;
      for (const auto successor : successors[instruction.index]) {
        inst_out.insert(live_inst_in[successor.index].begin(),
                        live_inst_in[successor.index].end());
        local_out.insert(live_local_in[successor.index].begin(),
                         live_local_in[successor.index].end());
      }
      auto inst_in = inst_out;
      inst_in.erase(instruction.index);
      const auto add_use = [&](std::uint32_t raw) {
        if (raw < sem_ir.instCount() &&
            carries_borrow(TypeId(sem_ir.inst(InstId(raw)).type)))
          inst_in.insert(raw);
      };
      for (std::size_t arg = 0; arg < 2; ++arg)
        if (semInstArgKind(inst.kind, arg) == SemArgKind::Inst &&
            !(inst.kind == SemInstKind::Assign && arg == 0))
          add_use(arg == 0 ? inst.arg0 : inst.arg1);
      if (inst.kind == SemInstKind::Call ||
          inst.kind == SemInstKind::ForeignOperationCall ||
          inst.kind == SemInstKind::CompilerIntrinsicCall ||
          inst.kind == SemInstKind::MakeCallbackRegistration ||
          inst.kind == SemInstKind::AggregateInit ||
          inst.kind == SemInstKind::Closure ||
          inst.kind == SemInstKind::ArrayLiteral) {
        const auto block = (inst.kind == SemInstKind::Call ||
                            inst.kind == SemInstKind::ForeignOperationCall ||
                            inst.kind == SemInstKind::CompilerIntrinsicCall)
                               ? inst.arg1
                           : inst.kind == SemInstKind::Closure ? inst.arg1
                                                               : inst.arg0;
        for (const auto value : sem_ir.instBlock(InstBlockId(block)))
          add_use(value.index);
      }
      auto local_in = local_out;
      if (inst.kind == SemInstKind::BindName)
        local_in.erase(inst.arg0);
      else if (inst.kind == SemInstKind::Assign) {
        const auto &target = sem_ir.inst(InstId(inst.arg0));
        if (target.kind == SemInstKind::NameRef)
          local_in.erase(target.arg0);
      }
      if (inst.kind == SemInstKind::NameRef &&
          !assignment_targets.contains(instruction.index) &&
          carries_borrow(sem_ir.local(LocalId(inst.arg0)).type))
        local_in.insert(inst.arg0);
      if (live_inst_out[instruction.index] != inst_out ||
          live_inst_in[instruction.index] != inst_in ||
          live_local_out[instruction.index] != local_out ||
          live_local_in[instruction.index] != local_in) {
        live_inst_out[instruction.index] = std::move(inst_out);
        live_inst_in[instruction.index] = std::move(inst_in);
        live_local_out[instruction.index] = std::move(local_out);
        live_local_in[instruction.index] = std::move(local_in);
        if (metrics)
          ++metrics->state_changes;
        for (const auto predecessor : predecessors[instruction.index])
          live_worklist.push(predecessor);
      }
    }
    if (metrics)
      metrics->max_worklist_depth = std::max<std::uint64_t>(
          metrics->max_worklist_depth, live_worklist.peakPendingCount());
    if (analysis_non_convergent)
      break;

    for (const auto &observation : sem_ir.placeStates().observations()) {
      if (!reachable.contains(observation.instruction.index))
        continue;
      const auto operation_region =
          region_for(sem_ir.placeStates().place(observation.place));
      if (!operation_region)
        continue;
      const auto &inst = sem_ir.inst(observation.instruction);
      if (inst.kind == SemInstKind::Call ||
          inst.kind == SemInstKind::ForeignOperationCall ||
          inst.kind == SemInstKind::CompilerIntrinsicCall)
        continue;
      const auto new_borrow = observation.kind == PlaceObservationKind::Borrow;
      const auto new_mutable =
          new_borrow &&
          sem_ir.type(TypeId(inst.type)).kind == SemTypeKind::Reference &&
          sem_ir.referenceMutability(TypeId(inst.type)) ==
              SemReferenceMutability::Mutable;
      const auto mutating =
          observation.kind == PlaceObservationKind::Move ||
          observation.kind == PlaceObservationKind::Reinitialize;
      const auto reading = observation.kind == PlaceObservationKind::Read ||
                           observation.kind == PlaceObservationKind::Copy;
      LoanSet active;
      for (const auto value : live_inst_in[observation.instruction.index])
        append_unique(active, inst_loans[value]);
      for (const auto local : live_local_in[observation.instruction.index])
        append_unique(active, in_states[observation.instruction.index][local]);
      bool conflict = false;
      InstId conflicting_origin = InstId::invalid();
      for (const auto loan_id : active) {
        const auto &loan = loans[loan_id];
        if (!overlaps(loan.region, *operation_region))
          continue;
        if ((new_borrow && (new_mutable || loan.mutable_borrow)) || mutating ||
            (reading && loan.mutable_borrow)) {
          conflict = true;
          conflicting_origin = loan.origin;
          break;
        }
      }
      if (conflict)
        diagnostics.push_back(
            {observation.instruction, PlaceStateDiagnosticKind::BorrowConflict,
             {{conflicting_origin,
               OwnershipEvidenceKind::BorrowOrigin,
               0, "live overlapping ownership region"}}});
    }

    for (const auto instruction : order) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind != SemInstKind::Call &&
          inst.kind != SemInstKind::ForeignOperationCall &&
          inst.kind != SemInstKind::CompilerIntrinsicCall &&
          inst.kind != SemInstKind::IndirectCall &&
          inst.kind != SemInstKind::IndirectForeignCall &&
          inst.kind != SemInstKind::CallbackAdapterCall &&
          inst.kind != SemInstKind::MakeCallbackRegistration)
        continue;
      std::optional<CallableOwnershipSummary> adapter_summary;
      const auto *summary = [&]() -> const CallableOwnershipSummary * {
        if (inst.kind == SemInstKind::ForeignOperationCall) {
          adapter_summary = foreignOutcomeCallOwnershipSummary(
              sem_ir, FunctionRefId(inst.arg0), TypeId(inst.type));
          if (adapter_summary)
            return &*adapter_summary;
        }
        if (inst.kind == SemInstKind::Call ||
            inst.kind == SemInstKind::ForeignOperationCall ||
            inst.kind == SemInstKind::CompilerIntrinsicCall)
          return callableOwnershipSummary(sem_ir, FunctionRefId(inst.arg0));
        if (inst.kind == SemInstKind::IndirectCall) {
      adapter_summary = indirectCallOwnershipSummary(sem_ir, TypeId(sem_ir.inst(InstId(inst.arg0)).type));
      return &*adapter_summary;
    }
    if (inst.kind == SemInstKind::IndirectForeignCall)
          return &sem_ir.callbackContract(
              TypeId(sem_ir.inst(InstId(inst.arg0)).type));
        if (inst.kind == SemInstKind::MakeCallbackRegistration) {
          adapter_summary =
              callbackRegistrationCallOwnershipSummary(sem_ir, instruction);
          return adapter_summary ? &*adapter_summary : nullptr;
        }
        adapter_summary = callbackAdapterCallOwnershipSummary(
            sem_ir, TypeId(sem_ir.inst(InstId(inst.arg0)).type));
        return adapter_summary ? &*adapter_summary : nullptr;
      }();
      if (!summary || summary->effects.empty())
        continue;
      LoanSet active;
      for (const auto value : live_inst_in[instruction.index])
        append_unique(active, inst_loans[value]);
      for (const auto local : live_local_in[instruction.index])
        append_unique(active, in_states[instruction.index][local]);

      const auto arguments = sem_ir.instBlock(InstBlockId(
          inst.kind == SemInstKind::MakeCallbackRegistration ? inst.arg0
                                                             : inst.arg1));
      std::vector<TypeId> adapter_parameter_types;
      std::span<const TypeId> parameter_types;
      if (inst.kind == SemInstKind::CallbackAdapterCall) {
        const auto adapter_type = TypeId(sem_ir.inst(InstId(inst.arg0)).type);
        const auto fields =
            sem_ir.typeBlock(TypeBlockId(sem_ir.type(adapter_type).arg0));
        const auto entry_parameters =
            sem_ir.typeBlock(TypeBlockId(sem_ir.type(fields[0]).arg0));
        const auto context = sem_ir.callbackContextParameter(fields[0]);
        for (std::size_t index = 0; index < entry_parameters.size(); ++index)
          if (index != context)
            adapter_parameter_types.push_back(entry_parameters[index]);
        parameter_types = adapter_parameter_types;
      } else if (inst.kind == SemInstKind::MakeCallbackRegistration) {
        for (const auto argument : arguments)
          adapter_parameter_types.push_back(TypeId(sem_ir.inst(argument).type));
        parameter_types = adapter_parameter_types;
      } else {
        const auto callee_type =
            (inst.kind == SemInstKind::Call ||
             inst.kind == SemInstKind::ForeignOperationCall ||
             inst.kind == SemInstKind::CompilerIntrinsicCall)
                ? sem_ir.functionRef(FunctionRefId(inst.arg0)).local_type
                : TypeId(sem_ir.inst(InstId(inst.arg0)).type);
        parameter_types =
            sem_ir.typeBlock(TypeBlockId(sem_ir.type(callee_type).arg0));
      }
      bool conflict = false;
      InstId conflicting_origin = InstId::invalid();
      for (const auto &effect : summary->effects) {
        if (metrics)
          ++metrics->call_conflict_checks;
        if (effect.region.parameter_index >= arguments.size() ||
            effect.region.parameter_index >= parameter_types.size())
          continue;
        const auto parameter_type =
            parameter_types[effect.region.parameter_index];
        if (sem_ir.type(parameter_type).kind != SemTypeKind::Reference &&
            std::ranges::none_of(effect.region.path, [](const auto &step) {
              return step.kind == OwnershipRegionStepKind::Dereference;
            }))
          continue;
        const auto argument = arguments[effect.region.parameter_index];
        std::vector<Region> bases;
        const auto &supplied_loans = inst_loans[argument.index];
        for (const auto loan : supplied_loans)
          bases.push_back(loans[loan].region);
        if (const auto observed = observed_places.find(argument.index);
            observed != observed_places.end())
          if (auto region =
                  region_for(sem_ir.placeStates().place(observed->second)))
            bases.push_back(std::move(*region));
        for (auto &base : bases) {
          const auto operation =
              project_region(std::move(base),
                             TypeId(sem_ir.inst(argument).type), effect.region);
          if (!operation)
            continue;
          for (const auto loan_id : active) {
            if (std::ranges::find(supplied_loans, loan_id) !=
                supplied_loans.end())
              continue;
            const auto &loan = loans[loan_id];
            if (!overlaps(loan.region, *operation))
              continue;
            const auto incompatible =
                effect.kind == CallableEffectKind::Write ||
                effect.kind == CallableEffectKind::Initialize ||
                effect.kind == CallableEffectKind::Take ||
                effect.kind == CallableEffectKind::BorrowMutable ||
                ((effect.kind == CallableEffectKind::Read ||
                  effect.kind == CallableEffectKind::BorrowShared) &&
                 loan.mutable_borrow);
            if (incompatible) {
              conflict = true;
              conflicting_origin = loan.origin;
              break;
            }
          }
          if (conflict)
            break;
        }
        if (conflict)
          break;
      }
      const auto return_mutable =
          sem_ir.loanCarrierCapability(TypeId(inst.type)) ==
          SemLoanCarrierCapability::Mutable;
      if (!conflict && !summary->return_provenance.empty()) {
        for (const auto &source : summary->return_provenance) {
          if (metrics)
            ++metrics->call_conflict_checks;
          if (source.region.parameter_index >= arguments.size() ||
              !guardMayMatch(sem_ir, source, arguments))
            continue;
          const auto argument = arguments[source.region.parameter_index];
          const auto &supplied_loans = inst_loans[argument.index];
          for (const auto supplied : supplied_loans) {
            auto operation = loans[supplied].region;
            if (!source.region.path.empty() || source.region.has_bit_range) {
              const auto projected = project_region(
                  std::move(operation), TypeId(sem_ir.inst(argument).type),
                  source.region);
              if (!projected)
                continue;
              operation = std::move(*projected);
            }
            for (const auto loan_id : active) {
              if (std::ranges::find(supplied_loans, loan_id) !=
                  supplied_loans.end())
                continue;
              const auto &loan = loans[loan_id];
              if (overlaps(loan.region, operation) &&
                  (return_mutable || loan.mutable_borrow)) {
                conflict = true;
                break;
              }
            }
            if (conflict)
              break;
          }
          if (conflict)
            break;
        }
      }
      if (conflict)
        diagnostics.push_back(
            {instruction, PlaceStateDiagnosticKind::BorrowConflict,
             {{conflicting_origin, OwnershipEvidenceKind::BorrowOrigin, 0,
               "live borrow overlaps a callable effect"},
              {instruction, OwnershipEvidenceKind::CallEffect, 0,
               "callee effect requires incompatible access"}}});
    }
  }
  if (analysis_non_convergent)
    diagnostics.push_back(
        {InstId::invalid(), PlaceStateDiagnosticKind::AnalysisNonConvergent});
  if (metrics)
    metrics->elapsed_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
  return diagnostics;
}
