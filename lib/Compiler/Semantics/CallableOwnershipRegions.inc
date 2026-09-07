// Included by CallableOwnership.cpp; this fragment aliases the existing
// ownership graph, region, and summary state.
std::optional<std::uint32_t> parameterIndex(const SemIR &sem_ir,
                                            const SemFunction &function,
                                            LocalId local) {
  const auto parameters = sem_ir.localBlock(function.parameters);
  const auto found = std::ranges::find(parameters, local);
  if (found == parameters.end())
    return std::nullopt;
  return static_cast<std::uint32_t>(found - parameters.begin());
}

Regions canonicalizeRegions(Regions regions) {
  if (regions.empty())
    return regions;
  CallableOwnershipSummary summary;
  summary.returns_owned = false;
  for (auto &region : regions)
    summary.return_provenance.push_back(
        {.region = std::move(region.region),
         .condition = std::move(region.condition)});
  summary.canonicalize();
  Regions result;
  result.reserve(summary.return_provenance.size());
  for (auto &source : summary.return_provenance)
    result.push_back({std::move(source.region), std::move(source.condition)});
  return result;
}

void appendRegions(Regions &target, std::span<const ConditionedRegion> source) {
  target.insert(target.end(), source.begin(), source.end());
  target = canonicalizeRegions(std::move(target));
}

void mergeState(RegionState &target, const RegionState &source,
                const CallableConditionDescriptor &edge_condition =
                    CallableConditionDescriptor::always()) {
  for (const auto &[local, regions] : source) {
    Regions conditioned;
    for (const auto &region : regions) {
      auto condition = conditionAnd(region.condition, edge_condition);
      if (!condition.isNever())
        conditioned.push_back({region.region, std::move(condition)});
    }
    appendRegions(target[local], conditioned);
  }
}

std::optional<OwnershipRegion>
composeRegion(OwnershipRegion base, const OwnershipRegion &suffix,
              bool &path_overflow, FixedPointAnalysisMetrics *metrics) {
  const auto skip_root_dereference =
      !base.path.empty() && !suffix.path.empty() &&
      base.path.back().kind == OwnershipRegionStepKind::Dereference &&
      suffix.path.front().kind == OwnershipRegionStepKind::Dereference;
  const auto suffix_begin = skip_root_dereference ? 1U : 0U;
  if (base.path.size() + suffix.path.size() - suffix_begin >
      MaxOwnershipRegionPath) {
    (void)path_overflow;
    if (metrics)
      ++metrics->region_widenings;
    base.path.clear();
    base.has_bit_range = false;
    base.bit_begin = 0;
    base.bit_end = 0;
    return base;
  }
  base.path.insert(base.path.end(), suffix.path.begin() + suffix_begin,
                   suffix.path.end());
  if (suffix.has_bit_range) {
    base.has_bit_range = true;
    base.bit_begin = suffix.bit_begin;
    base.bit_end = suffix.bit_end;
  }
  return base;
}

Regions projectField(const SemIR &sem_ir, InstId base_value,
                     std::uint32_t field, Regions regions, bool &path_overflow,
                     FixedPointAnalysisMetrics *metrics) {
  const auto base_type = TypeId(sem_ir.inst(base_value).type);
  const auto tuple = sem_ir.type(base_type).kind == SemTypeKind::Tuple;
  for (auto &conditioned : regions) {
    auto &region = conditioned.region;
    if (region.path.size() == MaxOwnershipRegionPath) {
      (void)path_overflow;
      if (metrics)
        ++metrics->region_widenings;
      region.path.clear();
      region.has_bit_range = false;
      region.bit_begin = 0;
      region.bit_end = 0;
      continue;
    }
    region.path.push_back({tuple ? OwnershipRegionStepKind::StaticElement
                                 : OwnershipRegionStepKind::Field,
                           field});
    if (sem_ir.type(base_type).kind != SemTypeKind::Nominal)
      continue;
    const auto &nominal =
        sem_ir.nominalType(NominalTypeId(sem_ir.type(base_type).arg0));
    if (field < nominal.fields.size() &&
        nominal.fields[field].projection_kind ==
            PublicObjectProjectionKind::BitPacked) {
      region.has_bit_range = true;
      region.bit_begin = nominal.fields[field].bit_begin;
      region.bit_end = nominal.fields[field].bit_end;
    }
  }
  return canonicalizeRegions(std::move(regions));
}

Regions projectElement(const SemIR &sem_ir, InstId index_value, Regions regions,
                       bool &path_overflow,
                       FixedPointAnalysisMetrics *metrics) {
  const auto &index = sem_ir.inst(index_value);
  const auto is_static =
      index.kind == SemInstKind::IntegerLiteral &&
      sem_ir.integer(IntegerId(index.arg0)) >= 0 &&
      static_cast<std::uint64_t>(sem_ir.integer(IntegerId(index.arg0))) <=
          std::numeric_limits<std::uint32_t>::max();
  const auto static_index =
      is_static
          ? static_cast<std::uint32_t>(sem_ir.integer(IntegerId(index.arg0)))
          : 0U;
  for (auto &conditioned : regions) {
    auto &region = conditioned.region;
    if (region.path.size() == MaxOwnershipRegionPath) {
      (void)path_overflow;
      if (metrics)
        ++metrics->region_widenings;
      region.path.clear();
      region.has_bit_range = false;
      region.bit_begin = 0;
      region.bit_end = 0;
      continue;
    }
    region.path.push_back({is_static ? OwnershipRegionStepKind::StaticElement
                                     : OwnershipRegionStepKind::AnyElement,
                           static_index});
  }
  return canonicalizeRegions(std::move(regions));
}

struct ProvenanceResult {
  std::unordered_map<std::uint32_t, Regions> instruction_regions;
  bool path_overflow = false;
};

CallableConditionDescriptor
substituteCondition(const SemIR &sem_ir, const SemFunction &caller,
                    const CallableConditionDescriptor &condition,
                    std::span<const InstId> arguments) {
  if (!condition.exact) return CallableConditionDescriptor::unknown();
  auto result = CallableConditionDescriptor::never();
  for (const auto &clause : condition.clauses) {
    auto substituted_clause = CallableConditionDescriptor::always();
    for (const auto &atom : clause.atoms) {
      if (atom.parameter_index >= arguments.size())
        return CallableConditionDescriptor::unknown();
      auto argument = atom.variant == core::AnyId::InvalidIndex
          ? conditionForValue(sem_ir, caller, arguments[atom.parameter_index])
          : std::optional(conditionForEnumParameter(sem_ir, caller, arguments[atom.parameter_index], atom.variant));
      if (!argument)
        return CallableConditionDescriptor::unknown();
      if (!atom.expected)
        *argument = conditionNot(*argument);
      substituted_clause =
          conditionAnd(std::move(substituted_clause), *argument);
    }
    result = conditionOr(std::move(result), substituted_clause);
  }
  return result;
}

ProvenanceResult analyzeProvenance(const SemIR &sem_ir,
                                   const SemFunction &function,
                                   const FunctionGraph &graph,
                                   FixedPointAnalysisMetrics *metrics) {
  ProvenanceResult result;
  RegionState initial_state;
  for (std::uint32_t parameter = 0;
       const auto local : sem_ir.localBlock(function.parameters))
    initial_state[local.index] = {
        {OwnershipRegion{.parameter_index = parameter++},
         CallableConditionDescriptor::always()}};

  std::unordered_set<std::uint32_t> entries;
  for (const auto entry : graph.entries)
    entries.insert(entry.index);
  std::unordered_map<std::uint32_t, RegionState> in_states;
  std::unordered_map<std::uint32_t, RegionState> out_states;
  std::unordered_map<std::uint32_t, std::vector<InstId>> consumers;
  const auto add_consumer = [&](InstId source, InstId consumer) {
    if (!source.hasValue())
      return;
    auto &values = consumers[source.index];
    if (std::ranges::find(values, consumer) == values.end())
      values.push_back(consumer);
  };
  for (const auto instruction : graph.instructions) {
    const auto &inst = sem_ir.inst(instruction);
    for (std::size_t arg = 0; arg < 2; ++arg)
      if (semInstArgKind(inst.kind, arg) == SemArgKind::Inst)
        add_consumer(InstId(arg == 0 ? inst.arg0 : inst.arg1), instruction);
    if (inst.kind == SemInstKind::Call ||
        inst.kind == SemInstKind::CompilerIntrinsicCall ||
        inst.kind == SemInstKind::ForeignOperationCall ||
        inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
        inst.kind == SemInstKind::CallbackAdapterCall ||
        inst.kind == SemInstKind::MakeCallbackRegistration ||
        inst.kind == SemInstKind::AggregateInit ||
        inst.kind == SemInstKind::ArrayLiteral ||
        inst.kind == SemInstKind::TupleLiteral ||
        inst.kind == SemInstKind::EnumInit ||
        inst.kind == SemInstKind::Closure) {
      const auto call_operands =
          inst.kind == SemInstKind::Call ||
                  inst.kind == SemInstKind::CompilerIntrinsicCall ||
                  inst.kind == SemInstKind::ForeignOperationCall ||
                  inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
                  inst.kind == SemInstKind::CallbackAdapterCall
              ? inst.arg1
          : inst.kind == SemInstKind::MakeCallbackRegistration ? inst.arg0
          : inst.kind == SemInstKind::Closure                  ? inst.arg1
                                                               : inst.arg0;
      for (const auto value : sem_ir.instBlock(InstBlockId(call_operands)))
        add_consumer(value, instruction);
    }
    const auto add_block_result = [&](InstBlockId block) {
      const auto body = sem_ir.instBlock(block);
      if (!body.empty())
        add_consumer(body.back(), instruction);
    };
    if (inst.kind == SemInstKind::ScopedBlock) {
      add_block_result(InstBlockId(inst.arg0));
    } else if (inst.kind == SemInstKind::If) {
      for (const auto arm : sem_ir.instBlock(InstBlockId(inst.arg1)))
        add_block_result(InstBlockId(sem_ir.inst(arm).arg0));
    } else if (inst.kind == SemInstKind::Switch) {
      for (const auto arm : sem_ir.instBlock(InstBlockId(inst.arg1)))
        add_block_result(InstBlockId(sem_ir.inst(arm).arg1));
    }
  }
  for (auto &[source, values] : consumers) {
    (void)source;
    std::ranges::sort(values, {}, &InstId::index);
  }

  DenseWorklist<InstId> worklist(sem_ir.instCount());
  for (const auto instruction : graph.instructions)
    worklist.push(instruction);
  std::size_t edge_count = 0;
  for (const auto &[instruction, predecessors] : graph.predecessors) {
    (void)instruction;
    edge_count += predecessors.size();
  }
  const auto limit = std::max<std::size_t>(
      4096, 64 * (graph.instructions.size() + edge_count + 1));
  while (const auto pending = worklist.pop()) {
    const auto instruction = *pending;
    if (worklist.processedCount() > limit) {
      result.path_overflow = true;
      break;
    }
    if (metrics)
      ++metrics->provenance_work_items;
    RegionState input;
    if (entries.contains(instruction.index))
      input = initial_state;
    if (const auto found = graph.predecessors.find(instruction.index);
        found != graph.predecessors.end())
      for (const auto predecessor : found->second)
        mergeState(input, out_states[predecessor.instruction.index],
                   predecessor.condition);
    if (in_states[instruction.index] != input) {
      in_states[instruction.index] = input;
      if (metrics)
        ++metrics->provenance_state_changes;
    }

    const auto &inst = sem_ir.inst(instruction);
    const auto inst_regions = [&](InstId id) -> Regions {
      if (const auto found = result.instruction_regions.find(id.index);
          found != result.instruction_regions.end())
        return found->second;
      return {};
    };
    Regions value;
    if (inst.kind == SemInstKind::Parameter) {
      value = input[inst.arg0];
    } else if (inst.kind == SemInstKind::NameRef ||
               inst.kind == SemInstKind::BorrowLocal) {
      value = input[inst.arg0];
    } else if (inst.kind == SemInstKind::Yield) {
      value = inst_regions(InstId(inst.arg0));
    } else if (inst.kind == SemInstKind::BorrowPlace ||
               inst.kind == SemInstKind::Move ||
               inst.kind == SemInstKind::Copy ||
               inst.kind == SemInstKind::CarrierView ||
               inst.kind == SemInstKind::CallbackRegistrationBinding) {
      value = inst_regions(InstId(inst.arg0));
    } else if (inst.kind == SemInstKind::Dereference) {
      value = inst_regions(InstId(inst.arg0));
      const auto operand_type = TypeId(sem_ir.inst(InstId(inst.arg0)).type);
      const auto transparent_reference =
          sem_ir.type(operand_type).kind == SemTypeKind::Reference;
      for (auto &conditioned : value) {
        if (conditioned.region.path.size() == MaxOwnershipRegionPath) {
          if (metrics)
            ++metrics->region_widenings;
          conditioned.region.path.clear();
          conditioned.region.has_bit_range = false;
          conditioned.region.bit_begin = 0;
          conditioned.region.bit_end = 0;
          continue;
        }
        const auto already_dereferenced =
            (transparent_reference && conditioned.region.path.empty()) ||
            (!conditioned.region.path.empty() &&
             conditioned.region.path.back().kind ==
                 OwnershipRegionStepKind::Dereference);
        if (!already_dereferenced)
          conditioned.region.path.push_back(
              {OwnershipRegionStepKind::Dereference, 0});
      }
    } else if (inst.kind == SemInstKind::StructFieldAccess) {
      const auto field_value = sem_ir.integer(IntegerId(inst.arg1));
      if (field_value >= 0 && static_cast<std::uint64_t>(field_value) <=
                                  std::numeric_limits<std::uint32_t>::max())
        value = projectField(
            sem_ir, InstId(inst.arg0), static_cast<std::uint32_t>(field_value),
            inst_regions(InstId(inst.arg0)), result.path_overflow, metrics);
    } else if (inst.kind == SemInstKind::Index) {
      value = projectElement(sem_ir, InstId(inst.arg1),
                             inst_regions(InstId(inst.arg0)),
                             result.path_overflow, metrics);
    } else if (inst.kind == SemInstKind::ArrayLiteral ||
               inst.kind == SemInstKind::TupleLiteral ||
               inst.kind == SemInstKind::AggregateInit ||
               inst.kind == SemInstKind::EnumInit ||
               inst.kind == SemInstKind::Closure) {
      const auto block = InstBlockId(
          inst.kind == SemInstKind::Closure ? inst.arg1 : inst.arg0);
      for (const auto element : sem_ir.instBlock(block))
        appendRegions(value, inst_regions(element));
      value = canonicalizeRegions(std::move(value));
    } else if (inst.kind == SemInstKind::BoundMethod) {
      value = inst_regions(InstId(inst.arg0));
    } else if (inst.kind == SemInstKind::Slice) {
      const auto operands = sem_ir.instBlock(InstBlockId(inst.arg0));
      if (!operands.empty())
        value = inst_regions(operands.front());
    } else if (inst.kind == SemInstKind::Call ||
               inst.kind == SemInstKind::CompilerIntrinsicCall ||
               inst.kind == SemInstKind::ForeignOperationCall ||
               inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
               inst.kind == SemInstKind::CallbackAdapterCall ||
               inst.kind == SemInstKind::MakeCallbackRegistration) {
      std::optional<CallableOwnershipSummary> adapter_summary;
      const auto *summary =
          callSummary(sem_ir, instruction, inst, adapter_summary);
      if (summary && !summary->return_provenance.empty()) {
        const auto arguments = sem_ir.instBlock(InstBlockId(
            inst.kind == SemInstKind::MakeCallbackRegistration ? inst.arg0
                                                               : inst.arg1));
        for (const auto &source : summary->return_provenance) {
          if (source.region.parameter_index >= arguments.size())
            continue;
          const auto source_condition = substituteCondition(
              sem_ir, function, source.condition, arguments);
          for (auto base :
               inst_regions(arguments[source.region.parameter_index]))
            if (auto composed =
                    composeRegion(std::move(base.region), source.region,
                                  result.path_overflow, metrics)) {
              auto combined =
                  conditionAnd(std::move(base.condition), source_condition);
              if (!combined.isNever())
                value.push_back({std::move(*composed), std::move(combined)});
            }
        }
        value = canonicalizeRegions(std::move(value));
      }
    } else if (inst.kind == SemInstKind::ScopedBlock) {
      const auto body = sem_ir.instBlock(InstBlockId(inst.arg0));
      if (!body.empty())
        value = inst_regions(body.back());
    } else if (inst.kind == SemInstKind::If) {
      for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
        const auto &arm = sem_ir.inst(arm_id);
        if (arm.kind != SemInstKind::IfArm)
          continue;
        const auto body = sem_ir.instBlock(InstBlockId(arm.arg0));
        if (!body.empty())
          appendRegions(value, inst_regions(body.back()));
      }
      value = canonicalizeRegions(std::move(value));
    } else if (inst.kind == SemInstKind::Switch) {
      for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
        const auto &arm = sem_ir.inst(arm_id);
        if (arm.kind != SemInstKind::SwitchArm)
          continue;
        const auto body = sem_ir.instBlock(InstBlockId(arm.arg1));
        if (!body.empty())
          appendRegions(value, inst_regions(body.back()));
      }
      value = canonicalizeRegions(std::move(value));
    } else if (sem_ir.loanCarrierCapability(TypeId(inst.type)) !=
               SemLoanCarrierCapability::None) {
      for (std::size_t arg = 0; arg < 2; ++arg)
        if (semInstArgKind(inst.kind, arg) == SemArgKind::Inst)
          appendRegions(value,
                        inst_regions(InstId(arg == 0 ? inst.arg0 : inst.arg1)));
    }
    if (result.instruction_regions[instruction.index] != value) {
      result.instruction_regions[instruction.index] = value;
      if (metrics)
        ++metrics->provenance_state_changes;
      if (const auto found = consumers.find(instruction.index);
          found != consumers.end())
        for (const auto consumer : found->second)
          worklist.push(consumer);
    }

    auto output = input;
    if (inst.kind == SemInstKind::BindName) {
      output[inst.arg0] = result.instruction_regions[inst.arg1];
    } else if (inst.kind == SemInstKind::Assign) {
      const auto &target = sem_ir.inst(InstId(inst.arg0));
      if (target.kind == SemInstKind::NameRef &&
          sem_ir.type(sem_ir.local(LocalId(target.arg0)).type).kind ==
              SemTypeKind::Reference)
        output[target.arg0] = result.instruction_regions[inst.arg1];
    }
    if (out_states[instruction.index] != output) {
      out_states[instruction.index] = std::move(output);
      if (metrics)
        ++metrics->provenance_state_changes;
      if (const auto found = graph.successors.find(instruction.index);
          found != graph.successors.end())
        for (const auto &successor : found->second)
          worklist.push(successor.instruction);
    }
  }
  if (metrics)
    metrics->max_worklist_depth =
        std::max<std::uint64_t>(metrics->max_worklist_depth,
                                worklist.peakPendingCount());
  return result;
}

void appendEffects(CallableOwnershipSummary &target, CallableEffectKind kind,
                   std::span<const ConditionedRegion> regions) {
  for (const auto &region : regions)
    target.effects.push_back({kind, region.region});
}

void completeDeclaredPostconditions(CallableOwnershipSummary &summary) {
  const auto explicit_count = summary.postconditions.size();
  for (const auto &effect : summary.effects) {
    const auto outcome =
        effect.kind == CallableEffectKind::Initialize
            ? CallableOutcomeInitialize
        : effect.kind == CallableEffectKind::Write
            ? CallableOutcomePreserve | CallableOutcomeInitialize
        : effect.kind == CallableEffectKind::Take
            ? CallableOutcomePreserve | CallableOutcomeInvalidate
            : 0;
    if (outcome == 0 ||
        std::ranges::any_of(
            std::span(summary.postconditions).first(explicit_count),
            [&](const auto &entry) {
              return ownershipRegionCovers(entry.region, effect.region);
            }))
      continue;
    auto region = effect.region;
    auto outcomes = static_cast<std::uint8_t>(outcome);
    bool merged = true;
    while (merged) {
      merged = false;
      for (std::size_t index = explicit_count;
           index < summary.postconditions.size(); ++index) {
        const auto &existing = summary.postconditions[index];
        const auto existing_covers =
            ownershipRegionCovers(existing.region, region);
        if (!existing_covers && !ownershipRegionCovers(region, existing.region))
          continue;
        if (existing_covers)
          region = existing.region;
        outcomes |= existing.outcomes;
        summary.postconditions.erase(summary.postconditions.begin() + index);
        merged = true;
        break;
      }
    }
    summary.postconditions.push_back({std::move(region), outcomes});
  }
  summary.canonicalize();
}

const CallableOwnershipSummary *
callSummary(const SemIR &sem_ir, InstId instruction, const SemInst &inst,
            std::optional<CallableOwnershipSummary> &adapter_summary) {
  // Outcome facts are validated at the same boundary that imports a callable
  // summary. This keeps Result/task/channel/foreign projections from silently
  // taking a legacy ownership path when their canonical protocol is malformed.
  const auto validate_return_outcome = [&](TypeId return_type) {
    std::string error;
    if (sem_ir.canonicalResultShape(return_type))
      return makeResultOutcome().verify(error);
    const auto kind = sem_ir.type(return_type).kind;
    if (kind == SemTypeKind::CoroutineTask) {
      const bool fallible = sem_ir.coroutineTaskErrorType(return_type).has_value();
      return makeTaskOutcome(fallible).verify(error);
    }
    return true;
  };
  const auto validate = [&](const CallableOwnershipSummary *summary,
                            TypeId return_type) {
    return summary && validate_return_outcome(return_type) ? summary : nullptr;
  };
  if (inst.kind == SemInstKind::Call ||
      inst.kind == SemInstKind::CompilerIntrinsicCall ||
      inst.kind == SemInstKind::ForeignOperationCall) {
    const auto reference = FunctionRefId(inst.arg0);
    const auto &function_type =
        sem_ir.type(sem_ir.functionRef(reference).local_type);
    return validate(callableOwnershipSummary(sem_ir, reference),
                    TypeId(function_type.arg1));
  }
  if (inst.kind == SemInstKind::IndirectCall) {
    adapter_summary = indirectCallOwnershipSummary(sem_ir, TypeId(sem_ir.inst(InstId(inst.arg0)).type));
    return &*adapter_summary;
  }
  if (inst.kind == SemInstKind::IndirectForeignCall) {
    const auto callback_type = TypeId(sem_ir.inst(InstId(inst.arg0)).type);
    const auto &function_type = sem_ir.type(callback_type);
    return validate(&sem_ir.callbackContract(callback_type),
                    TypeId(function_type.arg1));
  }
  if (inst.kind == SemInstKind::CallbackAdapterCall) {
    const auto adapter_type = TypeId(sem_ir.inst(InstId(inst.arg0)).type);
    adapter_summary = callbackAdapterCallOwnershipSummary(sem_ir, adapter_type);
    const auto &type = sem_ir.type(adapter_type);
    const auto fields = sem_ir.typeBlock(TypeBlockId(type.arg0));
    const auto return_type = fields.empty()
                                 ? sem_ir.voidType()
                                 : TypeId(sem_ir.type(fields.front()).arg1);
    return validate(adapter_summary ? &*adapter_summary : nullptr,
                    return_type);
  }
  if (inst.kind == SemInstKind::MakeCallbackRegistration) {
    adapter_summary =
        callbackRegistrationCallOwnershipSummary(sem_ir, instruction);
    return adapter_summary ? &*adapter_summary : nullptr;
  }
  return nullptr;
}

using PostconditionState = std::vector<CallableRegionPostcondition>;

void setPostcondition(
    PostconditionState &state, OwnershipRegion region, std::uint8_t outcomes,
    CallableConditionDescriptor condition =
        CallableConditionDescriptor::always()) {
  condition.canonicalize();
  if (!condition.exact) { condition = CallableConditionDescriptor::always(); outcomes = CallableOutcomeAll; }
  if (condition.isNever())
    return;
  const auto found = std::ranges::find_if(
      state, [&](const auto &entry) {
        return entry.region == region && entry.condition == condition;
      });
  if (found == state.end())
    state.push_back({std::move(region), outcomes, std::move(condition)});
  else
    found->outcomes |= outcomes;
}

void mergePostconditions(PostconditionState &target,
                         const PostconditionState &source,
                         const CallableConditionDescriptor &edge_condition =
                             CallableConditionDescriptor::always()) {
  // A postcondition is a path fact. Preserve the source predicate while
  // crossing a conditional CFG edge, and explicitly add the complement as a
  // preserve fact when a region is not changed on part of that edge. This is
  // the same conservative join used by the ordinary place-state pass, but it
  // retains enough information for a caller to recover from an Err branch.
  std::vector<OwnershipRegion> regions;
  for (const auto &entry : target)
    if (std::ranges::find(regions, entry.region) == regions.end())
      regions.push_back(entry.region);
  for (const auto &entry : source)
    if (std::ranges::find(regions, entry.region) == regions.end())
      regions.push_back(entry.region);
  for (const auto &entry : source)
    setPostcondition(target, entry.region, entry.outcomes,
                     conditionAnd(entry.condition, edge_condition));
  for (const auto &region : regions) {
    auto source_condition = CallableConditionDescriptor::never();
    for (const auto &entry : source)
      if (entry.region == region)
        source_condition = conditionOr(std::move(source_condition),
                                       entry.condition);
    auto preserve = conditionAnd(edge_condition, conditionNot(source_condition));
    setPostcondition(target, region, CallableOutcomePreserve,
                     std::move(preserve));
  }
}

void canonicalizePostconditions(PostconditionState &state) {
  CallableOwnershipSummary summary;
  summary.postconditions = std::move(state);
  summary.canonicalize();
  state = std::move(summary.postconditions);
}

PostconditionState analyzePostconditions(const SemIR &sem_ir,
                                         const SemFunction &function,
                                         const FunctionGraph &graph,
                                         const ProvenanceResult &provenance,
                                         bool &path_overflow,
                                         FixedPointAnalysisMetrics *metrics) {
  std::unordered_map<std::uint32_t, PostconditionState> out_states;
  std::unordered_map<std::uint32_t, CallableConditionDescriptor>
      out_conditions;
  PostconditionState returns;
  const auto regions_for =
      [&](InstId id) -> std::span<const ConditionedRegion> {
    if (const auto found = provenance.instruction_regions.find(id.index);
        found != provenance.instruction_regions.end())
      return found->second;
    return {};
  };
  DenseWorklist<InstId> worklist(sem_ir.instCount());
  for (const auto instruction : graph.instructions)
    worklist.push(instruction);
  std::size_t edge_count = 0;
  for (const auto &[instruction, predecessors] : graph.predecessors) {
    (void)instruction;
    edge_count += predecessors.size();
  }
  const auto limit = std::max<std::size_t>(
      4096, 64 * (graph.instructions.size() + edge_count + 1));
  bool widened = false;
  while (const auto pending = worklist.pop()) {
    const auto instruction = *pending;
    if (worklist.processedCount() > limit) {
      widened = true;
      break;
    }
    if (metrics)
      ++metrics->postcondition_work_items;
    PostconditionState state;
    auto path_condition = CallableConditionDescriptor::never();
    if (const auto found = graph.predecessors.find(instruction.index);
        found != graph.predecessors.end()) {
      bool has_predecessor_state = false;
      for (const auto predecessor : found->second) {
        const auto &predecessor_state =
            out_states[predecessor.instruction.index];
        const auto predecessor_condition =
            out_conditions[predecessor.instruction.index];
        const auto incoming_condition =
            conditionAnd(predecessor_condition, predecessor.condition);
        path_condition = conditionOr(std::move(path_condition),
                                     incoming_condition);
        if (!has_predecessor_state) {
          mergePostconditions(state, predecessor_state,
                              predecessor.condition);
          has_predecessor_state = true;
        } else {
          mergePostconditions(state, predecessor_state,
                              predecessor.condition);
        }
      }
    }
    if (graph.predecessors.find(instruction.index) == graph.predecessors.end())
      path_condition = CallableConditionDescriptor::always();
    path_condition.canonicalize();
    const auto &inst = sem_ir.inst(instruction);
    if (inst.kind == SemInstKind::Move) {
      for (const auto &region : regions_for(InstId(inst.arg0)))
        setPostcondition(
            state, region.region, CallableOutcomeInvalidate,
            conditionAnd(path_condition, region.condition));
    } else if (inst.kind == SemInstKind::Assign) {
      for (const auto &region : regions_for(InstId(inst.arg0)))
        setPostcondition(
            state, region.region, CallableOutcomeInitialize,
            conditionAnd(path_condition, region.condition));
    } else if (inst.kind == SemInstKind::TypedChannelSendCommit) {
      const auto effect = makeChannelOutcome().transitionEffect(
          OutcomeState::Prepared, OutcomeState::Committed,
          OutcomePhase::Commit);
      if (effect && effect->source == OutcomeEffect::MoveSource)
        for (const auto &region : regions_for(InstId(inst.arg1)))
          setPostcondition(
              state, region.region, CallableOutcomeInvalidate,
              conditionAnd(path_condition, region.condition));
    } else if (inst.kind == SemInstKind::TypedChannelReceiveCommit) {
      const auto effect = makeChannelOutcome().transitionEffect(
          OutcomeState::Prepared, OutcomeState::Committed,
          OutcomePhase::Commit);
      if (effect)
        for (const auto &region : regions_for(InstId(inst.arg1)))
          setPostcondition(
              state, region.region, CallableOutcomeInitialize,
              conditionAnd(path_condition, region.condition));
    } else if (inst.kind == SemInstKind::Call ||
               inst.kind == SemInstKind::CompilerIntrinsicCall ||
               inst.kind == SemInstKind::ForeignOperationCall ||
               inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
               inst.kind == SemInstKind::CallbackAdapterCall ||
               inst.kind == SemInstKind::MakeCallbackRegistration) {
      std::optional<CallableOwnershipSummary> adapter_summary;
      const auto *callee =
          callSummary(sem_ir, instruction, inst, adapter_summary);
      if (callee) {
        const auto arguments = sem_ir.instBlock(InstBlockId(
            inst.kind == SemInstKind::MakeCallbackRegistration ? inst.arg0
                                                               : inst.arg1));
        for (const auto &postcondition : callee->postconditions) {
          if (postcondition.region.parameter_index >= arguments.size())
            continue;
          for (auto base :
               regions_for(arguments[postcondition.region.parameter_index])) {
            auto composed =
                composeRegion(std::move(base.region), postcondition.region,
                              path_overflow, metrics);
            if (!composed)
              continue;
            const auto substituted_condition = substituteCondition(
                sem_ir, function, postcondition.condition, arguments);
            const auto condition = conditionAnd(path_condition,
                                                substituted_condition);
            setPostcondition(state, std::move(*composed),
                             postcondition.result_variant == core::AnyId::InvalidIndex
                                 ? postcondition.outcomes : static_cast<std::uint8_t>(postcondition.outcomes | CallableOutcomePreserve),
                             std::move(condition));
          }
        }
      }
    }
    canonicalizePostconditions(state);
    if (out_states[instruction.index] != state ||
        out_conditions[instruction.index] != path_condition) {
      out_states[instruction.index] = std::move(state);
      out_conditions[instruction.index] = path_condition;
      if (metrics)
        ++metrics->postcondition_state_changes;
      if (const auto found = graph.successors.find(instruction.index);
          found != graph.successors.end())
        for (const auto &successor : found->second)
          worklist.push(successor.instruction);
    }
  }
  PostconditionState forwarded_outcomes;
  bool all_constructed_variants = true;
  for (const auto instruction : graph.instructions) {
    const auto &ret = sem_ir.inst(instruction);
    if (ret.kind == SemInstKind::Return && sem_ir.inst(InstId(ret.arg0)).kind != SemInstKind::EnumInit)
      all_constructed_variants = false;
  }
  bool has_return_state = false;
  for (const auto instruction : graph.instructions) {
    const auto &ret = sem_ir.inst(instruction);
    if (ret.kind != SemInstKind::Return) continue;
    auto returned = out_states[instruction.index];
    auto result_value = InstId(ret.arg0);
    std::unordered_set<std::uint32_t> visited_results;
    while (result_value.hasValue() && visited_results.insert(result_value.index).second) {
      const auto &candidate = sem_ir.inst(result_value);
      if (candidate.kind == SemInstKind::Move || candidate.kind == SemInstKind::Copy) {
        result_value = InstId(candidate.arg0); continue;
      }
      if (candidate.kind != SemInstKind::NameRef) break;
      InstId binding;
      bool reassigned = false;
      for (const auto node : graph.instructions) {
        const auto &source = sem_ir.inst(node);
        if (source.kind == SemInstKind::BindName && source.arg0 == candidate.arg0) binding = InstId(source.arg1);
        if (source.kind == SemInstKind::Assign) {
          const auto &target = sem_ir.inst(InstId(source.arg0));
          reassigned |= target.kind == SemInstKind::NameRef && target.arg0 == candidate.arg0;
        }
      }
      if (!binding.hasValue() || reassigned) break;
      result_value = binding;
    }
    const auto &result_inst = sem_ir.inst(result_value);
    if (result_inst.kind == SemInstKind::Call &&
        std::ranges::none_of(graph.instructions, [&](InstId id) {
          const auto kind = sem_ir.inst(id).kind;
          return id.index > result_value.index &&
              (kind == SemInstKind::Assign || kind == SemInstKind::Call || kind == SemInstKind::CompilerIntrinsicCall);
        })) {
      const auto *callee = callableOwnershipSummary(sem_ir, FunctionRefId(result_inst.arg0));
      const auto arguments = sem_ir.instBlock(InstBlockId(result_inst.arg1));
      if (callee) for (const auto &postcondition : callee->postconditions) {
        if (postcondition.result_variant == core::AnyId::InvalidIndex || postcondition.region.parameter_index >= arguments.size()) continue;
        for (const auto &base : regions_for(arguments[postcondition.region.parameter_index])) {
          auto region = composeRegion(base.region, postcondition.region, path_overflow, metrics);
          if (!region) continue;
          std::erase_if(returned, [&](const auto &fact) { return fact.region == *region; });
          auto fact = postcondition;
          fact.region = *region;
          fact.condition = substituteCondition(sem_ir, function, fact.condition, arguments);
          forwarded_outcomes.push_back(std::move(fact));
        }
      }
    }
    if (all_constructed_variants) {
      const auto &value = sem_ir.inst(InstId(ret.arg0));
      if (returned.empty()) {
        // No effects on this branch: synthesize preserve for regions observed
        // on other returns, so a join cannot grant initialization globally.
        for (const auto &[other_instruction, facts] : out_states) {
          (void)other_instruction;
          for (const auto &fact : facts)
            if (std::ranges::none_of(returned, [&](const auto &candidate) { return candidate.region == fact.region; }))
              returned.push_back({fact.region, CallableOutcomePreserve});
        }
      }
      for (auto &fact : returned) {
        fact.result_variant = static_cast<std::uint32_t>(sem_ir.integer(IntegerId(value.arg1)));
        if (conditionAnd(out_conditions[instruction.index], conditionNot(fact.condition)).isNever())
          fact.condition = CallableConditionDescriptor::always();
      }
      returns.insert(returns.end(), returned.begin(), returned.end());
    } else if (!has_return_state) { returns = std::move(returned); }
    else { mergePostconditions(returns, returned); }
    has_return_state = true;
  }
  returns.insert(returns.end(), forwarded_outcomes.begin(), forwarded_outcomes.end());
  if (widened) {
    if (metrics)
      ++metrics->postcondition_widenings;
    std::unordered_set<std::uint32_t> widened_parameters;
    for (const auto &[instruction, state] : out_states) {
      (void)instruction;
      for (const auto &entry : state)
        widened_parameters.insert(entry.region.parameter_index);
    }
    for (const auto &entry : returns)
      widened_parameters.insert(entry.region.parameter_index);
    returns.clear();
    for (const auto parameter : widened_parameters)
      returns.push_back(
          {OwnershipRegion{.parameter_index = parameter}, CallableOutcomeAll});
  }
  if (metrics)
    metrics->max_worklist_depth =
        std::max<std::uint64_t>(metrics->max_worklist_depth,
                                worklist.peakPendingCount());
  return returns;
}

void canonicalizeRepresentationRegions(const SemIR &sem_ir,
                                       FunctionId function_id,
                                       CallableOwnershipSummary &summary) {
  const auto &contract = sem_ir.functionSemanticContract(function_id);
  const auto role = contract.role;
  const auto is_projection =
      role == SemCanonicalFunctionRole::ProjectionLoad ||
      role == SemCanonicalFunctionRole::ProjectionStore ||
      role == SemCanonicalFunctionRole::ProjectionTake ||
      role == SemCanonicalFunctionRole::ProjectionInit ||
      role == SemCanonicalFunctionRole::ProjectionBorrow ||
      role == SemCanonicalFunctionRole::ProjectionBorrowMut;
  const auto is_shell = role == SemCanonicalFunctionRole::ObjectInit ||
                        role == SemCanonicalFunctionRole::ObjectCopyInit ||
                        role == SemCanonicalFunctionRole::ObjectMoveInit ||
                        role == SemCanonicalFunctionRole::ObjectDrop;
  if ((!is_projection && !is_shell) || !contract.owner.hasValue())
    return;

  std::optional<std::uint32_t> logical_field;
  bool logical_field_has_bits = false;
  std::uint32_t logical_bit_begin = 0;
  std::uint32_t logical_bit_end = 0;
  if (is_projection) {
    const auto &owner = sem_ir.nominalType(contract.owner);
    if (contract.projector_field >= owner.fields.size())
      return;
    logical_field = contract.projector_field;
    logical_field_has_bits = contract.has_bit_range;
    logical_bit_begin = contract.bit_begin;
    logical_bit_end = contract.bit_end;
  }
  const auto map_region = [&](OwnershipRegion &region) {
    if (region.parameter_index != 0)
      return;
    region.path.clear();
    region.has_bit_range = false;
    region.bit_begin = 0;
    region.bit_end = 0;
    if (logical_field) {
      region.path.push_back({OwnershipRegionStepKind::Field, *logical_field});
      region.has_bit_range = logical_field_has_bits;
      region.bit_begin = logical_bit_begin;
      region.bit_end = logical_bit_end;
    }
  };
  for (auto &effect : summary.effects)
    map_region(effect.region);
  for (auto &postcondition : summary.postconditions)
    map_region(postcondition.region);
  for (auto &source : summary.return_provenance)
    map_region(source.region);
}

// Region paths treat a reference parameter as transparently dereferenced at
// the root, but every reference encountered after a projection must be
// represented explicitly. This matters for synthesized callable adapters,
// whose environment field can itself be a borrowed receiver.
void normalizeRegionReferences(const SemIR &sem_ir,
                               std::span<const TypeId> parameter_types,
                               OwnershipRegion &region) {
  if (region.parameter_index >= parameter_types.size())
    return;
  TypeId current = parameter_types[region.parameter_index];
  if (sem_ir.type(current).kind == SemTypeKind::Reference)
    current = sem_ir.referencePointee(current);

  std::vector<OwnershipRegionStep> normalized;
  normalized.reserve(region.path.size() + 2);
  for (const auto &step : region.path) {
    auto current_kind = sem_ir.type(current).kind;
    if ((step.kind == OwnershipRegionStepKind::Field ||
         step.kind == OwnershipRegionStepKind::StaticElement ||
         step.kind == OwnershipRegionStepKind::AnyElement) &&
        current_kind == SemTypeKind::Reference) {
      normalized.push_back({OwnershipRegionStepKind::Dereference, 0});
      current = sem_ir.referencePointee(current);
      current_kind = sem_ir.type(current).kind;
    }
    normalized.push_back(step);
    switch (step.kind) {
    case OwnershipRegionStepKind::Dereference:
      if (step.index == 0 && (current_kind == SemTypeKind::Reference ||
                              current_kind == SemTypeKind::RawPointer))
        current = current_kind == SemTypeKind::Reference
                      ? sem_ir.referencePointee(current)
                      : sem_ir.rawPointerPointee(current);
      break;
    case OwnershipRegionStepKind::Field:
      if (current_kind == SemTypeKind::Nominal) {
        const auto &nominal =
            sem_ir.nominalType(NominalTypeId(sem_ir.type(current).arg0));
        if (step.index < nominal.fields.size())
          current = sem_ir.nominalFieldType(current, step.index);
      }
      break;
    case OwnershipRegionStepKind::StaticElement:
    case OwnershipRegionStepKind::AnyElement:
      if (current_kind == SemTypeKind::Array)
        current = TypeId(sem_ir.type(current).arg0);
      else if (current_kind == SemTypeKind::Slice &&
               step.kind == OwnershipRegionStepKind::AnyElement)
        current = TypeId(sem_ir.type(current).arg0);
      else if (current_kind == SemTypeKind::Tuple &&
               step.index < sem_ir.tupleArity(current))
        current = sem_ir.tupleElementType(current, step.index);
      break;
    default:
      break;
    }
  }
  region.path = std::move(normalized);
}

CallableOwnershipSummary
buildSummary(const SemIR &sem_ir, FunctionId function_id,
             const SemFunction &function, const FunctionGraph &graph,
             bool &path_overflow, FixedPointAnalysisMetrics *metrics) {
  CallableOwnershipSummary summary;
  const auto &function_type = sem_ir.type(function.type);
  const auto return_type = TypeId(function_type.arg1);
  summary.returns_owned = sem_ir.typeRepresentation(return_type).ownership !=
                          OwnershipReprKind::Borrowed;
  auto provenance = analyzeProvenance(sem_ir, function, graph, metrics);
  path_overflow |= provenance.path_overflow;

  std::unordered_set<std::uint32_t> projection_inputs;
  for (const auto instruction : graph.instructions) {
    const auto &inst = sem_ir.inst(instruction);
    if (inst.kind == SemInstKind::StructFieldAccess ||
        inst.kind == SemInstKind::Index || inst.kind == SemInstKind::Move ||
        inst.kind == SemInstKind::Copy ||
        inst.kind == SemInstKind::BorrowPlace ||
        inst.kind == SemInstKind::Assign ||
        inst.kind == SemInstKind::CarrierView ||
        inst.kind == SemInstKind::Dereference)
      projection_inputs.insert(inst.arg0);
  }

  const auto regions_for =
      [&](InstId id) -> std::span<const ConditionedRegion> {
    if (const auto found = provenance.instruction_regions.find(id.index);
        found != provenance.instruction_regions.end())
      return found->second;
    return {};
  };
  for (const auto instruction : graph.instructions) {
    const auto &inst = sem_ir.inst(instruction);
    if (((inst.kind == SemInstKind::NameRef &&
          sem_ir.type(TypeId(inst.type)).kind != SemTypeKind::Reference) ||
         inst.kind == SemInstKind::StructFieldAccess ||
         inst.kind == SemInstKind::Index ||
         inst.kind == SemInstKind::Dereference) &&
        !projection_inputs.contains(instruction.index))
      appendEffects(summary, CallableEffectKind::Read,
                    regions_for(instruction));
    else if (inst.kind == SemInstKind::Move)
      appendEffects(summary, CallableEffectKind::Take,
                    regions_for(InstId(inst.arg0)));
    else if (inst.kind == SemInstKind::Copy)
      appendEffects(summary, CallableEffectKind::Read,
                    regions_for(InstId(inst.arg0)));
    else if (inst.kind == SemInstKind::BorrowLocal ||
             inst.kind == SemInstKind::BorrowPlace)
      appendEffects(summary,
                    sem_ir.referenceMutability(TypeId(inst.type)) ==
                            SemReferenceMutability::Mutable
                        ? CallableEffectKind::BorrowMutable
                        : CallableEffectKind::BorrowShared,
                    regions_for(instruction));
    else if (inst.kind == SemInstKind::Assign)
      appendEffects(summary, CallableEffectKind::Write,
                    regions_for(InstId(inst.arg0)));
    else if (inst.kind == SemInstKind::TypedChannelSendCommit)
      // The source owner moves only at the verified MultiSubmit commit edge.
      appendEffects(summary, CallableEffectKind::Take,
                    regions_for(InstId(inst.arg1)));
    else if (inst.kind == SemInstKind::TypedChannelReceiveCommit)
      // Receive commit establishes the destination; acquire/cancel are
      // intentionally ownership-neutral.
      appendEffects(summary, CallableEffectKind::Initialize,
                    regions_for(InstId(inst.arg1)));
    else if (inst.kind == SemInstKind::Call ||
             inst.kind == SemInstKind::CompilerIntrinsicCall ||
             inst.kind == SemInstKind::ForeignOperationCall ||
             inst.kind == SemInstKind::IndirectCall ||
        inst.kind == SemInstKind::IndirectForeignCall ||
             inst.kind == SemInstKind::CallbackAdapterCall ||
             inst.kind == SemInstKind::MakeCallbackRegistration) {
      std::optional<CallableOwnershipSummary> adapter_summary;
      const auto *callee =
          callSummary(sem_ir, instruction, inst, adapter_summary);
      if (!callee)
        continue;
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
        const auto arguments = sem_ir.instBlock(InstBlockId(inst.arg0));
        for (const auto argument : arguments)
          adapter_parameter_types.push_back(TypeId(sem_ir.inst(argument).type));
        parameter_types = adapter_parameter_types;
      } else {
        const auto callee_type_id =
            (inst.kind == SemInstKind::Call ||
             inst.kind == SemInstKind::CompilerIntrinsicCall ||
             inst.kind == SemInstKind::ForeignOperationCall)
                ? sem_ir.functionRef(FunctionRefId(inst.arg0)).local_type
                : TypeId(sem_ir.inst(InstId(inst.arg0)).type);
        parameter_types =
            sem_ir.typeBlock(TypeBlockId(sem_ir.type(callee_type_id).arg0));
      }
      const auto arguments = sem_ir.instBlock(InstBlockId(
          inst.kind == SemInstKind::MakeCallbackRegistration ? inst.arg0
                                                             : inst.arg1));
      for (const auto &effect : callee->effects) {
        if (effect.region.parameter_index >= arguments.size() ||
            effect.region.parameter_index >= parameter_types.size() ||
            (sem_ir.type(parameter_types[effect.region.parameter_index]).kind !=
                 SemTypeKind::Reference &&
             std::ranges::none_of(effect.region.path, [](const auto &step) {
               return step.kind == OwnershipRegionStepKind::Dereference;
             })))
          continue;
        for (auto base : regions_for(arguments[effect.region.parameter_index]))
          if (auto composed =
                  composeRegion(std::move(base.region), effect.region,
                                path_overflow, metrics))
            summary.effects.push_back({effect.kind, std::move(*composed)});
      }
    } else if (inst.kind == SemInstKind::Return &&
               sem_ir.loanCarrierCapability(return_type) !=
                   SemLoanCarrierCapability::None) {
      for (const auto &region : regions_for(InstId(inst.arg0)))
        summary.return_provenance.push_back(
            {.region = region.region, .condition = region.condition});
    }
  }
  summary.postconditions =
      analyzePostconditions(sem_ir, function, graph, provenance, path_overflow,
                            metrics);
  canonicalizeRepresentationRegions(sem_ir, function_id, summary);
  const auto parameter_types =
      sem_ir.typeBlock(TypeBlockId(function_type.arg0));
  for (auto &effect : summary.effects)
    normalizeRegionReferences(sem_ir, parameter_types, effect.region);
  for (auto &postcondition : summary.postconditions)
    normalizeRegionReferences(sem_ir, parameter_types, postcondition.region);
  for (auto &source : summary.return_provenance)
    normalizeRegionReferences(sem_ir, parameter_types, source.region);
  std::erase_if(summary.return_provenance, [&](const auto &source) {
    return source.region.parameter_index >= parameter_types.size() ||
           sem_ir.loanCarrierCapability(
               parameter_types[source.region.parameter_index]) ==
               SemLoanCarrierCapability::None;
  });
  const auto carrier_paths = sem_ir.loanCarrierPaths(return_type);
  if (!carrier_paths.empty() &&
      !(carrier_paths.size() == 1 && carrier_paths.front().empty())) {
    const auto sources = std::move(summary.return_provenance);
    summary.return_provenance.clear();
    for (const auto &source : sources)
      for (const auto &path : carrier_paths) {
        auto expanded = source;
        expanded.carrier_path = path;
        summary.return_provenance.push_back(std::move(expanded));
      }
  }
  summary.canonicalize();
  return summary;
}

std::vector<std::vector<FunctionId>>
callGraphComponents(const SemIR &sem_ir,
                    std::span<const FunctionGraph> graphs) {
  std::vector<std::vector<FunctionId>> edges(sem_ir.functionCount());
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index)
    for (const auto instruction : graphs[index].instructions) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind != SemInstKind::Call &&
          inst.kind != SemInstKind::CompilerIntrinsicCall)
        continue;
      const auto &reference = sem_ir.functionRef(FunctionRefId(inst.arg0));
      if (reference.local_function.hasValue() &&
          std::ranges::find(edges[index], reference.local_function) ==
              edges[index].end())
        edges[index].push_back(reference.local_function);
    }

  std::vector<std::uint32_t> indices(sem_ir.functionCount(),
                                     std::numeric_limits<std::uint32_t>::max());
  std::vector<std::uint32_t> low_links(sem_ir.functionCount());
  std::vector<bool> on_stack(sem_ir.functionCount());
  std::vector<FunctionId> stack;
  std::vector<std::vector<FunctionId>> components;
  std::uint32_t next_index = 0;
  const auto visit = [&](auto &&self, FunctionId function) -> void {
    indices[function.index] = next_index;
    low_links[function.index] = next_index++;
    stack.push_back(function);
    on_stack[function.index] = true;
    for (const auto callee : edges[function.index]) {
      if (indices[callee.index] == std::numeric_limits<std::uint32_t>::max()) {
        self(self, callee);
        low_links[function.index] =
            std::min(low_links[function.index], low_links[callee.index]);
      } else if (on_stack[callee.index]) {
        low_links[function.index] =
            std::min(low_links[function.index], indices[callee.index]);
      }
    }
    if (low_links[function.index] != indices[function.index])
      return;
    auto &component = components.emplace_back();
    for (;;) {
      const auto member = stack.back();
      stack.pop_back();
      on_stack[member.index] = false;
      component.push_back(member);
      if (member == function)
        break;
    }
    std::ranges::sort(component, {}, &FunctionId::index);
  };
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index)
    if (indices[index] == std::numeric_limits<std::uint32_t>::max())
      visit(visit, FunctionId(index));
  return components;
}
