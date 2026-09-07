// Included by CallableOwnership.cpp; this fragment aliases the existing
// ownership graph, region, and summary state.
const CallableOwnershipSummary *
callableOwnershipSummary(const SemIR &sem_ir, FunctionRefId reference) {
  const auto &ref = sem_ir.functionRef(reference);
  if (ref.local_function.hasValue())
    return &sem_ir.functionOwnership(ref.local_function);
  if (ref.public_entity.hasValue()) {
    if (const auto *entity = sem_ir.importIRs().tryGetEntity(ref.public_entity))
      return &entity->ownership_summary;
  }
  return nullptr;
}

std::optional<CallableOwnershipSummary>
foreignOutcomeCallOwnershipSummary(const SemIR &sem_ir, FunctionRefId function,
                                   TypeId projected_result) {
  // Foreign read ownership is described by the same canonical outcome facts
  // used by LowIR. Keep this check at the semantic boundary so malformed
  // projections fail closed before a return-provenance summary is published.
  std::string outcome_error;
  const auto &reference = sem_ir.functionRef(function);
  const auto *operation = [&]() -> const interop::ForeignOperationArtifact * {
    if (reference.local_function.hasValue()) {
      const auto &declaration =
          sem_ir.functionDeclaration(reference.local_function);
      return declaration.interop_artifact
                 ? sem_ir.importIRs().interopRegistry().resolve(
                       *declaration.interop_artifact)
                 : nullptr;
    }
    const auto *entity =
        sem_ir.importIRs().tryGetEntity(reference.public_entity);
    return entity && entity->interop_artifact
               ? sem_ir.importIRs().interopRegistry().resolve(
                     *entity->interop_artifact)
               : nullptr;
  }();
  if (!operation ||
      operation->outcome_projection ==
          interop::ForeignOperationArtifact::OutcomeProjection::None)
    return std::nullopt;
  if (!makeForeignReadOutcome(
           operation->outcome_projection !=
           interop::ForeignOperationArtifact::OutcomeProjection::Win32Read)
           .verify(outcome_error))
    return std::nullopt;
  const auto result = sem_ir.canonicalResultShape(projected_result);
  const auto outcome =
      result ? sem_ir.canonicalReadOutcomeShape(result->success) : std::nullopt;
  if (!result || !outcome)
    return std::nullopt;
  using Step = CallableReturnSource::CarrierStep;
  using Kind = CallableReturnSource::CarrierStepKind;
  CallableOwnershipSummary summary;
  summary.returns_owned = true;
  const auto buffer_source =
      operation->outcome_buffer_lane < operation->argument_sources.size()
          ? operation->argument_sources[operation->outcome_buffer_lane]
          : interop::ForeignOperationArtifact::ArgumentSource{};
  if (buffer_source.kind !=
      interop::ForeignOperationArtifact::ArgumentSourceKind::PublicArgument)
    return std::nullopt;
  summary.return_provenance.push_back(
      {.region = {.parameter_index = buffer_source.index},
       .carrier_path = {Step{Kind::EnumVariant, result->ok_variant},
                        Step{Kind::Field, 0},
                        Step{Kind::EnumVariant, outcome->data_variant},
                        Step{Kind::Field, 0}}});
  summary.canonicalize();
  return summary;
}

CallableOwnershipSummary
effectiveCallableOwnershipSummary(CallableOwnershipSummary summary) {
  completeDeclaredPostconditions(summary);
  return summary;
}

bool callableOwnershipSubstitutes(const CallableOwnershipSummary &source,
                                  const CallableOwnershipSummary &contract) {
  const auto effects_covered =
      std::ranges::all_of(source.effects, [&](const auto &effect) {
        return std::ranges::any_of(contract.effects, [&](const auto &limit) {
          return limit.kind == effect.kind &&
                 ownershipRegionCovers(limit.region, effect.region);
        });
      });
  const auto postconditions_satisfied = std::ranges::all_of(
      contract.postconditions, [&](const auto &requirement) {
        return std::ranges::any_of(
                source.postconditions, [&](const auto &actual) {
              return ownershipRegionCovers(actual.region, requirement.region) &&
                     (actual.outcomes & ~requirement.outcomes) == 0 &&
                     (actual.result_variant == core::AnyId::InvalidIndex || actual.result_variant == requirement.result_variant) &&
                     conditionAnd(requirement.condition,
                                  conditionNot(actual.condition))
                         .isNever();
            });
      });
  return effects_covered && postconditions_satisfied &&
         source.returns_owned == contract.returns_owned &&
         source.return_provenance == contract.return_provenance;
}

bool callbackAdapterOwnershipSubstitutes(const SemIR &sem_ir,
                                         FunctionRefId source,
                                         TypeId callback_type) {
  const auto *summary = callableOwnershipSummary(sem_ir, source);
  if (!summary || !callback_type.hasValue())
    return false;
  auto effective_source = *summary;
  const auto &callback = sem_ir.type(callback_type);
  if (callback.kind != SemTypeKind::CFunctionPointer &&
      callback.kind != SemTypeKind::CVariadicFunctionPointer)
    return false;
  const auto parameter_types = sem_ir.typeBlock(TypeBlockId(callback.arg0));
  const auto context_parameter = sem_ir.callbackContextParameter(callback_type);
  const auto observable_region = [&](const OwnershipRegion &region) {
    return region.parameter_index < parameter_types.size() &&
           (region.parameter_index == context_parameter ||
            sem_ir.type(parameter_types[region.parameter_index]).kind ==
                SemTypeKind::Reference ||
            std::ranges::any_of(region.path, [](const auto &step) {
              return step.kind == OwnershipRegionStepKind::Dereference;
            }));
  };
  std::erase_if(effective_source.effects, [&](const auto &effect) {
    return !observable_region(effect.region);
  });
  std::erase_if(effective_source.postconditions,
                [&](const auto &postcondition) {
                  return !observable_region(postcondition.region);
                });
  const auto &contract = sem_ir.callbackContract(callback_type);
  for (const auto &required : contract.postconditions) {
    const auto source_mentions_region =
        std::ranges::any_of(effective_source.effects, [&](const auto &effect) {
          return (effect.kind == CallableEffectKind::Write ||
                  effect.kind == CallableEffectKind::Initialize ||
                  effect.kind == CallableEffectKind::Take) &&
                 (ownershipRegionCovers(effect.region, required.region) ||
                  ownershipRegionCovers(required.region, effect.region));
        });
    const auto source_has_postcondition = std::ranges::any_of(
        effective_source.postconditions, [&](const auto &postcondition) {
          return ownershipRegionCovers(postcondition.region, required.region);
        });
    if (!source_mentions_region && !source_has_postcondition)
      effective_source.postconditions.push_back(
          {required.region, CallableOutcomePreserve});
  }
  effective_source.canonicalize();
  return callableOwnershipSubstitutes(effective_source, contract);
}

std::optional<CallableOwnershipSummary>
callbackAdapterCallOwnershipSummary(const SemIR &sem_ir, TypeId adapter_type) {
  if (!adapter_type.hasValue() ||
      sem_ir.type(adapter_type).kind != SemTypeKind::CallbackAdapter)
    return std::nullopt;
  const auto fields =
      sem_ir.typeBlock(TypeBlockId(sem_ir.type(adapter_type).arg0));
  if (fields.size() != 3)
    return std::nullopt;
  const auto context = sem_ir.callbackContextParameter(fields[0]);
  const auto parameters =
      sem_ir.typeBlock(TypeBlockId(sem_ir.type(fields[0]).arg0));
  if (context >= parameters.size())
    return std::nullopt;
  auto summary = sem_ir.callbackContract(fields[0]);
  if (std::ranges::any_of(summary.return_provenance, [&](const auto &source) {
        return std::ranges::any_of(
            source.condition.clauses, [&](const auto &clause) {
              return std::ranges::any_of(clause.atoms, [&](const auto &atom) {
                return atom.parameter_index == context;
              });
            });
      }))
    return std::nullopt;
  const auto remap_region = [&](OwnershipRegion &region) {
    if (region.parameter_index == context)
      return false;
    if (region.parameter_index > context)
      --region.parameter_index;
    return true;
  };
  std::erase_if(summary.effects,
                [&](auto &effect) { return !remap_region(effect.region); });
  std::erase_if(summary.postconditions, [&](auto &postcondition) {
    if (!remap_region(postcondition.region))
      return true;
    for (auto &clause : postcondition.condition.clauses)
      for (auto &atom : clause.atoms) {
        if (atom.parameter_index == context)
          return true;
        if (atom.parameter_index > context)
          --atom.parameter_index;
      }
    return false;
  });
  std::erase_if(summary.return_provenance, [&](auto &source) {
    if (!remap_region(source.region))
      return true;
    for (auto &clause : source.condition.clauses)
      for (auto &atom : clause.atoms) {
        if (atom.parameter_index > context)
          --atom.parameter_index;
      }
    return false;
  });
  summary.canonicalize();
  return summary;
}

std::optional<CallableOwnershipSummary>
callbackRegistrationCallOwnershipSummary(const SemIR &sem_ir,
                                         InstId instruction) {
  if (!instruction.hasValue() || instruction.index >= sem_ir.instCount())
    return std::nullopt;
  const auto &make = sem_ir.inst(instruction);
  const auto registration_type = TypeId(make.type);
  if (make.kind != SemInstKind::MakeCallbackRegistration ||
      sem_ir.type(registration_type).kind != SemTypeKind::CallbackRegistration)
    return std::nullopt;
  const auto operands = sem_ir.instBlock(InstBlockId(make.arg0));
  const auto fields =
      sem_ir.typeBlock(TypeBlockId(sem_ir.type(registration_type).arg0));
  const auto bindings = sem_ir.callbackRegistrationBindings(registration_type);
  if (fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
      fields.size() != 10)
    return std::nullopt;
  const auto fixed_count = fields.size() == 5 ? 4U : fields.size() - 1U;
  if (operands.size() != fixed_count + bindings.size())
    return std::nullopt;

  const auto source_parameter =
      [&](std::uint32_t physical_parameter) -> std::optional<std::uint32_t> {
    const auto binding =
        std::ranges::find(bindings, physical_parameter,
                          &CallbackRegistrationBinding::parameter_index);
    if (binding == bindings.end())
      return std::nullopt;
    for (std::size_t source = fixed_count; source < operands.size(); ++source) {
      const auto &operand = sem_ir.inst(operands[source]);
      if (operand.kind == SemInstKind::CallbackRegistrationBinding &&
          sem_ir.identifier(sem_ir.name(NameId(operand.arg0)).text) ==
              binding->name)
        return static_cast<std::uint32_t>(source);
    }
    return std::nullopt;
  };
  auto summary = sem_ir.callbackContract(fields[2]);
  const auto remap_region = [&](OwnershipRegion &region) {
    const auto source = source_parameter(region.parameter_index);
    if (!source)
      return false;
    region.parameter_index = *source;
    return true;
  };
  if (std::ranges::any_of(
          summary.effects,
          [&](auto &effect) { return !remap_region(effect.region); }) ||
      std::ranges::any_of(summary.postconditions, [&](auto &postcondition) {
        if (!remap_region(postcondition.region))
          return true;
        for (auto &clause : postcondition.condition.clauses)
          for (auto &atom : clause.atoms) {
            const auto parameter = source_parameter(atom.parameter_index);
            if (!parameter)
              return true;
            atom.parameter_index = *parameter;
          }
        return false;
      }))
    return std::nullopt;
  for (auto &source : summary.return_provenance) {
    if (!remap_region(source.region))
      return std::nullopt;
    for (auto &clause : source.condition.clauses)
      for (auto &atom : clause.atoms) {
        const auto parameter = source_parameter(atom.parameter_index);
        if (!parameter)
          return std::nullopt;
        atom.parameter_index = *parameter;
      }
  }
  summary.canonicalize();
  return summary;
}

std::vector<CallableOwnershipViolation>
analyzeCallableOwnership(SemIR &sem_ir) {
  return analyzeCallableOwnership(sem_ir, nullptr);
}

std::vector<CallableOwnershipViolation>
analyzeCallableOwnership(SemIR &sem_ir, FixedPointAnalysisMetrics *metrics) {
  const auto started = std::chrono::steady_clock::now();
  if (metrics)
    *metrics = {};
  const auto finish_metrics = [&] {
    if (metrics)
      metrics->elapsed_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
  };
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    const auto function = FunctionId(index);
    const auto return_type =
        TypeId(sem_ir.type(sem_ir.function(function).type).arg1);
    CallableOwnershipSummary bottom;
    bottom.returns_owned = sem_ir.typeRepresentation(return_type).ownership !=
                           OwnershipReprKind::Borrowed;
    const auto &declaration = sem_ir.functionDeclaration(function);
    const auto &record = sem_ir.function(function);
    const auto preserve_artifact_summary =
        (record.flags & SemFunctionEvaluatorArtifact) != 0 &&
        sem_ir.instBlock(record.body).empty() &&
        sem_ir.expectedFunctionOwnership(function).has_value();
    if (preserve_artifact_summary) {
      continue;
    }
    if (declaration.kind != SemCallableDeclarationKind::Definition &&
        declaration.declared_contract) {
      auto declared = *declaration.declared_contract;
      completeDeclaredPostconditions(declared);
      sem_ir.setFunctionOwnership(function, std::move(declared));
    } else {
      sem_ir.setFunctionOwnership(function, std::move(bottom));
    }
  }
  std::vector<FunctionGraph> graphs;
  graphs.reserve(sem_ir.functionCount());
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    graphs.push_back(
        buildFunctionGraph(sem_ir, sem_ir.function(FunctionId(index)),
                           sem_ir.function(FunctionId(index)).body));
    if (metrics) {
      ++metrics->functions;
      metrics->cfg_instructions += graphs.back().instructions.size();
      for (const auto &[instruction, successors] : graphs.back().successors) {
        (void)instruction;
        metrics->cfg_edges += successors.size();
      }
    }
  }

  std::vector<std::vector<FunctionId>> callers(sem_ir.functionCount());
  for (std::uint32_t caller_index = 0; caller_index < graphs.size();
       ++caller_index) {
    for (const auto instruction : graphs[caller_index].instructions) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind != SemInstKind::Call &&
          inst.kind != SemInstKind::CompilerIntrinsicCall)
        continue;
      const auto callee = sem_ir.functionRef(FunctionRefId(inst.arg0));
      if (!callee.local_function.hasValue())
        continue;
      auto &values = callers[callee.local_function.index];
      const auto caller = FunctionId(caller_index);
      if (std::ranges::find(values, caller) == values.end())
        values.push_back(caller);
    }
  }
  for (auto &values : callers)
    std::ranges::sort(values, {}, &FunctionId::index);

  bool non_convergent = false;
  for (const auto &component : callGraphComponents(sem_ir, graphs)) {
    if (metrics)
      ++metrics->scc_components;
    std::vector<bool> in_component(sem_ir.functionCount());
    auto ordered_component = component;
    std::ranges::sort(ordered_component, {}, &FunctionId::index);
    DenseWorklist<FunctionId> worklist(sem_ir.functionCount());
    std::size_t component_edges = 0;
    for (const auto function_id : ordered_component) {
      in_component[function_id.index] = true;
      worklist.push(function_id);
      component_edges += callers[function_id.index].size();
    }
    const auto limit = std::max<std::size_t>(
        4096, 64 * (component.size() + component_edges + 1));
    while (const auto pending = worklist.pop()) {
      const auto function_id = *pending;
      if (worklist.processedCount() > limit) {
        non_convergent = true;
        break;
      }
      if (sem_ir.functionDeclaration(function_id).kind !=
          SemCallableDeclarationKind::Definition)
        continue;
      bool path_overflow = false;
      if (metrics)
        ++metrics->scc_function_evaluations;
      auto summary =
          buildSummary(sem_ir, function_id, sem_ir.function(function_id),
                       graphs[function_id.index], path_overflow, metrics);
      non_convergent |= path_overflow;
      if (summary == sem_ir.functionOwnership(function_id))
        continue;
      sem_ir.setFunctionOwnership(function_id, std::move(summary));
      for (const auto caller : callers[function_id.index])
        if (in_component[caller.index])
          worklist.push(caller);
    }
    if (metrics)
      metrics->max_worklist_depth =
          std::max<std::uint64_t>(metrics->max_worklist_depth,
                                  worklist.peakPendingCount());
    if (worklist.pendingCount() != 0)
      non_convergent = true;
  }

  std::vector<CallableOwnershipViolation> violations;
  if (non_convergent) {
    violations.push_back(
        {InstId::invalid(), CallableOwnershipViolationKind::NonConvergent,
         {{InstId::invalid(), OwnershipEvidenceKind::WidenedRegion, 0,
           "ownership fixed-point budget was exhausted"}}});
    finish_metrics();
    return violations;
  }
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    const auto function_id = FunctionId(index);
    if (const auto &expected = sem_ir.expectedFunctionOwnership(function_id);
        expected && *expected != sem_ir.functionOwnership(function_id)) {
      violations.push_back({graphs[index].instructions.empty()
                                ? InstId::invalid()
                                : graphs[index].instructions.front(),
                            CallableOwnershipViolationKind::ArtifactMismatch,
                            {{graphs[index].instructions.empty()
                                  ? InstId::invalid()
                                  : graphs[index].instructions.front(),
                              OwnershipEvidenceKind::ContractBoundary, 0,
                              "cached ownership summary differs from the verified callable"}}});
      continue;
    }
    const auto &declaration = sem_ir.functionDeclaration(function_id);
    if (declaration.kind == SemCallableDeclarationKind::Definition &&
        declaration.declared_contract) {
      const auto &inferred = sem_ir.functionOwnership(function_id);
      const auto &declared = *declaration.declared_contract;
      const auto effects_covered =
          std::ranges::all_of(inferred.effects, [&](const auto &effect) {
            return std::ranges::any_of(
                declared.effects, [&](const auto &limit) {
                  return limit.kind == effect.kind &&
                         ownershipRegionCovers(limit.region, effect.region);
                });
          });
      const auto postconditions_satisfied = std::ranges::all_of(
          declared.postconditions, [&](const auto &requirement) {
            return std::ranges::any_of(
                inferred.postconditions, [&](const auto &actual) {
                  return ownershipRegionCovers(actual.region,
                                               requirement.region) &&
                         (actual.outcomes & ~requirement.outcomes) == 0 &&
                     (actual.result_variant == core::AnyId::InvalidIndex || actual.result_variant == requirement.result_variant) &&
                         conditionAnd(requirement.condition,
                                      conditionNot(actual.condition))
                             .isNever();
                });
          });
      if (!effects_covered || !postconditions_satisfied ||
          inferred.returns_owned != declared.returns_owned ||
          inferred.return_provenance != declared.return_provenance) {
        violations.push_back(
            {graphs[index].instructions.empty()
                 ? InstId::invalid()
                 : graphs[index].instructions.front(),
             CallableOwnershipViolationKind::ContractMismatch,
             {{graphs[index].instructions.empty()
                   ? InstId::invalid()
                   : graphs[index].instructions.front(),
               OwnershipEvidenceKind::ContractBoundary, 0,
               "declared callable contract does not cover the inferred effect"}}});
        continue;
      }
      sem_ir.setFunctionOwnership(function_id, declared);
    }
    const auto &summary = sem_ir.functionOwnership(function_id);
    const auto &function = sem_ir.function(function_id);
    const auto &function_type = sem_ir.type(function.type);
    const auto return_type = TypeId(function_type.arg1);
    const auto parameter_types =
        sem_ir.typeBlock(TypeBlockId(function_type.arg0));
    const auto valid_return_sources =
        !summary.return_provenance.empty() &&
        std::ranges::all_of(
            summary.return_provenance, [&](const CallableReturnSource &source) {
              if (source.region.parameter_index >= parameter_types.size())
                return false;
              const auto parameter =
                  parameter_types[source.region.parameter_index];
              const auto return_capability =
                  sem_ir.loanCarrierCapability(return_type);
              const auto parameter_capability =
                  sem_ir.loanCarrierCapability(parameter);
              return parameter_capability != SemLoanCarrierCapability::None &&
                     (return_capability != SemLoanCarrierCapability::Mutable ||
                      parameter_capability ==
                          SemLoanCarrierCapability::Mutable) &&
                     std::ranges::all_of(
                         source.condition.clauses, [&](const auto &clause) {
                           return std::ranges::all_of(
                               clause.atoms, [&](const auto &atom) {
                                 return atom.parameter_index <
                                            parameter_types.size() &&
                                        (atom.variant == core::AnyId::InvalidIndex
                                         ? sem_ir.type(parameter_types[atom.parameter_index]).kind == SemTypeKind::Bool
                                         : sem_ir.type(parameter_types[atom.parameter_index]).kind == SemTypeKind::Nominal);
                               });
                         });
            });
    if (sem_ir.loanCarrierCapability(return_type) ==
            SemLoanCarrierCapability::None ||
        valid_return_sources)
      continue;
    const auto returned = std::ranges::find_if(
        graphs[index].instructions, [&](InstId instruction) {
          return sem_ir.inst(instruction).kind == SemInstKind::Return;
        });
    if (returned != graphs[index].instructions.end())
      violations.push_back(
          {*returned, CallableOwnershipViolationKind::BorrowEscape,
           {{*returned, OwnershipEvidenceKind::ReturnProvenance, 0,
             "returned reference has no valid parameter provenance"}}});
  }
  for (std::uint32_t index = 0; index < sem_ir.instCount(); ++index) {
    const auto instruction = InstId(index);
    const auto &inst = sem_ir.inst(instruction);
    if (inst.kind != SemInstKind::CallbackAdapter)
      continue;
    const auto callback_type = TypeId(inst.type);
    if (!callbackAdapterOwnershipSubstitutes(sem_ir, FunctionRefId(inst.arg0),
                                             callback_type))
      violations.push_back(
          {instruction, CallableOwnershipViolationKind::ContractMismatch,
           {{instruction, OwnershipEvidenceKind::ContractBoundary, 0,
             "callback adapter does not preserve the callable ownership contract"}}});
  }
  finish_metrics();
  return violations;
}
