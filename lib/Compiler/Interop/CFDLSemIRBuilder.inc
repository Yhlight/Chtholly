// Included by CFDL.cpp; this fragment aliases the existing CFDL parser,
// validation, rendering, and SemIR state without duplicating ownership.
SemIR buildCFDLSemIR(const CFDLSyntaxFile &file, core::Arena &arena,
                     SharedValueStores &values, DiagnosticEmitter &diagnostics,
                     const PublicInterfaceRegistry &public_interfaces,
                     interop::ArtifactRegistry &interop_registry,
                     CheckIRId check_ir_id, std::string_view canonical_package,
                     IdentifierId module_name, std::string_view target_triple,
                     std::span<const ImportIR> imports,
                     LanguageVersion language_version) {
  SemIR sem_ir(arena, values, check_ir_id, module_name, public_interfaces,
               interop_registry, imports, language_version);
#include "CFDLSemIRBuilderSetup.inc"
#include "CFDLSemIRBuilderTypes.inc"
#include "CFDLSemIRBuilderConstants.inc"
#include "CFDLSemIRBuilderCallables.inc"
}
