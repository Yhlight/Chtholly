#include "chtholly/Compiler/Semantics.h"

#include "SemanticContext.h"

#include <string>
#include <utility>
#include <vector>

namespace chtholly::compiler {

SemIR buildSemIR(const ParseTree &tree, core::Arena &arena,
                 SharedValueStores &values, DiagnosticEmitter &diagnostics,
                 const PublicInterfaceRegistry &public_interfaces,
                 const interop::ArtifactRegistry &interop_registry,
                 CheckIRId check_ir_id, IdentifierId module_name,
                 std::span<const ImportIR> imports,
                 StableFingerprint semantic_options_fingerprint,
                 const ConcreteSpecializationLoader &specialization_loader,
                 std::uint32_t pointer_width,
                 std::string_view normalized_target_triple,
                 std::span<const CompilerIntrinsicBinding> compiler_intrinsics,
                 LanguageVersion language_version) {
  if (!module_name.hasValue())
    module_name = values.internIdentifier("module");
  auto result =
      semantics_internal::SemanticContext(
          tree, arena, values, diagnostics, public_interfaces, interop_registry,
          check_ir_id, module_name, imports, semantic_options_fingerprint,
          specialization_loader, pointer_width, normalized_target_triple,
          compiler_intrinsics, language_version)
          .run();
  std::vector<LineColumn> locations;
  locations.reserve(tree.size());
  for (std::uint32_t index = 0; index < tree.size(); ++index) {
    const auto token = tree.token(NodeId(index));
    locations.push_back(
        tree.tokens().source().lineColumn(tree.tokens().get(token).offset));
  }
  result.setSourceMetadata(std::string(tree.tokens().source().filename()),
                           std::move(locations));
  return result;
}

} // namespace chtholly::compiler
