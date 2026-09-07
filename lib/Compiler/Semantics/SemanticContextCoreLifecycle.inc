// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
SemanticContext::SemanticContext(
    const ParseTree &tree, core::Arena &arena, SharedValueStores &values,
    DiagnosticEmitter &diagnostics,
    const PublicInterfaceRegistry &public_interfaces,
    const interop::ArtifactRegistry &interop_registry, CheckIRId check_ir_id,
    IdentifierId module_name, std::span<const ImportIR> imports,
    StableFingerprint semantic_options_fingerprint,
    ConcreteSpecializationLoader specialization_loader,
    std::uint32_t pointer_width, std::string_view normalized_target_triple,
    std::span<const CompilerIntrinsicBinding> compiler_intrinsics,
    LanguageVersion language_version)
    : tree_(tree), values_(values), diagnostics_(diagnostics),
      sem_ir_(arena, values, check_ir_id, module_name, public_interfaces,
              interop_registry, imports, language_version),
      conversions_(sem_ir_, pointer_width),
      semantic_options_fingerprint_(semantic_options_fingerprint),
      specialization_loader_(std::move(specialization_loader)),
      pointer_width_(pointer_width), compiler_intrinsics_(compiler_intrinsics) {
  sem_ir_.setTargetLayout({normalized_target_triple.empty()
                               ? "unknown-" + std::to_string(pointer_width)
                               : std::string(normalized_target_triple),
                           pointer_width, NominalLayoutAbiEpoch});
}

SemIR SemanticContext::run() {
  std::vector<InstId> declarations;
  const auto root = NodeId(static_cast<std::uint32_t>(tree_.size() - 1));
  const auto declaration_nodes = tree_.children(root);
  for (const auto declaration : declaration_nodes)
    predeclareTypeAlias(declaration);
  for (const auto declaration : declaration_nodes)
    predeclareInterface(declaration);
  for (const auto declaration : declaration_nodes)
    predeclareNominalType(declaration);
  for (const auto declaration : declaration_nodes)
    completeTypeAlias(declaration);
  for (const auto declaration : declaration_nodes)
    completeInterface(declaration);
  for (const auto declaration : declaration_nodes)
    completeNominalType(declaration);
  validateNominalTypeCycles();
  validateCRepresentationPolicies();
  for (const auto declaration : declaration_nodes)
    predeclareModuleConstant(declaration);
  for (const auto declaration : declaration_nodes)
    if (!function_nodes_.contains(declaration.index))
      predeclareFunction(declaration);
  validateUnboundForeignContracts();
  for (const auto declaration : declaration_nodes)
    predeclareInterfaceImpl(declaration);
  for (const auto declaration : declaration_nodes)
    predeclareCanonicalImpl(declaration);
  validateLifecyclePolicies();
  validateRepresentationCarrierCycles();
  validateObjectRepresentationProjections();
  for (const auto declaration : function_declarations_)
    checkFunctionBody(declaration, declarations, true, true);
  for (const auto declaration : function_declarations_)
    checkFunctionBody(declaration, declarations, false, true);
  for (const auto declaration : declaration_nodes)
    checkModuleConstant(declaration, declarations);
  validateConstants(true);
  for (const auto declaration : function_declarations_)
    checkFunctionBody(declaration, declarations, true, false);
  for (const auto declaration : function_declarations_)
    checkFunctionBody(declaration, declarations, false, false);
  validateConstants(false);
  validateRepresentationInitializers();
  materializeRepresentationSpecifics();
  drainSpecificQueue();
  sem_ir_.setTopBlock(sem_ir_.addInstBlock(declarations));
  for (std::uint32_t index = 0; index < sem_ir_.functionCount(); ++index)
    if (sem_ir_.functionDeclaration(FunctionId(index)).kind ==
            SemCallableDeclarationKind::Forward &&
        sem_ir_.function(FunctionId(index)).intrinsic_role ==
            CompilerIntrinsicRole::None &&
        !sem_ir_.functionDeclaration(FunctionId(index)).declared_contract &&
        (sem_ir_.function(FunctionId(index)).flags &
         (SemFunctionInterfaceMember | SemFunctionEvaluatorArtifact |
          SemFunctionSpecific)) == 0)
      emit(DiagnosticKind::MissingFunctionDefinition, root);
  for (const auto violation : analyzeCarrierViews(sem_ir_))
    emit(violation.kind == CarrierViewViolationKind::Invalid
             ? DiagnosticKind::InvalidCarrierView
         : violation.kind == CarrierViewViolationKind::Escape
             ? DiagnosticKind::CarrierViewEscape
             : DiagnosticKind::CarrierViewRegionViolation,
         sem_ir_.location(violation.instruction));
  for (const auto violation : analyzeCallableOwnership(
           sem_ir_, &sem_ir_.analysisMetrics().callable_ownership)) {
    DiagnosticKind primary = DiagnosticKind::BorrowReturnEscape;
    if (violation.kind == CallableOwnershipViolationKind::NonConvergent) {
      primary = DiagnosticKind::OwnershipSummaryNonConvergent;
    } else if (violation.kind ==
               CallableOwnershipViolationKind::ArtifactMismatch) {
      primary = DiagnosticKind::OwnershipSummaryArtifactMismatch;
    } else if (violation.kind ==
               CallableOwnershipViolationKind::ContractMismatch) {
      primary = DiagnosticKind::CallableContractMismatch;
    }
    emitOwnershipDiagnostic(primary, violation.instruction, violation.evidence,
                            root);
  }
  for (const auto diagnostic :
       analyzePlaceStates(sem_ir_, &sem_ir_.analysisMetrics().place_state)) {
    DiagnosticKind kind = DiagnosticKind::InvalidMoveOperand;
    switch (diagnostic.kind) {
    case PlaceStateDiagnosticKind::InvalidMoveOperand:
      kind = DiagnosticKind::InvalidMoveOperand;
      break;
    case PlaceStateDiagnosticKind::InvalidCopyOperand:
      kind = DiagnosticKind::InvalidCopyOperand;
      break;
    case PlaceStateDiagnosticKind::InvalidAssignmentTarget:
      kind = DiagnosticKind::InvalidAssignmentTarget;
      break;
    case PlaceStateDiagnosticKind::AssignmentToImmutablePlace:
      kind = DiagnosticKind::AssignmentToImmutablePlace;
      break;
    case PlaceStateDiagnosticKind::CopyUnavailable:
      kind = DiagnosticKind::CopyUnavailable;
      break;
    case PlaceStateDiagnosticKind::MoveUnavailable:
      kind = DiagnosticKind::MoveUnavailable;
      break;
    case PlaceStateDiagnosticKind::UseAfterMove:
      kind = DiagnosticKind::UseAfterMove;
      break;
    case PlaceStateDiagnosticKind::UseOfMaybeMovedPlace:
      kind = DiagnosticKind::UseOfMaybeMovedPlace;
      break;
    case PlaceStateDiagnosticKind::UseOfPartiallyMovedPlace:
      kind = DiagnosticKind::UseOfPartiallyMovedPlace;
      break;
    case PlaceStateDiagnosticKind::BorrowConflict:
      kind = DiagnosticKind::BorrowConflict;
      break;
    case PlaceStateDiagnosticKind::InactiveUnionMember:
      kind = DiagnosticKind::InactiveUnionMember;
      break;
    case PlaceStateDiagnosticKind::UnknownActiveUnionMember:
      kind = DiagnosticKind::UnknownActiveUnionMember;
      break;
    case PlaceStateDiagnosticKind::TaskNotConsumed:
      kind = DiagnosticKind::TaskNotConsumed;
      break;
    case PlaceStateDiagnosticKind::TaskAlreadyConsumed:
      kind = DiagnosticKind::TaskAlreadyConsumed;
      break;
    case PlaceStateDiagnosticKind::TaskScopeEscape:
      kind = DiagnosticKind::TaskScopeEscape;
      break;
    case PlaceStateDiagnosticKind::UninitializedStorage:
      kind = DiagnosticKind::UninitializedStorage;
      break;
    case PlaceStateDiagnosticKind::AnalysisNonConvergent:
      kind = DiagnosticKind::OwnershipSummaryNonConvergent;
      break;
    }
    emitOwnershipDiagnostic(kind, diagnostic.instruction, diagnostic.evidence,
                            root);
  }
  for (const auto diagnostic : analyzeInterproceduralLoans(
           sem_ir_, &sem_ir_.analysisMetrics().place_state))
    emitOwnershipDiagnostic(
        diagnostic.kind == PlaceStateDiagnosticKind::AnalysisNonConvergent
            ? DiagnosticKind::OwnershipSummaryNonConvergent
            : DiagnosticKind::BorrowConflict,
        diagnostic.instruction, diagnostic.evidence, root);
  if (!diagnostics_.hasError()) {
    buildConcreteSpecializationComponents();
    finalizeSpecificFingerprints();
  }

  std::string error;
  if (!diagnostics_.hasError() && !sem_ir_.verify(error)) {
    emit(DiagnosticKind::InvalidSemIR, root);
  }
  return std::move(sem_ir_);
}
