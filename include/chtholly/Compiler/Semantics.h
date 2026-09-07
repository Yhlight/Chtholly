#pragma once

#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/SemIR.h"

namespace chtholly::compiler {

[[nodiscard]] SemIR
buildSemIR(const ParseTree &tree, core::Arena &arena, SharedValueStores &values,
           DiagnosticEmitter &diagnostics,
           const PublicInterfaceRegistry &public_interfaces,
           const interop::ArtifactRegistry &interop_registry,
           CheckIRId check_ir_id = CheckIRId(0),
           IdentifierId module_name = IdentifierId::invalid(),
           std::span<const ImportIR> imports = {},
           StableFingerprint semantic_options_fingerprint = {},
           const ConcreteSpecializationLoader &specialization_loader = {},
           std::uint32_t pointer_width = 64,
           std::string_view normalized_target_triple = {},
           std::span<const CompilerIntrinsicBinding> compiler_intrinsics = {},
           LanguageVersion language_version = DefaultLanguageVersion);

} // namespace chtholly::compiler
