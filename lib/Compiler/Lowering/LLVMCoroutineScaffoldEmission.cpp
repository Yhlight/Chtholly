#include "LLVMInternal.h"

#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <cassert>
#include <filesystem>

namespace chtholly::compiler {

bool LLVMCoroutineScaffoldService::emit(std::string &error, LLVMCoroutineScaffoldState &scaffold_state) {
  const auto value = [&](LowInstId id) { return scaffold_state.value(id); };
  const auto lowerValueType = [&](TypeId type) {
    return scaffold_state.lower_value_type(type);
  };
  const auto lowerObjectType = [&](TypeId type) {
    return scaffold_state.lower_object_type(type);
  };
  const auto entryAlloca = [&](llvm::Function &function, llvm::Type *type,
                               llvm::StringRef name) {
    return scaffold_state.entry_alloca(function, type, name);
  };
  const auto loadValueFromObject = [&](llvm::Value *address, TypeId type,
                                       llvm::IRBuilder<> &builder) {
    return scaffold_state.load_value_from_object(address, type, builder);
  };
  const auto storeValueToObject = [&](llvm::Value *address, llvm::Value *source,
                                      TypeId type, llvm::IRBuilder<> &builder) {
    scaffold_state.store_value_to_object(address, source, type, builder);
  };
  const auto finishCallbackCompletionValue =
      [&](CallbackCompletionPlanId plan, llvm::Value *completion,
          llvm::IRBuilder<> &builder, llvm::Function &function) {
        return scaffold_state.finish_callback_completion(plan, completion,
                                                         builder, function);
      };
  const auto armCompletionSet =
      [&](const CompletionProviderPlan &provider, llvm::Value *set,
          std::uint32_t count, llvm::IRBuilder<> &builder,
          llvm::Function &function) {
        scaffold_state.arm_completion_set(provider, set, count, builder,
                                          function);
      };
  const auto detachCompletionSet =
      [&](const CompletionProviderPlan &provider, llvm::Value *set,
          std::uint32_t count, llvm::IRBuilder<> &builder,
          llvm::Function &function) {
        scaffold_state.detach_completion_set(provider, set, count, builder,
                                             function);
      };
  const auto releaseCompletionSet =
      [&](const CompletionProviderPlan &provider, llvm::Value *set,
          std::uint32_t count, llvm::IRBuilder<> &builder,
          llvm::Function &function) {
        scaffold_state.release_completion_set(provider, set, count, builder,
                                              function);
      };
  const auto probeCompletionSet =
      [&](const CoroutineTaskCompletionCombinePlan &plan, llvm::Value *set,
          llvm::IRBuilder<> &builder, llvm::Function &function) {
        return scaffold_state.probe_completion_set(plan, set, builder, function);
      };
  const auto setSlotPlacesInitialized =
      [&](SlotId slot, bool initialized, llvm::IRBuilder<> &builder) {
        scaffold_state.set_slot_places_initialized(slot, initialized, builder);
      };
  const auto testCoroutineInitializationBit =
      [&](std::uint32_t bit, llvm::IRBuilder<> &builder) {
        return scaffold_state.test_initialization_bit(bit, builder);
      };
  const auto lowerInst =
      [&](LowInstId id, llvm::IRBuilder<> &builder, llvm::Function &function) {
        scaffold_state.lower_instruction(id, builder, function);
      };
  const auto emitCoroutineProtocolTrap =
      [&](std::uint32_t reason, llvm::IRBuilder<> &builder) {
        scaffold_state.protocol_trap(reason, builder);
      };
  const auto emitCoroutineDestroyAddress =
      [&](TypeId type, llvm::Value *address, llvm::IRBuilder<> &builder,
          llvm::Function &function) {
        LLVMCoroutineScaffoldService::destroyAddress(
            type, address, builder, function, scaffold_state);
      };
  const auto finishCoroutineTaskGroupDrain =
      [&](llvm::Value *group, const CoroutineResumeState &resume_state,
          llvm::BasicBlock *continuation, llvm::IRBuilder<> &builder,
          llvm::Function &function) {
        LLVMCoroutineScaffoldService::finishTaskGroupDrain(
            group, resume_state, continuation, builder, function,
            scaffold_state);
      };
  const auto emitCoroutineCleanupChain =
      [&](llvm::Function &function, llvm::StructType *frame_type,
          llvm::Value *frame, const CoroutineFramePlan &plan,
          const std::unordered_map<std::uint32_t, unsigned> &value_fields,
          llvm::BasicBlock *start, llvm::BasicBlock *terminal,
          std::string_view prefix) {
        LLVMCoroutineScaffoldService::cleanupChain(
            function, frame_type, frame, plan, value_fields, start, terminal,
            prefix, scaffold_state);
      };
  const auto emitCoroutineCleanupGraph =
      [&](llvm::Function &function, llvm::StructType *frame_type,
          llvm::Value *frame, const CoroutineFramePlan &plan,
          CoroutineCleanupGraphId graph_id, llvm::BasicBlock *start,
          llvm::BasicBlock *terminal, std::string prefix,
          bool use_frame_storage = true) {
        LLVMCoroutineScaffoldService::cleanupGraph(
            function, frame_type, frame, plan, graph_id, start, terminal,
            prefix, use_frame_storage, scaffold_state);
      };
    auto *pointer_type = llvm::PointerType::getUnqual(scaffold_state.context);
    auto *i32 = llvm::Type::getInt32Ty(scaffold_state.context);
    auto *i64 = llvm::Type::getInt64Ty(scaffold_state.context);
    auto *void_type = llvm::Type::getVoidTy(scaffold_state.context);
    if (!LLVMCoroutineScaffoldService::registerConstructors(error,
                                                            scaffold_state))
      return false;
#include "LLVMCoroutineScaffoldFrame.inc"
#include "LLVMCoroutineScaffoldResume.inc"
#include "LLVMCoroutineScaffoldResult.inc"
#include "LLVMCoroutineScaffoldConstructor.inc"
} // namespace chtholly::compiler
