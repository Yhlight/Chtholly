#include "SemanticContext.h"

namespace chtholly::compiler::semantics_internal {

[[nodiscard]] std::optional<SemanticContext::ResultTypeShape>
SemanticContext::foreignErrorResultTypeShape(
    TypeId raw_result, const interop::ForeignOperationArtifact &artifact,
    NodeId node) {
  auto success =
      artifact.error_success_payload ==
              interop::ForeignOperationArtifact::ErrorSuccessPayload::Void
          ? sem_ir_.voidType()
          : raw_result;
  if (artifact.outcome_projection ==
          interop::ForeignOperationArtifact::OutcomeProjection::PosixRead ||
      artifact.outcome_projection ==
          interop::ForeignOperationArtifact::OutcomeProjection::Win32Read ||
      artifact.outcome_projection ==
          interop::ForeignOperationArtifact::OutcomeProjection::Fread) {
    if (!usesWideSliceIndices(sem_ir_.languageVersion())) {
      emit(DiagnosticKind::MissingCFFIOutcomeImport, node);
      return std::nullopt;
    }
    const auto result_module = values_.internIdentifier("std::result");
    const auto io_module = values_.internIdentifier("std::io");
    if (!sem_ir_.importIRs().findByModule(result_module).hasValue() ||
        !sem_ir_.importIRs().findByModule(io_module).hasValue()) {
      emit(DiagnosticKind::MissingCFFIOutcomeImport, node);
      return std::nullopt;
    }
    const auto entity = sem_ir_.importIRs().registry().findEntity(
        "std", "std::io", "ReadOutcome", PublicEntityKind::NominalType);
    if (!entity.hasValue()) {
      emit(DiagnosticKind::MissingCFFIOutcomeImport, node);
      return std::nullopt;
    }
    const auto element =
        sem_ir_.addIntegerType(artifact.outcome_element_type->scalar_width,
                               artifact.outcome_element_type->integer_signed);
    const auto slice = sem_ir_.addSliceType(element, false);
    const auto nominal = materializeImportedNominal(entity, node);
    if (!nominal.hasValue()) {
      emit(DiagnosticKind::MissingCFFIOutcomeImport, node);
      return std::nullopt;
    }
    const std::array arguments{slice};
    success = sem_ir_.addNominalType(nominal, arguments);
    const auto shape = sem_ir_.canonicalReadOutcomeShape(success);
    if (!shape || shape->data != slice) {
      emit(DiagnosticKind::MissingCFFIOutcomeImport, node);
      return std::nullopt;
    }
  }
  auto code_error = sem_ir_.foreignRepresentationType(raw_result);
  if (!code_error.hasValue())
    code_error = raw_result;
  const auto error =
      artifact.error_extractor ==
              interop::ForeignOperationArtifact::ErrorExtractor::ReturnedCode
          ? code_error
      : artifact.error_extractor ==
              interop::ForeignOperationArtifact::ErrorExtractor::Win32LastError
          ? sem_ir_.addIntegerType(32, false)
          : sem_ir_.i32Type();
  return canonicalResultTypeShape(
      success, error, node,
      artifact.outcome_projection !=
              interop::ForeignOperationArtifact::OutcomeProjection::None
          ? DiagnosticKind::MissingCFFIOutcomeImport
          : DiagnosticKind::MissingCFFIResultImport);
}

} // namespace chtholly::compiler::semantics_internal
