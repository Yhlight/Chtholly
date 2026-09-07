// Included by SemanticContextSpecialization.cpp; this fragment aliases the
// single SemanticContext specialization state and preserves queue ordering.
[[nodiscard]] FunctionRefId SemanticContext::specializeFunction(
    FunctionRefId template_ref, std::span<const InstId> arguments,
    NodeId call_node, bool diagnose,
    std::span<const CanonicalTypeId> explicit_arguments,
    std::size_t parameter_offset) {
  auto reference = sem_ir_.functionRef(template_ref);
  if (!reference.generic.hasValue())
    return template_ref;
  const auto generic_id = reference.generic;
  const auto generic = values_.generics().generic(generic_id);
  if (reference.specific != generic.self_specific) {
    const auto canonical = generic_template_refs_.find(generic_id.index);
    if (canonical == generic_template_refs_.end()) {
      if (diagnose)
        emit(DiagnosticKind::ConcreteSpecializationFailure, call_node);
      return FunctionRefId::invalid();
    }
    template_ref = canonical->second;
    reference = sem_ir_.functionRef(template_ref);
  }
  auto inference_type = reference.local_type;
  if (reference.local_function.hasValue() &&
      reference.specific == generic.self_specific)
    inference_type = sem_ir_.function(reference.local_function).type;
  const auto &function_type = sem_ir_.type(inference_type);
  const auto parameters = sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
  std::vector<CanonicalTypeId> inferred(generic.binding_count,
                                        CanonicalTypeId::invalid());
  bool conflict = false;
  if (!explicit_arguments.empty()) {
    if (explicit_arguments.size() != inferred.size()) {
      if (diagnose)
        emit(DiagnosticKind::ArgumentCountMismatch, call_node);
      return FunctionRefId::invalid();
    }
    std::ranges::copy(explicit_arguments, inferred.begin());
  }
  const auto infer_type = [&](auto &&self, CanonicalTypeId formal_id,
                              CanonicalTypeId actual_id) -> void {
    const auto &formal = values_.generics().type(formal_id);
    const auto &actual = values_.generics().type(actual_id);
    if (formal.kind == CanonicalTypeKind::TypeParameter &&
        formal.arg0 == generic_id.index && formal.arg1 < inferred.size()) {
      if (!sem_ir_.isGenericArgumentType(actual_id)) {
        conflict = true;
        return;
      }
      auto &slot = inferred[formal.arg1];
      if (slot.hasValue() && slot != actual_id)
        conflict = true;
      else
        slot = actual_id;
      return;
    }
    if (formal.kind == CanonicalTypeKind::TypeProjection) {
      const auto resolved =
          substituteCanonicalType(formal_id, generic_id, inferred);
      if (!resolved.hasValue()) {
        conflict = true;
        return;
      }
      if (values_.generics().type(resolved).kind ==
          CanonicalTypeKind::TypeProjection)
        return;
      if (resolved != actual_id)
        conflict = true;
      return;
    }
    if (formal.kind == CanonicalTypeKind::Array) {
      if (actual.kind != CanonicalTypeKind::Array ||
          formal.arg1 != actual.arg1) {
        conflict = true;
        return;
      }
      self(self, CanonicalTypeId(formal.arg0), CanonicalTypeId(actual.arg0));
      return;
    }
    if (formal.kind == CanonicalTypeKind::Reference) {
      constexpr std::uint32_t MutableReferenceBit = 1U;
      if (formal.elements.size() != 1) {
        conflict = true;
        return;
      }
      if (actual.kind != CanonicalTypeKind::Reference) {
        self(self, formal.elements.front(), actual_id);
        return;
      }
      if ((formal.arg0 & MutableReferenceBit) !=
              (actual.arg0 & MutableReferenceBit) ||
          actual.elements.size() != 1) {
        conflict = true;
        return;
      }
      // Provenance is local to a function and is not generic type identity.
      self(self, formal.elements.front(), actual.elements.front());
      return;
    }
    if (formal.kind != actual.kind || formal.arg0 != actual.arg0 ||
        formal.arg1 != actual.arg1 ||
        formal.nominal_key != actual.nominal_key ||
        formal.callable_contract != actual.callable_contract ||
        formal.callable_context_parameter !=
            actual.callable_context_parameter ||
        formal.registration_authority != actual.registration_authority ||
        formal.registration_entry_parameter !=
            actual.registration_entry_parameter ||
        formal.registration_userdata_parameter !=
            actual.registration_userdata_parameter ||
        formal.registration_release_parameter !=
            actual.registration_release_parameter ||
        formal.registration_bindings != actual.registration_bindings ||
        formal.registration_arm_parameters !=
            actual.registration_arm_parameters ||
        formal.registration_detach_parameters !=
            actual.registration_detach_parameters ||
        formal.elements.size() != actual.elements.size()) {
      conflict = true;
      return;
    }
    for (std::size_t element = 0; element < formal.elements.size(); ++element)
      self(self, formal.elements[element], actual.elements[element]);
  };
  const auto contextual_literal = [&](InstId value) {
    return conversions_.isContextualLiteral(value);
  };
  for (std::size_t index = 0;
       index < arguments.size() && index + parameter_offset < parameters.size();
       ++index)
    if (!contextual_literal(arguments[index]))
      infer_type(infer_type,
                 sem_ir_.canonicalType(parameters[index + parameter_offset]),
                 sem_ir_.canonicalType(instType(arguments[index])));
  for (std::size_t index = 0;
       index < arguments.size() && index + parameter_offset < parameters.size();
       ++index) {
    if (!contextual_literal(arguments[index]))
      continue;
    const auto formal_id =
        sem_ir_.canonicalType(parameters[index + parameter_offset]);
    const auto &formal = values_.generics().type(formal_id);
    if (formal.kind == CanonicalTypeKind::TypeParameter &&
        formal.arg0 == generic_id.index && formal.arg1 < inferred.size() &&
        inferred[formal.arg1].hasValue()) {
      const auto target = sem_ir_.materializeType(inferred[formal.arg1]);
      if (!target.hasValue() || !canAdjustExpression(arguments[index], target))
        conflict = true;
      continue;
    }
    infer_type(infer_type, formal_id,
               sem_ir_.canonicalType(instType(arguments[index])));
  }
  if (conflict) {
    if (diagnose)
      emit(DiagnosticKind::ConflictingGenericInference, call_node);
    return FunctionRefId::invalid();
  }
  if (std::ranges::any_of(inferred,
                          [](CanonicalTypeId id) { return !id.hasValue(); })) {
    if (diagnose)
      emit(DiagnosticKind::CannotInferGenericArguments, call_node);
    return FunctionRefId::invalid();
  }
  const auto declaration_only =
      std::ranges::any_of(inferred, [&](CanonicalTypeId id) {
        return values_.generics().type(id).kind ==
               CanonicalTypeKind::TypeParameter;
      });
  std::vector<StableFingerprint> constraint_witnesses;
  std::vector<SemGenericSubstitution> dependent_substitutions;
  if (reference.local_function.hasValue()) {
    const auto constraints =
        sem_ir_.functionConstraints(reference.local_function);
    if (declaration_only) {
      for (const auto &constraint : constraints) {
        const auto subject =
            substituteType(constraint.subject, generic_id, inferred);
        if (!subject.hasValue()) {
          if (diagnose)
            emit(DiagnosticKind::UnsatisfiedConstraint, call_node);
          return FunctionRefId::invalid();
        }
        const auto &interface_value =
            sem_ir_.interface(constraint.interface_id);
        constraint_witnesses.push_back(StableFingerprint::fromCanonicalBytes(
            "symbolic\0" + interface_value.fingerprint.hex() + "\0" +
            std::to_string(sem_ir_.canonicalType(subject).index)));
      }
    } else {
      std::vector<std::optional<StableFingerprint>> resolved(
          constraints.size());
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
      std::size_t remaining = constraints.size();
      while (remaining != 0) {
        bool progress = false;
        for (std::size_t constraint_index = 0;
             constraint_index < constraints.size(); ++constraint_index) {
          if (resolved[constraint_index])
            continue;
          const auto &constraint = constraints[constraint_index];
          auto subject =
              substituteType(constraint.subject, generic_id, inferred);
          std::vector<TypeId> constraint_arguments;
          for (const auto argument : sem_ir_.typeBlock(constraint.arguments))
            constraint_arguments.push_back(
                substituteType(argument, generic_id, inferred));
          for (const auto &substitution : dependent_substitutions) {
            subject = substituteType(subject, substitution.generic,
                                     substitution.arguments);
            for (auto &argument : constraint_arguments)
              argument = substituteType(argument, substitution.generic,
                                        substitution.arguments);
          }
          if (!subject.hasValue() ||
              std::ranges::any_of(constraint_arguments, [](TypeId value) {
                return !value.hasValue();
              }))
            continue;
          if (dependent(dependent, sem_ir_.canonicalType(subject)) ||
              std::ranges::any_of(constraint_arguments, [&](TypeId argument) {
                return dependent(dependent, sem_ir_.canonicalType(argument));
              }))
            continue;
          const auto witness =
              lookupInterfaceWitness(subject, constraint.interface_id,
                                     constraint_arguments, call_node);
          if (!witness.hasValue())
            continue;
          const auto &witness_value = sem_ir_.interfaceWitness(witness.witness);
          resolved[constraint_index] = witness_value.fingerprint;
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
              sem_ir_.canonicalType(subject);
          for (const auto &requirement : interface_value.requirements) {
            if (requirement.kind !=
                    SemInterfaceRequirementKind::AssociatedAlias ||
                requirement.binding_index >= substitution.arguments.size())
              continue;
            const auto entry = std::ranges::find(
                witness_value.entries,
                static_cast<std::uint32_t>(&requirement -
                                           interface_value.requirements.data()),
                &SemInterfaceWitnessEntry::requirement);
            if (entry != witness_value.entries.end() &&
                entry->associated_type.hasValue())
              substitution.arguments[requirement.binding_index] =
                  sem_ir_.canonicalType(entry->associated_type);
          }
          if (std::ranges::any_of(
                  substitution.arguments,
                  [](CanonicalTypeId value) { return !value.hasValue(); }))
            continue;
          dependent_substitutions.push_back(std::move(substitution));
          --remaining;
          progress = true;
        }
        if (progress)
          continue;
        if (diagnose) {
          DiagnosticKind failure = DiagnosticKind::UnsatisfiedConstraint;
          for (std::size_t pending_index = 0;
               pending_index < constraints.size(); ++pending_index) {
            if (resolved[pending_index])
              continue;
            const auto &pending = constraints[pending_index];
            const auto &interface_value =
                sem_ir_.interface(pending.interface_id);
            if (!interface_value.canonical_entity.hasValue())
              continue;
            const auto *entity = sem_ir_.importIRs().tryGetEntity(
                interface_value.canonical_entity);
            if (!entity || values_.identifier(entity->package_name) != "std" ||
                values_.identifier(entity->module_name) != "std::hash")
              continue;
            const auto name = values_.identifier(entity->name);
            if (name == "Hash") {
              failure = DiagnosticKind::MissingHashWitness;
              break;
            }
            if (name == "Equal") {
              failure = DiagnosticKind::MissingEqualWitness;
              break;
            }
          }
          emit(failure, call_node);
        }
        return FunctionRefId::invalid();
      }
      for (const auto &fingerprint : resolved)
        constraint_witnesses.push_back(*fingerprint);
    }
  }
  if (reference.public_entity.hasValue()) {
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(reference.public_entity);
    if (entity && values_.identifier(entity->package_name) == "std" &&
        values_.identifier(entity->module_name) == "std::iter::algorithms" &&
        values_.identifier(entity->name) == "collect_vec") {
      auto return_type =
          substituteType(TypeId(function_type.arg1), generic_id, inferred);
      for (const auto &substitution : dependent_substitutions)
        return_type = substituteType(return_type, substitution.generic,
                                     substitution.arguments);
      TypeId item_type;
      if (return_type.hasValue() &&
          sem_ir_.type(return_type).kind == SemTypeKind::Nominal) {
        const auto return_arguments =
            sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(return_type).arg1));
        if (return_arguments.size() == 1)
          item_type = return_arguments.front();
      }
      const auto representation = item_type.hasValue()
                                      ? sem_ir_.typeRepresentation(item_type)
                                      : TypeRepresentationFacts{};
      if (!item_type.hasValue() || !hasObjectRepresentation(item_type) ||
          sem_ir_.loanCarrierCapability(item_type) !=
              SemLoanCarrierCapability::None ||
          representation.move == MoveReprKind::Unavailable) {
        if (diagnose)
          emit(DiagnosticKind::CollectVecRequiresOwnedItem, call_node);
        return FunctionRefId::invalid();
      }
    }
  }
  std::vector<CanonicalConstantId> constants;
  constants.reserve(inferred.size());
  for (const auto type : inferred)
    constants.push_back(values_.generics().internTypeConstant(
        type, declaration_only ? ConstantDependence::CheckedSymbolic
                               : ConstantDependence::Concrete));
  const auto specific = values_.generics().getOrAddSpecific(
      generic_id, constants, constraint_witnesses);
  if (!declaration_only &&
      sem_ir_.functionIntrinsicRole(template_ref) ==
          CompilerIntrinsicRole::None &&
      reference.public_entity.hasValue() &&
      semantic_options_fingerprint_.hasValue()) {
    const auto entity = publicEntityReference(reference.public_entity);
    std::vector<PublicType> public_arguments;
    public_arguments.reserve(inferred.size());
    for (const auto type : inferred) {
      const auto public_type = concretePublicType(type);
      if (!public_type) {
        public_arguments.clear();
        break;
      }
      public_arguments.push_back(*public_type);
    }
    if (entity && !public_arguments.empty()) {
      const auto request_fingerprint = fingerprintConcreteSpecializationRequest(
          *entity, public_arguments, constraint_witnesses,
          semantic_options_fingerprint_);
      CacheableSpecific cacheable{request_fingerprint, *entity,
                                  std::move(public_arguments),
                                  constraint_witnesses};
      cacheable_specifics_.insert_or_assign(specific.index, cacheable);
      if (!specific_function_refs_.contains(specific.index)) {
        const auto loaded = tryLoadConcreteSpecific(cacheable, call_node);
        if (loaded.hasValue())
          return loaded;
      }
    }
  }
  return materializeLocalSpecific(template_ref, specific, inferred, call_node,
                                  declaration_only, true,
                                  dependent_substitutions);
}
