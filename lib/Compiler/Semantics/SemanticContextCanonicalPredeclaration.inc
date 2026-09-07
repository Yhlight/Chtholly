// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
void SemanticContext::predeclareCanonicalImpl(NodeId node) {
  if (tree_.kind(node) != NodeKind::ImplDecl)
    return;
  const auto trait_impl =
      hasNodeFlag(tree_.get(node).flags, NodeFlags::IsTraitImpl);
  const auto children = tree_.children(node);
  const auto has_generic_list =
      !children.empty() &&
      tree_.kind(children.front()) == NodeKind::GenericParameterList;
  const auto has_generic =
      has_generic_list && !tree_.children(children.front()).empty();
  const auto generic_index = has_generic_list ? 0U : children.size();
  const auto owner_index =
      (trait_impl ? 1U : 0U) + (has_generic_list ? 1U : 0U);
  if (children.size() <= owner_index) {
    emit(DiagnosticKind::InvalidLifecycleImpl, node);
    return;
  }
  const auto trait_name =
      trait_impl ? typePath(children[has_generic_list ? 1U : 0U]) : "";
  if (trait_impl) {
    const auto trait_node = children[has_generic_list ? 1U : 0U];
    auto name_node = trait_node;
    if (tree_.kind(trait_node) == NodeKind::GenericType) {
      const auto trait_children = tree_.children(trait_node);
      if (!trait_children.empty())
        name_node = trait_children.front();
    }
    const auto name = tokenString(name_node);
    if (name.hasValue() && interface_names_.contains(name.index))
      return;
    const auto interface_path = typePath(name_node);
    const auto separator = interface_path.rfind("::");
    if (separator != std::string::npos) {
      const auto module = values_.internIdentifier(
          std::string_view(interface_path).substr(0, separator));
      const auto imported = sem_ir_.importIRs().findByModule(module);
      const auto *public_interface =
          sem_ir_.importIRs().tryGetInterface(imported);
      if (public_interface &&
          public_interface->findInterface(values_.internIdentifier(
              std::string_view(interface_path).substr(separator + 2))))
        return;
    }
  }
  const auto representation_impl = trait_name == "core::ValueRepresentation";
  const auto projection_impl = trait_name == "core::ObjectProjection";
  const auto object_lifecycle_impl = trait_name == "core::ObjectLifecycle";
  if (trait_impl && trait_name != "core::Copy" && !representation_impl &&
      !projection_impl && !object_lifecycle_impl) {
    emit(DiagnosticKind::InvalidCanonicalImpl, children[0]);
    return;
  }
  GenericId impl_generic;
  generic_bindings_.clear();
  current_generic_ = GenericId::invalid();
  if (has_generic) {
    const auto generic_nodes = tree_.children(children[generic_index]);
    auto owner_name_node = children[owner_index];
    if (tree_.kind(owner_name_node) == NodeKind::GenericType) {
      const auto owner_children = tree_.children(owner_name_node);
      if (!owner_children.empty())
        owner_name_node = owner_children.front();
    }
    const auto canonical_generic_name =
        std::string(representation_impl     ? "$representation$"
                    : projection_impl       ? "$projection$"
                    : object_lifecycle_impl ? "$object$"
                                            : "$lifecycle$") +
        typePath(owner_name_node) +
        (representation_impl     ? "$pack"
         : projection_impl       ? "$projector"
         : object_lifecycle_impl ? "$shell"
         : trait_impl            ? "$copy"
                                 : "$drop");
    impl_generic = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(),
        values_.internIdentifier(canonical_generic_name),
        static_cast<std::uint32_t>(generic_nodes.size()));
    for (std::uint32_t index = 0; index < generic_nodes.size(); ++index) {
      const auto name = tokenString(generic_nodes[index]);
      if (!name.hasValue() ||
          !generic_bindings_.emplace(name.index, index).second)
        emit(DiagnosticKind::DuplicateName, generic_nodes[index]);
    }
    current_generic_ = impl_generic;
  }
  const auto owner_type = checkType(children[owner_index]);
  const auto owner = sem_ir_.type(owner_type).kind == SemTypeKind::Nominal
                         ? NominalTypeId(sem_ir_.type(owner_type).arg0)
                         : NominalTypeId::invalid();
  if (!owner.hasValue()) {
    emit((representation_impl || projection_impl || object_lifecycle_impl)
             ? DiagnosticKind::InvalidRepresentationImpl
             : DiagnosticKind::InvalidLifecycleImpl,
         children[owner_index]);
    return;
  }
  const auto member_start =
      owner_index + 1 < children.size() &&
              tree_.kind(children[owner_index + 1]) == NodeKind::ConstraintList
          ? owner_index + 2
          : owner_index + 1;
  const auto ordinary_inherent_impl =
      !trait_impl &&
      std::ranges::any_of(
          std::span<const NodeId>(children).subspan(member_start),
          [&](NodeId member) {
            const auto member_name = functionChild(member, NodeKind::Name);
            return tree_.kind(member) != NodeKind::FunctionDecl ||
                   !member_name.hasValue() ||
                   values_.identifier(tokenString(member_name)) != "drop";
          });
  if (ordinary_inherent_impl) {
    auto nominal = sem_ir_.nominalType(owner);
    const auto &owner_sem_type = sem_ir_.type(owner_type);
    const auto owner_arguments =
        sem_ir_.typeBlock(TypeBlockId(owner_sem_type.arg1));
    const auto expected_generic_count =
        nominal.generic.hasValue()
            ? values_.generics().generic(nominal.generic).binding_count
            : 0U;
    bool valid_pattern = has_generic == (expected_generic_count != 0) &&
                         owner_arguments.size() == expected_generic_count;
    for (std::uint32_t index = 0;
         valid_pattern && index < owner_arguments.size(); ++index) {
      const auto &argument = sem_ir_.type(owner_arguments[index]);
      valid_pattern = argument.kind == SemTypeKind::TypeParameter &&
                      argument.arg0 == impl_generic.index &&
                      argument.arg1 == index;
    }
    if (nominal.canonical_entity.hasValue() ||
        hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic) ||
        !valid_pattern) {
      emit(DiagnosticKind::InvalidInherentMethod, node);
      return;
    }
    for (std::size_t index = member_start; index < children.size(); ++index) {
      const auto member = children[index];
      const auto member_children = tree_.children(member);
      const auto async_modifier =
          functionChild(member, NodeKind::AsyncModifier);
      if (async_modifier.hasValue()) {
        emit(DiagnosticKind::InvalidAsyncDeclaration, async_modifier);
        continue;
      }
      const auto method_name = member_children.empty()
                                   ? NameId::invalid()
                                   : nameFor(member_children.front());
      const auto intrinsic_role =
          method_name.hasValue() ? compilerIntrinsicRole(method_name, owner)
                                 : CompilerIntrinsicRole::None;
      const bool member_has_contract =
          functionChild(member, NodeKind::CallableContract).hasValue();
      if (tree_.kind(member) != NodeKind::FunctionDecl ||
          member_children.size() < 4 ||
          hasNodeFlag(tree_.get(member).flags, NodeFlags::IsForeign) ||
          (hasNodeFlag(tree_.get(member).flags, NodeFlags::IsBodyless) &&
           intrinsic_role == CompilerIntrinsicRole::None &&
           !member_has_contract) ||
          !tree_.children(member_children[1]).empty()) {
        emit(DiagnosticKind::InvalidInherentMethod, member);
        continue;
      }
      const auto field = std::ranges::find(nominal.fields, method_name,
                                           &SemNominalField::name);
      const auto parameter_nodes = functionParameters(member_children[2]);
      const auto has_receiver =
          !parameter_nodes.empty() &&
          values_.identifier(tokenString(parameter_nodes.front().name)) ==
              "self";
      bool misplaced_self = false;
      for (std::size_t parameter = has_receiver ? 1U : 0U;
           parameter < parameter_nodes.size(); ++parameter)
        misplaced_self |=
            values_.identifier(tokenString(parameter_nodes[parameter].name)) ==
            "self";
      if (field != nominal.fields.end() || misplaced_self) {
        emit(field != nominal.fields.end()
                 ? DiagnosticKind::DuplicateName
                 : DiagnosticKind::InvalidInherentMethod,
             member);
        continue;
      }
      auto method_generic = impl_generic;
      auto member_owner_type = owner_type;
      if (has_generic) {
        generic_bindings_.clear();
        const auto generic_nodes = tree_.children(children[generic_index]);
        const auto owner_name =
            std::string(values_.identifier(sem_ir_.name(nominal.name).text));
        const auto method_text =
            std::string(values_.identifier(sem_ir_.name(method_name).text));
        method_generic = values_.generics().addGeneric(
            sem_ir_.checkIRId(), sem_ir_.moduleName(),
            values_.internIdentifier(owner_name + "::" + method_text),
            static_cast<std::uint32_t>(generic_nodes.size()));
        for (std::uint32_t binding = 0; binding < generic_nodes.size();
             ++binding) {
          const auto name = tokenString(generic_nodes[binding]);
          if (name.hasValue())
            generic_bindings_.emplace(name.index, binding);
        }
        current_generic_ = method_generic;
        member_owner_type = checkType(children[owner_index]);
      }
      const auto method_is_constructor =
          intrinsic_role == CompilerIntrinsicRole::None && !has_receiver &&
          values_.identifier(sem_ir_.name(method_name).text) == "init";
      predeclareFunction(
          member, owner,
          method_is_constructor ? SemCanonicalFunctionRole::Constructor
                                : SemCanonicalFunctionRole::None,
          false, method_generic, member_owner_type, NameId::invalid(), true);
      const auto found = function_nodes_.find(member.index);
      if (found == function_nodes_.end())
        continue;
      const auto function = found->second;
      const auto &function_type = sem_ir_.type(sem_ir_.function(function).type);
      const auto parameters =
          sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
      bool receiver_valid = !has_receiver || !parameters.empty();
      if (has_receiver && receiver_valid) {
        auto receiver = parameters.front();
        if (sem_ir_.type(receiver).kind == SemTypeKind::Reference)
          receiver = sem_ir_.referencePointee(receiver);
        receiver_valid = sem_ir_.canonicalType(receiver) ==
                         sem_ir_.canonicalType(member_owner_type);
      }
      if (!receiver_valid) {
        emit(DiagnosticKind::InvalidInherentMethod, member);
        continue;
      }
      const auto target = functionReference(function);
      if (!target.hasValue()) {
        emit(DiagnosticKind::InvalidInherentMethod, member);
        continue;
      }
      nominal.member_functions.push_back(
          {method_name, target,
           (hasNodeFlag(tree_.get(member).flags, NodeFlags::IsPublic)
                ? SemNominalMemberFunctionPublic
                : 0U) |
               (has_receiver ? 0U : SemNominalMemberFunctionAssociated)});
    }
    sem_ir_.setNominalType(owner, std::move(nominal));
    current_generic_ = GenericId::invalid();
    generic_bindings_.clear();
    return;
  }
  if ((!trait_impl || representation_impl || projection_impl ||
       object_lifecycle_impl) &&
      hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic)) {
    emit((representation_impl || projection_impl || object_lifecycle_impl)
             ? DiagnosticKind::InvalidRepresentationImpl
             : DiagnosticKind::InvalidLifecycleImpl,
         node);
    return;
  }
  auto nominal = sem_ir_.nominalType(owner);
  if ((representation_impl && !nominal.value_repr_pattern.hasValue()) ||
      ((projection_impl || object_lifecycle_impl) &&
       !nominal.object_repr_pattern.hasValue())) {
    emit(DiagnosticKind::InvalidRepresentationImpl, node);
    return;
  }
  NameId projector_name;
  std::size_t canonical_member_start = owner_index + 1;
  SemObjectProjector *projector = nullptr;
  if (projection_impl) {
    if (canonical_member_start >= children.size() ||
        tree_.kind(children[canonical_member_start]) != NodeKind::Name) {
      emit(DiagnosticKind::InvalidRepresentationImpl, node);
      return;
    }
    projector_name = nameFor(children[canonical_member_start++]);
    const auto duplicate = std::ranges::find(
        nominal.object_projectors, projector_name, &SemObjectProjector::name);
    if (duplicate != nominal.object_projectors.end()) {
      emit(DiagnosticKind::InvalidRepresentationImpl, node);
      return;
    }
    nominal.object_projectors.push_back(
        {projector_name, impl_generic, owner_type});
    projector = &nominal.object_projectors.back();
  }
  if (canonical_member_start < children.size() &&
      tree_.kind(children[canonical_member_start]) == NodeKind::ConstraintList)
    ++canonical_member_start;
  for (std::size_t index = canonical_member_start; index < children.size();
       ++index) {
    const auto member = children[index];
    if (tree_.kind(member) != NodeKind::FunctionDecl) {
      emit((representation_impl || projection_impl || object_lifecycle_impl)
               ? DiagnosticKind::InvalidRepresentationImpl
               : DiagnosticKind::InvalidLifecycleImpl,
           member);
      continue;
    }
    const auto member_children = tree_.children(member);
    const auto async_modifier = functionChild(member, NodeKind::AsyncModifier);
    if (async_modifier.hasValue()) {
      emit(DiagnosticKind::InvalidAsyncDeclaration, async_modifier);
      continue;
    }
    if (member_children.empty()) {
      emit((representation_impl || projection_impl || object_lifecycle_impl)
               ? DiagnosticKind::InvalidRepresentationImpl
               : DiagnosticKind::InvalidLifecycleImpl,
           member);
      continue;
    }
    const auto name = values_.identifier(tokenString(member_children[0]));
    const auto role =
        representation_impl && name == "pack"   ? SemCanonicalFunctionRole::Pack
        : representation_impl && name == "init" ? SemCanonicalFunctionRole::Init
        : projection_impl && name == "load"
            ? SemCanonicalFunctionRole::ProjectionLoad
        : projection_impl && name == "store"
            ? SemCanonicalFunctionRole::ProjectionStore
        : projection_impl && name == "take"
            ? SemCanonicalFunctionRole::ProjectionTake
        : projection_impl && name == "init"
            ? SemCanonicalFunctionRole::ProjectionInit
        : projection_impl && name == "borrow"
            ? SemCanonicalFunctionRole::ProjectionBorrow
        : projection_impl && name == "borrow_mut"
            ? SemCanonicalFunctionRole::ProjectionBorrowMut
        : object_lifecycle_impl && name == "object_init"
            ? SemCanonicalFunctionRole::ObjectInit
        : object_lifecycle_impl && name == "object_copy_init"
            ? SemCanonicalFunctionRole::ObjectCopyInit
        : object_lifecycle_impl && name == "object_move_init"
            ? SemCanonicalFunctionRole::ObjectMoveInit
        : object_lifecycle_impl && name == "object_drop"
            ? SemCanonicalFunctionRole::ObjectDrop
        : trait_impl && name == "copy"  ? SemCanonicalFunctionRole::Copy
        : !trait_impl && name == "drop" ? SemCanonicalFunctionRole::Drop
                                        : SemCanonicalFunctionRole::None;
    if (role == SemCanonicalFunctionRole::None ||
        (trait_impl &&
         hasNodeFlag(tree_.get(member).flags, NodeFlags::IsPublic))) {
      emit((representation_impl || projection_impl || object_lifecycle_impl)
               ? DiagnosticKind::InvalidRepresentationImpl
               : DiagnosticKind::InvalidLifecycleImpl,
           member);
      continue;
    }
    FunctionId *target = nullptr;
    if (role == SemCanonicalFunctionRole::Copy)
      target = &nominal.lifecycle_copy_function;
    else if (role == SemCanonicalFunctionRole::Drop)
      target = &nominal.lifecycle_drop_function;
    else if (role == SemCanonicalFunctionRole::Pack)
      target = &nominal.representation_pack_function;
    else if (role == SemCanonicalFunctionRole::Init)
      target = &nominal.representation_init_function;
    else if (role == SemCanonicalFunctionRole::ProjectionLoad)
      target = &projector->load_function;
    else if (role == SemCanonicalFunctionRole::ProjectionStore)
      target = &projector->store_function;
    else if (role == SemCanonicalFunctionRole::ProjectionTake)
      target = &projector->take_function;
    else if (role == SemCanonicalFunctionRole::ProjectionInit)
      target = &projector->init_function;
    else if (role == SemCanonicalFunctionRole::ProjectionBorrow)
      target = &projector->borrow_function;
    else if (role == SemCanonicalFunctionRole::ProjectionBorrowMut)
      target = &projector->borrow_mut_function;
    else if (role == SemCanonicalFunctionRole::ObjectInit)
      target = &nominal.object_init_function;
    else if (role == SemCanonicalFunctionRole::ObjectCopyInit)
      target = &nominal.object_copy_init_function;
    else if (role == SemCanonicalFunctionRole::ObjectMoveInit)
      target = &nominal.object_move_init_function;
    else if (role == SemCanonicalFunctionRole::ObjectDrop)
      target = &nominal.object_drop_function;
    assert(target != nullptr);
    if (target->hasValue()) {
      emit((representation_impl || projection_impl || object_lifecycle_impl)
               ? DiagnosticKind::InvalidRepresentationImpl
               : DiagnosticKind::InvalidLifecycleImpl,
           member);
      continue;
    }
    auto member_generic = impl_generic;
    auto member_owner_type = owner_type;
    if (has_generic &&
        ((representation_impl && role == SemCanonicalFunctionRole::Init) ||
         projection_impl || object_lifecycle_impl)) {
      const auto owner_name =
          std::string(values_.identifier(sem_ir_.name(nominal.name).text));
      const auto member_name =
          projection_impl ? "$projection$" + owner_name + "$" +
                                std::string(values_.identifier(
                                    sem_ir_.name(projector_name).text)) +
                                "$" + std::string(name)
          : object_lifecycle_impl
              ? "$object$" + owner_name + "$" + std::string(name)
              : "$representation$" + owner_name + "$init";
      member_generic = values_.generics().addGeneric(
          sem_ir_.checkIRId(), sem_ir_.moduleName(),
          values_.internIdentifier(member_name),
          values_.generics().generic(impl_generic).binding_count);
      current_generic_ = member_generic;
      member_owner_type = checkType(children[owner_index]);
    }
    predeclareFunction(
        member, owner, role,
        trait_impl && !representation_impl &&
            hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic),
        member_generic, member_owner_type, projector_name);
    const auto found = function_nodes_.find(member.index);
    if (found != function_nodes_.end())
      *target = found->second;
    if (role == SemCanonicalFunctionRole::Copy)
      nominal.lifecycle_copy =
          static_cast<std::uint8_t>(SemLifecycleCopyPolicy::Custom);
    else if (role == SemCanonicalFunctionRole::Drop)
      nominal.lifecycle_drop =
          static_cast<std::uint8_t>(SemLifecycleDropPolicy::Custom);
  }
  if (representation_impl) {
    nominal.representation_generic = impl_generic;
    nominal.representation_pattern = owner_type;
  } else if (projection_impl || object_lifecycle_impl) {
    // The canonical targets above are persisted by semantic witnesses.
  } else if (trait_impl) {
    nominal.lifecycle_copy_generic = impl_generic;
    nominal.lifecycle_copy_pattern = owner_type;
  } else {
    nominal.lifecycle_drop_generic = impl_generic;
    nominal.lifecycle_drop_pattern = owner_type;
  }
  sem_ir_.setNominalType(owner, std::move(nominal));
}
