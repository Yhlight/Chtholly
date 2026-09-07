#include "CompilerPipelineInternal.h"

#include "chtholly/Compiler/CompilationUnit.h"

namespace chtholly {

bool CompilerPipelineOutputService::writeDumps(
    CompilerPipelineOutputState &state,
    const compiler::CompilationSession &session,
    const compiler::CompilationUnit &unit, std::string &error) {
  const auto &invocation = state.invocation;
  if (!invocation.compiler_tokens_output_path.empty() &&
      !state.write_output(invocation.compiler_tokens_output_path,
                          unit.printTokens(), error))
    return false;
  if (!invocation.compiler_parse_tree_output_path.empty() &&
      !state.write_output(invocation.compiler_parse_tree_output_path,
                          unit.printParseTree(), error))
    return false;
  if (!invocation.compiler_sem_ir_output_path.empty() &&
      !state.write_output(invocation.compiler_sem_ir_output_path,
                          unit.printSemIR(), error))
    return false;
  if (!invocation.compiler_low_ir_output_path.empty() &&
      !state.write_output(invocation.compiler_low_ir_output_path,
                          unit.printLowIR(), error))
    return false;
  if (!invocation.compiler_foreign_protocols_output_path.empty() &&
      !state.write_output(invocation.compiler_foreign_protocols_output_path,
                          session.printForeignProtocols(), error))
    return false;
  if (!invocation.compiler_metrics_output_path.empty() &&
      !state.write_output(invocation.compiler_metrics_output_path,
                          session.metricsJson() + "\n", error))
    return false;
  return true;
}

bool CompilerPipelineOutputService::writeArtifactLoadMetrics(
    CompilerPipelineOutputState &state, std::string_view metrics,
    std::string &error) {
  const auto &path = state.invocation.compiler_artifact_load_metrics_output_path;
  return path.empty() ||
         state.write_output(path, std::string(metrics) + "\n", error);
}

bool CompilerPipelineOutputService::writeAnalysisMetrics(
    CompilerPipelineOutputState &state,
    std::span<const PackageQueryResult> results, std::string &error) {
  const auto &path = state.invocation.compiler_analysis_metrics_output_path;
  return path.empty() ||
         state.write_output(path,
                            CompilerPipelineDiagnosticsService::analysisMetricsJson(
                                results) + "\n",
                            error);
}

} // namespace chtholly
