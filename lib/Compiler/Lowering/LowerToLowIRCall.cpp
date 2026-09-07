#include "LowerToLowIRInternal.h"

#include <string>
#include <vector>

namespace chtholly::compiler::internal {
namespace {

template <typename InstT>
LowInstId emit(LoweringExpressionState &state, LowBlockId block, InstT inst,
               InstId origin) {
  const auto id = state.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.low_ir.blockCount()].push_back(id);
  return id;
}

} // namespace

void LoweringExpressionService::call(InstId semantic_id, SemCall semantic,
                                     LowBlockId current,
                                     LoweringExpressionState &state) {
  std::vector<LowInstId> arguments;
  for (const auto argument : state.sem_ir.instBlock(semantic.arg1))
    arguments.push_back(state.value_for(argument));
  const auto &reference = state.sem_ir.functionRef(semantic.arg0);
  const auto foreign =
      reference.local_function.hasValue()
          ? state.sem_ir.functionDeclaration(reference.local_function).kind ==
                SemCallableDeclarationKind::Foreign
          : state.sem_ir.importIRs().tryGetEntity(reference.public_entity) &&
                state.sem_ir.importIRs()
                        .tryGetEntity(reference.public_entity)
                        ->declaration_kind ==
                    PublicCallableDeclarationKind::Foreign;
  if (foreign) {
    const auto fixed = state.low_ir.foreignAbiLayoutFor(semantic.arg0);
    const auto fixed_count =
        fixed.hasValue()
            ? state.low_ir.foreignAbiLayout(fixed).parameters.size()
            : arguments.size();
    std::vector<TypeId> suffix_types;
    for (std::size_t index = fixed_count; index < arguments.size(); ++index) {
      const auto source_type = TypeId(state.low_ir.inst(arguments[index]).type);
      suffix_types.push_back(source_type);
      const auto promoted = state.low_ir.cDefaultPromotedType(source_type);
      if (promoted.hasValue() && promoted != source_type)
        arguments[index] = emit(
            state, current,
            LowForeignDefaultPromote{promoted, arguments[index], {}},
            semantic_id);
    }
    std::string error;
    const auto call_layout = state.low_ir.addForeignAbiCallLayout(
        semantic.arg0, suffix_types, error);
    const auto argument_block = state.low_ir.addValueBlock(arguments);
    state.values[semantic_id.index] = emit(
        state, current,
        LowForeignCall{semantic.type, call_layout, argument_block}, semantic_id);
  } else {
    const auto argument_block = state.low_ir.addValueBlock(arguments);
    state.values[semantic_id.index] = emit(
        state, current, LowCall{semantic.type, semantic.arg0, argument_block},
        semantic_id);
  }
  if (semantic.type == state.sem_ir.neverType())
    (void)emit(state, current,
               LowUnreachable{state.sem_ir.voidType(), {}}, semantic_id);
}

} // namespace chtholly::compiler::internal
