#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace chtholly::compiler {

bool LLVMFunctionBodyService::lower(
    LowFunctionId id, llvm::Function &function,
    llvm::DISubprogram *debug_subprogram, LineColumn debug_location,
    LLVMFunctionBodyState &state, std::string &error) {
  const auto &low_function = state.low_ir.function(id);
  for (const auto block_id : state.low_ir.blockList(low_function.blocks)) {
    llvm::IRBuilder<> builder(state.blocks.at(block_id.index));
    for (const auto inst_id : state.low_ir.block(block_id)) {
      if (debug_subprogram) {
        auto instruction_location = debug_location;
        const auto origin = state.low_ir.origin(inst_id);
        if (origin.hasValue())
          instruction_location =
              state.sem_ir.sourceLocation(state.sem_ir.location(origin));
        builder.SetCurrentDebugLocation(llvm::DILocation::get(
            state.context, instruction_location.line,
            instruction_location.column, debug_subprogram));
      }
      state.lower_inst(inst_id, builder, function);
      if (!state.instruction_error.empty()) {
        error = state.instruction_error;
        return false;
      }
    }
  }
  llvm::raw_string_ostream stream(error);
  if (llvm::verifyFunction(function, &stream)) {
    stream.flush();
    return false;
  }
  return true;
}

} // namespace chtholly::compiler
