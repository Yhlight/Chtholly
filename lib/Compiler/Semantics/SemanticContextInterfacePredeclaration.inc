// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
void SemanticContext::predeclareInterfaceImpl(NodeId node) {
  if (tree_.kind(node) != NodeKind::ImplDecl ||
      !hasNodeFlag(tree_.get(node).flags, NodeFlags::IsTraitImpl))
    return;
  const auto children = tree_.children(node);
  if (children.size() < 4 ||
      tree_.kind(children[0]) != NodeKind::GenericParameterList)
    return;
  const auto interface_name = typePath(children[1]);
  if (interface_name == "core::Copy" ||
      interface_name == "core::ValueRepresentation" ||
      interface_name == "core::ObjectProjection" ||
      interface_name == "core::ObjectLifecycle")
    return;

  const auto generic_nodes = tree_.children(children[0]);
  GenericId impl_generic;
  generic_bindings_.clear();
  if (!generic_nodes.empty()) {
    impl_generic = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(),
        values_.internIdentifier("$conformance$" + interface_name + "$" +
                                 std::to_string(node.index)),
        static_cast<std::uint32_t>(generic_nodes.size()));
    for (std::uint32_t index = 0; index < generic_nodes.size(); ++index) {
      const auto name = tokenString(generic_nodes[index]);
      if (!name.hasValue() ||
          !generic_bindings_.emplace(name.index, index).second)
        emit(DiagnosticKind::DuplicateName, generic_nodes[index]);
    }
  }
  current_generic_ = impl_generic;
  std::vector<TypeId> interface_arguments;
  const auto interface_id =
      resolveInterfaceSpecific(children[1], interface_arguments);
  if (!interface_id.hasValue()) {
    emit(DiagnosticKind::UnknownInterface, children[1]);
    current_generic_ = GenericId::invalid();
    generic_bindings_.clear();
    return;
  }
  const auto self_type = checkType(children[2]);
  const auto &interface_value = sem_ir_.interface(interface_id);
  const auto &self_value = sem_ir_.type(self_type);
  const auto owns_self = self_value.kind == SemTypeKind::Nominal &&
                         !sem_ir_.nominalType(NominalTypeId(self_value.arg0))
                              .canonical_entity.hasValue();
  const auto compiler_owned_primitive = [&] {
    if (!interface_value.canonical_entity.hasValue() || owns_self ||
        (sem_ir_.type(self_type).kind != SemTypeKind::Integer &&
         sem_ir_.type(self_type).kind != SemTypeKind::Bool &&
         sem_ir_.type(self_type).kind != SemTypeKind::Char))
      return false;
    const auto *entity = sem_ir_.importIRs().tryGetEntity(
        interface_value.canonical_entity);
    return entity && entity->kind == PublicEntityKind::Interface &&
           values_.identifier(entity->package_name) == "std" &&
           values_.identifier(entity->module_name) == "std::hash" &&
           (values_.identifier(entity->name) == "Hash" ||
            values_.identifier(entity->name) == "Equal") &&
           values_.identifier(sem_ir_.moduleName()) == "std::hash";
  }();
  if (interface_value.canonical_entity.hasValue() && !owns_self &&
      !compiler_owned_primitive) {
    emit(DiagnosticKind::OrphanConformance, node);
    current_generic_ = GenericId::invalid();
    generic_bindings_.clear();
    return;
  }
  const auto previous_resolved_constraints = active_resolved_constraints_;
  active_resolved_constraints_ = checkConstraintList(children[3]);
  std::vector<CanonicalTypeId> substitutions(
      values_.generics().generic(interface_value.generic).binding_count);
  for (std::uint32_t index = 0; index < interface_arguments.size(); ++index)
    substitutions[index] = sem_ir_.canonicalType(interface_arguments[index]);
  substitutions[interface_value.explicit_parameter_count] =
      sem_ir_.canonicalType(self_type);

  std::unordered_map<std::uint32_t, TypeId> associated_types;
  std::unordered_map<std::uint32_t, FunctionRefId> functions;
  for (std::size_t index = 4; index < children.size(); ++index) {
    const auto member = children[index];
    if (tree_.kind(member) == NodeKind::AssociatedAliasDecl) {
      const auto member_children = tree_.children(member);
      if (member_children.size() != 2) {
        emit(DiagnosticKind::InvalidInterface, member);
        continue;
      }
      const auto name = nameFor(member_children[0]);
      if (!associated_types.emplace(name.index, checkType(member_children[1]))
               .second)
        emit(DiagnosticKind::DuplicateName, member_children[0]);
      continue;
    }
    if (tree_.kind(member) != NodeKind::FunctionDecl) {
      emit(DiagnosticKind::InvalidInterface, member);
      continue;
    }
    NominalTypeId owner;
    if (sem_ir_.type(self_type).kind == SemTypeKind::Nominal)
      owner = NominalTypeId(sem_ir_.type(self_type).arg0);
    predeclareFunction(member, owner, SemCanonicalFunctionRole::None, false,
                       impl_generic, self_type, NameId::invalid(), true);
    const auto function = function_nodes_.find(member.index);
    const auto name_node = functionChild(member, NodeKind::Name);
    if (function != function_nodes_.end() && name_node.hasValue()) {
      const auto name = nameFor(name_node);
      auto function_value = sem_ir_.function(function->second);
      function_value.name = sem_ir_.addName(values_.internIdentifier(
          "$impl$" + interface_name + "$" + typePath(children[2]) + "$" +
          std::string(values_.identifier(sem_ir_.name(name).text))));
      function_value.flags |= SemFunctionInterfaceMember;
      sem_ir_.setFunction(function->second, function_value);
      if (!functions.emplace(name.index, functionReference(function->second))
               .second)
        emit(DiagnosticKind::DuplicateName, name_node);
    }
  }

  for (const auto &requirement : interface_value.requirements) {
    if (requirement.kind != SemInterfaceRequirementKind::AssociatedAlias)
      continue;
    const auto found = associated_types.find(requirement.name.index);
    TypeId selected;
    if (found != associated_types.end())
      selected = found->second;
    else if (requirement.has_default)
      selected = requirement.type;
    else
      emit(DiagnosticKind::MissingInterfaceRequirement,
           requirement.declaration);
    if (selected.hasValue() && requirement.binding_index < substitutions.size())
      substitutions[requirement.binding_index] =
          sem_ir_.canonicalType(selected);
  }

  SemInterfaceWitness witness{
      interface_id, self_type, sem_ir_.addTypeBlock(interface_arguments),
      {},           node,      SemInterfaceWitnessState::Complete};
  witness.generic = impl_generic;
  witness.constraints = active_resolved_constraints_;
  bool valid = true;
  for (std::uint32_t slot = 0; slot < interface_value.requirements.size();
       ++slot) {
    const auto &requirement = interface_value.requirements[slot];
    if (requirement.kind == SemInterfaceRequirementKind::AssociatedAlias) {
      const auto found = associated_types.find(requirement.name.index);
      TypeId selected = found != associated_types.end() ? found->second
                        : requirement.has_default       ? requirement.type
                                                        : TypeId::invalid();
      if (selected.hasValue() && requirement.has_default &&
          found == associated_types.end())
        selected =
            substituteType(selected, interface_value.generic, substitutions);
      witness.entries.push_back({slot, FunctionRefId::invalid(), selected});
      valid &= selected.hasValue();
      continue;
    }
    const auto found = functions.find(requirement.name.index);
    auto target = found != functions.end()  ? found->second
                  : requirement.has_default ? requirement.function
                                            : FunctionRefId::invalid();
    if (!target.hasValue()) {
      emit(DiagnosticKind::MissingInterfaceRequirement,
           requirement.declaration);
      valid = false;
    } else if (found != functions.end()) {
      const auto expected = substituteType(
          requirement.type, interface_value.generic, substitutions);
      if (!expected.hasValue() ||
          sem_ir_.canonicalType(sem_ir_.functionRef(target).local_type) !=
              sem_ir_.canonicalType(expected)) {
        emit(DiagnosticKind::InterfaceSignatureMismatch,
             requirement.declaration);
        valid = false;
      }
    }
    witness.entries.push_back({slot, target, TypeId::invalid()});
  }
  const auto function_requirement_count =
      static_cast<std::size_t>(std::ranges::count_if(
          interface_value.requirements, [](const auto &requirement) {
            return requirement.kind == SemInterfaceRequirementKind::Function;
          }));
  const auto associated_alias_requirement_count =
      static_cast<std::size_t>(std::ranges::count_if(
          interface_value.requirements, [](const auto &requirement) {
            return requirement.kind ==
                   SemInterfaceRequirementKind::AssociatedAlias;
          }));
  if (functions.size() > function_requirement_count ||
      associated_types.size() > associated_alias_requirement_count) {
    emit(DiagnosticKind::InvalidInterface, node);
    valid = false;
  }
  if (!valid)
    witness.state = SemInterfaceWitnessState::Invalid;
  for (std::uint32_t index = 0; index < sem_ir_.interfaceWitnessCount();
       ++index) {
    const auto &other = sem_ir_.interfaceWitness(InterfaceWitnessId(index));
    if (other.state != SemInterfaceWitnessState::Complete ||
        other.interface_id != witness.interface_id ||
        (!other.generic.hasValue() && !witness.generic.hasValue()))
      continue;
    const auto overlaps = (witness.generic.hasValue() &&
                           deduceInterfaceWitnessArguments(
                               witness, other.self_type,
                               sem_ir_.typeBlock(other.interface_arguments))
                               .has_value()) ||
                          (other.generic.hasValue() &&
                           deduceInterfaceWitnessArguments(
                               other, witness.self_type,
                               sem_ir_.typeBlock(witness.interface_arguments))
                               .has_value());
    if (overlaps) {
      emit(DiagnosticKind::DuplicateConformance, node);
      witness.state = SemInterfaceWitnessState::Invalid;
      break;
    }
  }
  std::string key = std::to_string(interface_id.index) + ":" +
                    std::to_string(sem_ir_.canonicalType(self_type).index);
  for (const auto argument : interface_arguments)
    key += ":" + std::to_string(sem_ir_.canonicalType(argument).index);
  if (!conformance_keys_.insert(key).second) {
    emit(DiagnosticKind::DuplicateConformance, node);
    witness.state = SemInterfaceWitnessState::Invalid;
  }
  std::string identity = interface_value.fingerprint.hex() + "\0" + key;
  for (const auto &entry : witness.entries) {
    identity +=
        "\0" + std::to_string(entry.requirement) + ":" +
        std::to_string(entry.function.index) + ":" +
        std::to_string(entry.associated_type.hasValue()
                           ? sem_ir_.canonicalType(entry.associated_type).index
                           : core::AnyId::InvalidIndex);
  }
  witness.fingerprint = StableFingerprint::fromCanonicalBytes(identity);
  (void)sem_ir_.addInterfaceWitness(std::move(witness));
  current_generic_ = GenericId::invalid();
  current_impl_owner_ = NominalTypeId::invalid();
  current_impl_owner_type_ = TypeId::invalid();
  generic_bindings_.clear();
  active_resolved_constraints_ = previous_resolved_constraints;
}
