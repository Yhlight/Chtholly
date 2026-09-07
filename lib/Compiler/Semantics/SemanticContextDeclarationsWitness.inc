// Included by SemanticContextDeclarations.cpp; this fragment aliases the
// single SemanticContext and preserves declaration-phase ordering.
[[nodiscard]] std::optional<std::vector<CanonicalTypeId>>
SemanticContext::deduceInterfaceWitnessArguments(
    const SemInterfaceWitness &witness, TypeId subject,
    std::span<const TypeId> arguments) {
  if (!witness.generic.hasValue())
    return std::nullopt;
  const auto generic = witness.generic;
  std::vector<CanonicalTypeId> inferred(
      values_.generics().generic(generic).binding_count,
      CanonicalTypeId::invalid());
  bool conflict = false;
  const auto deduce = [&](auto &&self, CanonicalTypeId pattern_id,
                          CanonicalTypeId actual_id) -> void {
    const auto &pattern = values_.generics().type(pattern_id);
    const auto &actual = values_.generics().type(actual_id);
    if (pattern.kind == CanonicalTypeKind::TypeParameter &&
        pattern.arg0 == generic.index && pattern.arg1 < inferred.size()) {
      if (!sem_ir_.isGenericArgumentType(actual_id)) {
        conflict = true;
        return;
      }
      auto &slot = inferred[pattern.arg1];
      if (slot.hasValue() && slot != actual_id)
        conflict = true;
      else
        slot = actual_id;
      return;
    }
    if (pattern.kind == CanonicalTypeKind::Array) {
      if (actual.kind != CanonicalTypeKind::Array ||
          pattern.arg1 != actual.arg1) {
        conflict = true;
        return;
      }
      self(self, CanonicalTypeId(pattern.arg0), CanonicalTypeId(actual.arg0));
      return;
    }
    if (pattern.kind != actual.kind || pattern.arg0 != actual.arg0 ||
        pattern.arg1 != actual.arg1 ||
        pattern.nominal_key != actual.nominal_key ||
        pattern.callable_contract != actual.callable_contract ||
        pattern.callable_context_parameter !=
            actual.callable_context_parameter ||
        pattern.registration_authority != actual.registration_authority ||
        pattern.registration_entry_parameter !=
            actual.registration_entry_parameter ||
        pattern.registration_userdata_parameter !=
            actual.registration_userdata_parameter ||
        pattern.registration_release_parameter !=
            actual.registration_release_parameter ||
        pattern.registration_bindings != actual.registration_bindings ||
        pattern.registration_arm_parameters !=
            actual.registration_arm_parameters ||
        pattern.registration_detach_parameters !=
            actual.registration_detach_parameters ||
        pattern.elements.size() != actual.elements.size()) {
      conflict = true;
      return;
    }
    for (std::size_t index = 0; index < pattern.elements.size(); ++index)
      self(self, pattern.elements[index], actual.elements[index]);
  };
  deduce(deduce, sem_ir_.canonicalType(witness.self_type),
         sem_ir_.canonicalType(subject));
  const auto patterns = sem_ir_.typeBlock(witness.interface_arguments);
  if (patterns.size() != arguments.size())
    conflict = true;
  for (std::size_t index = 0;
       !conflict && index < patterns.size() && index < arguments.size();
       ++index)
    deduce(deduce, sem_ir_.canonicalType(patterns[index]),
           sem_ir_.canonicalType(arguments[index]));
  if (conflict || std::ranges::any_of(inferred, [](CanonicalTypeId type) {
        return !type.hasValue();
      }))
    return std::nullopt;
  return inferred;
}

[[nodiscard]] SemanticWitnessLookupResult
SemanticContext::instantiateInterfaceWitness(InterfaceWitnessId pattern_id,
                                             TypeId subject,
                                             std::span<const TypeId> arguments,
                                             NodeId location) {
  const auto pattern = sem_ir_.interfaceWitness(pattern_id);
  const auto inferred =
      deduceInterfaceWitnessArguments(pattern, subject, arguments);
  if (!inferred)
    return {};
  const auto append_field = [](std::string &key, std::string_view value) {
    key += std::to_string(value.size());
    key += ':';
    key += value;
  };
  const auto argument_fingerprint = [&](CanonicalTypeId argument) {
    if (auto public_type = concretePublicType(argument))
      return publicTypeFingerprint(*public_type);

    // A dependent or package-private nominal type cannot be represented as a
    // PublicType. Keep the fallback structural: canonical store indexes are
    // deliberately excluded from persistent witness identity.
    std::string canonical;
    const auto append_type = [&](const auto &self,
                                 CanonicalTypeId type_id) -> void {
      const auto &type = values_.generics().type(type_id);
      append_field(canonical, canonicalTypeKindName(type.kind));
      switch (type.kind) {
      case CanonicalTypeKind::Integer:
        append_field(canonical, std::to_string(type.arg0));
        append_field(canonical, std::to_string(type.arg1));
        break;
      case CanonicalTypeKind::Float:
        append_field(canonical, std::to_string(type.arg0));
        break;
      case CanonicalTypeKind::Array:
        append_field(canonical, std::to_string(type.arg1));
        self(self, CanonicalTypeId(type.arg0));
        break;
      case CanonicalTypeKind::TypeParameter: {
        const auto &generic = values_.generics().generic(GenericId(type.arg0));
        append_field(canonical, values_.identifier(generic.module_name));
        append_field(canonical, values_.identifier(generic.name));
        append_field(canonical, std::to_string(type.arg1));
        break;
      }
      case CanonicalTypeKind::Nominal:
      case CanonicalTypeKind::ForeignCompletion:
      case CanonicalTypeKind::ForeignWake:
        append_field(canonical, type.nominal_key);
        break;
      case CanonicalTypeKind::Reference:
      case CanonicalTypeKind::RawPointer:
        append_field(canonical, std::to_string(type.arg0));
        append_field(canonical, std::to_string(type.arg1));
        break;
      case CanonicalTypeKind::AsyncFunction:
      case CanonicalTypeKind::CoroutineTaskCompletionSet:
      case CanonicalTypeKind::CoroutineTaskSelection:
        append_field(canonical, std::to_string(type.arg0));
        break;
      case CanonicalTypeKind::CallbackRegistration:
      case CanonicalTypeKind::CallbackCompletion:
        append_field(canonical, std::to_string(type.registration_authority));
        append_field(canonical,
                     std::to_string(type.registration_entry_parameter));
        append_field(canonical,
                     std::to_string(type.registration_userdata_parameter));
        append_field(canonical,
                     std::to_string(type.registration_release_parameter));
        for (const auto &binding : type.registration_bindings) {
          append_field(canonical, binding.name);
          append_field(canonical, std::to_string(binding.parameter_index));
        }
        for (const auto parameter : type.registration_arm_parameters)
          append_field(canonical, std::to_string(parameter));
        for (const auto parameter : type.registration_detach_parameters)
          append_field(canonical, std::to_string(parameter));
        break;
      default:
        break;
      }
      append_field(canonical, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, element);
    };
    append_type(append_type, argument);
    return StableFingerprint::fromCanonicalBytes(canonical);
  };
  std::vector<StableFingerprint> inferred_fingerprints;
  inferred_fingerprints.reserve(inferred->size());
  std::string key = std::to_string(pattern_id.index);
  for (const auto argument : *inferred) {
    auto fingerprint = argument_fingerprint(argument);
    append_field(key, fingerprint.hex());
    inferred_fingerprints.push_back(std::move(fingerprint));
  }
  if (const auto found = instantiated_interface_witnesses_.find(key);
      found != instantiated_interface_witnesses_.end())
    return {.kind = SemanticWitnessLookupKind::Found, .witness = found->second};
  if (!resolving_interface_witnesses_.insert(key).second) {
    if (location.hasValue())
      emit(DiagnosticKind::ConstraintCycle, location);
    return {.kind = SemanticWitnessLookupKind::Cycle};
  }
  const auto finish = [&](SemanticWitnessLookupResult result) {
    resolving_interface_witnesses_.erase(key);
    return result;
  };
  std::vector<SemGenericSubstitution> dependent_substitutions;
  const auto dependent = [&](auto &&self, CanonicalTypeId id) -> bool {
    const auto &type = values_.generics().type(id);
    if (type.kind == CanonicalTypeKind::TypeParameter ||
        type.kind == CanonicalTypeKind::TypeProjection)
      return true;
    if (type.kind == CanonicalTypeKind::Array &&
        self(self, CanonicalTypeId(type.arg0)))
      return true;
    return std::ranges::any_of(type.elements, [&](CanonicalTypeId element) {
      return self(self, element);
    });
  };
  SemanticObligationWorklist constraint_worklist(pattern.constraints.size());
  SemanticWitnessLookupResult constraint_failure;
  const auto worklist_result =
      constraint_worklist.run([&](std::size_t constraint_index) {
        const auto &constraint = pattern.constraints[constraint_index];
        auto constraint_subject =
            substituteType(constraint.subject, pattern.generic, *inferred);
        std::vector<TypeId> constraint_arguments;
        for (const auto argument : sem_ir_.typeBlock(constraint.arguments))
          constraint_arguments.push_back(
              substituteType(argument, pattern.generic, *inferred));
        for (const auto &substitution : dependent_substitutions) {
          constraint_subject = substituteType(
              constraint_subject, substitution.generic, substitution.arguments);
          for (auto &argument : constraint_arguments)
            argument = substituteType(argument, substitution.generic,
                                      substitution.arguments);
        }
        if (!constraint_subject.hasValue() ||
            dependent(dependent, sem_ir_.canonicalType(constraint_subject)) ||
            std::ranges::any_of(constraint_arguments, [&](TypeId argument) {
              return !argument.hasValue() ||
                     dependent(dependent, sem_ir_.canonicalType(argument));
            }))
          return SemanticObligationState::Deferred;
        const auto constraint_result =
            lookupInterfaceWitness(constraint_subject, constraint.interface_id,
                                   constraint_arguments, location);
        if (!constraint_result.hasValue()) {
          constraint_failure = constraint_result;
          return SemanticObligationState::Failed;
        }
        const auto &interface_value =
            sem_ir_.interface(constraint.interface_id);
        SemGenericSubstitution substitution{
            interface_value.generic,
            std::vector<CanonicalTypeId>(values_.generics()
                                             .generic(interface_value.generic)
                                             .binding_count)};
        for (std::uint32_t index = 0; index < constraint_arguments.size();
             ++index)
          substitution.arguments[index] =
              sem_ir_.canonicalType(constraint_arguments[index]);
        substitution.arguments[interface_value.explicit_parameter_count] =
            sem_ir_.canonicalType(constraint_subject);
        const auto &constraint_witness =
            sem_ir_.interfaceWitness(constraint_result.witness);
        for (const auto &requirement : interface_value.requirements) {
          if (requirement.kind !=
                  SemInterfaceRequirementKind::AssociatedAlias ||
              requirement.binding_index >= substitution.arguments.size())
            continue;
          const auto requirement_index = static_cast<std::uint32_t>(
              &requirement - interface_value.requirements.data());
          const auto entry =
              std::ranges::find(constraint_witness.entries, requirement_index,
                                &SemInterfaceWitnessEntry::requirement);
          if (entry != constraint_witness.entries.end() &&
              entry->associated_type.hasValue())
            substitution.arguments[requirement.binding_index] =
                sem_ir_.canonicalType(entry->associated_type);
        }
        dependent_substitutions.push_back(std::move(substitution));
        return SemanticObligationState::Resolved;
      });
  if (worklist_result == SemanticObligationWorklistResult::Failed)
    return finish(constraint_failure);
  if (worklist_result == SemanticObligationWorklistResult::Stalled)
    return finish({.kind = SemanticWitnessLookupKind::Missing});
  SemInterfaceWitness concrete{pattern.interface_id,
                               subject,
                               sem_ir_.addTypeBlock(arguments),
                               {},
                               location,
                               SemInterfaceWitnessState::Complete};
  for (const auto &entry : pattern.entries) {
    auto function = entry.function;
    if (function.hasValue() &&
        sem_ir_.functionRef(function).generic == pattern.generic)
      function = specializeFunction(function, {}, location, false, *inferred);
    auto associated_type =
        entry.associated_type.hasValue()
            ? substituteType(entry.associated_type, pattern.generic, *inferred)
            : TypeId::invalid();
    for (const auto &substitution : dependent_substitutions)
      if (associated_type.hasValue())
        associated_type = substituteType(associated_type, substitution.generic,
                                         substitution.arguments);
    if ((entry.function.hasValue() && !function.hasValue()) ||
        (entry.associated_type.hasValue() && !associated_type.hasValue()))
      return finish({.kind = SemanticWitnessLookupKind::Invalid});
    concrete.entries.push_back({entry.requirement, function, associated_type});
  }
  std::string fingerprint_key = pattern.fingerprint.hex();
  for (const auto &fingerprint : inferred_fingerprints)
    append_field(fingerprint_key, fingerprint.hex());
  concrete.fingerprint = StableFingerprint::fromCanonicalBytes(fingerprint_key);
  const auto concrete_id = sem_ir_.addInterfaceWitness(std::move(concrete));
  instantiated_interface_witnesses_.emplace(std::move(key), concrete_id);
  return finish(
      {.kind = SemanticWitnessLookupKind::Found, .witness = concrete_id});
}

[[nodiscard]] SemanticWitnessLookupResult
SemanticContext::lookupInterfaceWitness(TypeId subject,
                                        InterfaceId interface_id,
                                        std::span<const TypeId> arguments,
                                        NodeId location) {
  materializeImportedInterfaceWitnesses(location);
  const auto witness_count = sem_ir_.interfaceWitnessCount();
  std::vector<SemanticWitnessCandidate> exact;
  for (std::uint32_t index = 0; index < witness_count; ++index) {
    const auto id = InterfaceWitnessId(index);
    const auto &witness = sem_ir_.interfaceWitness(id);
    if (witness.state != SemInterfaceWitnessState::Complete ||
        witness.generic.hasValue() || witness.interface_id != interface_id ||
        sem_ir_.canonicalType(witness.self_type) !=
            sem_ir_.canonicalType(subject))
      continue;
    const auto witness_arguments =
        sem_ir_.typeBlock(witness.interface_arguments);
    if (witness_arguments.size() == arguments.size() &&
        std::ranges::equal(
            witness_arguments, arguments, [&](TypeId lhs, TypeId rhs) {
              return sem_ir_.canonicalType(lhs) == sem_ir_.canonicalType(rhs);
            }))
      exact.push_back({id, witness.fingerprint});
  }
  const auto exact_result = selectSemanticWitness(exact);
  if (exact_result.kind != SemanticWitnessLookupKind::Missing)
    return exact_result;

  std::vector<SemanticWitnessCandidate> generic_patterns;
  for (std::uint32_t index = 0; index < witness_count; ++index) {
    const auto id = InterfaceWitnessId(index);
    const auto &witness = sem_ir_.interfaceWitness(id);
    if (witness.state != SemInterfaceWitnessState::Complete ||
        !witness.generic.hasValue() || witness.interface_id != interface_id)
      continue;
    generic_patterns.push_back({id, witness.fingerprint});
  }
  std::ranges::sort(generic_patterns, [](const auto &lhs, const auto &rhs) {
    if (lhs.fingerprint.hex() != rhs.fingerprint.hex())
      return lhs.fingerprint.hex() < rhs.fingerprint.hex();
    return lhs.witness.index < rhs.witness.index;
  });
  std::vector<SemanticWitnessCandidate> instantiated;
  SemanticWitnessLookupKind failure = SemanticWitnessLookupKind::Missing;
  for (const auto &candidate : generic_patterns) {
    const auto result = instantiateInterfaceWitness(candidate.witness, subject,
                                                    arguments, location);
    if (result.hasValue()) {
      instantiated.push_back(
          {result.witness,
           sem_ir_.interfaceWitness(result.witness).fingerprint});
    } else if (result.kind == SemanticWitnessLookupKind::Cycle ||
               result.kind == SemanticWitnessLookupKind::Invalid) {
      failure = result.kind;
    }
  }
  const auto result = selectSemanticWitness(instantiated);
  return result.kind == SemanticWitnessLookupKind::Missing &&
                 failure != SemanticWitnessLookupKind::Missing
             ? SemanticWitnessLookupResult{.kind = failure}
             : result;
}

[[nodiscard]] std::vector<FunctionRefId>
SemanticContext::collectInterfaceMemberCandidates(TypeId subject, NameId name,
                                                  NodeId location) {
  materializeImportedInterfaceWitnesses(location);
  std::vector<FunctionRefId> result;
  const auto interface_count = sem_ir_.interfaceCount();
  for (std::uint32_t index = 0; index < interface_count; ++index) {
    const auto interface_id = InterfaceId(index);
    const auto &interface_value = sem_ir_.interface(interface_id);
    if (interface_value.explicit_parameter_count != 0)
      continue;
    const auto requirement = std::ranges::find_if(
        interface_value.requirements, [&](const auto &candidate) {
          return candidate.kind == SemInterfaceRequirementKind::Function &&
                 candidate.name == name;
        });
    if (requirement == interface_value.requirements.end())
      continue;
    const auto witness =
        lookupInterfaceWitness(subject, interface_id, {}, location);
    if (!witness.hasValue())
      continue;
    const auto requirement_index = static_cast<std::uint32_t>(
        requirement - interface_value.requirements.begin());
    const auto &witness_value = sem_ir_.interfaceWitness(witness.witness);
    if (requirement_index < witness_value.entries.size() &&
        witness_value.entries[requirement_index].function.hasValue())
      result.push_back(witness_value.entries[requirement_index].function);
  }
  for (std::uint32_t index = 0; index < sem_ir_.interfaceWitnessCount();
       ++index) {
    const auto &witness = sem_ir_.interfaceWitness(InterfaceWitnessId(index));
    if (witness.state != SemInterfaceWitnessState::Complete ||
        witness.generic.hasValue() ||
        sem_ir_.canonicalType(witness.self_type) !=
            sem_ir_.canonicalType(subject))
      continue;
    const auto &interface_value = sem_ir_.interface(witness.interface_id);
    for (std::uint32_t requirement_index = 0;
         requirement_index < interface_value.requirements.size();
         ++requirement_index) {
      const auto &requirement = interface_value.requirements[requirement_index];
      if (requirement.kind == SemInterfaceRequirementKind::Function &&
          requirement.name == name &&
          requirement_index < witness.entries.size() &&
          witness.entries[requirement_index].function.hasValue())
        result.push_back(witness.entries[requirement_index].function);
    }
  }
  std::ranges::sort(result, {}, &FunctionRefId::index);
  result.erase(std::ranges::unique(result).begin(), result.end());
  return result;
}

