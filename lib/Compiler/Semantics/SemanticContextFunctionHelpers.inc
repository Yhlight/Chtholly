// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
[[nodiscard]] bool
SemanticContext::sameSourceParameterPattern(std::span<const TypeId> lhs,
                                            std::span<const TypeId> rhs) const {
  return lhs.size() == rhs.size() &&
         std::ranges::equal(lhs, rhs, [&](TypeId left, TypeId right) {
           return sameSourceTypePattern(sem_ir_.canonicalType(left),
                                        sem_ir_.canonicalType(right));
         });
}

[[nodiscard]] CompilerIntrinsicRole
SemanticContext::compilerIntrinsicRole(NameId function_name,
                                       NominalTypeId semantic_owner) const {
  std::string entity_name;
  if (semantic_owner.hasValue()) {
    const auto &owner = sem_ir_.nominalType(semantic_owner);
    entity_name =
        std::string(sem_ir_.identifier(sem_ir_.name(owner.name).text));
    entity_name += '.';
  }
  entity_name += sem_ir_.identifier(sem_ir_.name(function_name).text);
  const auto module_name = sem_ir_.identifier(sem_ir_.moduleName());
  const auto binding =
      std::ranges::find_if(compiler_intrinsics_, [&](const auto &item) {
        return item.module_name == module_name &&
               item.entity_name == entity_name;
      });
  return binding == compiler_intrinsics_.end() ? CompilerIntrinsicRole::None
                                               : binding->role;
}

[[nodiscard]] std::optional<std::uint8_t>
SemanticContext::memoryOrder(InstId argument) {
  ConstantEvaluator evaluator(sem_ir_);
  const auto outcome = evaluator.evaluateExpression(argument);
  if (!outcome.result.isConcrete())
    return std::nullopt;
  const auto &constant = sem_ir_.constantValue(outcome.result.value);
  if (constant.kind != ConstantValueKind::Enum ||
      constant.payload >= values_.integerCount())
    return std::nullopt;
  const auto value =
      values_.integer(IntegerId(static_cast<std::uint32_t>(constant.payload)));
  if (value < 0 || value > 4)
    return std::nullopt;
  return static_cast<std::uint8_t>(value);
}

[[nodiscard]] const interop::ForeignOperationArtifact *
SemanticContext::foreignOperationArtifact(FunctionRefId function_ref) const {
  const auto &reference = sem_ir_.functionRef(function_ref);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir_.functionDeclaration(reference.local_function);
    if (declaration.interop_artifact)
      return sem_ir_.importIRs().interopRegistry().resolve(
          *declaration.interop_artifact);
  }
  if (reference.public_entity.hasValue()) {
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(reference.public_entity);
    if (entity && entity->interop_artifact)
      return sem_ir_.importIRs().interopRegistry().resolve(
          *entity->interop_artifact);
  }
  return nullptr;
}

[[nodiscard]] bool
SemanticContext::hasInitializationContract(FunctionRefId function_ref,
                                           std::size_t parameter) const {
  const auto *summary = callableOwnershipSummary(sem_ir_, function_ref);
  // Call checking can run before the interprocedural ownership fixed point.
  // Bodyless source declarations already carry their canonical contract, so
  // consult it directly instead of treating the not-yet-populated summary as
  // a missing Initialize capability.
  CallableOwnershipSummary declared_summary;
  if (function_ref.hasValue()) {
    const auto &reference = sem_ir_.functionRef(function_ref);
    if (reference.local_function.hasValue()) {
      const auto &declaration =
          sem_ir_.functionDeclaration(reference.local_function);
      if (declaration.declared_contract) {
        declared_summary = *declaration.declared_contract;
        summary = &declared_summary;
      }
    }
  }
  if (!summary)
    return false;
  const OwnershipRegion root{.parameter_index =
                                 static_cast<std::uint32_t>(parameter)};
  return std::ranges::any_of(summary->effects,
                             [&](const auto &effect) {
                               return effect.kind ==
                                          CallableEffectKind::Initialize &&
                                      effect.region == root;
                             }) &&
         std::ranges::any_of(
             summary->postconditions, [&](const auto &postcondition) {
               return postcondition.region == root &&
                      (postcondition.outcomes & CallableOutcomeInitialize) != 0;
             });
}

[[nodiscard]] bool
SemanticContext::isMutableReferenceParameter(LocalId local,
                                             TypeId expected) const {
  if (!current_function_.hasValue() ||
      sem_ir_.type(expected).kind != SemTypeKind::Reference ||
      sem_ir_.referenceMutability(expected) != SemReferenceMutability::Mutable)
    return false;
  return sem_ir_.type(sem_ir_.local(local).type).kind ==
             SemTypeKind::Reference &&
         sem_ir_.referenceMutability(sem_ir_.local(local).type) ==
             SemReferenceMutability::Mutable &&
         sem_ir_.referencePointee(sem_ir_.local(local).type) ==
             sem_ir_.referencePointee(expected);
}

[[nodiscard]] bool
SemanticContext::mayDeclareInitializationParameter(FunctionRefId function_ref,
                                                   TypeId expected) const {
  if (sem_ir_.type(expected).kind != SemTypeKind::Reference ||
      sem_ir_.referenceMutability(expected) != SemReferenceMutability::Mutable)
    return false;
  const auto &reference = sem_ir_.functionRef(function_ref);
  if (!reference.local_function.hasValue())
    return false;
  const auto function = reference.local_function;
  const auto &declaration = sem_ir_.functionDeclaration(function);
  return declaration.kind == SemCallableDeclarationKind::Definition &&
         (sem_ir_.function(function).flags & SemFunctionEvaluatorArtifact) == 0;
}

[[nodiscard]] bool SemanticContext::prepareInitializationArgument(
    FunctionRefId function_ref, std::size_t parameter, InstId &argument,
    TypeId expected, NodeId node, std::vector<InstId> &block) {
  if (!hasInitializationContract(function_ref, parameter) &&
      !(sem_ir_.type(expected).kind == SemTypeKind::Reference &&
        sem_ir_.referenceMutability(expected) ==
            SemReferenceMutability::Mutable &&
        mayDeclareInitializationParameter(function_ref, expected)))
    return false;
  const bool requires_initialization = hasInitializationContract(function_ref, parameter);
  const auto value = sem_ir_.inst(argument);
  if (value.kind != SemInstKind::NameRef) {
    if (!requires_initialization) return false;
    emit(DiagnosticKind::InvalidAssignmentTarget, node);
    argument = appendInvalid(block, node);
    return true;
  }
  const auto local = LocalId(value.arg0);
  if (isMutableReferenceParameter(local, expected))
    return true;
  if (!requires_initialization && (sem_ir_.local(local).flags & SemLocalUninitialized) == 0)
    return false;
  if ((sem_ir_.local(local).flags & SemLocalUninitialized) == 0 ||
      sem_ir_.type(expected).kind != SemTypeKind::Reference ||
      sem_ir_.referenceMutability(expected) !=
          SemReferenceMutability::Mutable ||
      sem_ir_.referencePointee(expected) != sem_ir_.local(local).type) {
    emit(DiagnosticKind::InvalidAssignmentTarget, node);
    argument = appendInvalid(block, node);
    return true;
  }
  sem_ir_.replaceInst(argument, SemInitializePlace{expected, local});
  return true;
}

[[nodiscard]] bool SemanticContext::isInitializationArgument(
    FunctionRefId function_ref, std::size_t parameter, InstId argument,
    TypeId expected) const {
  if (!hasInitializationContract(function_ref, parameter) &&
      !(sem_ir_.type(expected).kind == SemTypeKind::Reference &&
        sem_ir_.referenceMutability(expected) ==
            SemReferenceMutability::Mutable &&
        mayDeclareInitializationParameter(function_ref, expected)))
    return false;
  const auto value = sem_ir_.inst(argument);
  if (value.kind != SemInstKind::NameRef)
    return false;
  const auto local = LocalId(value.arg0);
  if (isMutableReferenceParameter(local, expected))
    return true;
  return (sem_ir_.local(local).flags & SemLocalUninitialized) != 0 &&
         sem_ir_.type(expected).kind == SemTypeKind::Reference &&
         sem_ir_.referenceMutability(expected) ==
             SemReferenceMutability::Mutable &&
         sem_ir_.referencePointee(expected) == sem_ir_.local(local).type;
}

[[nodiscard]] InstId SemanticContext::appendResolvedCall(
    std::vector<InstId> &block, NodeId node, FunctionRefId function_ref,
    TypeId result_type, std::span<const InstId> arguments) {
  const auto role = sem_ir_.functionIntrinsicRole(function_ref);
  if (role == CompilerIntrinsicRole::None) {
    const auto *artifact = foreignOperationArtifact(function_ref);
    if (artifact) {
      if (artifact->error_extractor !=
          interop::ForeignOperationArtifact::ErrorExtractor::None) {
        const auto projected =
            foreignErrorResultTypeShape(result_type, *artifact, node);
        if (!projected)
          return appendInvalid(block, node);
        result_type = projected->type;
      }
      return appendInst<SemForeignOperationCall>(
          block, node, result_type, function_ref,
          sem_ir_.addInstBlock(arguments));
    }
    return appendInst<SemCall>(block, node, result_type, function_ref,
                               sem_ir_.addInstBlock(arguments));
  }
  if ((isAtomicCompilerIntrinsic(role) || isVolatileCompilerIntrinsic(role)) &&
      !requireVersion(FrozenV11LanguageVersion,
                      DiagnosticKind::ConcurrencyRequires11, node))
    return appendInvalid(block, node);

  // Channel transitions are represented explicitly in SemIR so ownership
  // analysis can distinguish reservation from the destructive commit.  The
  // intrinsic call itself is therefore not allowed to leak into the generic
  // call path for these roles.
  if (isChannelTransitionCompilerIntrinsic(role)) {
    const auto expected = compilerIntrinsicParameterCount(role);
    if (arguments.size() != expected || result_type != sem_ir_.voidType()) {
      emit(DiagnosticKind::InvalidCompilerIntrinsic, node);
      return appendInvalid(block, node);
    }
    switch (role) {
    case CompilerIntrinsicRole::ChannelSendPrepare:
      return appendInst<SemTypedChannelSendPrepare>(
          block, node, sem_ir_.voidType(), arguments[0], arguments[1]);
    case CompilerIntrinsicRole::ChannelSendCommit:
      return appendInst<SemTypedChannelSendCommit>(
          block, node, sem_ir_.voidType(), arguments[0], arguments[1]);
    case CompilerIntrinsicRole::ChannelSendCancel:
      return appendInst<SemTypedChannelSendCancel>(
          block, node, sem_ir_.voidType(), arguments[0]);
    case CompilerIntrinsicRole::ChannelReceiveAcquire:
      return appendInst<SemTypedChannelReceiveAcquire>(
          block, node, sem_ir_.voidType(), arguments[0]);
    case CompilerIntrinsicRole::ChannelReceiveCommit:
      return appendInst<SemTypedChannelReceiveCommit>(
          block, node, sem_ir_.voidType(), arguments[0], arguments[1]);
    case CompilerIntrinsicRole::ChannelReceiveCancel:
      return appendInst<SemTypedChannelReceiveCancel>(
          block, node, sem_ir_.voidType(), arguments[0]);
    case CompilerIntrinsicRole::ChannelClose:
      return appendInst<SemTypedChannelClose>(
          block, node, sem_ir_.voidType(), arguments[0]);
    default:
      break;
    }
  }

  // Intrinsic lowering is the final semantic boundary at which an
  // imported generic target may still be represented by a local evaluator
  // specific. Canonicalize it here, after overload resolution has finished,
  // and validate the exact concrete signature before emitting the intrinsic
  // instruction consumed by control-flow, ownership, and cleanup passes.
  const auto &reference = sem_ir_.functionRef(function_ref);
  if (reference.public_entity.hasValue()) {
    bool symbolic = false;
    if (reference.specific.hasValue())
      symbolic = std::ranges::any_of(
          values_.generics().specific(reference.specific).arguments,
          [&](CanonicalConstantId argument) {
            const auto &constant = values_.generics().constant(argument);
            const auto kind = values_.generics().type(constant.type).kind;
            return constant.dependence != ConstantDependence::Concrete ||
                   kind == CanonicalTypeKind::TypeParameter ||
                   kind == CanonicalTypeKind::TypeProjection;
          });
    if (!symbolic) {
      std::string resolution_error;
      const auto resolved =
          sem_ir_.resolveCanonicalIntrinsic(function_ref, resolution_error);
      if (!resolved) {
        emit(DiagnosticKind::InvalidCompilerIntrinsic, node);
        return appendInvalid(block, node);
      }
      function_ref = *resolved;
    }
  }

  const auto atomic = isAtomicCompilerIntrinsic(role);
  TypeId scalar_type;
  if (role == CompilerIntrinsicRole::AtomicInit && !arguments.empty())
    scalar_type = instType(arguments[0]);
  else if (role == CompilerIntrinsicRole::AtomicLoad)
    scalar_type = result_type;
  else if (atomic && arguments.size() >= 2)
    scalar_type = instType(arguments[1]);
  else if (role == CompilerIntrinsicRole::VolatileLoad)
    scalar_type = result_type;
  else if (role == CompilerIntrinsicRole::VolatileStore &&
           arguments.size() >= 2)
    scalar_type = instType(arguments[1]);
  else if (role == CompilerIntrinsicRole::WrappingMul &&
           !arguments.empty())
    scalar_type = instType(arguments[0]);
  else if ((role == CompilerIntrinsicRole::FloatHash ||
            role == CompilerIntrinsicRole::FloatEqual) &&
           !arguments.empty())
    scalar_type = instType(arguments[0]);
  else if ((role == CompilerIntrinsicRole::PointerHash ||
            role == CompilerIntrinsicRole::PointerEqual) &&
           !arguments.empty())
    scalar_type = instType(arguments[0]);

  const auto scalar_kind = scalar_type.hasValue()
                               ? sem_ir_.type(scalar_type).kind
                               : SemTypeKind::Invalid;
  const auto dependent_scalar = scalar_kind == SemTypeKind::TypeParameter ||
                                scalar_kind == SemTypeKind::TypeProjection;
  const auto integer_only = compilerIntrinsicRequiresInteger(role);
  if (!dependent_scalar &&
      ((integer_only && scalar_kind != SemTypeKind::Integer) ||
       (!integer_only && atomic && scalar_kind != SemTypeKind::Integer &&
        scalar_kind != SemTypeKind::Bool))) {
    emit(DiagnosticKind::UnsupportedAtomicType, node);
    return appendInvalid(block, node);
  }

  const auto require_order =
      [&](std::uint8_t ordering) -> std::optional<std::uint8_t> {
    const auto index = compilerIntrinsicOrderingParameter(role, ordering);
    if (index >= arguments.size())
      return std::nullopt;
    const auto order = memoryOrder(arguments[index]);
    if (!order || !isValidCompilerIntrinsicOrdering(role, ordering, *order))
      return std::nullopt;
    return order;
  };
  bool valid_order = true;
  if (compilerIntrinsicOrderingCount(role) == 1)
    valid_order = require_order(0).has_value();
  else if (compilerIntrinsicOrderingCount(role) == 2) {
    const auto success = require_order(0);
    const auto failure = require_order(1);
    valid_order = success && failure;
    if (valid_order)
      valid_order = isValidCompareExchangeOrderingPair(*success, *failure);
  }
  if (!valid_order) {
    emit(DiagnosticKind::InvalidMemoryOrder, node);
    return appendInvalid(block, node);
  }
  return appendInst<SemCompilerIntrinsicCall>(
      block, node, result_type, function_ref, sem_ir_.addInstBlock(arguments));
}
