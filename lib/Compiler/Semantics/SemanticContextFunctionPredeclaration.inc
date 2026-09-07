// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
void SemanticContext::predeclareFunction(
    NodeId node, NominalTypeId semantic_owner,
    SemCanonicalFunctionRole semantic_role, bool force_public,
    GenericId impl_generic, TypeId semantic_owner_type,
    NameId semantic_projector, bool ordinary_method) {
  if (tree_.kind(node) != NodeKind::FunctionDecl)
    return;
  const auto children = tree_.children(node);
  const auto node_flags = tree_.get(node).flags;
  const auto async_modifier = functionChild(node, NodeKind::AsyncModifier);
  const auto is_async = async_modifier.hasValue();
  if (is_async)
    (void)requireVersion(FrozenV11LanguageVersion,
                         DiagnosticKind::AsyncRequires11, async_modifier);
  const auto is_foreign = hasNodeFlag(node_flags, NodeFlags::IsForeign);
  const auto is_bodyless = hasNodeFlag(node_flags, NodeFlags::IsBodyless);
  const auto is_unsafe = hasNodeFlag(node_flags, NodeFlags::IsUnsafe);
  const auto is_const = hasNodeFlag(node_flags, NodeFlags::IsConst);
  const std::size_t base = (is_async ? 1U : 0U) + (is_foreign ? 1U : 0U);
  if (children.size() < base + 4 ||
      tree_.kind(children[base]) != NodeKind::Name ||
      tree_.kind(children[base + 1]) != NodeKind::GenericParameterList ||
      tree_.kind(children[base + 2]) != NodeKind::ParameterList) {
    emit(DiagnosticKind::InvalidSemanticShape, node);
    return;
  }
  const auto function_name = nameFor(children[base]);
  const auto intrinsic_role =
      compilerIntrinsicRole(function_name, semantic_owner);
  const auto existing_name = function_names_.find(function_name.index);
  if (!ordinary_method && semantic_role == SemCanonicalFunctionRole::None &&
      module_constant_names_.contains(function_name.index))
    emit(DiagnosticKind::DuplicateName, children[base]);
  FunctionId existing_function;
  const auto generic_nodes = tree_.children(children[base + 1]);
  if (!impl_generic.hasValue())
    generic_bindings_.clear();
  current_generic_ = GenericId::invalid();
  current_impl_owner_ = semantic_owner;
  current_impl_owner_type_ = semantic_owner_type;
  if (!generic_nodes.empty()) {
    if (semantic_owner.hasValue() && !impl_generic.hasValue())
      emit(semantic_role == SemCanonicalFunctionRole::Pack ||
                   semantic_role == SemCanonicalFunctionRole::Init
               ? DiagnosticKind::InvalidRepresentationImpl
               : DiagnosticKind::InvalidLifecycleImpl,
           children[base + 1]);
    current_generic_ = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(),
        sem_ir_.name(function_name).text,
        static_cast<std::uint32_t>(generic_nodes.size()));
    for (std::uint32_t index = 0; index < generic_nodes.size(); ++index) {
      const auto name = tokenString(generic_nodes[index]);
      if (!name.hasValue() ||
          !generic_bindings_.emplace(name.index, index).second)
        emit(DiagnosticKind::DuplicateName, generic_nodes[index]);
    }
  } else if (impl_generic.hasValue()) {
    current_generic_ = impl_generic;
    const auto &generic = values_.generics().generic(current_generic_);
    (void)generic;
  }
  auto raw_parameter_nodes = tree_.children(children[base + 2]);
  const auto is_variadic =
      !raw_parameter_nodes.empty() &&
      tree_.kind(raw_parameter_nodes.back()) == NodeKind::VariadicParameter;
  const auto parameters = functionParameters(children[base + 2]);
  const auto parameter_nodes = flattenedFunctionParameters(parameters);
  const auto previous_constraint_list = std::exchange(
      active_constraint_list_, functionChild(node, NodeKind::ConstraintList));
  std::vector<TypeId> parameter_types;
  for (const auto &parameter : parameters) {
    const auto parameter_type = checkType(parameter.type);
    parameter_types.push_back(parameter_type);
    const auto dependent_projection =
        current_generic_.hasValue() &&
        sem_ir_.type(parameter_type).kind == SemTypeKind::TypeProjection;
    if ((!sem_ir_.isCallAbiType(parameter_type) ||
         !hasObjectRepresentation(parameter_type)) &&
        !dependent_projection &&
        semantic_role != SemCanonicalFunctionRole::Pack &&
        semantic_role != SemCanonicalFunctionRole::Init)
      emit(DiagnosticKind::UnsupportedAbiType, parameter.type);
  }
  const auto return_type = checkType(children[base + 3]);
  active_constraint_list_ = previous_constraint_list;
  const auto async_result_shape =
      is_async ? sem_ir_.canonicalResultShape(return_type) : std::nullopt;
  const auto async_success_type =
      async_result_shape ? async_result_shape->success : return_type;
  const auto async_error_type =
      async_result_shape ? std::optional<TypeId>(async_result_shape->error)
                         : std::nullopt;
  const auto invalid_async_form =
      is_async && (is_foreign || is_const || is_unsafe || ordinary_method ||
                   semantic_owner.hasValue() || current_generic_.hasValue());
  if (invalid_async_form)
    emit(DiagnosticKind::InvalidAsyncDeclaration, async_modifier);
  const auto invalid_async_outcome =
      is_async &&
      (sem_ir_.type(async_success_type).kind == SemTypeKind::Reference ||
       (async_error_type &&
        sem_ir_.type(*async_error_type).kind == SemTypeKind::Reference));
  if (invalid_async_outcome)
    emit(DiagnosticKind::InvalidAsyncDeclaration, children[base + 3]);
  if (semantic_role == SemCanonicalFunctionRole::Constructor) {
    const auto owner_matches = semantic_owner_type.hasValue() &&
                               sem_ir_.canonicalType(return_type) ==
                                   sem_ir_.canonicalType(semantic_owner_type);
    const auto result_shape = sem_ir_.canonicalResultShape(return_type);
    const auto result_matches = result_shape.has_value() &&
                                semantic_owner_type.hasValue() &&
                                sem_ir_.canonicalType(result_shape->success) ==
                                    sem_ir_.canonicalType(semantic_owner_type);
    if (result_matches)
      (void)ensureResultOutcomeType(return_type);
    if (!owner_matches && !result_matches)
      emit(DiagnosticKind::InvalidConstructor, children[base + 3]);
  }
  const auto dependent_success_projection =
      current_generic_.hasValue() &&
      sem_ir_.type(async_success_type).kind == SemTypeKind::TypeProjection;
  const auto dependent_error_projection =
      async_error_type && current_generic_.hasValue() &&
      sem_ir_.type(*async_error_type).kind == SemTypeKind::TypeProjection;
  if (((!sem_ir_.isCallAbiType(async_success_type) &&
        !dependent_success_projection) ||
       (async_error_type && !sem_ir_.isCallAbiType(*async_error_type) &&
        !dependent_error_projection)) &&
      semantic_role != SemCanonicalFunctionRole::Pack &&
      semantic_role != SemCanonicalFunctionRole::Init)
    emit(DiagnosticKind::UnsupportedAbiType, children[base + 3]);
  const auto function_text =
      values_.identifier(sem_ir_.name(function_name).text);
  if (intrinsic_role != CompilerIntrinsicRole::None && !is_bodyless)
    emit(DiagnosticKind::InvalidCallableContract, node);
  const auto invalid_async_entry =
      is_async && isV1SourceEntryName(function_text) &&
      (!parameter_types.empty() || return_type != sem_ir_.i32Type());
  if (isV1SourceEntryName(function_text) &&
      (!parameter_types.empty() || return_type != sem_ir_.i32Type()))
    emit(is_async ? DiagnosticKind::InvalidAsyncEntryPoint
                  : DiagnosticKind::UnsupportedAbiType,
         parameter_types.empty() ? children[base + 3] : children[base + 2]);
  if (isV1SourceEntryName(function_text) && current_generic_.hasValue())
    emit(DiagnosticKind::GenericEntryPoint, children[base + 1]);
  const auto parameter_types_id = sem_ir_.addTypeBlock(parameter_types);
  const auto semantic_async = is_async && !invalid_async_form &&
                              !invalid_async_outcome && !invalid_async_entry;
  auto function_type =
      semantic_async
          ? sem_ir_.addAsyncFunctionType(parameter_types, async_success_type,
                                         async_error_type)
          : sem_ir_.addType({SemTypeKind::Function, parameter_types_id.index,
                             return_type.index, 0});
  const auto matches_source_overload = [&](FunctionId candidate) {
    const auto &candidate_function = sem_ir_.function(candidate);
    if (candidate_function.name != function_name ||
        candidate_function.semantic_owner != semantic_owner ||
        candidate_function.semantic_role !=
            static_cast<std::uint8_t>(semantic_role))
      return false;
    const auto candidate_generic_count =
        candidate_function.generic.hasValue()
            ? values_.generics()
                  .generic(candidate_function.generic)
                  .binding_count
            : 0U;
    if (candidate_generic_count != generic_nodes.size())
      return false;
    const auto &candidate_type = sem_ir_.type(candidate_function.type);
    if (!sameSourceParameterPattern(
            sem_ir_.typeBlock(TypeBlockId(candidate_type.arg0)),
            parameter_types))
      return false;
    if (!ordinary_method)
      return true;
    const auto &candidate_declaration = sem_ir_.functionDeclaration(candidate);
    const auto candidate_has_receiver =
        !candidate_declaration.parameter_names.empty() &&
        values_.identifier(candidate_declaration.parameter_names.front()) ==
            "self";
    const auto has_receiver =
        !parameters.empty() &&
        values_.identifier(tokenString(parameters.front().name)) == "self";
    return candidate_has_receiver == has_receiver;
  };
  if (semantic_role == SemCanonicalFunctionRole::None) {
    if (!ordinary_method && existing_name != function_names_.end()) {
      for (const auto candidate_ref : existing_name->second) {
        const auto candidate =
            sem_ir_.functionRef(candidate_ref).local_function;
        if (candidate.hasValue() && matches_source_overload(candidate)) {
          existing_function = candidate;
          break;
        }
      }
    } else if (ordinary_method) {
      for (std::uint32_t index = 0; index < sem_ir_.functionCount(); ++index) {
        const auto candidate = FunctionId(index);
        if (matches_source_overload(candidate)) {
          existing_function = candidate;
          break;
        }
      }
    }
  }
  if ((function_text == "main" || is_foreign) &&
      existing_name != function_names_.end() && !existing_function.hasValue()) {
    emit(DiagnosticKind::DuplicateName, children[base]);
    current_impl_owner_ = NominalTypeId::invalid();
    current_impl_owner_type_ = TypeId::invalid();
    return;
  }
  const auto signature_matches =
      !existing_function.hasValue() ||
      sem_ir_.function(existing_function).type == function_type;
  if (existing_function.hasValue() && signature_matches) {
    function_type = sem_ir_.function(existing_function).type;
    current_generic_ = sem_ir_.function(existing_function).generic;
  }
  auto contract =
      declaredContract(node, parameter_nodes, parameter_types, return_type);
  const auto has_contract =
      functionChild(node, NodeKind::CallableContract).hasValue();
  // Contracts describe declaration-side facts for bodyless callables. Keep
  // this invariant explicit for alternate front ends that may construct a
  // tree containing both a block and a contract node.
  if (has_contract && functionChild(node, NodeKind::Block).hasValue())
    emit(DiagnosticKind::InvalidCallableContract, node);
  IdentifierId foreign_abi;
  std::optional<ForeignAbiSignature> foreign_signature;
  if (is_const &&
      (is_foreign || is_unsafe ||
       (is_bodyless && intrinsic_role != CompilerIntrinsicRole::WrappingMul)))
    emit(DiagnosticKind::InvalidConstantFunction, node);
  if (is_foreign) {
    const auto spelling = tree_.tokens().text(tree_.token(children.front()));
    if (spelling == "\"C\"")
      foreign_abi = values_.internIdentifier("C");
    else
      emit(DiagnosticKind::InvalidCallableContract, children.front());
    const auto classify_foreign_type =
        [&](TypeId type, bool result) -> std::optional<ForeignAbiValue> {
      const auto representation = sem_ir_.foreignRepresentationType(type);
      if (representation.hasValue())
        type = representation;
      const auto &value = sem_ir_.type(type);
      ForeignAbiValue classified;
      if (value.kind == SemTypeKind::Void) {
        if (!result)
          return std::nullopt;
        classified.kind = ForeignAbiValueKind::Void;
      } else if (value.kind == SemTypeKind::Bool) {
        classified.kind = ForeignAbiValueKind::Bool;
        classified.width = 1;
      } else if (value.kind == SemTypeKind::Char) {
        classified.kind = ForeignAbiValueKind::UnsignedInteger;
        classified.width = 32;
      } else if (value.kind == SemTypeKind::Integer) {
        classified.kind = value.arg1 != 0
                              ? ForeignAbiValueKind::SignedInteger
                              : ForeignAbiValueKind::UnsignedInteger;
        classified.width = value.arg0;
      } else if (value.kind == SemTypeKind::Float) {
        classified.kind = ForeignAbiValueKind::Float;
        classified.width = value.arg0;
      } else if (value.kind == SemTypeKind::RawPointer) {
        classified.kind = ForeignAbiValueKind::RawPointer;
        classified.pointee_const = sem_ir_.rawPointerPointeeConst(type);
      } else if (value.kind == SemTypeKind::CFunctionPointer ||
                 value.kind == SemTypeKind::CVariadicFunctionPointer) {
        classified.kind = ForeignAbiValueKind::FunctionPointer;
      } else if (value.kind == SemTypeKind::Reference) {
        classified.kind = ForeignAbiValueKind::Reference;
        classified.pointee_const = sem_ir_.referenceMutability(type) ==
                                   SemReferenceMutability::ReadOnly;
      } else if (value.kind == SemTypeKind::Nominal &&
                 hasCRepresentation(type)) {
        classified.kind = ForeignAbiValueKind::Aggregate;
      } else {
        return std::nullopt;
      }
      return classified;
    };
    if (auto result = classify_foreign_type(return_type, true)) {
      ForeignAbiSignature signature{.result = *result,
                                    .is_variadic = is_variadic};
      for (const auto parameter : parameter_types) {
        auto classified = classify_foreign_type(parameter, false);
        if (!classified) {
          signature.parameters.clear();
          result.reset();
          break;
        }
        signature.parameters.push_back(*classified);
      }
      if (result)
        foreign_signature = std::move(signature);
    }
    if (!is_bodyless || !is_unsafe || !generic_nodes.empty() ||
        !foreign_signature)
      emit(DiagnosticKind::InvalidCallableContract, node);
    if (is_variadic && parameter_types.empty())
      emit(DiagnosticKind::InvalidVariadicDeclaration, children[base + 2]);
  } else if (is_unsafe && intrinsic_role == CompilerIntrinsicRole::None) {
    emit(DiagnosticKind::InvalidCallableContract, node);
  } else if (is_variadic) {
    emit(DiagnosticKind::InvalidVariadicDeclaration, children[base + 2]);
  }
  const auto new_kind = is_foreign    ? SemCallableDeclarationKind::Foreign
                        : is_bodyless ? SemCallableDeclarationKind::Forward
                                      : SemCallableDeclarationKind::Definition;
  std::vector<IdentifierId> parameter_names;
  std::vector<ConstantEntityId> default_arguments(parameters.size());
  bool saw_default = false;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    parameter_names.push_back(tokenString(parameters[index].name));
    if (!parameters[index].default_value.hasValue()) {
      if (saw_default)
        emit(DiagnosticKind::InvalidDefaultArgument, parameters[index].name);
      continue;
    }
    saw_default = true;
    if (is_foreign || is_variadic) {
      emit(DiagnosticKind::InvalidDefaultArgument,
           parameters[index].default_value);
      continue;
    }
    std::vector<InstId> initializer;
    auto value = checkExpression(parameters[index].default_value, initializer,
                                 parameter_types[index]);
    if (!adjustExpression(value, parameter_types[index],
                          parameters[index].default_value, initializer)) {
      emit(DiagnosticKind::TypeMismatch, parameters[index].default_value);
      continue;
    }
    ConstantEvaluator evaluator(sem_ir_);
    const auto outcome = evaluator.evaluateExpression(value);
    if (!outcome.result.isConcrete()) {
      emit(DiagnosticKind::InvalidDefaultArgument,
           parameters[index].default_value);
      continue;
    }
    const auto default_name = sem_ir_.addName(
        values_.internIdentifier("$default$" + std::string(function_text) +
                                 "$" + std::to_string(index)));
    default_arguments[index] = sem_ir_.addConstantEntity(
        {default_name, parameter_types[index],
         sem_ir_.addInstBlock(initializer), value,
         parameters[index].default_value, 0, outcome.result});
  }
  if (existing_function.hasValue()) {
    auto declaration = sem_ir_.functionDeclaration(existing_function);
    const auto &existing = sem_ir_.function(existing_function);
    const auto supplies_defaults =
        std::ranges::any_of(default_arguments, [](ConstantEntityId value) {
          return value.hasValue();
        });
    bool defaults_match = !supplies_defaults;
    if (supplies_defaults &&
        declaration.default_arguments.size() == default_arguments.size()) {
      defaults_match = true;
      for (std::size_t index = 0; index < default_arguments.size(); ++index) {
        if (!default_arguments[index].hasValue() ||
            !declaration.default_arguments[index].hasValue() ||
            sem_ir_.constantEntity(default_arguments[index]).result.state !=
                sem_ir_.constantEntity(declaration.default_arguments[index])
                    .result.state ||
            sem_ir_.constantEntity(default_arguments[index]).result.value !=
                sem_ir_.constantEntity(declaration.default_arguments[index])
                    .result.value) {
          defaults_match = false;
          break;
        }
      }
    }
    if (!signature_matches || existing.type != function_type ||
        declaration.parameter_names != parameter_names || !defaults_match ||
        (declaration.kind == SemCallableDeclarationKind::Foreign) !=
            is_foreign ||
        (declaration.foreign_abi.hasValue() &&
         declaration.foreign_abi != foreign_abi) ||
        (declaration.foreign_signature &&
         declaration.foreign_signature != foreign_signature) ||
        (declaration.declared_contract.has_value() != contract.has_value()) ||
        (declaration.declared_contract && contract &&
         *declaration.declared_contract != *contract) ||
        declaration.is_const != is_const ||
        (declaration.kind == SemCallableDeclarationKind::Definition &&
         new_kind == SemCallableDeclarationKind::Definition)) {
      emit(DiagnosticKind::DuplicateName, children[base]);
      return;
    }
    if (contract)
      declaration.declared_contract = std::move(contract);
    if (foreign_signature)
      declaration.foreign_signature = std::move(foreign_signature);
    if (new_kind == SemCallableDeclarationKind::Definition)
      declaration.kind = new_kind;
    sem_ir_.setFunctionDeclaration(existing_function, std::move(declaration));
    if (hasNodeFlag(node_flags, NodeFlags::IsPublic) &&
        (existing.flags & SemFunctionPublic) == 0) {
      auto merged = existing;
      merged.flags |= SemFunctionPublic;
      sem_ir_.setFunction(existing_function, std::move(merged));
    }
    sem_ir_.addFunctionOccurrence(children[base],
                                  SemSymbolOccurrenceKind::Declaration,
                                  functionReference(existing_function));
    function_nodes_.emplace(node.index, existing_function);
    function_declarations_.push_back(node);
    recordFunctionConstraints(existing_function, node);
    captureFunctionGenericEnvironment(existing_function);
    current_impl_owner_ = NominalTypeId::invalid();
    current_impl_owner_type_ = TypeId::invalid();
    return;
  }
  const std::vector<LocalId> empty_parameters;
  const std::vector<InstId> empty_body;
  const auto parameters_id = sem_ir_.addLocalBlock(empty_parameters);
  const auto body_id = sem_ir_.addInstBlock(empty_body);
  auto flags =
      (force_public || hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic))
          ? SemFunctionPublic
          : 0U;
  if (current_generic_.hasValue())
    flags |= SemFunctionTemplate;
  // An invalid async/const combination must not enter the constant
  // evaluator, which intentionally only accepts ordinary function types.
  if (is_const && !is_async)
    flags |= SemFunctionConst;
  if (semantic_async)
    flags |= SemFunctionAsync;
  if (semantic_async && function_text == "main")
    flags |= SemFunctionCoroutineExecutionEntry;
  auto function_record = SemFunction{
      function_name,
      function_type,
      parameters_id,
      body_id,
      flags,
      current_generic_,
      current_generic_.hasValue()
          ? values_.generics().generic(current_generic_).self_specific
          : SpecificId::invalid(),
      semantic_owner,
      static_cast<std::uint8_t>(semantic_role),
      semantic_projector};
  function_record.intrinsic_role = intrinsic_role;
  const auto function = sem_ir_.addFunction(std::move(function_record));
  sem_ir_.setFunctionDeclaration(
      function, {.kind = new_kind,
                 .is_unsafe = is_unsafe &&
                              (is_foreign ||
                               intrinsic_role != CompilerIntrinsicRole::None),
                 .is_const = is_const && !is_async,
                 .foreign_abi = foreign_abi,
                 .foreign_signature = std::move(foreign_signature),
                 .declared_contract = std::move(contract),
                 .parameter_names = std::move(parameter_names),
                 .default_arguments = std::move(default_arguments)});
  recordFunctionConstraints(function, node);
  captureFunctionGenericEnvironment(function);
  const auto function_ref = sem_ir_.addFunctionRef(
      {function, ImportIRInstId::invalid(), function_type,
       PublicEntityId::invalid(), current_generic_,
       current_generic_.hasValue()
           ? values_.generics().generic(current_generic_).self_specific
           : SpecificId::invalid()});
  if (current_generic_.hasValue())
    generic_template_refs_.insert_or_assign(current_generic_.index,
                                            function_ref);
  sem_ir_.addFunctionOccurrence(
      children[base], SemSymbolOccurrenceKind::Declaration, function_ref);
  function_nodes_.emplace(node.index, function);
  function_declarations_.push_back(node);
  if (semantic_role == SemCanonicalFunctionRole::None && !ordinary_method) {
    function_names_[function_name.index].push_back(function_ref);
  }
  current_impl_owner_ = NominalTypeId::invalid();
  current_impl_owner_type_ = TypeId::invalid();
}
