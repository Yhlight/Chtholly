#include "LLVMInternal.h"

#include "llvm/IR/DebugInfoMetadata.h"

namespace chtholly::compiler {

std::optional<LLVMFunctionSetupResult> LLVMFunctionSetupService::prepare(
    LowFunctionId id, llvm::Function &llvm_function,
    LLVMFunctionSetupState &state, std::string &error) {
  const auto &function = state.low_ir.function(id);
  const auto &semantic_function = state.sem_ir.function(function.semantic_function);
  LLVMFunctionSetupResult result;
  if (state.debug_builder && state.debug_file) {
    for (const auto &occurrence : state.sem_ir.symbolOccurrences()) {
      if (occurrence.kind != SemSymbolOccurrenceKind::Declaration ||
          occurrence.target_kind != SemSymbolTargetKind::Function)
        continue;
      const auto &reference =
          state.sem_ir.functionRef(FunctionRefId(occurrence.target));
      if (reference.local_function != function.semantic_function)
        continue;
      result.debug_location = state.sem_ir.sourceLocation(occurrence.location);
      break;
    }
    const auto name =
        state.sem_ir.identifier(state.sem_ir.name(semantic_function.name).text);
    auto *type = state.debug_builder->createSubroutineType(
        state.debug_builder->getOrCreateTypeArray({}));
    result.debug_subprogram = state.debug_builder->createFunction(
        state.debug_file, name, llvm_function.getName(), state.debug_file,
        result.debug_location.line, type, result.debug_location.line,
        llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition);
    llvm_function.setSubprogram(result.debug_subprogram);
  }

  const auto &semantic_function_type = state.sem_ir.type(semantic_function.type);
  const auto result_type = TypeId(semantic_function_type.arg1);
  const auto representation_pack =
      state.sem_ir.functionSemanticContract(function.semantic_function).role ==
      CallableSemanticRole::Pack;
  const auto fallible_constructor =
      state.sem_ir.functionSemanticContract(function.semantic_function).role ==
              CallableSemanticRole::Constructor
          ? state.sem_ir.canonicalResultShape(result_type)
          : std::optional<CanonicalResultShape>{};
  const auto parameter_count =
      state.sem_ir.typeBlock(TypeBlockId(semantic_function_type.arg0)).size();
  const auto hidden_result_count =
      fallible_constructor
          ? 2U
          : (!representation_pack &&
                     state.low_ir.typeRepresentation(result_type)
                             .facts.init_repr == InitReprKind::InPlace
                 ? 1U
                 : 0U);
  if (llvm_function.arg_size() != parameter_count + hidden_result_count) {
    error = "LLVM function declaration does not match its hidden result ABI";
    return std::nullopt;
  }
  state.current_result_slot =
      hidden_result_count != 0 ? llvm_function.getArg(0) : nullptr;
  state.current_outcome_slot =
      fallible_constructor ? llvm_function.getArg(1) : nullptr;

  for (const auto block_id : state.low_ir.blockList(function.blocks))
    state.blocks.emplace(
        block_id.index,
        llvm::BasicBlock::Create(state.context, "", &llvm_function));
  for (const auto slot_id : state.low_ir.slotBlock(function.slots)) {
    const auto &slot = state.low_ir.slot(slot_id);
    auto *storage = state.entry_alloca(
        llvm_function, state.lower_object_type(slot.type), "slot");
    state.slots.emplace(slot_id.index, storage);
    if (state.debug_info == DebugInfoMode::Full && result.debug_subprogram &&
        slot.semantic_local.hasValue() &&
        (slot.flags & LowSlotSynthetic) == 0) {
      const auto &local = state.sem_ir.local(slot.semantic_local);
      const auto location = state.sem_ir.sourceLocation(local.declaration);
      const auto name = state.sem_ir.identifier(state.sem_ir.name(local.name).text);
      auto *variable = state.debug_builder->createAutoVariable(
          result.debug_subprogram, name, state.debug_file, location.line,
          state.debug_type(local.type));
      state.debug_builder->insertDeclare(
          storage, variable, state.debug_builder->createExpression(),
          llvm::DILocation::get(state.context, location.line, location.column,
                                result.debug_subprogram),
          &llvm_function.getEntryBlock());
    }
  }
  std::unordered_set<std::uint32_t> function_slots;
  for (const auto slot_id : state.low_ir.slotBlock(function.slots))
    function_slots.insert(slot_id.index);
  for (std::uint32_t index = 0; index < state.low_ir.placeCount(); ++index) {
    const auto place = LowPlaceId(index);
    if (function_slots.contains(state.low_ir.place(place).root.index))
      state.place_flags.emplace(
          index, state.entry_alloca(llvm_function,
                                   llvm::Type::getInt1Ty(state.context),
                                   "place.init"));
  }
  return result;
}

} // namespace chtholly::compiler
