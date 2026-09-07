#include "LLVMInternal.h"

#include <algorithm>
#include <cassert>

namespace chtholly::compiler {
namespace {

class CallbackWakeEmitter {
public:
  explicit CallbackWakeEmitter(LLVMCallbackWakeState &state) : state_(state) {}
  void emitWakeAdapterRelease(llvm::Value *adapter,
                              const CallbackWakePlan &plan,
                              llvm::IRBuilder<> &builder) {
    const auto &call_layout =
        state_.callback.low_ir.foreignAbiCallLayout(plan.wake_release_call_layout);
    const auto &layout = state_.callback.low_ir.foreignAbiLayout(call_layout.function_layout);
    assert(layout.parameters.size() == 1 &&
           layout.result.kind == ForeignPassKind::Ignore);
    auto *context =
        builder.CreateExtractValue(adapter, 1, "wake.release.context");
    auto *release =
        builder.CreateExtractValue(adapter, 2, "wake.release.entry");
    auto *call =
        builder.CreateCall(foreignFunctionType(layout), release, {context});
    applyForeignAttributes(*call, layout);
  }

  llvm::Value *lowerInst(LowMakeCallbackWake inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto &plan = state_.callback.low_ir.callbackWakePlan(inst.arg0);
    const auto operands = state_.callback.low_ir.valueBlock(inst.arg1);
    auto *completion = value(operands[0]);
    auto *adapter = value(operands[1]);
    if (state_.coroutine.task) {
      // The scaffold adapter is an internal placeholder. Coroutine lowering
      // consumes it locally and substitutes the retained runtime task waker.
      emitWakeAdapterRelease(adapter, plan, builder);
      builder.CreateCall(
          state_.callback.module.getOrInsertFunction(
              "chtholly_next_task_v1_task_retain",
              llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                      false)),
          {state_.coroutine.task});
      auto *adapter_type =
          lowerValueType(TypeId(state_.callback.low_ir.inst(operands[1]).type));
      llvm::Value *task_adapter = llvm::UndefValue::get(adapter_type);
      task_adapter = builder.CreateInsertValue(
          task_adapter, state_.coroutine.wake_entry, 0);
      task_adapter =
          builder.CreateInsertValue(task_adapter, state_.coroutine.task, 1);
      adapter = builder.CreateInsertValue(task_adapter,
                                          state_.coroutine.wake_release, 2);
    }
    auto *token = builder.CreateExtractValue(completion, 2, "wake.arm.token");
    auto *is_null = builder.CreateICmpEQ(
        token, llvm::ConstantPointerNull::get(
                   llvm::cast<llvm::PointerType>(token->getType())));
    auto *arm_block = llvm::BasicBlock::Create(state_.callback.context, "wake.arm", &function);
    auto *release_block =
        llvm::BasicBlock::Create(state_.callback.context, "wake.release", &function);
    auto *done_block =
        llvm::BasicBlock::Create(state_.callback.context, "wake.arm.done", &function);
    builder.CreateCondBr(is_null, release_block, arm_block);

    builder.SetInsertPoint(arm_block);
    auto *arm = builder.CreateExtractValue(completion, 5, "wake.arm.entry");
    auto *entry = builder.CreateExtractValue(adapter, 0, "wake.entry");
    auto *userdata = builder.CreateExtractValue(adapter, 1, "wake.userdata");
    auto *release = builder.CreateExtractValue(adapter, 2, "wake.release.fn");
    const auto &call_layout =
        state_.callback.low_ir.foreignAbiCallLayout(plan.arm_call_layout);
    const auto &layout = state_.callback.low_ir.foreignAbiLayout(call_layout.function_layout);
    llvm::SmallVector<llvm::Value *, 4> arguments(layout.parameters.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (index == plan.arm_parameters[0])
        arguments[index] = token;
      else if (index == plan.arm_parameters[1])
        arguments[index] = entry;
      else if (index == plan.arm_parameters[2])
        arguments[index] = userdata;
      else if (index == plan.arm_parameters[3])
        arguments[index] = release;
      else
        llvm_unreachable("unassigned callback arm ABI parameter");
    }
    auto *ready = emitForeignCallValues(layout, call_layout, arm, arguments,
                                        builder, function);
    assert(ready && ready->getType()->isIntegerTy(1));
    builder.CreateCondBr(ready, release_block, done_block);

    builder.SetInsertPoint(release_block);
    emitWakeAdapterRelease(adapter, plan, builder);
    builder.CreateBr(done_block);

    builder.SetInsertPoint(done_block);
    auto *ready_result =
        builder.CreatePHI(llvm::Type::getInt1Ty(state_.callback.context), 2, "wake.ready");
    ready_result->addIncoming(ready, arm_block);
    ready_result->addIncoming(llvm::ConstantInt::getTrue(state_.callback.context),
                              release_block);
    llvm::Value *result = llvm::UndefValue::get(lowerValueType(inst.type));
    result = builder.CreateInsertValue(result, completion, 0);
    return builder.CreateInsertValue(result, ready_result, 1);
  }

  llvm::Value *lowerInst(LowForeignOperationProjectCompletion inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    const auto &projection = state_.callback.low_ir.foreignOperationCompletionPlan(inst.arg0);
    const auto operands = state_.callback.low_ir.valueBlock(inst.arg1);
    assert(operands.size() == 2);
    const interop::CompletionFamily *family = nullptr;
    for (std::uint32_t index = 0; index < state_.callback.sem_ir.functionRefCount(); ++index) {
      const auto &reference = state_.callback.sem_ir.functionRef(FunctionRefId(index));
      const auto *entity =
          state_.callback.sem_ir.importIRs().tryGetEntity(reference.public_entity);
      const auto *operation =
          entity && entity->interop_artifact
              ? state_.callback.sem_ir.importIRs().interopRegistry().resolve(
                    *entity->interop_artifact)
              : nullptr;
      if (operation &&
          entity->interop_artifact->fingerprint ==
              projection.operation_fingerprint &&
          operation->completion_descriptor) {
        family = &*operation->completion_descriptor;
        break;
      }
    }
    assert(family);
    const auto member_function = [&](const auto &operation_ref) {
      for (std::uint32_t index = 0; index < state_.callback.sem_ir.functionRefCount();
           ++index) {
        const auto id = FunctionRefId(index);
        const auto &reference = state_.callback.sem_ir.functionRef(id);
        const auto *entity =
            state_.callback.sem_ir.importIRs().tryGetEntity(reference.public_entity);
        if (entity && entity->interop_artifact &&
            entity->interop_artifact->fingerprint ==
                operation_ref.expected_fingerprint &&
            state_.callback.sem_ir.importIRs().interopRegistry().resolve(
                *entity->interop_artifact))
          return state_.callback.functions.at(id.index);
      }
      return static_cast<llvm::Function *>(nullptr);
    };
    auto *subscription = value(operands[0]);
    auto *token = value(operands[1]);
    auto *wait = member_function(family->wait);
    auto *poll = member_function(family->poll);
    auto *arm = member_function(family->arm);
    auto *detach = member_function(family->detach);
    assert(wait && poll && arm && detach);
    llvm::Value *result = llvm::UndefValue::get(lowerValueType(inst.type));
    const auto completion_fields =
        state_.callback.sem_ir.typeBlock(TypeBlockId(state_.callback.sem_ir.type(TypeId(inst.type)).arg0));
    llvm::Value *callback =
        llvm::UndefValue::get(lowerValueType(completion_fields[0]));
    for (std::uint32_t field = 0; field < 3; ++field)
      callback = builder.CreateInsertValue(
          callback, builder.CreateExtractValue(subscription, field), field);
    result = builder.CreateInsertValue(result, callback, 0);
    result = builder.CreateInsertValue(
        result, builder.CreateExtractValue(subscription, 3), 1);
    result = builder.CreateInsertValue(result, token, 2);
    result = builder.CreateInsertValue(result, wait, 3);
    result = builder.CreateInsertValue(result, poll, 4);
    result = builder.CreateInsertValue(result, arm, 5);
    return builder.CreateInsertValue(result, detach, 6);
  }

  llvm::Value *lowerInst(LowForeignOperationProjectWake inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto &projection = state_.callback.low_ir.foreignOperationCompletionPlan(inst.arg0);
    const auto operands = state_.callback.low_ir.valueBlock(inst.arg1);
    assert(operands.size() == 3);
    auto *completion = value(operands[0]);
    auto *adapter = value(operands[1]);
    auto *scalar = value(operands[2]);
    llvm::Value *ready = scalar;
    if (!scalar->getType()->isIntegerTy(1))
      ready = builder.CreateICmpEQ(
          scalar, llvm::ConstantInt::get(scalar->getType(),
                                         projection.readiness_success_literal));

    if (projection.wake_plan.hasValue()) {
      auto *release_block = llvm::BasicBlock::Create(
          state_.callback.context, "operation.wake.release", &function);
      auto *publish_block = llvm::BasicBlock::Create(
          state_.callback.context, "operation.wake.publish", &function);
      builder.CreateCondBr(ready, release_block, publish_block);
      builder.SetInsertPoint(release_block);
      emitWakeAdapterRelease(
          adapter, state_.callback.low_ir.callbackWakePlan(projection.wake_plan), builder);
      builder.CreateBr(publish_block);
      builder.SetInsertPoint(publish_block);
    }

    llvm::Value *result = llvm::UndefValue::get(lowerValueType(inst.type));
    result = builder.CreateInsertValue(result, completion, 0);
    return builder.CreateInsertValue(result, ready, 1);
  }

  llvm::Value *lowerInst(LowForeignOperationPortProject inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    const auto &plan = state_.callback.low_ir.foreignOperationCompletionPlan(inst.arg0);
    const auto operands = state_.callback.low_ir.valueBlock(inst.arg1);
    assert(operands.size() == 2 || operands.size() == 5);
    const auto lane = static_cast<std::uint32_t>(
        state_.callback.sem_ir.integer(IntegerId(state_.callback.low_ir.inst(operands.back()).arg0)));
    auto *source = value(operands[0]);
    const auto source_kind =
        state_.callback.sem_ir.type(TypeId(state_.callback.low_ir.inst(operands[0]).type)).kind;
    if (source_kind == SemTypeKind::CallbackAdapter) {
      const auto found = std::ranges::find(plan.wake_callback_lanes, lane);
      assert(found != plan.wake_callback_lanes.end());
      return builder.CreateExtractValue(
          source,
          static_cast<unsigned>(found - plan.wake_callback_lanes.begin()),
          "operation.callback.lane");
    }
    const auto source_owner = state_.callback.sem_ir.foreignOperationStateOwner(
        TypeId(state_.callback.low_ir.inst(operands[0]).type));
    if (plan.projection ==
            interop::ForeignCompletionProjectionKind::ScalarToSubscription &&
        plan.result_lane == lane) {
      assert(operands.size() == 5);
      llvm::Value *result = llvm::UndefValue::get(lowerValueType(inst.type));
      for (std::uint32_t field = 0; field < 4; ++field)
        result =
            builder.CreateInsertValue(result, value(operands[field]), field);
      return result;
    }
    if (source_owner &&
        source_owner->state == ForeignOperationStateKind::Subscription)
      return builder.CreateExtractValue(source, 3,
                                        "operation.subscription.handle");
    if (source_kind == SemTypeKind::CallbackWake)
      source =
          builder.CreateExtractValue(source, 0, "operation.wake.completion");
    assert(source_kind == SemTypeKind::CallbackCompletion ||
           source_kind == SemTypeKind::CallbackWake);
    assert(plan.carrier_lane == lane);
    return builder.CreateExtractValue(source, 2, "operation.completion.token");
  }

  llvm::Value *lowerInst(LowCallbackWakeReady inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return builder.CreateExtractValue(value(inst.arg0), 1, "wake.ready");
  }

  llvm::Value *finishCallbackWake(CallbackWakePlanId plan_id,
                                  LowValueBlockId operands,
                                  llvm::IRBuilder<> &builder,
                                  llvm::Function &function) {
    const auto &plan = state_.callback.low_ir.callbackWakePlan(plan_id);
    auto *wake = value(state_.callback.low_ir.valueBlock(operands).front());
    auto *completion = builder.CreateExtractValue(wake, 0, "wake.completion");
    return finishCallbackCompletionValue(plan.completion_plan, completion,
                                         builder, function);
  }

  llvm::Value *lowerInst(LowFinishCallbackWake inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return finishCallbackWake(inst.arg0, inst.arg1, builder, function);
  }

  llvm::Value *lowerInst(LowCallbackWakeWait inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return finishCallbackWake(inst.arg0, inst.arg1, builder, function);
  }

  llvm::Value *lowerInst(LowCallbackDetach inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto &plan = state_.callback.low_ir.callbackWakePlan(inst.arg0);
    const auto operand = state_.callback.low_ir.valueBlock(inst.arg1).front();
    auto *stored = value(operand);
    auto *completion =
        state_.callback.sem_ir.type(TypeId(state_.callback.low_ir.inst(operand).type)).kind ==
                SemTypeKind::CallbackWake
            ? builder.CreateExtractValue(stored, 0, "detach.completion")
            : stored;
    auto *token = builder.CreateExtractValue(completion, 2, "detach.token");
    auto *callback =
        builder.CreateExtractValue(completion, 0, "detach.callback");
    auto *is_null = builder.CreateICmpEQ(
        token, llvm::ConstantPointerNull::get(
                   llvm::cast<llvm::PointerType>(token->getType())));
    auto *local_block =
        llvm::BasicBlock::Create(state_.callback.context, "detach.local", &function);
    auto *call_block =
        llvm::BasicBlock::Create(state_.callback.context, "detach.foreign", &function);
    auto *done_block =
        llvm::BasicBlock::Create(state_.callback.context, "detach.done", &function);
    builder.CreateCondBr(is_null, local_block, call_block);

    builder.SetInsertPoint(local_block);
    const auto completion_fields =
        state_.callback.sem_ir.typeBlock(TypeBlockId(state_.callback.sem_ir.type(plan.completion_type).arg0));
    if (plan.authority == CallbackReleaseAuthority::Retained)
      emitCallbackAdapterRelease(callback, completion_fields[0], builder);
    builder.CreateBr(done_block);

    builder.SetInsertPoint(call_block);
    auto *detach = builder.CreateExtractValue(completion, 6, "detach.entry");
    const auto &call_layout =
        state_.callback.low_ir.foreignAbiCallLayout(plan.detach_call_layout);
    const auto &layout = state_.callback.low_ir.foreignAbiLayout(call_layout.function_layout);
    llvm::SmallVector<llvm::Value *, 3> arguments(layout.parameters.size());
    llvm::Value *userdata = nullptr;
    llvm::Value *callback_release = nullptr;
    if (plan.authority == CallbackReleaseAuthority::Retained) {
      userdata = builder.CreateExtractValue(callback, 1, "detach.userdata");
      callback_release =
          builder.CreateExtractValue(callback, 2, "detach.release");
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (index == plan.detach_parameters[0])
        arguments[index] = token;
      else if (index == plan.detach_parameters[1] && userdata)
        arguments[index] = userdata;
      else if (index == plan.detach_parameters[2] && callback_release)
        arguments[index] = callback_release;
      else
        llvm_unreachable("unassigned callback detach ABI parameter");
    }
    (void)emitForeignCallValues(layout, call_layout, detach, arguments, builder,
                                function);
    builder.CreateBr(done_block);
    builder.SetInsertPoint(done_block);
    return nullptr;
  }

private:
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return state_.callback.value(id);
  }
  [[nodiscard]] llvm::Type *lowerValueType(TypeId type) const {
    return state_.callback.lower_value_type(type);
  }
  [[nodiscard]] llvm::FunctionType *foreignFunctionType(
      const ForeignAbiFunctionLayout &layout) const {
    return state_.callback.foreign_function_type(layout);
  }
  void applyForeignAttributes(llvm::CallBase &call,
                              const ForeignAbiFunctionLayout &layout) const {
    state_.callback.apply_foreign_attributes(call, layout);
  }
  [[nodiscard]] llvm::Value *emitForeignCallValues(
      const ForeignAbiFunctionLayout &layout,
      const ForeignAbiCallLayout &call_layout, llvm::Value *callee,
      std::span<llvm::Value *const> arguments, llvm::IRBuilder<> &builder,
      llvm::Function &function) const {
    return state_.callback.emit_foreign_call_values(
        layout, call_layout, callee, arguments, builder, function);
  }
  void emitCallbackAdapterRelease(llvm::Value *adapter, TypeId type,
                                  llvm::IRBuilder<> &builder) const {
    LLVMCallbackRegistrationService::releaseAdapter(
        adapter, type, builder, state_.callback);
  }
  [[nodiscard]] llvm::Value *finishCallbackCompletionValue(
      CallbackCompletionPlanId plan, llvm::Value *completion,
      llvm::IRBuilder<> &builder, llvm::Function &function) const {
    return LLVMCallbackCompletionService::finishValue(
        plan, completion, builder, function, state_.callback);
  }

  LLVMCallbackWakeState &state_;
};

} // namespace

void LLVMCallbackWakeService::releaseAdapter(
    llvm::Value *adapter, const CallbackWakePlan &plan,
    llvm::IRBuilder<> &builder, LLVMCallbackWakeState &state) {
  CallbackWakeEmitter(state).emitWakeAdapterRelease(adapter, plan, builder);
}

llvm::Value *LLVMCallbackWakeService::makeWake(
    LowMakeCallbackWake inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCallbackWakeService::projectCompletion(
    LowForeignOperationProjectCompletion inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCallbackWakeService::projectWake(
    LowForeignOperationProjectWake inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCallbackWakeService::projectPort(
    LowForeignOperationPortProject inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCallbackWakeService::ready(
    LowCallbackWakeReady inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).lowerInst(inst, builder, function);
}

llvm::Value *LLVMCallbackWakeService::finish(
    CallbackWakePlanId plan_id, LowValueBlockId operands,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).finishCallbackWake(
      plan_id, operands, builder, function);
}

llvm::Value *LLVMCallbackWakeService::detach(
    LowCallbackDetach inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackWakeState &state) {
  return CallbackWakeEmitter(state).lowerInst(inst, builder, function);
}

} // namespace chtholly::compiler