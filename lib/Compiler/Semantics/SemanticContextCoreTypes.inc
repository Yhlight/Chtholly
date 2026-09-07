// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
[[nodiscard]] TypeId SemanticContext::checkType(NodeId node) {
  if (tree_.kind(node) == NodeKind::ElementTypeExpr ||
      tree_.kind(node) == NodeKind::PointeeTypeExpr) {
    const auto children = tree_.children(node);
    if (children.empty() || (tree_.kind(node) == NodeKind::ElementTypeExpr &&
                             children.size() != 2)) {
      emit(DiagnosticKind::UnknownType, node);
      return sem_ir_.voidType();
    }
    const auto source = checkType(children.front());
    const auto source_kind = sem_ir_.type(source).kind;
    if (tree_.kind(node) == NodeKind::PointeeTypeExpr) {
      if (source_kind == SemTypeKind::Reference)
        return sem_ir_.referencePointee(source);
      if (source_kind == SemTypeKind::RawPointer)
        return sem_ir_.rawPointerPointee(source);
      if (source_kind == SemTypeKind::Slice)
        return sem_ir_.sliceElementType(source);
      if (source_kind == SemTypeKind::TypeParameter ||
          source_kind == SemTypeKind::TypeProjection)
        return sem_ir_.addType(
            {SemTypeKind::TypeProjection, source.index,
             static_cast<std::uint32_t>(SemTypeProjectionKind::Pointee)
                 << 31U});
      emit(DiagnosticKind::TypeMismatch, children.front());
      return sem_ir_.voidType();
    }
    if (tree_.kind(children[1]) != NodeKind::IntegerLiteral) {
      emit(DiagnosticKind::InvalidConstantInitializer, children[1]);
      return sem_ir_.voidType();
    }
    const auto parsed =
        parseIntegerLiteral(tree_.tokens().text(tree_.token(children[1])));
    if (!parsed || parsed->magnitude > 0x7fffffffU) {
      emit(DiagnosticKind::TypeMismatch, node);
      return sem_ir_.voidType();
    }
    if (source_kind == SemTypeKind::TypeParameter ||
        source_kind == SemTypeKind::TypeProjection)
      return sem_ir_.addType({SemTypeKind::TypeProjection, source.index,
                              static_cast<std::uint32_t>(parsed->magnitude)});
    if (source_kind == SemTypeKind::Tuple &&
        parsed->magnitude < sem_ir_.tupleArity(source))
      return sem_ir_.tupleElementType(
          source, static_cast<std::uint32_t>(parsed->magnitude));
    if (source_kind == SemTypeKind::Array &&
        parsed->magnitude < sem_ir_.type(source).arg1)
      return TypeId(sem_ir_.type(source).arg0);
    emit(DiagnosticKind::TypeMismatch, node);
    return sem_ir_.voidType();
  }
  if (tree_.kind(node) == NodeKind::OwnedPointerType) {
    const auto children = tree_.children(node);
    if (!checking_callback_context_ || children.size() != 1 ||
        tree_.kind(children.front()) != NodeKind::RawPointerType) {
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
      return sem_ir_.voidType();
    }
    const auto pointer = checkType(children.front());
    if (sem_ir_.type(pointer).kind != SemTypeKind::RawPointer ||
        sem_ir_.rawPointerPointee(pointer) != sem_ir_.voidType() ||
        sem_ir_.rawPointerPointeeConst(pointer))
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
    return pointer;
  }
  if (tree_.kind(node) == NodeKind::CallbackAdapterType) {
    TypeId entry;
    TypeId context;
    TypeId release;
    for (const auto field : tree_.children(node)) {
      const auto children = tree_.children(field);
      if (tree_.kind(field) != NodeKind::CallbackAdapterTypeField ||
          children.size() != 2)
        continue;
      const auto name = values_.identifier(tokenString(children.front()));
      const auto old_checking_context = checking_callback_context_;
      checking_callback_context_ = name == "context";
      const auto field_type = checkType(children.back());
      checking_callback_context_ = old_checking_context;
      auto *slot = name == "entry"     ? &entry
                   : name == "context" ? &context
                   : name == "release" ? &release
                                       : nullptr;
      if (!slot || slot->hasValue()) {
        emit(DiagnosticKind::InvalidCallbackAdapter, field);
        continue;
      }
      *slot = field_type;
      if (name == "context" &&
          tree_.kind(children.back()) != NodeKind::OwnedPointerType)
        emit(DiagnosticKind::InvalidCallbackAdapter, children.back());
    }
    const auto valid_context = [&] {
      return context.hasValue() &&
             sem_ir_.type(context).kind == SemTypeKind::RawPointer &&
             sem_ir_.rawPointerPointee(context) == sem_ir_.voidType() &&
             !sem_ir_.rawPointerPointeeConst(context);
    };
    const auto valid_context_callable = [&](TypeId callable, bool is_release) {
      if (!callable.hasValue() ||
          sem_ir_.type(callable).kind != SemTypeKind::CFunctionPointer)
        return false;
      const auto parameters =
          sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callable).arg0));
      const auto context_index = sem_ir_.callbackContextParameter(callable);
      if (context_index >= parameters.size() ||
          parameters[context_index] != context)
        return false;
      const auto &contract = sem_ir_.callbackContract(callable);
      if (!is_release) {
        const auto invalid_effect =
            std::ranges::any_of(contract.effects, [&](const auto &effect) {
              return effect.region.parameter_index == context_index &&
                     effect.kind != CallableEffectKind::Read &&
                     effect.kind != CallableEffectKind::Write;
            });
        const auto invalid_post = std::ranges::any_of(
            contract.postconditions, [&](const auto &postcondition) {
              if (postcondition.region.parameter_index != context_index)
                return false;
              const auto derived_from_write = std::ranges::any_of(
                  contract.effects, [&](const auto &effect) {
                    return effect.kind == CallableEffectKind::Write &&
                           effect.region == postcondition.region;
                  });
              return !derived_from_write ||
                     postcondition.outcomes !=
                         (CallableOutcomePreserve | CallableOutcomeInitialize);
            });
        const auto invalid_return = std::ranges::any_of(
            contract.return_provenance, [&](const auto &source) {
              return source.region.parameter_index == context_index ||
                     std::ranges::any_of(
                         source.condition.clauses, [&](const auto &clause) {
                           return std::ranges::any_of(
                               clause.atoms, [&](const auto &atom) {
                                 return atom.parameter_index == context_index;
                               });
                         });
            });
        return !invalid_effect && !invalid_post && !invalid_return;
      }
      const OwnershipRegion root{.parameter_index = 0};
      return parameters.size() == 1 && context_index == 0 &&
             TypeId(sem_ir_.type(callable).arg1) == sem_ir_.voidType() &&
             contract.effects ==
                 std::vector<CallableRegionEffect>{
                     {CallableEffectKind::Take, root}} &&
             contract.postconditions ==
                 std::vector<CallableRegionPostcondition>{
                     {root, CallableOutcomeInvalidate}} &&
             contract.returns_owned && contract.return_provenance.empty();
    };
    if (!entry.hasValue() || !release.hasValue() || !valid_context() ||
        !valid_context_callable(entry, false) ||
        !valid_context_callable(release, true)) {
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
      return sem_ir_.voidType();
    }
    return sem_ir_.addCallbackAdapterType(entry, context, release);
  }
  if (tree_.kind(node) == NodeKind::FunctionPointerType) {
    const auto children = tree_.children(node);
    if (children.size() == 2 &&
        tree_.kind(children[0]) == NodeKind::ParameterList) {
      std::vector<TypeId> parameters;
      for (const auto parameter : tree_.children(children[0])) {
        const auto type = checkType(parameter);
        if (!hasObjectRepresentation(type))
          emit(DiagnosticKind::InvalidObjectType, parameter);
        parameters.push_back(type);
      }
      return sem_ir_.addFunctionType(parameters, checkType(children[1]));
    }
    if ((children.size() != 3 && children.size() != 4) ||
        tree_.kind(children[0]) != NodeKind::ForeignAbi ||
        tree_.kind(children[1]) != NodeKind::ParameterList ||
        tree_.tokens().text(tree_.token(children[0])) != "\"C\"") {
      emit(DiagnosticKind::UnknownType, node);
      return sem_ir_.voidType();
    }
    auto parameter_nodes = tree_.children(children[1]);
    const auto is_variadic =
        !parameter_nodes.empty() &&
        tree_.kind(parameter_nodes.back()) == NodeKind::VariadicParameter;
    if (is_variadic)
      parameter_nodes.pop_back();
    if (is_variadic && parameter_nodes.empty())
      emit(DiagnosticKind::InvalidVariadicDeclaration, children[1]);
    std::vector<TypeId> parameters;
    std::vector<NodeId> contract_parameter_nodes;
    bool has_names = false;
    bool has_unnamed = false;
    std::uint32_t context_parameter = core::AnyId::InvalidIndex;
    CallbackParameterRoles registration_roles;
    for (std::size_t index = 0; index < parameter_nodes.size();) {
      if (tree_.kind(parameter_nodes[index]) == NodeKind::ContextParameter ||
          tree_.kind(parameter_nodes[index]) == NodeKind::EntryParameter ||
          tree_.kind(parameter_nodes[index]) == NodeKind::UserdataParameter ||
          tree_.kind(parameter_nodes[index]) == NodeKind::ReleaseParameter ||
          tree_.kind(parameter_nodes[index]) == NodeKind::TokenParameter ||
          tree_.kind(parameter_nodes[index]) == NodeKind::BoundParameter) {
        const auto context_children = tree_.children(parameter_nodes[index]);
        const auto parameter_kind = tree_.kind(parameter_nodes[index]);
        const auto is_bound = parameter_kind == NodeKind::BoundParameter;
        const auto role_index =
            parameter_kind == NodeKind::EntryParameter      ? 0
            : parameter_kind == NodeKind::UserdataParameter ? 1
            : parameter_kind == NodeKind::ReleaseParameter  ? 2
            : parameter_kind == NodeKind::TokenParameter    ? 3
                                                            : -1;
        if ((!is_bound && role_index < 0 &&
             context_parameter != core::AnyId::InvalidIndex) ||
            (role_index >= 0 &&
             registration_roles
                     .synthesized[static_cast<std::size_t>(role_index)] !=
                 core::AnyId::InvalidIndex) ||
            context_children.size() != 2 ||
            tree_.kind(context_children.front()) != NodeKind::Name) {
          emit(DiagnosticKind::InvalidCallbackAdapter,
               parameter_nodes[index++]);
          continue;
        }
        if (is_bound) {
          const auto binding_name = std::string(
              values_.identifier(tokenString(context_children.front())));
          if (std::ranges::any_of(registration_roles.bindings,
                                  [&](const auto &binding) {
                                    return binding.name == binding_name;
                                  }))
            emit(DiagnosticKind::InvalidCallbackRegistrationBinding,
                 parameter_nodes[index]);
          registration_roles.bindings.push_back(
              {std::move(binding_name),
               static_cast<std::uint32_t>(parameters.size())});
        } else if (role_index < 0) {
          context_parameter = static_cast<std::uint32_t>(parameters.size());
        } else {
          registration_roles.synthesized[static_cast<std::size_t>(role_index)] =
              static_cast<std::uint32_t>(parameters.size());
        }
        has_names = true;
        contract_parameter_nodes.push_back(context_children.front());
        contract_parameter_nodes.push_back(context_children.back());
        const auto parameter_type = checkType(context_children.back());
        if (!hasObjectRepresentation(parameter_type))
          emit(DiagnosticKind::InvalidObjectType, context_children.back());
        parameters.push_back(parameter_type);
        ++index;
      } else if (tree_.kind(parameter_nodes[index]) == NodeKind::Name) {
        has_names = true;
        if (index + 1 >= parameter_nodes.size()) {
          emit(DiagnosticKind::InvalidCallableContract, node);
          break;
        }
        contract_parameter_nodes.push_back(parameter_nodes[index]);
        contract_parameter_nodes.push_back(parameter_nodes[index + 1]);
        const auto parameter_type = checkType(parameter_nodes[index + 1]);
        if (!hasObjectRepresentation(parameter_type))
          emit(DiagnosticKind::InvalidObjectType, parameter_nodes[index + 1]);
        parameters.push_back(parameter_type);
        index += 2;
      } else {
        has_unnamed = true;
        const auto parameter = parameter_nodes[index++];
        const auto parameter_type = checkType(parameter);
        if (!hasObjectRepresentation(parameter_type))
          emit(DiagnosticKind::InvalidObjectType, parameter);
        parameters.push_back(parameter_type);
      }
    }
    const auto result = checkType(children[2]);
    CallableOwnershipSummary contract;
    const auto has_contract = children.size() == 4;
    if (has_contract) {
      if (!has_names || has_unnamed) {
        emit(DiagnosticKind::InvalidCallableContract, node);
      } else if (auto parsed = declaredContract(node, contract_parameter_nodes,
                                                parameters, result)) {
        contract = std::move(*parsed);
      }
    }
    const auto contains_reference = [&](auto &&self, TypeId type,
                                        std::uint32_t depth) -> bool {
      if (depth > 128)
        return true;
      const auto &value = sem_ir_.type(type);
      if (value.kind == SemTypeKind::Reference)
        return true;
      if (value.kind == SemTypeKind::Array)
        return self(self, TypeId(value.arg0), depth + 1);
      if (value.kind != SemTypeKind::Nominal)
        return false;
      const auto &fields =
          sem_ir_.nominalType(NominalTypeId(value.arg0)).fields;
      for (std::uint32_t index = 0; index < fields.size(); ++index)
        if (self(self, sem_ir_.nominalFieldType(type, index), depth + 1))
          return true;
      return false;
    };
    if (!has_contract &&
        (contains_reference(contains_reference, result, 0) ||
         std::ranges::any_of(parameters, [&](TypeId parameter) {
           return contains_reference(contains_reference, parameter, 0);
         })))
      emit(DiagnosticKind::InvalidCallableContract, node);
    if (context_parameter != core::AnyId::InvalidIndex) {
      if (is_variadic || context_parameter >= parameters.size() ||
          sem_ir_.type(parameters[context_parameter]).kind !=
              SemTypeKind::RawPointer ||
          sem_ir_.rawPointerPointee(parameters[context_parameter]) !=
              sem_ir_.voidType() ||
          sem_ir_.rawPointerPointeeConst(parameters[context_parameter]))
        emit(DiagnosticKind::InvalidCallbackAdapter, node);
      if (!has_contract)
        emit(DiagnosticKind::InvalidCallableContract, node);
    }
    const auto type =
        sem_ir_.addCFunctionPointerType(parameters, result, is_variadic,
                                        std::move(contract), context_parameter);
    std::ranges::sort(registration_roles.bindings, {},
                      &CallbackRegistrationBinding::parameter_index);
    parameter_roles_[type.index] = std::move(registration_roles);
    return type;
  }
  if (tree_.kind(node) == NodeKind::RawPointerType) {
    const auto children = tree_.children(node);
    if (children.size() != 1) {
      emit(DiagnosticKind::UnknownType, node);
      return sem_ir_.voidType();
    }
    return sem_ir_.addRawPointerType(
        checkType(children.front()),
        tree_.tokens().get(tree_.token(node)).kind == TokenKind::KwConst);
  }
  if (tree_.kind(node) == NodeKind::ReferenceType) {
    const auto children = tree_.children(node);
    if (children.size() != 1) {
      emit(DiagnosticKind::UnknownType, node);
      return sem_ir_.voidType();
    }
    const auto pointee = checkType(children.front());
    return sem_ir_.addReferenceType(
        pointee,
        tree_.tokens().get(tree_.token(node)).kind == TokenKind::KwConst
            ? SemReferenceMutability::ReadOnly
            : SemReferenceMutability::Mutable,
        SemReferenceProvenanceKind::Erased, core::AnyId::InvalidIndex);
  }
  if (tree_.kind(node) == NodeKind::TupleType) {
    std::vector<TypeId> elements;
    for (const auto child : tree_.children(node))
      elements.push_back(checkType(child));
    return sem_ir_.addTupleType(elements);
  }
  if (tree_.kind(node) == NodeKind::GenericType) {
    const auto children = tree_.children(node);
    if (children.empty()) {
      emit(DiagnosticKind::UnknownType, node);
      return sem_ir_.voidType();
    }
    if (tokenString(children.front()).hasValue() &&
        (values_.identifier(tokenString(children.front())) == "slice" ||
         values_.identifier(tokenString(children.front())) == "slice_mut")) {
      if (children.size() != 2) {
        emit(DiagnosticKind::ArgumentCountMismatch, node);
        return sem_ir_.voidType();
      }
      return sem_ir_.addSliceType(
          checkType(children[1]),
          values_.identifier(tokenString(children.front())) == "slice_mut");
    }
    TypeAliasId alias_id;
    const auto alias_name = tokenString(children.front());
    if (alias_name.hasValue()) {
      const auto alias_found = type_alias_names_.find(alias_name.index);
      if (alias_found != type_alias_names_.end())
        alias_id = alias_found->second;
    }
    if (!alias_id.hasValue() &&
        tree_.kind(children.front()) == NodeKind::QualifiedType) {
      const auto entity = resolveImportedTypeEntity(
          children.front(), PublicEntityKind::TypeAlias);
      alias_id = materializeImportedTypeAlias(entity, children.front());
    }
    if (alias_id.hasValue()) {
      std::vector<CanonicalTypeId> arguments;
      for (std::size_t index = 1; index < children.size(); ++index)
        arguments.push_back(sem_ir_.canonicalType(checkType(children[index])));
      const auto &alias = sem_ir_.typeAlias(alias_id);
      const auto expected =
          alias.generic.hasValue()
              ? values_.generics().generic(alias.generic).binding_count
              : 0U;
      if (arguments.size() != expected ||
          alias.state != SemTypeAliasState::Complete) {
        emit(arguments.size() != expected
                 ? DiagnosticKind::ArgumentCountMismatch
                 : DiagnosticKind::RecursiveTypeAlias,
             node);
        return sem_ir_.voidType();
      }
      return alias.generic.hasValue()
                 ? substituteType(alias.target, alias.generic, arguments)
                 : alias.target;
    }
    const auto nominal_id = resolveNominalType(children.front());
    if (!nominal_id.hasValue()) {
      emit(DiagnosticKind::UnknownType, children.front());
      return sem_ir_.voidType();
    }
    std::vector<TypeId> arguments;
    for (std::size_t index = 1; index < children.size(); ++index)
      arguments.push_back(checkType(children[index]));
    const auto &nominal = sem_ir_.nominalType(nominal_id);
    const auto expected =
        nominal.generic.hasValue()
            ? values_.generics().generic(nominal.generic).binding_count
            : 0U;
    if (arguments.size() != expected) {
      emit(DiagnosticKind::ArgumentCountMismatch, node);
      return sem_ir_.voidType();
    }
    return sem_ir_.addNominalType(nominal_id, arguments);
  }
  if (tree_.kind(node) == NodeKind::QualifiedType) {
    const auto components = tree_.children(node);
    if (components.size() >= 2) {
      const auto associated =
          values_.identifier(tokenString(components.back()));
      if (components.size() == 2 && current_generic_.hasValue() &&
          tokenString(components.front()).hasValue() &&
          values_.identifier(tokenString(components.front())) == "Self") {
        const auto self_binding =
            generic_bindings_.find(values_.internIdentifier("Self").index);
        const auto associated_binding =
            generic_bindings_.find(tokenString(components.back()).index);
        if (self_binding != generic_bindings_.end() &&
            associated_binding != generic_bindings_.end() &&
            associated_binding->second > self_binding->second)
          return sem_ir_.addTypeParameter(current_generic_,
                                          associated_binding->second);
      }
      if (components.size() == 2 && (active_constraint_list_.hasValue() ||
                                     !active_resolved_constraints_.empty())) {
        const auto subject_name = tokenString(components.front());
        const auto binding = generic_bindings_.find(subject_name.index);
        if (binding != generic_bindings_.end()) {
          const auto subject =
              sem_ir_.addTypeParameter(current_generic_, binding->second);
          struct Projection {
            const SemInterface *interface_value;
            const SemInterfaceConstraint *constraint;
            const SemInterfaceRequirement *requirement;
          };
          std::vector<Projection> projections;
          auto constraints = active_resolved_constraints_;
          if (active_constraint_list_.hasValue()) {
            auto local = checkConstraintList(active_constraint_list_);
            constraints.insert(constraints.end(), local.begin(), local.end());
          }
          for (const auto &constraint : constraints) {
            if (sem_ir_.canonicalType(constraint.subject) !=
                sem_ir_.canonicalType(subject))
              continue;
            const auto &interface_value =
                sem_ir_.interface(constraint.interface_id);
            for (const auto &requirement : interface_value.requirements) {
              if (requirement.kind ==
                      SemInterfaceRequirementKind::AssociatedAlias &&
                  values_.identifier(sem_ir_.name(requirement.name).text) ==
                      associated)
                projections.push_back(
                    {&interface_value, &constraint, &requirement});
            }
          }
          if (projections.size() > 1) {
            emit(DiagnosticKind::AmbiguousAssociatedAlias, node);
            return sem_ir_.voidType();
          }
          if (projections.size() == 1) {
            const auto &projection = projections.front();
            if (!projection.requirement->has_default)
              return sem_ir_.addTypeParameter(
                  projection.interface_value->generic,
                  projection.requirement->binding_index);
            std::vector<CanonicalTypeId> substitutions(
                values_.generics()
                    .generic(projection.interface_value->generic)
                    .binding_count);
            const auto arguments =
                sem_ir_.typeBlock(projection.constraint->arguments);
            for (std::uint32_t index = 0; index < arguments.size(); ++index)
              substitutions[index] = sem_ir_.canonicalType(arguments[index]);
            substitutions[projection.interface_value
                              ->explicit_parameter_count] =
                sem_ir_.canonicalType(subject);
            return substituteType(projection.requirement->type,
                                  projection.interface_value->generic,
                                  substitutions);
          }
        }
      }
      if (components.size() >= 3 &&
          (associated == "Subscription" || associated == "Completion" ||
           associated == "Wake")) {
        std::string operation_module;
        for (std::size_t index = 0; index + 2 < components.size(); ++index) {
          if (!operation_module.empty())
            operation_module += "::";
          operation_module +=
              values_.identifier(tokenString(components[index]));
        }
        const auto imported = sem_ir_.importIRs().findByModule(
            values_.internIdentifier(operation_module));
        const auto *interface = sem_ir_.importIRs().tryGetInterface(imported);
        if (interface) {
          const auto operation_name = values_.identifier(
              tokenString(components[components.size() - 2]));
          const auto binding =
              interface->findFunction(values_.internIdentifier(operation_name));
          if (binding.hasValue()) {
            const auto &function = interface->function(binding);
            const auto *entity = sem_ir_.importIRs().registry().tryGetEntity(
                function.canonical_entity);
            if (entity && entity->interop_artifact) {
              const auto state = associated == "Subscription"
                                     ? ForeignOperationStateKind::Subscription
                                 : associated == "Completion"
                                     ? ForeignOperationStateKind::Completion
                                     : ForeignOperationStateKind::Wake;
              const PublicEntityReferenceArtifact owner_entity{
                  PublicEntityKind::ForeignOperation,
                  std::string(values_.identifier(interface->packageName())),
                  std::string(values_.identifier(interface->moduleName())),
                  std::string(operation_name),
                  entity->interop_artifact->fingerprint};
              return mapImportedPublicType(
                  PublicType::foreignOperationState(owner_entity, state),
                  current_generic_, node);
            }
          }
        }
      }
      NominalTypeId owner;
      if (components.size() == 2) {
        const auto found =
            nominal_names_.find(tokenString(components.front()).index);
        if (found != nominal_names_.end())
          owner = found->second;
      } else {
        std::string module_name;
        for (std::size_t index = 0; index + 2 < components.size(); ++index) {
          if (!module_name.empty())
            module_name += "::";
          module_name += values_.identifier(tokenString(components[index]));
        }
        const auto imported = sem_ir_.importIRs().findByModule(
            values_.internIdentifier(module_name));
        const auto *interface = sem_ir_.importIRs().tryGetInterface(imported);
        if (interface) {
          const auto binding = interface->findNominalType(
              tokenString(components[components.size() - 2]));
          if (binding.hasValue())
            owner = materializeImportedNominal(
                interface->nominalType(binding).canonical_entity,
                components[components.size() - 2]);
        }
      }
      if (owner.hasValue() &&
          sem_ir_.nominalType(owner).kind == NominalKind::ForeignResource) {
        if (associated == "Completion" &&
            sem_ir_.nominalType(owner)
                .foreign_completion_handle_type.hasValue())
          return sem_ir_.addForeignCompletionType(owner);
        if (associated == "Wake" &&
            sem_ir_.nominalType(owner).foreign_wake_storage_type.hasValue())
          return sem_ir_.addForeignWakeType(owner);
      }
    }
    const auto alias_entity =
        resolveImportedTypeEntity(node, PublicEntityKind::TypeAlias);
    const auto alias = materializeImportedTypeAlias(alias_entity, node);
    if (alias.hasValue()) {
      const auto &definition = sem_ir_.typeAlias(alias);
      if (!definition.generic.hasValue() &&
          definition.state == SemTypeAliasState::Complete)
        return definition.target;
    }
    const auto nominal = resolveNominalType(node);
    if (nominal.hasValue() && !sem_ir_.nominalType(nominal).generic.hasValue())
      return sem_ir_.addNominalType(nominal);
    emit(DiagnosticKind::UnknownType, node);
    return sem_ir_.voidType();
  }
  if (tree_.kind(node) == NodeKind::ArrayType) {
    const auto children = tree_.children(node);
    if (children.size() != 2) {
      emit(DiagnosticKind::UnknownType, node);
      return sem_ir_.voidType();
    }
    const auto element_type = checkType(children[0]);
    if (!hasObjectRepresentation(element_type))
      emit(DiagnosticKind::InvalidObjectType, children[0]);
    std::uint32_t bound = 0;
    std::vector<InstId> constant_instructions;
    const auto usize_type = sem_ir_.addIntegerType(pointer_width_, false);
    auto bound_value =
        checkExpression(children[1], constant_instructions, usize_type);
    if (!adjustExpression(bound_value, usize_type, children[1],
                          constant_instructions)) {
      emit(DiagnosticKind::UnknownType, children[1]);
      return sem_ir_.voidType();
    }
    ConstantEvaluator evaluator(sem_ir_);
    const auto evaluated = evaluator.evaluateExpression(bound_value);
    if (!evaluated.result.isConcrete()) {
      emit(DiagnosticKind::InvalidConstantInitializer, children[1]);
      return sem_ir_.voidType();
    }
    const auto &constant = sem_ir_.constantValue(evaluated.result.value);
    const auto raw = static_cast<std::uint64_t>(sem_ir_.integer(
        IntegerId(static_cast<std::uint32_t>(constant.payload))));
    if (constant.kind != ConstantValueKind::Integer || raw == 0 ||
        raw > std::numeric_limits<std::uint32_t>::max()) {
      emit(DiagnosticKind::UnknownType, children[1]);
      return sem_ir_.voidType();
    }
    bound = static_cast<std::uint32_t>(raw);
    return sem_ir_.addType({SemTypeKind::Array, element_type.index, bound, 0});
  }
  if (tree_.kind(node) != NodeKind::TypeName &&
      tree_.kind(node) != NodeKind::Name) {
    emit(DiagnosticKind::UnknownType, node);
    return sem_ir_.voidType();
  }
  switch (tree_.tokens().get(tree_.token(node)).kind) {
  case TokenKind::KwVoid:
    return sem_ir_.voidType();
  case TokenKind::KwNever:
    return sem_ir_.neverType();
  case TokenKind::KwBool:
    return sem_ir_.boolType();
  case TokenKind::KwChar:
    return sem_ir_.charType();
  case TokenKind::KwI8:
    return sem_ir_.addIntegerType(8, true);
  case TokenKind::KwI16:
    return sem_ir_.addIntegerType(16, true);
  case TokenKind::KwI32:
    return sem_ir_.i32Type();
  case TokenKind::KwI64:
    return sem_ir_.addIntegerType(64, true);
  case TokenKind::KwISize:
    return sem_ir_.addIntegerType(pointer_width_, true);
  case TokenKind::KwU8:
    return sem_ir_.addIntegerType(8, false);
  case TokenKind::KwU16:
    return sem_ir_.addIntegerType(16, false);
  case TokenKind::KwU32:
    return sem_ir_.addIntegerType(32, false);
  case TokenKind::KwU64:
    return sem_ir_.addIntegerType(64, false);
  case TokenKind::KwUSize:
    return sem_ir_.addIntegerType(pointer_width_, false);
  case TokenKind::KwF32:
    return sem_ir_.addFloatType(32);
  case TokenKind::KwF64:
    return sem_ir_.addFloatType(64);
  case TokenKind::KwString:
    return sem_ir_.stringType();
  default:
    if (const auto name = tokenString(node); name.hasValue()) {
      if (values_.identifier(name) == "Self" &&
          (current_impl_owner_type_.hasValue() ||
           current_impl_owner_.hasValue()))
        return current_impl_owner_type_.hasValue()
                   ? current_impl_owner_type_
                   : sem_ir_.addNominalType(current_impl_owner_);
      if (const auto found = generic_bindings_.find(name.index);
          found != generic_bindings_.end())
        return sem_ir_.addTypeParameter(current_generic_, found->second);
      if (const auto nominal = nominal_names_.find(name.index);
          nominal != nominal_names_.end()) {
        const auto &definition = sem_ir_.nominalType(nominal->second);
        if (!definition.generic.hasValue())
          return sem_ir_.addNominalType(nominal->second);
      }
      if (const auto alias = type_alias_names_.find(name.index);
          alias != type_alias_names_.end()) {
        const auto &definition = sem_ir_.typeAlias(alias->second);
        if (!definition.generic.hasValue() &&
            definition.state == SemTypeAliasState::Complete)
          return definition.target;
        if (definition.state == SemTypeAliasState::Resolving)
          emit(DiagnosticKind::RecursiveTypeAlias, node);
      }
    }
    emit(DiagnosticKind::UnknownType, node);
    return sem_ir_.voidType();
  }
}
