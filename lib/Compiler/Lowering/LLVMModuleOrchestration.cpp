#include "LLVMInternal.h"

#include <unordered_set>

namespace chtholly::compiler {

bool LLVMModuleOrchestrationService::run(
    LLVMModuleOrchestrationState &state, std::string &error) {
  if (!state.register_functions(error) ||
      !state.emit_static_globals(error))
    return false;

  std::unordered_set<std::uint32_t> coroutine_functions;
  for (std::uint32_t index = 0;
       index < state.low_ir.coroutineFramePlanCount(); ++index)
    coroutine_functions.insert(
        state.low_ir.coroutineFramePlan(CoroutineFramePlanId(index))
            .function.index);
  if (!state.emit_coroutine_scaffolds(error))
    return false;
  for (std::uint32_t index = 0; index < state.low_ir.functionCount(); ++index) {
    const auto semantic_function =
        state.low_ir.function(LowFunctionId(index)).semantic_function;
    if (coroutine_functions.contains(semantic_function.index) ||
        (state.function_flags(semantic_function) &
         SemFunctionCoroutineScaffold) != 0)
      continue;
    if (!state.lower_function(LowFunctionId(index), error))
      return false;
  }

  bool has_execution_entry = false;
  for (std::uint32_t index = 0;
       index < state.low_ir.coroutineFramePlanCount(); ++index)
    if (state.low_ir.coroutineFramePlan(CoroutineFramePlanId(index))
            .execution_entry) {
      has_execution_entry = true;
      break;
    }
  for (std::uint32_t index = 0;
       state.emission_role == ModuleEmissionRole::ExecutableEntry &&
       index < state.sem_ir.functionCount(); ++index) {
    const auto function_id = FunctionId(index);
    if ((state.function_flags(function_id) &
         SemFunctionCoroutineTaskDriver) == 0)
      continue;
    ++state.entry_candidate_count;
    if (!has_execution_entry) {
      const auto reference = state.local_function_refs.at(index);
      state.source_entry =
          state.emit_task_driver_host(*state.functions.at(reference.index));
    }
  }
  if ((state.emission_role == ModuleEmissionRole::ExecutableEntry ||
       state.emission_role == ModuleEmissionRole::CoroutineExecutionEntry) &&
      state.entry_candidate_count != 1) {
    error = state.entry_candidate_count == 0
                ? "executable module has no authorized entry candidate"
                : "executable module has multiple authorized entry candidates";
    return false;
  }
  if (state.source_entry &&
      (state.emission_role == ModuleEmissionRole::ExecutableEntry ||
       state.emission_role == ModuleEmissionRole::CoroutineExecutionEntry))
    state.emit_entry_points(*state.source_entry);
  if (!state.emit_component_exports(error))
    return false;
  state.finalize_debug();
  return true;
}

} // namespace chtholly::compiler
