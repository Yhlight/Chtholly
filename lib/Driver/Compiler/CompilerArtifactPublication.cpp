#include "CompilerPipelineInternal.h"

#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/SemIR.h"

namespace chtholly {
namespace {
std::string moduleIdentityKey(std::string_view package,
                              std::string_view module) {
  return std::string(package) + "\n" + std::string(module);
}
} // namespace

bool CompilerArtifactPublicationService::collect(
    CompilerArtifactPublicationState &state, std::string &error) {
  for (const auto &query : state.query_results) {
    const auto &session = *query.session;
    for (std::uint32_t index = 0; index < session.unitCount(); ++index) {
      const auto &unit = session.unit(compiler::CheckIRId(index));
      const auto *artifact =
          session.packageManifest().findModule(unit.moduleName());
      if (!artifact) {
        error = "compiler package manifest omitted a compiled module";
        return false;
      }
      auto object = unit.emitObject(error);
      if (!error.empty() || object.empty()) {
        if (error.empty())
          error = "compiler compiler pipeline emitted an empty native object";
        return false;
      }
      state.object_paths.push_back(state.artifact_store.objectPath(
          artifact->object_fingerprint,
          state.plan.build.target.object_extension));
      const auto module_key = moduleIdentityKey(
          session.packageManifest().packageName(),
          std::string(unit.moduleName()));
      state.object_paths_by_module[module_key] = state.object_paths.back();
      state.artifacts_by_module[module_key] = artifact;
      state.published_objects.push_back(
          {.fingerprint = artifact->object_fingerprint,
           .specific_fingerprint = artifact->specific_fingerprint,
           .target_triple = state.plan.build.target.info.triple,
           .extension = state.plan.build.target.object_extension,
           .bytes = std::move(object)});
      if (const auto *sem_ir = unit.semIR())
        for (const auto &component : sem_ir->specializationComponents())
          state.published_specializations.push_back({component});
      for (const auto &specific : unit.nominalTypeSpecificArtifacts())
        state.published_nominal_specifics.push_back({specific});
      for (const auto &witness : unit.nominalSemanticWitnessArtifacts())
        state.published_nominal_semantic_witnesses.push_back({witness});
      for (const auto &layout : unit.nominalTypeLayoutArtifacts())
        state.published_nominal_layouts.push_back({layout});
    }
  }
  return true;
}

} // namespace chtholly
