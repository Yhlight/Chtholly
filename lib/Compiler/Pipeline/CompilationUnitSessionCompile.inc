// Included at namespace scope; this definition aliases the original
// CompilationSession::Impl and preserves the compile phase state machine.
bool CompilationSession::compile(std::string &error,
#include "CompilationUnitCompileSetup.inc"
#include "CompilationUnitCompileInspection.inc"
#include "CompilationUnitCompileInvalidation.inc"
#include "CompilationUnitCompileArtifacts.inc"
#include "CompilationUnitCompileUnits.inc"
#include "CompilationUnitCompileFinalize.inc"
}
