// Included inside SemanticContext::checkCallExpression; this fragment
// aliases the single semantic call-resolution state.
InstId SemanticContext::checkCallExpression(NodeId node,
                                            std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  if (children.empty()) {
    emit(DiagnosticKind::NotCallable, node);
    return appendInvalid(block, node);
  }
  const auto callee_node = children.front();
  if (tree_.kind(callee_node) == NodeKind::QualifiedName) {
    const auto names = tree_.children(callee_node);
    if (names.size() >= 2) {
      NominalTypeId resource_owner;
      if (names.size() == 2) {
        const auto owner_name = tokenString(names[0]);
        const auto owner = nominal_names_.find(owner_name.index);
        if (owner != nominal_names_.end())
          resource_owner = owner->second;
      } else {
        std::string module_text;
        for (std::size_t index = 0; index + 2 < names.size(); ++index) {
          if (!module_text.empty())
            module_text += "::";
          module_text += values_.identifier(tokenString(names[index]));
        }
        const auto imported = sem_ir_.importIRs().findByModule(
            values_.internIdentifier(module_text));
        const auto *interface = sem_ir_.importIRs().tryGetInterface(imported);
        if (interface) {
          const auto binding =
              interface->findNominalType(tokenString(names[names.size() - 2]));
          if (binding.hasValue())
            resource_owner = materializeImportedNominal(
                interface->nominalType(binding).canonical_entity,
                names[names.size() - 2]);
        }
      }
      const auto operation_name = tokenString(names.back());
      if (resource_owner.hasValue()) {
        const auto &nominal = sem_ir_.nominalType(resource_owner);
        if (nominal.kind == NominalKind::ForeignResource) {
          if (values_.identifier(operation_name) == "valid") {
            if (children.size() != 2) {
              emit(DiagnosticKind::ArgumentCountMismatch, node);
              return appendInvalid(block, node);
            }
            auto argument = checkExpression(children[1], block);
            if (!adjustExpression(argument,
                                  sem_ir_.addNominalType(resource_owner),
                                  children[1], block))
              emit(DiagnosticKind::TypeMismatch, children[1]);
            return appendInst<SemForeignResourceValid>(
                block, node, sem_ir_.boolType(), argument);
          }
          const auto operation = std::ranges::find_if(
              nominal.foreign_resource_operations, [&](const auto &item) {
                return sem_ir_.name(item.name).text == operation_name;
              });
          if (operation == nominal.foreign_resource_operations.end()) {
            emit(DiagnosticKind::UnknownName, names.back());
            return appendInvalid(block, node);
          }
          if (operation->role != ForeignResourceRoleKind::InspectReady)
            (void)requireUnsafe(UnsafeOperationKind::ForeignResourceMutation,
                                callee_node);
          const auto &target = sem_ir_.functionRef(operation->target);
          const auto &target_type = sem_ir_.type(target.local_type);
          const auto parameters =
              sem_ir_.typeBlock(TypeBlockId(target_type.arg0));
          if (nominal.foreign_registration_storage_type.hasValue()) {
            const auto &protocol =
                values_.generics()
                    .foreignResourceProtocol(nominal.foreign_resource_protocol)
                    .facts;
            const auto *role = protocol.findRole(operation->role);
            const auto injected_ref = [&](const auto &item) {
              sem_ir_.addFunctionOccurrence(names.back(),
                                            SemSymbolOccurrenceKind::Reference,
                                            item.target);
              return appendInst<SemForeignFunctionRef>(
                  block, node, foreignResourceTargetPointer(item.target),
                  item.target);
            };
            const auto operation_for = [&](ForeignResourceRoleKind kind) {
              return std::ranges::find(nominal.foreign_resource_operations,
                                       kind,
                                       &SemForeignResourceOperation::role);
            };
            const auto end = nominal.foreign_resource_operations.end();
            if (operation->role == ForeignResourceRoleKind::AcquireOwned) {
              std::vector<const ForeignResourceParameterBinding *> bound;
              for (const auto &mapping : role->parameters)
                if (mapping.kind == ForeignResourceParameterKind::Bound)
                  bound.push_back(&mapping);
              std::ranges::sort(
                  bound, {}, &ForeignResourceParameterBinding::parameter_index);
              if (children.size() != bound.size() + 2) {
                emit(DiagnosticKind::ArgumentCountMismatch, node);
                return appendInvalid(block, node);
              }
              auto callback_value = checkExpression(children[1], block);
              if (!adjustExpression(callback_value,
                                    nominal.foreign_callback_type, children[1],
                                    block))
                emit(DiagnosticKind::TypeMismatch, children[1]);
              const auto close =
                  operation_for(ForeignResourceRoleKind::CloseQuiescent);
              const auto cancel =
                  operation_for(ForeignResourceRoleKind::CancelQuiescent);
              const auto cancel_async =
                  operation_for(ForeignResourceRoleKind::CancelAsync);
              const auto wait =
                  operation_for(ForeignResourceRoleKind::WaitCompletion);
              const auto poll =
                  operation_for(ForeignResourceRoleKind::InspectReady);
              const auto arm =
                  operation_for(ForeignResourceRoleKind::ArmOneShot);
              const auto detach =
                  operation_for(ForeignResourceRoleKind::DetachCompletion);
              std::vector<InstId> storage{
                  callback_value, injected_ref(*operation),
                  injected_ref(*close), injected_ref(*cancel)};
              if (cancel_async != end && wait != end) {
                storage.push_back(injected_ref(*cancel_async));
                storage.push_back(injected_ref(*wait));
                if (poll != end)
                  storage.push_back(injected_ref(*poll));
                if (arm != end && detach != end) {
                  storage.push_back(injected_ref(*arm));
                  storage.push_back(injected_ref(*detach));
                }
              }
              for (std::size_t index = 0; index < bound.size(); ++index) {
                auto value = checkExpression(children[index + 2], block);
                const auto expected = parameters[bound[index]->parameter_index];
                if (!adjustExpression(value, expected, children[index + 2],
                                      block))
                  emit(DiagnosticKind::TypeMismatch, children[index + 2]);
                storage.push_back(appendInst<SemCallbackRegistrationBinding>(
                    block, children[index + 2], expected,
                    sem_ir_.addName(
                        values_.internIdentifier(bound[index]->name)),
                    value));
              }
              const auto made = appendInst<SemMakeCallbackRegistration>(
                  block, node, nominal.foreign_registration_storage_type,
                  sem_ir_.addInstBlock(storage, true));
              return appendInst<SemForeignResourceWrap>(
                  block, node, sem_ir_.addNominalType(resource_owner), made);
            }
            const auto expected_count =
                operation->role == ForeignResourceRoleKind::ArmOneShot ? 2U
                                                                       : 1U;
            if (children.size() != expected_count + 1) {
              emit(DiagnosticKind::ArgumentCountMismatch, node);
              return appendInvalid(block, node);
            }
            auto source = checkExpression(children[1], block);
            const auto source_kind = sem_ir_.type(instType(source)).kind;
            const auto completion =
                operation->role == ForeignResourceRoleKind::WaitCompletion ||
                operation->role == ForeignResourceRoleKind::InspectReady ||
                operation->role == ForeignResourceRoleKind::ArmOneShot ||
                operation->role == ForeignResourceRoleKind::DetachCompletion;
            if (!completion) {
              if (source_kind != SemTypeKind::Nominal ||
                  NominalTypeId(sem_ir_.type(instType(source)).arg0) !=
                      resource_owner)
                emit(DiagnosticKind::TypeMismatch, children[1]);
              source = appendInst<SemForeignResourceUnwrap>(
                  block, children[1], nominal.foreign_registration_storage_type,
                  source);
            } else if (source_kind == SemTypeKind::ForeignWake) {
              source = appendInst<SemForeignResourceUnwrap>(
                  block, children[1], nominal.foreign_wake_storage_type,
                  source);
            } else {
              if (source_kind != SemTypeKind::ForeignCompletion)
                emit(DiagnosticKind::TypeMismatch, children[1]);
              source = appendInst<SemForeignResourceUnwrap>(
                  block, children[1], nominal.foreign_completion_storage_type,
                  source);
            }
            if (operation->role == ForeignResourceRoleKind::CloseQuiescent)
              return appendInst<SemCallbackUnregister>(
                  block, node, sem_ir_.voidType(), source);
            if (operation->role == ForeignResourceRoleKind::CancelQuiescent)
              return appendInst<SemCallbackCancel>(block, node,
                                                   sem_ir_.voidType(), source);
            if (operation->role == ForeignResourceRoleKind::CancelAsync) {
              const auto value = appendInst<SemCallbackCancelAsync>(
                  block, node, nominal.foreign_completion_storage_type, source);
              return appendInst<SemForeignResourceWrap>(
                  block, node, sem_ir_.addForeignCompletionType(resource_owner),
                  value);
            }
            if (operation->role == ForeignResourceRoleKind::InspectReady)
              return source_kind == SemTypeKind::ForeignWake
                         ? appendInst<SemCallbackWakeReady>(
                               block, node, sem_ir_.boolType(), source)
                         : appendInst<SemCallbackCompletionPoll>(
                               block, node, sem_ir_.boolType(), source);
            if (operation->role == ForeignResourceRoleKind::WaitCompletion)
              return appendInst<SemCallbackWait>(block, node,
                                                 sem_ir_.voidType(), source);
            if (operation->role == ForeignResourceRoleKind::ArmOneShot) {
              auto adapter = checkExpression(children[2], block);
              if (!adjustExpression(adapter, nominal.foreign_waker_type,
                                    children[2], block))
                emit(DiagnosticKind::TypeMismatch, children[2]);
              const std::array operands{source, adapter};
              const auto wake = appendInst<SemMakeCallbackWake>(
                  block, node, nominal.foreign_wake_storage_type,
                  sem_ir_.addInstBlock(operands, true));
              return appendInst<SemForeignResourceWrap>(
                  block, node, sem_ir_.addForeignWakeType(resource_owner),
                  wake);
            }
            if (operation->role == ForeignResourceRoleKind::DetachCompletion)
              return appendInst<SemCallbackDetach>(block, node,
                                                   sem_ir_.voidType(), source);
          }
          if (children.size() - 1 != parameters.size())
            emit(DiagnosticKind::ArgumentCountMismatch, node);
          std::vector<InstId> arguments;
          for (std::size_t index = 1; index < children.size(); ++index) {
            auto argument = checkExpression(children[index], block);
            const auto physical = static_cast<std::uint32_t>(index - 1);
            if (physical == operation->resource_parameter) {
              const auto expected = sem_ir_.addNominalType(resource_owner);
              if (!adjustExpression(argument, expected, children[index], block))
                emit(DiagnosticKind::TypeMismatch, children[index]);
              argument = appendInst<SemForeignResourceUnwrap>(
                  block, children[index], nominal.foreign_handle_type,
                  argument);
            } else if (physical == operation->completion_parameter) {
              const auto expected =
                  sem_ir_.addForeignCompletionType(resource_owner);
              if (!adjustExpression(argument, expected, children[index], block))
                emit(DiagnosticKind::TypeMismatch, children[index]);
              argument = appendInst<SemForeignResourceUnwrap>(
                  block, children[index],
                  nominal.foreign_completion_handle_type, argument);
            } else if (physical < parameters.size() &&
                       !adjustExpression(argument, parameters[physical],
                                         children[index], block)) {
              emit(DiagnosticKind::TypeMismatch, children[index]);
            }
            arguments.push_back(argument);
          }
          sem_ir_.addFunctionOccurrence(names.back(),
                                        SemSymbolOccurrenceKind::Reference,
                                        operation->target);
          const auto call = appendInst<SemCall>(
              block, node, TypeId(target_type.arg1), operation->target,
              sem_ir_.addInstBlock(arguments));
          if (operation->role == ForeignResourceRoleKind::AcquireOwned ||
              operation->role == ForeignResourceRoleKind::CancelAsync)
            return appendInst<SemForeignResourceWrap>(
                block, node, operation->source_result, call);
          return call;
        }
      }
    }
  }
  if (tree_.kind(callee_node) == NodeKind::Name &&
      lookup(nameFor(callee_node)).hasValue())
    return checkCallableValue(node, checkExpression(callee_node, block), block);
  if (tree_.kind(callee_node) != NodeKind::Name &&
      tree_.kind(callee_node) != NodeKind::QualifiedName &&
      tree_.kind(callee_node) != NodeKind::TypeMemberExpr &&
      tree_.kind(callee_node) != NodeKind::MemberExpr)
    return checkCallableValue(node, checkExpression(callee_node, block), block);
  if (isCoreCarrierName(callee_node))
    return checkCarrierView(node, children, block);
  FunctionRefId function_ref;
  std::vector<FunctionRefId> free_candidates;
  InstId method_receiver;
  NodeId method_receiver_node;
  NodeId occurrence_node = callee_node;
  std::vector<CanonicalTypeId> explicit_member_arguments;
  if (tree_.kind(callee_node) == NodeKind::MemberExpr) {
    const auto member_children = tree_.children(callee_node);
    if (member_children.size() != 2) {
      emit(DiagnosticKind::NotCallable, callee_node);
      return appendInvalid(block, node);
    }
    method_receiver_node = member_children.front();
    occurrence_node = member_children.back();
    method_receiver = checkExpression(method_receiver_node, block);
    auto receiver_type = instType(method_receiver);
    while (sem_ir_.type(receiver_type).kind == SemTypeKind::Reference)
      receiver_type = sem_ir_.referencePointee(receiver_type);
    const auto member_name = nameFor(occurrence_node);
    const auto &receiver_semantics = sem_ir_.type(receiver_type);
    if (receiver_semantics.kind == SemTypeKind::Tuple ||
        (receiver_semantics.kind == SemTypeKind::Nominal &&
         std::ranges::any_of(sem_ir_.nominalType(NominalTypeId(receiver_semantics.arg0)).fields,
             [&](const auto &field) { return field.name == member_name; })))
      return checkCallableValue(node,
          checkMemberValue(callee_node, method_receiver, block), block);

    if (sem_ir_.type(receiver_type).kind == SemTypeKind::TypeParameter &&
        current_function_.hasValue()) {
      const auto constraints = sem_ir_.functionConstraints(current_function_);
      for (std::uint32_t constraint_index = 0;
           constraint_index < constraints.size(); ++constraint_index) {
        const auto &constraint = constraints[constraint_index];
        if (sem_ir_.canonicalType(constraint.subject) !=
            sem_ir_.canonicalType(receiver_type))
          continue;
        const auto &interface_value =
            sem_ir_.interface(constraint.interface_id);
        for (std::uint32_t requirement_index = 0;
             requirement_index < interface_value.requirements.size();
             ++requirement_index) {
          const auto &requirement =
              interface_value.requirements[requirement_index];
          if (requirement.kind != SemInterfaceRequirementKind::Function ||
              requirement.name != member_name)
            continue;
          std::vector<CanonicalTypeId> substitutions(
              values_.generics()
                  .generic(interface_value.generic)
                  .binding_count);
          const auto constraint_arguments =
              sem_ir_.typeBlock(constraint.arguments);
          for (std::uint32_t index = 0; index < constraint_arguments.size();
               ++index)
            substitutions[index] =
                sem_ir_.canonicalType(constraint_arguments[index]);
          substitutions[interface_value.explicit_parameter_count] =
              sem_ir_.canonicalType(receiver_type);
          const auto callable = substituteType(
              requirement.type, interface_value.generic, substitutions);
          if (!callable.hasValue() ||
              sem_ir_.type(callable).kind != SemTypeKind::Function) {
            DiagnosticKind witness_diagnostic =
                DiagnosticKind::UnsatisfiedConstraint;
            if (interface_value.canonical_entity.hasValue()) {
              const auto *entity = sem_ir_.importIRs().tryGetEntity(
                  interface_value.canonical_entity);
              if (entity && sem_ir_.identifier(entity->package_name) == "std" &&
                  sem_ir_.identifier(entity->module_name) == "std::hash") {
                const auto interface_name =
                    sem_ir_.identifier(entity->name);
                if (interface_name == "Hash")
                  witness_diagnostic = DiagnosticKind::MissingHashWitness;
                else if (interface_name == "Equal")
                  witness_diagnostic = DiagnosticKind::MissingEqualWitness;
              }
            }
            emit(witness_diagnostic, occurrence_node);
            return appendInvalid(block, node);
          }
          const auto parameters =
              sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callable).arg0));
          if (parameters.empty() || children.size() != parameters.size()) {
            emit(DiagnosticKind::ArgumentCountMismatch, node);
            return appendInvalid(block, node);
          }
          std::vector<InstId> arguments{method_receiver};
          auto receiver = method_receiver;
          if (!adjustMethodReceiver(receiver, parameters.front(),
                                    method_receiver_node, block))
            return appendInvalid(block, node);
          arguments.front() = receiver;
          for (std::size_t index = 1; index < children.size(); ++index) {
            if (tree_.kind(children[index]) == NodeKind::NamedArgument) {
              emit(DiagnosticKind::InvalidNamedArgument, children[index]);
              return appendInvalid(block, node);
            }
            auto argument = checkExpression(children[index], block);
            if (!adjustExpression(argument, parameters[index], children[index],
                                  block))
              emit(DiagnosticKind::TypeMismatch, children[index]);
            arguments.push_back(argument);
          }
          const auto encoded =
              (static_cast<std::uint64_t>(constraint_index) << 32U) |
              requirement_index;
          sem_ir_.addFunctionOccurrence(occurrence_node,
                                        SemSymbolOccurrenceKind::Reference,
                                        requirement.function);
          return appendInst<SemInterfaceCall>(
              block, node, TypeId(sem_ir_.type(callable).arg1),
              sem_ir_.addInteger(static_cast<std::int64_t>(encoded)),
              sem_ir_.addInstBlock(arguments));
        }
      }
    }
    if (sem_ir_.type(receiver_type).kind != SemTypeKind::Nominal) {
      emit(DiagnosticKind::InvalidMember, callee_node);
      return appendInvalid(block, node);
    }
    // Imported member resolution may add nominal shells and reallocate
    // SemIR storage. Keep a value snapshot across that mutation.
    const auto nominal =
        sem_ir_.nominalType(NominalTypeId(sem_ir_.type(receiver_type).arg0));
    for (const auto argument :
         sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(receiver_type).arg1)))
      explicit_member_arguments.push_back(sem_ir_.canonicalType(argument));
    sem_ir_.addMemberAccessContext(
        occurrence_node, NominalTypeId(sem_ir_.type(receiver_type).arg0),
        SemMemberAccessKind::Instance);
    for (const auto &member : nominal.member_functions)
      if (member.name == member_name &&
          (member.flags & SemNominalMemberFunctionAssociated) == 0)
        free_candidates.push_back(member.target);
    if (free_candidates.empty())
      free_candidates = collectInterfaceMemberCandidates(
          receiver_type, member_name, occurrence_node);
    if (free_candidates.empty() && nominal.canonical_entity.hasValue()) {
      const auto nominal_name = values_.identifier(
          sem_ir_.name(nominal.name).text);
      for (const auto candidate : resolveImportedMemberFunctions(
               nominal.canonical_entity, sem_ir_.name(member_name).text,
               occurrence_node)) {
        const auto *entity = sem_ir_.importIRs().tryGetEntity(
            sem_ir_.functionRef(candidate).public_entity);
        // `Option<T>::unwrap` consumes its receiver by value. Older public
        // artifacts classified value receivers as associated members because
        // they have no reference capability; the source operation is still an
        // instance method and must participate in ordinary method receiver
        // adjustment. Keep the compatibility exception local to the
        // compiler-owned Option operation rather than weakening member-kind
        // verification for arbitrary imported functions.
        if (entity &&
            (entity->member_kind == PublicFunctionArtifact::MemberKind::Instance ||
             (nominal_name == "Option" &&
              values_.identifier(sem_ir_.name(member_name).text) == "unwrap")))
          free_candidates.push_back(candidate);
      }
    }
    if (!free_candidates.empty())
      function_ref = free_candidates.front();
    if (!function_ref.hasValue()) {
      emit(DiagnosticKind::InvalidMember, occurrence_node);
      return appendInvalid(block, node);
    }
    if (nominal.canonical_entity.hasValue())
      for (const auto &member : nominal.member_functions)
        if (member.name == member_name &&
            (member.flags & SemNominalMemberFunctionAssociated) == 0 &&
            (member.flags & SemNominalMemberFunctionPublic) == 0) {
          emit(DiagnosticKind::PrivateMemberAccess, occurrence_node);
          return appendInvalid(block, node);
        }
  } else if (tree_.kind(callee_node) == NodeKind::Name ||
             tree_.kind(callee_node) == NodeKind::MoveExpr) {
    const auto callee_name_node =
        tree_.kind(callee_node) == NodeKind::MoveExpr
            ? tree_.children(callee_node).empty()
                  ? callee_node
                  : tree_.children(callee_node).front()
            : callee_node;
    const auto callee_name = nameFor(callee_name_node);
    const auto lexical_callee = lookupValue(callee_name);
    if (lexical_callee.local.hasValue()) {
      const auto closure_type = sem_ir_.local(lexical_callee.local).type;
      const auto *closure = sem_ir_.tryGetCallableEnvironment(closure_type);
      if (closure) {
        const auto &reference = sem_ir_.functionRef(closure->target);
        const auto &signature = sem_ir_.type(reference.local_type);
        const auto parameters = sem_ir_.typeBlock(TypeBlockId(signature.arg0));
        auto receiver =
            tree_.kind(callee_node) == NodeKind::MoveExpr
                ? checkExpression(callee_node, block)
                : appendInst<SemNameRef>(block, callee_node, closure_type,
                                         lexical_callee.local);
        if (parameters.empty() ||
            !adjustMethodReceiver(receiver, parameters.front(), callee_node,
                                  block))
          return appendInvalid(block, node);
        std::vector<InstId> arguments{receiver};
        for (std::size_t index = 1; index < children.size(); ++index)
          arguments.push_back(checkExpression(children[index], block));
        if (arguments.size() != parameters.size())
          emit(DiagnosticKind::ArgumentCountMismatch, node);
        for (std::size_t index = 1;
             index < arguments.size() && index < parameters.size(); ++index)
          if (!adjustExpression(arguments[index], parameters[index],
                                children[index], block))
            emit(DiagnosticKind::TypeMismatch, children[index]);
        return appendInst<SemCall>(block, node, TypeId(signature.arg1),
                                   closure->target,
                                   sem_ir_.addInstBlock(arguments));
      }
      emit(DiagnosticKind::NotCallable, callee_node);
      return appendInvalid(block, node);
    }
    if (lexical_callee.constant.hasValue()) {
      emit(DiagnosticKind::NotCallable, callee_node);
      return appendInvalid(block, node);
    }
    const auto found = function_names_.find(callee_name.index);
    if (found == function_names_.end()) {
      emit(DiagnosticKind::UnknownName, callee_node);
      return appendInvalid(block, node);
    }
    if (found->second.empty()) {
      emit(DiagnosticKind::UnknownName, callee_node);
      return appendInvalid(block, node);
    }
    free_candidates = found->second;
    function_ref = found->second.front();
  } else {
    const auto path_children = tree_.children(callee_node);
    TypeId owner_type;
    NominalTypeId owner;
    if (tree_.kind(callee_node) == NodeKind::TypeMemberExpr &&
        path_children.size() == 2) {
      owner_type = checkType(path_children.front());
      if (sem_ir_.type(owner_type).kind == SemTypeKind::Nominal)
        owner = NominalTypeId(sem_ir_.type(owner_type).arg0);
      occurrence_node = path_children.back();
    } else if (tree_.kind(callee_node) == NodeKind::QualifiedName &&
               path_children.size() >= 2) {
      const auto owner_components = std::span<const NodeId>(path_children).first(path_children.size() - 1);
      owner_type = resolveNominalTypeComponents(owner_components, callee_node);
      owner = owner_type.hasValue() ? NominalTypeId(sem_ir_.type(owner_type).arg0)
                                  : resolveNominalComponents(owner_components, callee_node);
      occurrence_node = path_children.back();
    }
    if (owner.hasValue()) {
      // Imported member resolution may grow the nominal store.
      const auto nominal = sem_ir_.nominalType(owner);
      sem_ir_.addMemberAccessContext(occurrence_node, owner,
                                     SemMemberAccessKind::Associated);
      const auto member_name = nameFor(occurrence_node);
      for (const auto &member : nominal.member_functions)
        if (member.name == member_name &&
            (member.flags & SemNominalMemberFunctionAssociated) != 0)
          free_candidates.push_back(member.target);
      if (free_candidates.empty() && nominal.canonical_entity.hasValue()) {
        for (const auto candidate : resolveImportedMemberFunctions(
                 nominal.canonical_entity, sem_ir_.name(member_name).text,
                 occurrence_node)) {
          const auto *entity = sem_ir_.importIRs().tryGetEntity(
              sem_ir_.functionRef(candidate).public_entity);
          if (entity && entity->member_kind ==
                            PublicFunctionArtifact::MemberKind::Associated)
            free_candidates.push_back(candidate);
        }
      }
      if (!free_candidates.empty())
        function_ref = free_candidates.front();
      if (!function_ref.hasValue()) {
        emit(DiagnosticKind::InvalidMember, occurrence_node);
        return appendInvalid(block, node);
      }
      if (owner_type.hasValue()) {
        const auto &semantic_owner = sem_ir_.type(owner_type);
        for (const auto argument :
             sem_ir_.typeBlock(TypeBlockId(semantic_owner.arg1)))
          explicit_member_arguments.push_back(sem_ir_.canonicalType(argument));
      }
    } else {
      free_candidates = resolveImportedFunctions(callee_node);
      if (free_candidates.empty())
        return appendInvalid(block, node);
      function_ref = free_candidates.front();
    }
  }
