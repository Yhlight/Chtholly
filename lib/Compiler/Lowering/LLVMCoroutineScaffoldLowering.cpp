#include "LLVMInternal.h"

#include <algorithm>
#include <cassert>
#include <filesystem>

#include "llvm/IR/Verifier.h"

namespace chtholly::compiler {
namespace {

class CoroutineScaffoldEmitter {
public:
  explicit CoroutineScaffoldEmitter(LLVMCoroutineScaffoldState &state)
      : state_(state) {}
  llvm::Function *emitTaskDriverHost(llvm::Function &driver) {
    return LLVMCoroutineScaffoldService::taskDriverHost(driver, state_);
  }

  CompletionProviderPlan completionProviderFor(TypeId aggregate_type) const {
    return state_.low_ir.completionProviderFor(aggregate_type);
  }

  void emitCoroutineDestroyAddress(TypeId type, llvm::Value *address,
                                   llvm::IRBuilder<> &builder,
                                   llvm::Function &function) {
    LLVMCoroutineScaffoldService::destroyAddress(
        type, address, builder, function, state_);
  }

  void emitCoroutineDetachValue(const CallbackWakePlan &plan, llvm::Value *wake,
                                llvm::IRBuilder<> &builder,
                                llvm::Function &function) {
    auto *completion = builder.CreateExtractValue(wake, 0);
    emitCoroutineDetachCompletion(plan, completion, builder, function);
  }

  void emitCoroutineDetachCompletion(const CallbackWakePlan &plan,
                                     llvm::Value *completion,
                                     llvm::IRBuilder<> &builder,
                                     llvm::Function &function) {
    LLVMCoroutineScaffoldService::detachCompletion(
        plan, completion, builder, function, state_);
  }

  void emitCoroutineCleanupChain(
      llvm::Function &function, llvm::StructType *frame_type,
      llvm::Value *frame, const CoroutineFramePlan &plan,
      const std::unordered_map<std::uint32_t, unsigned> &value_fields,
      llvm::BasicBlock *start, llvm::BasicBlock *terminal,
      std::string_view prefix) {
    LLVMCoroutineScaffoldService::cleanupChain(
        function, frame_type, frame, plan, value_fields, start, terminal,
        prefix, state_);
  }
  void finishCoroutineTaskGroupDrain(llvm::Value *group,
                                     const CoroutineResumeState &state,
                                     llvm::BasicBlock *continuation,
                                     llvm::IRBuilder<> &builder,
                                     llvm::Function &function) {
    LLVMCoroutineScaffoldService::finishTaskGroupDrain(
        group, state, continuation, builder, function, state_);
  }

  void emitCoroutineCleanupGraph(llvm::Function &function,
                                 llvm::StructType *frame_type,
                                 llvm::Value *frame,
                                 const CoroutineFramePlan &plan,
                                 CoroutineCleanupGraphId graph_id,
                                 llvm::BasicBlock *start,
                                 llvm::BasicBlock *terminal, std::string prefix,
                                 bool use_frame_storage = true) {
    LLVMCoroutineScaffoldService::cleanupGraph(
        function, frame_type, frame, plan, graph_id, start, terminal, prefix,
        use_frame_storage, state_);
  }
  bool emitCoroutineScaffolds(std::string &error) {
    return LLVMCoroutineScaffoldService::emit(error, state_);
  }
private:
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return state_.value(id);
  }
  [[nodiscard]] llvm::Type *lowerValueType(TypeId type) const {
    return state_.lower_value_type(type);
  }
  [[nodiscard]] llvm::Type *lowerObjectType(TypeId type) const {
    return state_.lower_object_type(type);
  }
  [[nodiscard]] llvm::AllocaInst *entryAlloca(
      llvm::Function &function, llvm::Type *type, llvm::StringRef name) const {
    return state_.entry_alloca(function, type, name);
  }
  [[nodiscard]] llvm::Value *loadValueFromObject(
      llvm::Value *address, TypeId type, llvm::IRBuilder<> &builder) const {
    return state_.load_value_from_object(address, type, builder);
  }
  void storeValueToObject(llvm::Value *address, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) const {
    state_.store_value_to_object(address, source, type, builder);
  }
  [[nodiscard]] llvm::Value *enumPayloadAddress(
      llvm::Value *owner, TypeId type, std::uint32_t variant,
      std::uint32_t field, llvm::IRBuilder<> &builder) const {
    return state_.enum_payload_address(owner, type, variant, field, builder);
  }
  void emitCallbackAdapterRelease(llvm::Value *adapter, TypeId type,
                                  llvm::IRBuilder<> &builder) const {
    state_.release_callback_adapter(adapter, type, builder);
  }
  llvm::Value *finishCallbackCompletionValue(
      CallbackCompletionPlanId plan, llvm::Value *completion,
      llvm::IRBuilder<> &builder, llvm::Function &function) const {
    return state_.finish_callback_completion(
        plan, completion, builder, function);
  }
  void emitWakeAdapterRelease(llvm::Value *adapter,
                              const CallbackWakePlan &plan,
                              llvm::IRBuilder<> &builder) const {
    state_.release_wake_adapter(adapter, plan, builder);
  }
  void armCompletionSet(const CompletionProviderPlan &provider,
                        llvm::Value *set, std::uint32_t count,
                        llvm::IRBuilder<> &builder,
                        llvm::Function &function) const {
    state_.arm_completion_set(provider, set, count, builder, function);
  }
  void detachCompletionSet(const CompletionProviderPlan &provider,
                           llvm::Value *set, std::uint32_t count,
                           llvm::IRBuilder<> &builder,
                           llvm::Function &function) const {
    state_.detach_completion_set(provider, set, count, builder, function);
  }
  void releaseCompletionSet(const CompletionProviderPlan &provider,
                            llvm::Value *set, std::uint32_t count,
                            llvm::IRBuilder<> &builder,
                            llvm::Function &function) const {
    state_.release_completion_set(provider, set, count, builder, function);
  }
  [[nodiscard]] LLVMCompletionCombineProbe probeCompletionSet(
      const CoroutineTaskCompletionCombinePlan &plan, llvm::Value *set,
      llvm::IRBuilder<> &builder, llvm::Function &function) const {
    return state_.probe_completion_set(plan, set, builder, function);
  }
  [[nodiscard]] bool pathPrefix(
      std::span<const LowPlaceProjection> prefix,
      std::span<const LowPlaceProjection> path) const {
    return state_.path_prefix(prefix, path);
  }
  [[nodiscard]] llvm::Value *testCoroutineInitializationBit(
      std::uint32_t bit, llvm::IRBuilder<> &builder) const {
    return state_.test_initialization_bit(bit, builder);
  }
  void setSlotPlacesInitialized(SlotId slot, bool initialized,
                                llvm::IRBuilder<> &builder) const {
    state_.set_slot_places_initialized(slot, initialized, builder);
  }
  void lowerInst(LowInstId id, llvm::IRBuilder<> &builder,
                 llvm::Function &function) const {
    state_.lower_instruction(id, builder, function);
  }
  [[nodiscard]] llvm::Value *emitForeignCallValues(
      const ForeignAbiFunctionLayout &layout,
      const ForeignAbiCallLayout &call_layout, llvm::Value *callee,
      std::span<llvm::Value *const> arguments, llvm::IRBuilder<> &builder,
      llvm::Function &function) const {
    return state_.emit_foreign_call_values(
        layout, call_layout, callee, arguments, builder, function);
  }
  void emitCoroutineProtocolTrap(std::uint32_t reason,
                                 llvm::IRBuilder<> &builder) const {
    state_.protocol_trap(reason, builder);
  }
  [[nodiscard]] llvm::BasicBlock *coroutineCancellationTarget(
      llvm::IRBuilder<> &builder, llvm::Function &function) const {
    return state_.cancellation_target(builder, function);
  }
  [[nodiscard]] std::string coroutineConstructorSymbol(
      const PublicEntity &entity) const {
    return state_.constructor_symbol(entity);
  }

  LLVMCoroutineScaffoldState &state_;
};

} // namespace

} // namespace chtholly::compiler
