#include "LLVMInternal.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace chtholly::compiler {
namespace {

class CompletionSetEmitter {
public:
  explicit CompletionSetEmitter(LLVMCoroutineCompletionSetState &state)
      : state_(state) {}
  llvm::StructType *
  completionSetStorageType(const CompletionProviderPlan &provider,
                           std::uint32_t count) {
    const auto words = coroutineTaskCompletionBitmapWordCount(count);
    auto *bitmap =
        llvm::ArrayType::get(llvm::Type::getInt64Ty(state_.context), words);
    return llvm::StructType::get(
        state_.context,
        {llvm::ArrayType::get(lowerValueType(provider.completion_type), count),
         bitmap, bitmap});
  }

  llvm::Value *completionSetBitAddress(const CompletionProviderPlan &provider,
                                       llvm::Value *set, std::uint32_t count,
                                       std::uint32_t index,
                                       std::uint32_t bitmap_field,
                                       llvm::IRBuilder<> &builder) {
    auto *storage = completionSetStorageType(provider, count);
    assert(bitmap_field == 1 || bitmap_field == 2);
    auto *bitmap = builder.CreateStructGEP(storage, set, bitmap_field);
    return builder.CreateInBoundsGEP(
        storage->getElementType(bitmap_field), bitmap,
        {builder.getInt32(0), builder.getInt32(index / 64U)});
  }

  llvm::Value *completionSetBit(const CompletionProviderPlan &provider,
                                llvm::Value *set, std::uint32_t count,
                                std::uint32_t index, std::uint32_t bitmap_field,
                                llvm::IRBuilder<> &builder) {
    auto *word_address = completionSetBitAddress(provider, set, count, index,
                                                 bitmap_field, builder);
    auto *word = builder.CreateLoad(builder.getInt64Ty(), word_address);
    return builder.CreateICmpNE(
        builder.CreateAnd(word, builder.getInt64(UINT64_C(1) << (index % 64U))),
        builder.getInt64(0));
  }

  llvm::Value *completionSetActive(const CompletionProviderPlan &provider,
                                   llvm::Value *set, std::uint32_t count,
                                   std::uint32_t index,
                                   llvm::IRBuilder<> &builder) {
    return completionSetBit(provider, set, count, index, 1, builder);
  }

  llvm::Value *completionSetArmed(const CompletionProviderPlan &provider,
                                  llvm::Value *set, std::uint32_t count,
                                  std::uint32_t index,
                                  llvm::IRBuilder<> &builder) {
    return completionSetBit(provider, set, count, index, 2, builder);
  }

  void setCompletionSetBit(const CompletionProviderPlan &provider,
                           llvm::Value *set, std::uint32_t count,
                           std::uint32_t index, std::uint32_t bitmap_field,
                           bool value, llvm::IRBuilder<> &builder) {
    auto *word_address = completionSetBitAddress(provider, set, count, index,
                                                 bitmap_field, builder);
    auto *word = builder.CreateLoad(builder.getInt64Ty(), word_address);
    const auto mask = UINT64_C(1) << (index % 64U);
    builder.CreateStore(value
                            ? builder.CreateOr(word, builder.getInt64(mask))
                            : builder.CreateAnd(word, builder.getInt64(~mask)),
                        word_address);
  }

  void clearCompletionSetMember(const CompletionProviderPlan &provider,
                                llvm::Value *set, std::uint32_t count,
                                std::uint32_t index,
                                llvm::IRBuilder<> &builder) {
    setCompletionSetBit(provider, set, count, index, 1, false, builder);
    setCompletionSetBit(provider, set, count, index, 2, false, builder);
  }

  llvm::Value *completionSetElement(const CompletionProviderPlan &provider,
                                    llvm::Value *set, std::uint32_t count,
                                    std::uint32_t index,
                                    llvm::IRBuilder<> &builder) {
    auto *storage = completionSetStorageType(provider, count);
    auto *elements = builder.CreateStructGEP(storage, set, 0);
    auto *address = builder.CreateInBoundsGEP(
        storage->getElementType(0), elements,
        {builder.getInt32(0), builder.getInt32(index)});
    return builder.CreateLoad(lowerValueType(provider.completion_type),
                              address);
  }

  // Completion lowering is keyed by the operation completion plan. The wake
  // plan remains a source-independent ABI primitive shared by callback and
  // operation providers; it is never reconstructed from CFDL spelling here.
  const CallbackWakePlan &
  completionWakePlan(const CompletionProviderPlan &provider) const {
    if (provider.operation_completion.hasValue())
      return state_.low_ir.callbackWakePlan(
          state_.low_ir.foreignOperationCompletionPlan(provider.operation_completion)
              .wake_plan);
    // Compatibility providers are removed after the stdlib fixture wave.
    return state_.low_ir.callbackWakePlan(provider.wake_plan);
  }

  llvm::Value *probeCompletion(const CompletionProviderPlan &provider,
                               llvm::Value *completion,
                               llvm::IRBuilder<> &builder,
                               llvm::Function &function) {
    if (provider.kind == CompletionProviderKind::Task) {
      return builder.CreateICmpNE(
          builder.CreateCall(
              state_.module.getOrInsertFunction(
                  "chtholly_next_task_v1_completion_ready",
                  llvm::FunctionType::get(builder.getInt8Ty(),
                                          {builder.getPtrTy()}, false)),
              {completion}),
          builder.getInt8(0));
    }
    const auto &wake = completionWakePlan(provider);
    const auto &readiness = state_.low_ir.callbackReadinessPlan(wake.readiness_plan);
    auto *token = builder.CreateExtractValue(completion, 2);
    auto *done = llvm::BasicBlock::Create(state_.context, "completion.set.ready.done",
                                          &function);
    auto *poll = llvm::BasicBlock::Create(state_.context, "completion.set.ready.poll",
                                          &function);
    auto *origin = builder.GetInsertBlock();
    builder.CreateCondBr(builder.CreateIsNull(token), done, poll);
    builder.SetInsertPoint(poll);
    auto *entry = builder.CreateExtractValue(completion, 4);
    const auto &call = state_.low_ir.foreignAbiCallLayout(readiness.poll_call_layout);
    const auto &layout = state_.low_ir.foreignAbiLayout(call.function_layout);
    llvm::SmallVector<llvm::Value *, 1> arguments{token};
    auto *ready = emitForeignCallValues(layout, call, entry, arguments, builder,
                                        function);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    auto *result = builder.CreatePHI(builder.getInt1Ty(), 2);
    result->addIncoming(builder.getTrue(), origin);
    result->addIncoming(ready, poll);
    return result;
  }

  void finishCompletion(const CompletionProviderPlan &provider,
                        llvm::Value *completion, llvm::IRBuilder<> &builder,
                        llvm::Function &function) {
    if (provider.kind == CompletionProviderKind::Task) {
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_task_v1_completion_release",
              llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                      false)),
          {completion});
      return;
    }
    (void)finishCallbackCompletionValue(
        completionWakePlan(provider).completion_plan, completion, builder,
        function);
  }

  void armCompletionSet(const CompletionProviderPlan &provider,
                        llvm::Value *set, std::uint32_t count,
                        llvm::IRBuilder<> &builder, llvm::Function &function) {
    if (provider.kind != CompletionProviderKind::Operation)
      return;
    assert(state_.coroutine.task && state_.coroutine.wake_entry &&
           state_.coroutine.wake_release);
    const auto &plan = completionWakePlan(provider);
    auto retain = state_.module.getOrInsertFunction(
        "chtholly_next_task_v1_task_retain",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    auto *current = builder.GetInsertBlock();
    for (std::uint32_t index = 0; index < count; ++index) {
      llvm::IRBuilder<> test(current);
      auto *active =
          llvm::BasicBlock::Create(state_.context, "completion.set.arm", &function);
      auto *next =
          llvm::BasicBlock::Create(state_.context, "completion.set.armed", &function);
      test.CreateCondBr(
          test.CreateAnd(completionSetActive(provider, set, count, index, test),
                         test.CreateNot(completionSetArmed(provider, set, count,
                                                           index, test))),
          active, next);
      llvm::IRBuilder<> arm(active);
      auto *completion = completionSetElement(provider, set, count, index, arm);
      auto *token = arm.CreateExtractValue(completion, 2);
      auto *call_block = llvm::BasicBlock::Create(
          state_.context, "completion.set.arm.call", &function);
      auto *ready_release = llvm::BasicBlock::Create(
          state_.context, "completion.set.arm.ready", &function);
      auto *transferred = llvm::BasicBlock::Create(
          state_.context, "completion.set.arm.transferred", &function);
      arm.CreateCondBr(arm.CreateIsNull(token), next, call_block);
      llvm::IRBuilder<> call(call_block);
      call.CreateCall(retain, {state_.coroutine.task});
      auto *entry = call.CreateExtractValue(completion, 5);
      const auto &call_layout =
          state_.low_ir.foreignAbiCallLayout(plan.arm_call_layout);
      const auto &layout =
          state_.low_ir.foreignAbiLayout(call_layout.function_layout);
      llvm::SmallVector<llvm::Value *, 4> arguments(layout.parameters.size());
      for (std::size_t parameter = 0; parameter < arguments.size();
           ++parameter) {
        if (parameter == plan.arm_parameters[0])
          arguments[parameter] = token;
        else if (parameter == plan.arm_parameters[1])
          arguments[parameter] = state_.coroutine.wake_entry;
        else if (parameter == plan.arm_parameters[2])
          arguments[parameter] = state_.coroutine.task;
        else if (parameter == plan.arm_parameters[3])
          arguments[parameter] = state_.coroutine.wake_release;
        else
          llvm_unreachable("unassigned completion-set arm parameter");
      }
      auto *ready = emitForeignCallValues(layout, call_layout, entry, arguments,
                                          call, function);
      call.CreateCondBr(ready, ready_release, transferred);
      llvm::IRBuilder<> release(ready_release);
      release.CreateCall(state_.coroutine.wake_release,
                         {state_.coroutine.task});
      release.CreateBr(next);
      llvm::IRBuilder<> mark_armed(transferred);
      setCompletionSetBit(provider, set, count, index, 2, true, mark_armed);
      mark_armed.CreateBr(next);
      current = next;
    }
    builder.SetInsertPoint(current);
  }

  void detachCompletionSet(const CompletionProviderPlan &provider,
                           llvm::Value *set, std::uint32_t count,
                           llvm::IRBuilder<> &builder,
                           llvm::Function &function) {
    if (provider.kind == CompletionProviderKind::Task) {
      releaseCompletionSet(provider, set, count, builder, function);
      return;
    }
    auto free_call = state_.module.getOrInsertFunction(
        "free", llvm::FunctionType::get(builder.getVoidTy(),
                                        {builder.getPtrTy()}, false));
    auto *current = builder.GetInsertBlock();
    for (std::uint32_t index = 0; index < count; ++index) {
      llvm::IRBuilder<> test(current);
      auto *active = llvm::BasicBlock::Create(
          state_.context, "completion.set.detach.active", &function);
      auto *next = llvm::BasicBlock::Create(
          state_.context, "completion.set.detach.next", &function);
      test.CreateCondBr(completionSetActive(provider, set, count, index, test),
                        active, next);
      llvm::IRBuilder<> detach(active);
      emitCoroutineDetachCompletion(
          completionWakePlan(provider),
          completionSetElement(provider, set, count, index, detach), detach,
          function);
      clearCompletionSetMember(provider, set, count, index, detach);
      detach.CreateBr(next);
      current = next;
    }
    llvm::IRBuilder<> finish(current);
    finish.CreateCall(free_call, {set});
    builder.SetInsertPoint(current);
  }

  void releaseCompletionSet(const CompletionProviderPlan &provider,
                            llvm::Value *set, std::uint32_t count,
                            llvm::IRBuilder<> &builder,
                            llvm::Function &function) {
    auto free_call = state_.module.getOrInsertFunction(
        "free", llvm::FunctionType::get(builder.getVoidTy(),
                                        {builder.getPtrTy()}, false));
    auto *null =
        llvm::BasicBlock::Create(state_.context, "task.set.release.null", &function);
    auto *scan =
        llvm::BasicBlock::Create(state_.context, "task.set.release.scan", &function);
    auto *done =
        llvm::BasicBlock::Create(state_.context, "task.set.release.done", &function);
    builder.CreateCondBr(
        builder.CreateICmpEQ(
            set, llvm::ConstantPointerNull::get(builder.getPtrTy())),
        null, scan);
    llvm::IRBuilder<>(null).CreateBr(done);
    auto *current = scan;
    for (std::uint32_t index = 0; index < count; ++index) {
      llvm::IRBuilder<> test(current);
      auto *active = llvm::BasicBlock::Create(
          state_.context, "task.set.release.active", &function);
      auto *next = llvm::BasicBlock::Create(state_.context, "task.set.release.next",
                                            &function);
      test.CreateCondBr(completionSetActive(provider, set, count, index, test),
                        active, next);
      llvm::IRBuilder<> release_builder(active);
      finishCompletion(
          provider,
          completionSetElement(provider, set, count, index, release_builder),
          release_builder, function);
      clearCompletionSetMember(provider, set, count, index, release_builder);
      release_builder.CreateBr(next);
      current = next;
    }
    llvm::IRBuilder<> finish(current);
    finish.CreateCall(free_call, {set});
    finish.CreateBr(done);
    builder.SetInsertPoint(done);
  }

  LLVMCompletionCombineProbe
  probeCompletionSet(const CoroutineTaskCompletionCombinePlan &plan,
                     llvm::Value *set, llvm::IRBuilder<> &builder,
                     llvm::Function &function) {
    if (plan.operand_count == 0) {
      if (plan.operation == CoroutineTaskCompletionCombineKind::WaitAll)
        return {builder.getTrue(), nullptr};
      auto *payload_type =
          plan.operation == CoroutineTaskCompletionCombineKind::Select
              ? lowerObjectType(
                    state_.sem_ir.coroutineCheckedPayloadType(plan.result_type))
              : builder.getInt32Ty();
      auto *payload = entryAlloca(function, payload_type,
                                  "task.combination.invalid.payload");
      builder.CreateStore(llvm::Constant::getNullValue(payload_type), payload);
      return {builder.getTrue(),
              makeCoroutineChecked(
                  builder.getInt32(static_cast<std::uint32_t>(-2601)), payload,
                  plan.result_type, builder)};
    }

    auto *ready_flag = entryAlloca(function, builder.getInt1Ty(),
                                   "task.combination.ready.flag");
    builder.CreateStore(
        builder.getInt1(plan.operation ==
                        CoroutineTaskCompletionCombineKind::WaitAll),
        ready_flag);
    auto *winner =
        entryAlloca(function, builder.getInt32Ty(), "task.combination.winner");
    builder.CreateStore(builder.getInt32(UINT32_MAX), winner);
    auto *current = builder.GetInsertBlock();
    for (const auto operand_index : plan.canonical_operand_order) {
      llvm::IRBuilder<> scan(current);
      auto *active = llvm::BasicBlock::Create(
          state_.context, "task.combination.active", &function);
      auto *next = llvm::BasicBlock::Create(state_.context, "task.combination.next",
                                            &function);
      llvm::Value *continue_scan = scan.getTrue();
      if (plan.operation != CoroutineTaskCompletionCombineKind::WaitAll)
        continue_scan =
            scan.CreateICmpEQ(scan.CreateLoad(scan.getInt32Ty(), winner),
                              scan.getInt32(UINT32_MAX));
      scan.CreateCondBr(scan.CreateAnd(continue_scan, completionSetActive(
                                                          plan.provider, set,
                                                          plan.operand_count,
                                                          operand_index, scan)),
                        active, next);
      llvm::IRBuilder<> observe(active);
      auto *completion = completionSetElement(
          plan.provider, set, plan.operand_count, operand_index, observe);
      auto *is_ready =
          probeCompletion(plan.provider, completion, observe, function);
      auto *consume = llvm::BasicBlock::Create(
          state_.context, "task.combination.consume", &function);
      auto *pending = llvm::BasicBlock::Create(
          state_.context, "task.combination.pending", &function);
      observe.CreateCondBr(is_ready, consume, pending);
      llvm::IRBuilder<> consume_builder(consume);
      finishCompletion(plan.provider, completion, consume_builder, function);
      clearCompletionSetMember(plan.provider, set, plan.operand_count,
                               operand_index, consume_builder);
      if (plan.operation != CoroutineTaskCompletionCombineKind::WaitAll) {
        consume_builder.CreateStore(consume_builder.getInt32(operand_index),
                                    winner);
        consume_builder.CreateStore(consume_builder.getTrue(), ready_flag);
      }
      consume_builder.CreateBr(next);
      llvm::IRBuilder<> pending_builder(pending);
      if (plan.operation == CoroutineTaskCompletionCombineKind::WaitAll)
        pending_builder.CreateStore(pending_builder.getFalse(), ready_flag);
      pending_builder.CreateBr(next);
      current = next;
    }
    builder.SetInsertPoint(current);
    auto *ready = builder.CreateLoad(builder.getInt1Ty(), ready_flag,
                                     "task.combination.ready");
    if (plan.operation == CoroutineTaskCompletionCombineKind::WaitAll)
      return {ready, nullptr};

    auto *payload_type =
        plan.operation == CoroutineTaskCompletionCombineKind::Select
            ? lowerObjectType(
                  state_.sem_ir.coroutineCheckedPayloadType(plan.result_type))
            : builder.getInt32Ty();
    auto *payload =
        entryAlloca(function, payload_type, "task.combination.payload");
    if (plan.operation == CoroutineTaskCompletionCombineKind::Select) {
      llvm::Value *selection = llvm::UndefValue::get(payload_type);
      selection = builder.CreateInsertValue(
          selection, builder.CreateLoad(builder.getInt32Ty(), winner), 0);
      selection = builder.CreateInsertValue(selection, set, 1);
      builder.CreateStore(selection, payload);
    } else {
      builder.CreateStore(builder.CreateLoad(builder.getInt32Ty(), winner),
                          payload);
    }
    auto *checked = makeCoroutineChecked(builder.getInt32(0), payload,
                                         plan.result_type, builder);
    return {ready, checked};
  }

  llvm::Value *lowerInst(LowCoroutineTaskCompletionSetCreate inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto &plan = state_.low_ir.coroutineTaskCompletionSetPlan(inst.arg0);
    const auto operands = state_.low_ir.valueBlock(inst.arg1);
    auto *owner_storage =
        entryAlloca(function, builder.getPtrTy(), "task.completion.set.owner");
    if (plan.operand_count == 0) {
      builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                          owner_storage);
      return makeCoroutineChecked(builder.getInt32(0), owner_storage, inst.type,
                                  builder);
    }
    auto *storage_type =
        completionSetStorageType(plan.provider, plan.operand_count);
    const auto bytes =
        state_.module.getDataLayout().getTypeAllocSize(storage_type).getFixedValue();
    auto malloc_call = state_.module.getOrInsertFunction(
        "malloc", llvm::FunctionType::get(builder.getPtrTy(),
                                          {builder.getInt64Ty()}, false));
    auto *set = builder.CreateCall(malloc_call, {builder.getInt64(bytes)},
                                   "task.completion.set");
    auto *allocated = llvm::BasicBlock::Create(
        state_.context, "task.completion.set.allocated", &function);
    auto *failed = llvm::BasicBlock::Create(
        state_.context, "task.completion.set.failed", &function);
    auto *done = llvm::BasicBlock::Create(state_.context, "task.completion.set.done",
                                          &function);
    builder.CreateCondBr(
        builder.CreateICmpNE(
            set, llvm::ConstantPointerNull::get(builder.getPtrTy())),
        allocated, failed);
    llvm::IRBuilder<> initialize(allocated);
    initialize.CreateStore(llvm::Constant::getNullValue(storage_type), set);
    for (std::uint32_t index = 0; index < plan.operand_count; ++index) {
      auto *elements = initialize.CreateStructGEP(storage_type, set, 0);
      auto *address = initialize.CreateInBoundsGEP(
          storage_type->getElementType(0), elements,
          {initialize.getInt32(0), initialize.getInt32(index)});
      initialize.CreateStore(value(operands[index]), address);
      setCompletionSetBit(plan.provider, set, plan.operand_count, index, 1,
                          true, initialize);
    }
    initialize.CreateBr(done);
    llvm::IRBuilder<> allocation_failed(failed);
    for (const auto operand : operands)
      finishCompletion(plan.provider, value(operand), allocation_failed,
                       function);
    auto *allocation_cleanup_done = allocation_failed.GetInsertBlock();
    allocation_failed.CreateBr(done);
    builder.SetInsertPoint(done);
    auto *owner =
        builder.CreatePHI(builder.getPtrTy(), 2, "task.completion.set.result");
    owner->addIncoming(set, allocated);
    owner->addIncoming(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                       allocation_cleanup_done);
    auto *status = builder.CreatePHI(builder.getInt32Ty(), 2,
                                     "task.completion.set.status");
    status->addIncoming(builder.getInt32(0), allocated);
    status->addIncoming(builder.getInt32(static_cast<std::uint32_t>(-2602)),
                        allocation_cleanup_done);
    builder.CreateStore(owner, owner_storage);
    return makeCoroutineChecked(status, owner_storage, inst.type, builder);
  }

  llvm::Value *lowerInst(LowCoroutineTaskCompletionCombine inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto &plan = state_.low_ir.coroutineTaskCompletionCombinePlan(inst.arg0);
    assert(plan.operand_count == 0);
    return probeCompletionSet(plan, value(inst.arg1), builder, function).result;
  }

  llvm::Value *lowerInst(LowCoroutineTaskSelectionWinner inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return builder.CreateExtractValue(value(inst.arg0), 0,
                                      "task.selection.winner");
  }

  llvm::Value *lowerInst(LowCoroutineTaskSelectionTakeRemaining inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return builder.CreateExtractValue(value(inst.arg0), 1,
                                      "task.selection.remaining");
  }

  llvm::Value *lowerInst(LowFinishCoroutineTaskCompletionSet inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto set_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto provider = completionProviderFor(set_type);
    releaseCompletionSet(provider, value(inst.arg0),
                         state_.sem_ir.coroutineTaskCompletionCapacity(set_type),
                         builder, function);
    return nullptr;
  }

  llvm::Value *lowerInst(LowFinishCoroutineTaskSelection inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto selection_type = TypeId(state_.low_ir.inst(inst.arg0).type);
    const auto provider = completionProviderFor(selection_type);
    releaseCompletionSet(
        provider, builder.CreateExtractValue(value(inst.arg0), 1),
        state_.sem_ir.coroutineTaskCompletionCapacity(selection_type), builder,
        function);
    return nullptr;
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
  [[nodiscard]] llvm::Value *makeCoroutineChecked(
      llvm::Value *status, llvm::Value *storage, TypeId type,
      llvm::IRBuilder<> &builder) const {
    return state_.make_checked(status, storage, type, builder);
  }
  [[nodiscard]] llvm::Value *emitForeignCallValues(
      const ForeignAbiFunctionLayout &layout,
      const ForeignAbiCallLayout &call_layout, llvm::Value *callee,
      std::span<llvm::Value *const> arguments, llvm::IRBuilder<> &builder,
      llvm::Function &function) const {
    return state_.callback_wake.callback.emit_foreign_call_values(
        layout, call_layout, callee, arguments, builder, function);
  }
  [[nodiscard]] llvm::Value *finishCallbackCompletionValue(
      CallbackCompletionPlanId plan, llvm::Value *completion,
      llvm::IRBuilder<> &builder, llvm::Function &function) const {
    return LLVMCallbackCompletionService::finishValue(
        plan, completion, builder, function, state_.callback_wake.callback);
  }
  void emitWakeAdapterRelease(llvm::Value *adapter,
                              const CallbackWakePlan &plan,
                              llvm::IRBuilder<> &builder) const {
    LLVMCallbackWakeService::releaseAdapter(
        adapter, plan, builder, state_.callback_wake);
  }
  void emitCoroutineDetachCompletion(const CallbackWakePlan &plan,
                                     llvm::Value *completion,
                                     llvm::IRBuilder<> &builder,
                                     llvm::Function &function) const {
    state_.detach_completion(plan, completion, builder, function);
  }
  [[nodiscard]] CompletionProviderPlan completionProviderFor(
      TypeId type) const {
    return state_.low_ir.completionProviderFor(type);
  }

  LLVMCoroutineCompletionSetState &state_;
};

} // namespace

void LLVMCoroutineCompletionSetService::arm(
    const CompletionProviderPlan &provider, llvm::Value *set,
    std::uint32_t count, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineCompletionSetState &state) {
  CompletionSetEmitter(state).armCompletionSet(
      provider, set, count, builder, function);
}

void LLVMCoroutineCompletionSetService::detach(
    const CompletionProviderPlan &provider, llvm::Value *set,
    std::uint32_t count, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineCompletionSetState &state) {
  CompletionSetEmitter(state).detachCompletionSet(
      provider, set, count, builder, function);
}

void LLVMCoroutineCompletionSetService::release(
    const CompletionProviderPlan &provider, llvm::Value *set,
    std::uint32_t count, llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineCompletionSetState &state) {
  CompletionSetEmitter(state).releaseCompletionSet(
      provider, set, count, builder, function);
}

LLVMCompletionCombineProbe LLVMCoroutineCompletionSetService::probe(
    const CoroutineTaskCompletionCombinePlan &plan, llvm::Value *set,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCoroutineCompletionSetState &state) {
  return CompletionSetEmitter(state).probeCompletionSet(
      plan, set, builder, function);
}

llvm::Value *LLVMCoroutineCompletionSetService::create(
    LowCoroutineTaskCompletionSetCreate inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineCompletionSetState &state) {
  return CompletionSetEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCoroutineCompletionSetService::combine(
    LowCoroutineTaskCompletionCombine inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineCompletionSetState &state) {
  return CompletionSetEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCoroutineCompletionSetService::selectionWinner(
    LowCoroutineTaskSelectionWinner inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineCompletionSetState &state) {
  return builder.CreateExtractValue(state.value(inst.arg0), 0,
                                    "task.selection.winner");
}

llvm::Value *LLVMCoroutineCompletionSetService::selectionRemaining(
    LowCoroutineTaskSelectionTakeRemaining inst, llvm::IRBuilder<> &builder,
    LLVMCoroutineCompletionSetState &state) {
  return builder.CreateExtractValue(state.value(inst.arg0), 1,
                                    "task.selection.remaining");
}

void LLVMCoroutineCompletionSetService::finishSet(
    LowFinishCoroutineTaskCompletionSet inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineCompletionSetState &state) {
  CompletionSetEmitter emitter(state);
  const auto set_type = TypeId(state.low_ir.inst(inst.arg0).type);
  emitter.releaseCompletionSet(
      state.low_ir.completionProviderFor(set_type), state.value(inst.arg0),
      state.sem_ir.coroutineTaskCompletionCapacity(set_type), builder,
      function);
}

void LLVMCoroutineCompletionSetService::finishSelection(
    LowFinishCoroutineTaskSelection inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineCompletionSetState &state) {
  CompletionSetEmitter emitter(state);
  const auto selection_type = TypeId(state.low_ir.inst(inst.arg0).type);
  auto *selection = state.value(inst.arg0);
  emitter.releaseCompletionSet(
      state.low_ir.completionProviderFor(selection_type),
      builder.CreateExtractValue(selection, 1),
      state.sem_ir.coroutineTaskCompletionCapacity(selection_type), builder,
      function);
}

} // namespace chtholly::compiler
