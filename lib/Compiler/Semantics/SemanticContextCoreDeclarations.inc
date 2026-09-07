// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
void SemanticContext::validateUnboundForeignContracts() {
  for (const auto node : function_declarations_) {
    if (!hasNodeFlag(tree_.get(node).flags, NodeFlags::IsForeign) ||
        functionChild(node, NodeKind::CallableContract).hasValue())
      continue;
    const auto function = function_nodes_.find(node.index);
    if (function == function_nodes_.end())
      continue;
    emit(DiagnosticKind::InvalidCallableContract, node);
  }
}

void SemanticContext::predeclareTypeAlias(NodeId node) {
  if (tree_.kind(node) != NodeKind::AliasDecl)
    return;
  const auto children = tree_.children(node);
  if (children.size() < 4 || tree_.kind(children[0]) != NodeKind::Name ||
      tree_.kind(children[1]) != NodeKind::GenericParameterList ||
      tree_.kind(children[2]) != NodeKind::ConstraintList) {
    emit(DiagnosticKind::RecursiveTypeAlias, node);
    return;
  }
  const auto name = nameFor(children[0]);
  const auto parameters = tree_.children(children[1]);
  GenericId generic;
  if (!parameters.empty())
    generic = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(), sem_ir_.name(name).text,
        static_cast<std::uint32_t>(parameters.size()));
  const auto flags = hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic)
                         ? SemTypeAliasPublic
                         : 0U;
  const auto id =
      sem_ir_.addTypeAlias({name, generic, TypeId::invalid(), {}, node, flags});
  type_alias_nodes_.emplace(node.index, id);
  if (!type_alias_names_.emplace(sem_ir_.name(name).text.index, id).second)
    emit(DiagnosticKind::DuplicateName, children[0]);
}

void SemanticContext::completeTypeAlias(NodeId node) {
  if (tree_.kind(node) != NodeKind::AliasDecl)
    return;
  const auto found = type_alias_nodes_.find(node.index);
  if (found == type_alias_nodes_.end())
    return;
  auto alias = sem_ir_.typeAlias(found->second);
  if (alias.state == SemTypeAliasState::Resolving) {
    emit(DiagnosticKind::RecursiveTypeAlias, node);
    alias.state = SemTypeAliasState::Invalid;
    sem_ir_.setTypeAlias(found->second, std::move(alias));
    return;
  }
  const auto children = tree_.children(node);
  alias.state = SemTypeAliasState::Resolving;
  sem_ir_.setTypeAlias(found->second, alias);
  generic_bindings_.clear();
  current_generic_ = alias.generic;
  if (alias.generic.hasValue()) {
    const auto parameters = tree_.children(children[1]);
    for (std::uint32_t index = 0; index < parameters.size(); ++index) {
      const auto name = tokenString(parameters[index]);
      if (!name.hasValue() ||
          !generic_bindings_.emplace(name.index, index).second)
        emit(DiagnosticKind::DuplicateName, parameters[index]);
    }
  }
  alias.constraints = checkConstraintList(children[2]);
  alias.target = checkType(children.back());
  alias.state = SemTypeAliasState::Complete;
  std::string identity =
      std::string(values_.identifier(sem_ir_.moduleName())) +
      "::" + std::string(values_.identifier(sem_ir_.name(alias.name).text)) +
      "\0" +
      std::to_string(
          alias.generic.hasValue()
              ? values_.generics().generic(alias.generic).binding_count
              : 0U) +
      "\0" + std::to_string(sem_ir_.canonicalType(alias.target).index);
  alias.fingerprint = StableFingerprint::fromCanonicalBytes(identity);
  sem_ir_.setTypeAlias(found->second, std::move(alias));
  current_generic_ = GenericId::invalid();
  generic_bindings_.clear();
}

void SemanticContext::predeclareInterface(NodeId node) {
  if (tree_.kind(node) != NodeKind::InterfaceDecl)
    return;
  const auto children = tree_.children(node);
  if (children.size() < 3 || tree_.kind(children[0]) != NodeKind::Name ||
      tree_.kind(children[1]) != NodeKind::GenericParameterList ||
      tree_.kind(children[2]) != NodeKind::ConstraintList) {
    emit(DiagnosticKind::InvalidInterface, node);
    return;
  }
  const auto name = nameFor(children[0]);
  const auto explicit_parameters = tree_.children(children[1]);
  const auto associated_count = static_cast<std::uint32_t>(
      std::ranges::count_if(children, [&](NodeId child) {
        return tree_.kind(child) == NodeKind::AssociatedAliasDecl;
      }));
  const auto generic = values_.generics().addGeneric(
      sem_ir_.checkIRId(), sem_ir_.moduleName(), sem_ir_.name(name).text,
      static_cast<std::uint32_t>(explicit_parameters.size()) + 1U +
          associated_count);
  const auto flags = hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic)
                         ? SemInterfacePublic
                         : 0U;
  const auto id = sem_ir_.addInterface(
      {name,
       generic,
       static_cast<std::uint32_t>(explicit_parameters.size()),
       {},
       {},
       node,
       flags});
  interface_nodes_.emplace(node.index, id);
  if (!interface_names_.emplace(sem_ir_.name(name).text.index, id).second)
    emit(DiagnosticKind::DuplicateName, children[0]);
}

void SemanticContext::completeInterface(NodeId node) {
  if (tree_.kind(node) != NodeKind::InterfaceDecl)
    return;
  const auto found = interface_nodes_.find(node.index);
  if (found == interface_nodes_.end())
    return;
  auto interface_value = sem_ir_.interface(found->second);
  const auto children = tree_.children(node);
  const auto explicit_parameters = tree_.children(children[1]);
  generic_bindings_.clear();
  current_generic_ = interface_value.generic;
  for (std::uint32_t index = 0; index < explicit_parameters.size(); ++index) {
    const auto name = tokenString(explicit_parameters[index]);
    if (!name.hasValue() ||
        !generic_bindings_.emplace(name.index, index).second)
      emit(DiagnosticKind::DuplicateName, explicit_parameters[index]);
  }
  const auto self_index = interface_value.explicit_parameter_count;
  generic_bindings_.emplace(values_.internIdentifier("Self").index, self_index);
  interface_value.constraints = checkConstraintList(children[2]);
  auto associated_index = self_index + 1U;
  for (const auto child : children) {
    if (tree_.kind(child) != NodeKind::AssociatedAliasDecl)
      continue;
    const auto alias_children = tree_.children(child);
    if (alias_children.empty()) {
      emit(DiagnosticKind::InvalidInterface, child);
      continue;
    }
    const auto alias_name = nameFor(alias_children.front());
    const auto alias_text = sem_ir_.name(alias_name).text;
    if (!generic_bindings_.emplace(alias_text.index, associated_index).second)
      emit(DiagnosticKind::DuplicateName, alias_children.front());
    const auto has_default =
        !hasNodeFlag(tree_.get(child).flags, NodeFlags::IsBodyless);
    TypeId default_type;
    if (has_default && alias_children.size() >= 2)
      default_type = checkType(alias_children[1]);
    interface_value.requirements.push_back(
        {SemInterfaceRequirementKind::AssociatedAlias, alias_name, default_type,
         FunctionRefId::invalid(), associated_index, has_default, child});
    ++associated_index;
  }
  const auto self_type =
      sem_ir_.addTypeParameter(interface_value.generic, self_index);
  for (const auto child : children) {
    if (tree_.kind(child) != NodeKind::FunctionDecl)
      continue;
    predeclareFunction(
        child, NominalTypeId::invalid(), SemCanonicalFunctionRole::None, false,
        interface_value.generic, self_type, NameId::invalid(), true);
    const auto function = function_nodes_.find(child.index);
    if (function == function_nodes_.end())
      continue;
    auto function_value = sem_ir_.function(function->second);
    function_value.flags |= SemFunctionInterfaceMember;
    const auto source_name =
        values_.identifier(sem_ir_.name(function_value.name).text);
    function_value.name = sem_ir_.addName(
        values_.internIdentifier("$interface$" +
                                 std::string(values_.identifier(
                                     sem_ir_.name(interface_value.name).text)) +
                                 "$" + std::string(source_name)));
    sem_ir_.setFunction(function->second, function_value);
    const auto function_ref = functionReference(function->second);
    const auto name_node = functionChild(child, NodeKind::Name);
    interface_value.requirements.push_back(
        {SemInterfaceRequirementKind::Function, nameFor(name_node),
         function_value.type, function_ref, core::AnyId::InvalidIndex,
         !hasNodeFlag(tree_.get(child).flags, NodeFlags::IsBodyless), child});
  }
  std::ranges::sort(
      interface_value.requirements, [&](const auto &lhs, const auto &rhs) {
        if (lhs.kind != rhs.kind)
          return lhs.kind < rhs.kind;
        const auto left = values_.identifier(sem_ir_.name(lhs.name).text);
        const auto right = values_.identifier(sem_ir_.name(rhs.name).text);
        if (left != right)
          return left < right;
        const auto left_type = lhs.type.hasValue()
                                   ? sem_ir_.canonicalType(lhs.type).index
                                   : core::AnyId::InvalidIndex;
        const auto right_type = rhs.type.hasValue()
                                    ? sem_ir_.canonicalType(rhs.type).index
                                    : core::AnyId::InvalidIndex;
        return left_type < right_type;
      });
  std::string identity =
      std::string(values_.identifier(sem_ir_.moduleName())) + "::" +
      std::string(values_.identifier(sem_ir_.name(interface_value.name).text));
  for (const auto &requirement : interface_value.requirements) {
    identity.push_back('\0');
    identity += std::to_string(static_cast<unsigned>(requirement.kind));
    identity.push_back(':');
    identity += values_.identifier(sem_ir_.name(requirement.name).text);
    identity.push_back(':');
    identity +=
        std::to_string(requirement.type.hasValue()
                           ? sem_ir_.canonicalType(requirement.type).index
                           : core::AnyId::InvalidIndex);
    identity.push_back(requirement.has_default ? '1' : '0');
  }
  interface_value.fingerprint = StableFingerprint::fromCanonicalBytes(identity);
  sem_ir_.setInterface(found->second, std::move(interface_value));
  current_generic_ = GenericId::invalid();
  current_impl_owner_type_ = TypeId::invalid();
  generic_bindings_.clear();
}

[[nodiscard]] InterfaceId
SemanticContext::resolveInterfaceSpecific(NodeId node,
                                          std::vector<TypeId> &arguments) {
  auto name_node = node;
  if (tree_.kind(node) == NodeKind::GenericType) {
    const auto children = tree_.children(node);
    if (children.empty())
      return InterfaceId::invalid();
    name_node = children.front();
    for (std::size_t index = 1; index < children.size(); ++index)
      arguments.push_back(checkType(children[index]));
  }
  const auto name = tokenString(name_node);
  const auto found = name.hasValue() ? interface_names_.find(name.index)
                                     : interface_names_.end();
  InterfaceId resolved =
      found == interface_names_.end() ? InterfaceId::invalid() : found->second;
  if (!resolved.hasValue() &&
      (tree_.kind(name_node) == NodeKind::QualifiedType ||
       tree_.kind(name_node) == NodeKind::QualifiedName)) {
    const auto entity =
        resolveImportedTypeEntity(name_node, PublicEntityKind::Interface);
    resolved = materializeImportedInterface(entity, name_node);
  }
  if (!resolved.hasValue())
    return InterfaceId::invalid();
  const auto &interface_value = sem_ir_.interface(resolved);
  if (arguments.size() != interface_value.explicit_parameter_count) {
    emit(DiagnosticKind::ArgumentCountMismatch, node);
    return InterfaceId::invalid();
  }
  return resolved;
}

[[nodiscard]] std::vector<SemInterfaceConstraint>
SemanticContext::checkConstraintList(NodeId node) {
  std::vector<SemInterfaceConstraint> result;
  if (!node.hasValue() || tree_.kind(node) != NodeKind::ConstraintList)
    return result;
  const auto owned_constraint_list =
      std::exchange(active_constraint_list_, NodeId::invalid());
  const auto inherited = active_resolved_constraints_;
  for (const auto clause : tree_.children(node)) {
    active_resolved_constraints_ = inherited;
    active_resolved_constraints_.insert(active_resolved_constraints_.end(),
                                        result.begin(), result.end());
    const auto children = tree_.children(clause);
    if (tree_.kind(clause) != NodeKind::ConstraintClause ||
        children.size() != 2) {
      emit(DiagnosticKind::InvalidInterface, clause);
      continue;
    }
    const auto subject = checkType(children[0]);
    std::vector<TypeId> arguments;
    const auto interface_id = resolveInterfaceSpecific(children[1], arguments);
    if (!interface_id.hasValue()) {
      emit(DiagnosticKind::UnknownInterface, children[1]);
      continue;
    }
    SemInterfaceConstraint constraint{subject, interface_id,
                                      sem_ir_.addTypeBlock(arguments),
                                      InterfaceWitnessId::invalid()};
    const auto duplicate =
        std::ranges::any_of(result, [&](const auto &candidate) {
          return sem_ir_.canonicalType(candidate.subject) ==
                     sem_ir_.canonicalType(constraint.subject) &&
                 candidate.interface_id == constraint.interface_id &&
                 std::ranges::equal(sem_ir_.typeBlock(candidate.arguments),
                                    sem_ir_.typeBlock(constraint.arguments),
                                    [&](TypeId lhs, TypeId rhs) {
                                      return sem_ir_.canonicalType(lhs) ==
                                             sem_ir_.canonicalType(rhs);
                                    });
        });
    if (!duplicate)
      result.push_back(std::move(constraint));
  }
  std::ranges::sort(result, [&](const auto &lhs, const auto &rhs) {
    if (lhs.interface_id != rhs.interface_id)
      return lhs.interface_id.index < rhs.interface_id.index;
    return sem_ir_.canonicalType(lhs.subject).index <
           sem_ir_.canonicalType(rhs.subject).index;
  });
  active_resolved_constraints_ = inherited;
  active_constraint_list_ = owned_constraint_list;
  return result;
}

void SemanticContext::recordFunctionConstraints(FunctionId function,
                                                NodeId node) {
  const auto constraint_node = functionChild(node, NodeKind::ConstraintList);
  auto constraints = active_resolved_constraints_;
  auto local = checkConstraintList(constraint_node);
  for (auto &constraint : local) {
    const auto duplicate =
        std::ranges::any_of(constraints, [&](const auto &candidate) {
          return sem_ir_.canonicalType(candidate.subject) ==
                     sem_ir_.canonicalType(constraint.subject) &&
                 candidate.interface_id == constraint.interface_id &&
                 std::ranges::equal(sem_ir_.typeBlock(candidate.arguments),
                                    sem_ir_.typeBlock(constraint.arguments),
                                    [&](TypeId lhs, TypeId rhs) {
                                      return sem_ir_.canonicalType(lhs) ==
                                             sem_ir_.canonicalType(rhs);
                                    });
        });
    if (!duplicate)
      constraints.push_back(std::move(constraint));
  }
  std::ranges::sort(constraints, [&](const auto &lhs, const auto &rhs) {
    if (lhs.interface_id != rhs.interface_id)
      return lhs.interface_id.index < rhs.interface_id.index;
    return sem_ir_.canonicalType(lhs.subject).index <
           sem_ir_.canonicalType(rhs.subject).index;
  });
  sem_ir_.setFunctionConstraints(function, std::move(constraints));
}

void SemanticContext::captureFunctionGenericEnvironment(FunctionId function) {
  SemanticGenericEnvironment environment;
  environment.generic = current_generic_;
  environment.constraints.assign(sem_ir_.functionConstraints(function).begin(),
                                 sem_ir_.functionConstraints(function).end());
  environment.bindings.reserve(generic_bindings_.size());
  for (const auto &[name, index] : generic_bindings_)
    environment.bindings.emplace_back(IdentifierId(name), index);
  std::ranges::sort(environment.bindings, [](const auto &lhs, const auto &rhs) {
    return lhs.second < rhs.second;
  });
  function_generic_environments_.insert_or_assign(function.index,
                                                  std::move(environment));
}

void SemanticContext::restoreFunctionGenericEnvironment(FunctionId function) {
  generic_bindings_.clear();
  active_resolved_constraints_.clear();
  const auto found = function_generic_environments_.find(function.index);
  if (found == function_generic_environments_.end())
    return;
  current_generic_ = found->second.generic;
  active_resolved_constraints_ = found->second.constraints;
  for (const auto &[name, index] : found->second.bindings)
    generic_bindings_.emplace(name.index, index);
}

void SemanticContext::materializeImportedInterfaceWitnesses(NodeId location) {
  if (imported_witnesses_materialized_ || imported_witnesses_materializing_)
    return;
  imported_witnesses_materializing_ = true;
  for (std::uint32_t import_index = 0;
       import_index < sem_ir_.importIRs().size(); ++import_index) {
    const auto *interface =
        sem_ir_.importIRs().tryGetInterface(ImportIRId(import_index));
    if (!interface)
      continue;
    for (const auto &source : interface->interfaceWitnessArtifacts()) {
      if (!imported_witness_fingerprints_.insert(source.fingerprint.hex())
               .second)
        continue;
      const auto interface_entity =
          resolveArtifactEntity(source.interface_entity);
      const auto interface_id =
          materializeImportedInterface(interface_entity, location);
      GenericId witness_generic;
      if (source.generic_parameter_count != 0) {
        const auto *owner = sem_ir_.importIRs().tryGetEntity(interface_entity);
        if (!owner)
          continue;
        witness_generic = values_.generics().addGeneric(
            CheckIRId::invalid(), owner->module_name,
            values_.internIdentifier("$witness$" + source.fingerprint.hex()),
            source.generic_parameter_count);
      }
      const auto self_type =
          mapImportedPublicType(source.self_type, witness_generic, location);
      std::vector<TypeId> interface_arguments;
      for (const auto &argument : source.interface_arguments)
        interface_arguments.push_back(
            mapImportedPublicType(argument, witness_generic, location));
      if (!interface_id.hasValue() || !self_type.hasValue() ||
          std::ranges::any_of(interface_arguments,
                              [](TypeId type) { return !type.hasValue(); }))
        continue;
      const auto &declaration = sem_ir_.interface(interface_id);
      SemInterfaceWitness witness{interface_id,
                                  self_type,
                                  sem_ir_.addTypeBlock(interface_arguments),
                                  {},
                                  location,
                                  SemInterfaceWitnessState::Complete,
                                  source.fingerprint};
      witness.generic = witness_generic;
      bool valid = source.entries.size() == declaration.requirements.size();
      for (const auto &constraint : source.constraints) {
        const auto subject = mapImportedPublicType(constraint.subject,
                                                   witness_generic, location);
        const auto referenced =
            resolveArtifactEntity(constraint.interface_entity);
        const auto referenced_interface =
            materializeImportedInterface(referenced, location);
        std::vector<TypeId> arguments;
        for (const auto &argument : constraint.arguments)
          arguments.push_back(
              mapImportedPublicType(argument, witness_generic, location));
        if (!subject.hasValue() || !referenced_interface.hasValue() ||
            std::ranges::any_of(arguments,
                                [](TypeId type) { return !type.hasValue(); })) {
          valid = false;
          break;
        }
        witness.constraints.push_back({subject, referenced_interface,
                                       sem_ir_.addTypeBlock(arguments),
                                       InterfaceWitnessId::invalid()});
      }
      for (const auto &entry : source.entries) {
        FunctionRefId function;
        TypeId associated_type;
        if (entry.function.expected_fingerprint.hasValue()) {
          const auto entity = resolveArtifactEntity(entry.function);
          function = resolvePublicEntity(entity, location);
          valid &= function.hasValue();
        } else {
          associated_type = mapImportedPublicType(entry.associated_type,
                                                  witness_generic, location);
          valid &= associated_type.hasValue();
        }
        valid &= entry.requirement < declaration.requirements.size();
        witness.entries.push_back(
            {entry.requirement, function, associated_type});
      }
      if (valid)
        (void)sem_ir_.addInterfaceWitness(std::move(witness));
    }
  }
  imported_witnesses_materializing_ = false;
  imported_witnesses_materialized_ = true;
}
