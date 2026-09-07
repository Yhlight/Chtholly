add_library(chtholly_frontend
  Frontend/Diagnostics.cpp
  Frontend/Generic.cpp
  Frontend/Grammar.cpp
  Frontend/Lexer.cpp
  Frontend/Parser.cpp
  Frontend/ParseTree.cpp
  Frontend/SharedValueStores.cpp
  Frontend/Source.cpp
  Frontend/Token.cpp
  Frontend/TokenBuffer.cpp)

chtholly_configure_target(chtholly_frontend)
target_link_libraries(chtholly_frontend PUBLIC chtholly_compiler_core)

add_library(chtholly_semir
  IR/AnalysisMetrics.cpp
  Interop/CFFIIdentity.cpp
  Interop/CFDL.cpp
  Semantics/CallableOwnership.cpp
  IR/CarrierView.cpp
  Interop/ComponentABI.cpp
  Interop/ComponentABI2Protocol.cpp
  Interop/ComponentABI2Registry.cpp
  Interop/ComponentABI2Artifact.cpp
  IR/ConcreteSpecialization.cpp
  Semantics/ConstantEvaluation.cpp
  Interop/ForeignDeclaration.cpp
  Interop/InteropArtifact.cpp
  IR/IncrementalDependencies.cpp
  IR/ImportIR.cpp
  IR/IteratorDesugaring.cpp
  IR/NominalTypeArtifact.cpp
  IR/NominalTypeArtifactVerification.cpp
  IR/NominalCompletion.cpp
  IR/Outcome.cpp
  Semantics/PlaceState.cpp
  IR/PublicInterface.cpp
  IR/PublicInterfaceLookup.cpp
  IR/PublicInterfaceValidation.cpp
  IR/PublicInterfacePrinting.cpp
  IR/PublicInterfaceCanonicalize.cpp
  IR/PublicInterfaceRegistry.cpp
  IR/PublicInterfaceServices.cpp
  IR/PublicInterfaceClosureConstruction.cpp
  IR/PublicInterfaceDeclarationConstruction.cpp
  IR/PublicInterfaceFingerprintEncoding.cpp
  IR/PublicInterfaceForeignProtocol.cpp
  IR/PublicInterfaceFunctionConstruction.cpp
  IR/PublicInterfaceGenericTemplateConstruction.cpp
  IR/PublicInterfaceIdentity.cpp
  IR/PublicInterfaceTypeConstruction.cpp
  IR/PublicInterfaceArtifactRegistration.cpp
  IR/PublicInterfaceRegistryConstruction.cpp
  IR/PublicInterfaceNominalRegistration.cpp
  IR/PublicInterfaceSecondaryRegistration.cpp
  IR/PublicInterfaceFunctionValidation.cpp
  IR/PublicInterfaceFunctionSemantics.cpp
  IR/PublicInterfaceArtifactIdentity.cpp
  IR/PublicInterfaceArtifactVerifier.cpp
  IR/PublicInterfaceResolver.cpp
  IR/PublicInterfaceArtifactFunctionVerification.cpp
  IR/PublicInterfaceArtifactNominalVerification.cpp
  IR/PublicInterfaceArtifactValueVerification.cpp
  IR/PublicInterfaceOwnershipValidation.cpp
  IR/PublicInterfaceRegistryRegistration.cpp
  IR/PublicInterfaceGenericTemplateValidation.cpp
  Semantics/SemanticCallResolution.cpp
  Semantics/SemanticConversion.cpp
  Semantics/SemanticContextCall.cpp
  Semantics/SemanticContextCore.cpp
  Semantics/SemanticForeignError.cpp
  Semantics/SemanticContextDeclarations.cpp
  Semantics/SemanticContextExpressions.cpp
  Semantics/SemanticContextFunctions.cpp
  Semantics/SemanticContextIteration.cpp
  Semantics/SemanticContextSpecialization.cpp
  Semantics/SemanticContextStatements.cpp
  Semantics/SemanticControlFlow.cpp
  Semantics/SemanticLiteral.cpp
  Semantics/SemanticNameScopes.cpp
  Semantics/SemanticWitnessResolution.cpp
  IR/SemIR.cpp
  IR/SemIRTypeQueries.cpp
  IR/SemIRSpecializationCloning.cpp
  IR/SemIRMaterialization.cpp
  IR/SemIRVerify.cpp
  IR/SemIRVerifyInstructions.cpp
  IR/SemIRVerifyIntrinsics.cpp
  IR/IncrementalDependenciesFingerprint.cpp
  IR/IncrementalCompilationPlan.cpp
  IR/IncrementalManifestPresentation.cpp
  IR/IncrementalModuleVerification.cpp
  IR/IncrementalPackageCheckVerification.cpp
  IR/IncrementalPackageManifestVerification.cpp
  IR/NominalLayoutVerification.cpp
  IR/NominalLayoutConfig.cpp
  IR/NominalTypeSpecificVerification.cpp
  IR/NominalSemanticWitnessVerification.cpp
  IR/TypeConcurrency.cpp
  IR/SemIRCallbackTypes.cpp
  IR/SemIRTypeMaterialization.cpp
  IR/PublicInterfaceArtifactBuild.cpp
  IR/PublicInterfaceRegistryClosure.cpp
  IR/PublicInterfaceRegistryVerification.cpp
  IR/PublicInterfaceValueVerification.cpp
  IR/PublicInterfaceTypeValidation.cpp
  IR/PublicInterfaceForeignOperationValidation.cpp
  IR/PublicInterfaceConstantValidation.cpp
  IR/SemIRVerifyControlFlow.cpp
  IR/SemIRVerifyFunctions.cpp
  IR/SemIRVerifyRecords.cpp
  IR/SemIRVerifyTypes.cpp
  IR/SemIRVerificationContext.cpp
  IR/SemIRKinds.cpp
  Semantics/Semantics.cpp)
target_sources(chtholly_semir PRIVATE IR/TypeLayout.cpp)

chtholly_configure_target(chtholly_semir)
target_link_libraries(chtholly_semir
  PUBLIC chtholly_frontend
  PRIVATE chtholly_support)

add_library(chtholly_lowir
  Lowering/LowIR.cpp
  Lowering/LowIRForeignABI.cpp
  Lowering/LowIRForeignABIPlans.cpp
  Lowering/LowIRPrint.cpp
  Lowering/LowIRVerify.cpp
  Lowering/LowIRVerifyCoroutine.cpp
  Lowering/LowIRVerifyForeign.cpp
  Lowering/LowIRVerifyRepresentations.cpp
  Lowering/LowIRVerifyObjects.cpp
  Lowering/LowIRVerifyScalar.cpp
  Lowering/LowIRVerificationContext.cpp
  Lowering/LowIRVerifyForeignPlans.cpp
  Lowering/LowIRVerifyCoroutinePlans.cpp
  Lowering/LowerToLowIR.cpp
  Lowering/LowerToLowIRContext.cpp
  Lowering/LowerToLowIRExpression.cpp
  Lowering/LowerToLowIRCall.cpp
  Lowering/LowerToLowIRPlace.cpp
  Lowering/LowerToLowIRControl.cpp
  Lowering/LowerToLowIRLoops.cpp
  Lowering/LowerToLowIROwnership.cpp
  Lowering/LowerToLowIRBinding.cpp
  Lowering/LowerToLowIRFunction.cpp
  Lowering/LowerToLowIRInterop.cpp
  Lowering/LowerToLowIRCleanup.cpp
  Lowering/LowerToLowIRCoroutine.cpp
  Lowering/LowerToLowIRDestroy.cpp
  Lowering/LowerToLowIRCoroutineFrame.cpp)

chtholly_configure_target(chtholly_lowir)
target_link_libraries(chtholly_lowir PUBLIC chtholly_semir)

add_library(chtholly_llvm Lowering/LLVM.cpp Lowering/LLVMModule.cpp
  Lowering/LLVMTarget.cpp Lowering/LLVMModuleLowering.cpp
  Lowering/LLVMModuleOrchestration.cpp
  Lowering/LLVMValueLowering.cpp Lowering/LLVMObjectLowering.cpp
  Lowering/LLVMObjectIdentityLowering.cpp
  Lowering/LLVMFunctionRegistration.cpp
  Lowering/LLVMAbiIdentityLowering.cpp
  Lowering/LLVMInteropLowering.cpp Lowering/LLVMCleanupLowering.cpp
  Lowering/LLVMCoroutineInstruction.cpp Lowering/LLVMForeignCallLowering.cpp
  Lowering/LLVMForeignValueLowering.cpp Lowering/LLVMObjectValueLowering.cpp
  Lowering/LLVMForeignResultLowering.cpp Lowering/LLVMCoroutineTaskLowering.cpp
  Lowering/LLVMCallbackRegistrationLowering.cpp
  Lowering/LLVMCallbackThunkLowering.cpp
  Lowering/LLVMCallbackCompletionLowering.cpp
  Lowering/LLVMCallbackWakeLowering.cpp
  Lowering/LLVMComponentExportLowering.cpp
  Lowering/LLVMStaticGlobalLowering.cpp
  Lowering/LLVMEntryPointLowering.cpp
  Lowering/LLVMCallLowering.cpp
  Lowering/LLVMDebugTypeLowering.cpp
  Lowering/LLVMFunctionSetup.cpp
  Lowering/LLVMFunctionBodyLowering.cpp
  Lowering/LLVMVectorDropLowering.cpp
  Lowering/LLVMTransferReturnLowering.cpp
  Lowering/LLVMCoroutineTaskGroupLowering.cpp
  Lowering/LLVMCoroutineCompletionLowering.cpp
  Lowering/LLVMCoroutineCompletionSetLowering.cpp
  Lowering/LLVMCoroutineTaskResultLowering.cpp
  Lowering/LLVMCoroutineDestroyLowering.cpp
  Lowering/LLVMCoroutineDetachLowering.cpp
  Lowering/LLVMCoroutineConstructorRegistration.cpp
  Lowering/LLVMCoroutineTaskGroupDrainLowering.cpp
  Lowering/LLVMCoroutineCleanupChainLowering.cpp
  Lowering/LLVMCoroutineCleanupGraphLowering.cpp
  Lowering/LLVMCoroutineScaffoldEmission.cpp
  Lowering/LLVMTaskDriverHostLowering.cpp
  Lowering/LLVMIntrinsicLowering.cpp
  Lowering/LLVMScalarInstructionLowering.cpp
  Lowering/LLVMObjectInstructionLowering.cpp
  Lowering/LLVMCoroutineScaffoldLowering.cpp)
chtholly_configure_target(chtholly_llvm)
target_link_libraries(chtholly_llvm PUBLIC chtholly_lowir)
set(CHTHOLLY_COMPILER_LLVM_TARGET_COMPONENTS nativecodegen)
if("AArch64" IN_LIST LLVM_TARGETS_TO_BUILD)
  list(APPEND CHTHOLLY_COMPILER_LLVM_TARGET_COMPONENTS AArch64)
  target_compile_definitions(chtholly_llvm
    PRIVATE CHTHOLLY_COMPILER_LLVM_HAS_AARCH64=1)
endif()
chtholly_link_llvm(chtholly_llvm core support target mc
  passes ${CHTHOLLY_COMPILER_LLVM_TARGET_COMPONENTS})

add_library(chtholly_pipeline Pipeline/CompilationUnit.cpp
  Pipeline/CompilationUnitOutput.cpp Pipeline/PackageQueryGraph.cpp)
chtholly_configure_target(chtholly_pipeline)
target_link_libraries(chtholly_pipeline PUBLIC chtholly_llvm)
