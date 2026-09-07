// Included by SemanticContextSpecialization.cpp; this fragment aliases the
// single SemanticContext specialization state and preserves queue ordering.
void SemanticContext::drainSpecificQueue() {
  constexpr std::size_t InstantiationLimit = 4096;
  std::size_t checked_definitions = 0;
  while (const auto task = deferred_definitions_.popNext()) {
    if (!task->hasPayload())
      continue;
    const auto request = task->definition();
    if (checked_definitions++ >= InstantiationLimit) {
      emit(DiagnosticKind::GenericInstantiationLimit, request.location);
      break;
    }
    const auto state =
        values_.generics().specific(request.specific).definition_state;
    if (state == SpecificRegionState::Ready ||
        state == SpecificRegionState::Failed)
      continue;
    values_.generics().specific(request.specific).definition_state =
        SpecificRegionState::Evaluating;
    const auto &template_reference = sem_ir_.functionRef(request.template_ref);
    const auto template_function =
        sem_ir_.function(template_reference.local_function);
    std::unordered_map<std::uint32_t, LocalId> locals;
    std::string clone_error;
    const auto cloned_body = sem_ir_.cloneSpecificBody(
        template_function.body, template_function.generic, request.arguments,
        request.location, request.dependent_substitutions, locals,
        [&](FunctionRefId callee, std::span<const InstId> arguments,
            NodeId location) {
          auto reference = sem_ir_.functionRef(callee);
          FunctionRefId target;
          // Formation has no call arguments. Rebind its already-selected
          // dependent specific through the enclosing concrete arguments.
          if (arguments.empty() && reference.generic.hasValue() &&
              reference.specific.hasValue() &&
              reference.specific !=
                  values_.generics().generic(reference.generic).self_specific) {
            std::vector<CanonicalTypeId> explicit_arguments;
            for (const auto argument :
                 values_.generics().specific(reference.specific).arguments) {
              const auto symbolic = sem_ir_.materializeType(
                  values_.generics().constant(argument).type);
              auto concrete = substituteType(
                  symbolic, template_function.generic, request.arguments);
              for (const auto &substitution : request.dependent_substitutions)
                concrete = substituteType(concrete, substitution.generic,
                                          substitution.arguments);
              explicit_arguments.push_back(sem_ir_.canonicalType(concrete));
            }
            target = specializeFunction(callee, {}, location, true,
                                        explicit_arguments);
          } else {
            target = specializeFunction(callee, arguments, location, true);
          }
          if (target.hasValue()) {
            const auto dependency = sem_ir_.functionRef(target);
            auto &dependencies =
                values_.generics().specific(request.specific).dependencies;
            dependencies.push_back(
                {.target = dependency.specific,
                 .public_entity = dependency.public_entity.index,
                 .call_ordinal =
                     static_cast<std::uint32_t>(dependencies.size())});
          }
          return target;
        },
        [&](std::uint64_t encoded, std::span<const InstId> call_arguments,
            NodeId location) {
          const auto constraint_index =
              static_cast<std::uint32_t>(encoded >> 32U);
          const auto requirement_index = static_cast<std::uint32_t>(encoded);
          const auto constraints =
              sem_ir_.functionConstraints(template_reference.local_function);
          if (constraint_index >= constraints.size())
            return FunctionRefId::invalid();
          const auto &constraint = constraints[constraint_index];
          auto subject = substituteType(
              constraint.subject, template_function.generic, request.arguments);
          std::vector<TypeId> interface_arguments;
          for (const auto argument : sem_ir_.typeBlock(constraint.arguments))
            interface_arguments.push_back(substituteType(
                argument, template_function.generic, request.arguments));
          for (const auto &substitution : request.dependent_substitutions) {
            subject = substituteType(subject, substitution.generic,
                                     substitution.arguments);
            for (auto &argument : interface_arguments)
              argument = substituteType(argument, substitution.generic,
                                        substitution.arguments);
          }
          const auto witness = lookupInterfaceWitness(
              subject, constraint.interface_id, interface_arguments);
          if (!witness.hasValue())
            return FunctionRefId::invalid();
          const auto &witness_value = sem_ir_.interfaceWitness(witness.witness);
          if (requirement_index >= witness_value.entries.size())
            return FunctionRefId::invalid();
          auto target = witness_value.entries[requirement_index].function;
          if (target.hasValue() &&
              sem_ir_.functionRef(target).generic.hasValue()) {
            const auto &interface_value =
                sem_ir_.interface(constraint.interface_id);
            std::vector<CanonicalTypeId> interface_substitutions(
                values_.generics()
                    .generic(interface_value.generic)
                    .binding_count);
            for (std::uint32_t index = 0; index < interface_arguments.size();
                 ++index)
              interface_substitutions[index] =
                  sem_ir_.canonicalType(interface_arguments[index]);
            interface_substitutions[interface_value.explicit_parameter_count] =
                sem_ir_.canonicalType(subject);
            for (const auto &requirement : interface_value.requirements) {
              if (requirement.kind !=
                      SemInterfaceRequirementKind::AssociatedAlias ||
                  requirement.binding_index >= interface_substitutions.size())
                continue;
              const auto entry = std::ranges::find(
                  witness_value.entries,
                  static_cast<std::uint32_t>(
                      &requirement - interface_value.requirements.data()),
                  &SemInterfaceWitnessEntry::requirement);
              if (entry != witness_value.entries.end() &&
                  entry->associated_type.hasValue())
                interface_substitutions[requirement.binding_index] =
                    sem_ir_.canonicalType(entry->associated_type);
            }
            target =
                sem_ir_.functionRef(target).generic == interface_value.generic
                    ? specializeFunction(target, call_arguments, location, true,
                                         interface_substitutions)
                    : specializeFunction(target, call_arguments, location,
                                         true);
          }
          if (target.hasValue())
            sem_ir_.addFunctionOccurrence(
                location, SemSymbolOccurrenceKind::Reference, target);
          return target;
        },
        clone_error);
    if (!cloned_body.hasValue()) {
      values_.generics().specific(request.specific).definition_state =
          SpecificRegionState::Failed;
      emit(DiagnosticKind::ConcreteSpecializationFailure, request.location);
      continue;
    }
    std::vector<LocalId> parameters;
    bool missing_parameter = false;
    for (const auto old : sem_ir_.localBlock(template_function.parameters)) {
      const auto found = locals.find(old.index);
      if (found == locals.end()) {
        missing_parameter = true;
        break;
      }
      parameters.push_back(found->second);
    }
    if (missing_parameter) {
      values_.generics().specific(request.specific).definition_state =
          SpecificRegionState::Failed;
      emit(DiagnosticKind::ConcreteSpecializationFailure, request.location);
      continue;
    }
    const auto placeholder = sem_ir_.functionRef(request.specific_ref);
    auto concrete = sem_ir_.function(placeholder.local_function);
    concrete.parameters = sem_ir_.addLocalBlock(parameters);
    concrete.body = cloned_body;
    sem_ir_.setFunction(placeholder.local_function, concrete);
    values_.generics().specific(request.specific).definition_state =
        SpecificRegionState::Ready;
  }
}

void SemanticContext::materializeRepresentationSpecifics() {
  const auto concrete_type = [&](auto &&self, TypeId type) -> bool {
    const auto &value = sem_ir_.type(type);
    if (value.kind == SemTypeKind::TypeParameter ||
        value.kind == SemTypeKind::TypeProjection ||
        value.kind == SemTypeKind::Invalid || value.kind == SemTypeKind::Void ||
        value.kind == SemTypeKind::Function || value.kind == SemTypeKind::Count)
      return false;
    if (value.kind == SemTypeKind::Array)
      return self(self, TypeId(value.arg0));
    if (value.kind != SemTypeKind::Nominal)
      return true;
    return std::ranges::all_of(
        sem_ir_.typeBlock(TypeBlockId(value.arg1)),
        [&](TypeId argument) { return self(self, argument); });
  };
  for (std::uint32_t type_index = 0; type_index < sem_ir_.typeCount();
       ++type_index) {
    const auto type = TypeId(type_index);
    const auto &value = sem_ir_.type(type);
    if (value.kind != SemTypeKind::Nominal ||
        !concrete_type(concrete_type, type))
      continue;
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(value.arg0));
    const auto has_computed =
        std::ranges::any_of(nominal.fields, [](const auto &field) {
          return field.projection_kind == PublicObjectProjectionKind::Computed;
        });
    if (!nominal.value_repr_pattern.hasValue() &&
        nominal.object_projectors.empty() &&
        !nominal.object_init_function.hasValue() && !has_computed)
      continue;
    std::vector<CanonicalTypeId> arguments;
    for (const auto argument : sem_ir_.typeBlock(TypeBlockId(value.arg1)))
      arguments.push_back(sem_ir_.canonicalType(argument));
    const auto materialize = [&](FunctionId function_id) {
      if (!function_id.hasValue())
        return;
      const auto &function = sem_ir_.function(function_id);
      if (!function.generic.hasValue())
        return;
      std::vector<CanonicalConstantId> constants;
      constants.reserve(arguments.size());
      for (const auto argument : arguments)
        constants.push_back(values_.generics().internTypeConstant(
            argument, ConstantDependence::Concrete));
      const auto specific =
          values_.generics().getOrAddSpecific(function.generic, constants);
      FunctionRefId template_ref;
      for (std::uint32_t ref_index = 0; ref_index < sem_ir_.functionRefCount();
           ++ref_index) {
        const auto candidate = FunctionRefId(ref_index);
        if (sem_ir_.functionRef(candidate).local_function == function_id) {
          template_ref = candidate;
          break;
        }
      }
      if (!template_ref.hasValue() ||
          !materializeLocalSpecific(template_ref, specific, arguments,
                                    nominal.declaration)
               .hasValue())
        emit(DiagnosticKind::ConcreteSpecializationFailure,
             nominal.declaration);
    };
    materialize(nominal.representation_pack_function);
    materialize(nominal.representation_init_function);
    for (const auto &projector : nominal.object_projectors) {
      materialize(projector.load_function);
      materialize(projector.store_function);
      materialize(projector.take_function);
      materialize(projector.init_function);
      materialize(projector.borrow_function);
      materialize(projector.borrow_mut_function);
    }
    materialize(nominal.object_init_function);
    materialize(nominal.object_copy_init_function);
    materialize(nominal.object_move_init_function);
    materialize(nominal.object_drop_function);
    if (has_computed && nominal.object_projectors.empty()) {
      const auto *owner =
          sem_ir_.importIRs().tryGetEntity(nominal.canonical_entity);
      const auto materialize_hidden = [&](std::string hidden) {
        if (!owner)
          return;
        const auto entity = sem_ir_.importIRs().registry().findEntity(
            values_.identifier(owner->package_name),
            values_.identifier(owner->module_name), hidden,
            PublicEntityKind::Function);
        if (!entity.hasValue())
          return;
        const auto target = resolvePublicEntity(entity, nominal.declaration);
        if (!target.hasValue() ||
            !sem_ir_.functionRef(target).local_function.hasValue()) {
          emit(DiagnosticKind::ConcreteSpecializationFailure,
               nominal.declaration);
          return;
        }
        materialize(sem_ir_.functionRef(target).local_function);
      };
      std::unordered_set<std::string> projectors;
      for (const auto &field : nominal.fields) {
        if (field.projection_kind != PublicObjectProjectionKind::Computed)
          continue;
        const auto projector = std::string(
            values_.identifier(sem_ir_.name(field.projector_name).text));
        if (!projectors.insert(projector).second)
          continue;
        for (const auto role :
             {"load", "store", "take", "init", "borrow", "borrow_mut"})
          materialize_hidden("$projection$" +
                             std::string(values_.identifier(owner->name)) +
                             "$" + projector + "$" + role);
      }
      for (const auto role : {"object_init", "object_copy_init",
                              "object_move_init", "object_drop"})
        materialize_hidden("$object$" +
                           std::string(values_.identifier(owner->name)) + "$" +
                           role);
    }
  }
}

[[nodiscard]] std::optional<ConcreteSpecificNodeArtifact>
