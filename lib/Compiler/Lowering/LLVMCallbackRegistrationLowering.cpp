#include "LLVMInternal.h"

#include <algorithm>
#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMCallbackRegistrationService::callbackAdapter(
    LowCallbackAdapter inst, LLVMCallbackState &state) {
  return state.callback_thunk(inst.arg0);
}

llvm::Value *LLVMCallbackRegistrationService::foreignFunctionRef(
    LowForeignFunctionRef inst, LLVMCallbackState &state) {
  return state.functions.at(inst.arg0.index);
}

llvm::Value *LLVMCallbackRegistrationService::indirectCall(
    LowIndirectForeignCall inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackState &state) {
  const auto &call_layout = state.low_ir.foreignAbiCallLayout(inst.arg0);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  const auto operands = state.low_ir.valueBlock(inst.arg1);
  assert(layout.callback_type.hasValue() && !layout.target.hasValue() &&
         !operands.empty());
  llvm::SmallVector<llvm::Value *, 4> arguments;
  for (const auto argument : operands.subspan(1))
    arguments.push_back(state.value(argument));
  return state.emit_foreign_call_values(
      layout, call_layout, state.value(operands.front()), arguments, builder,
      function);
}

llvm::Value *LLVMCallbackRegistrationService::adapterCall(
    LowCallbackAdapterCall inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackState &state) {
  const auto &plan = state.low_ir.callbackAdapterPlan(inst.arg0);
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.entry_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  const auto operands = state.low_ir.valueBlock(inst.arg1);
  auto *adapter = state.value(operands.front());
  auto *entry = builder.CreateExtractValue(adapter, 0, "callback.entry");
  auto *context = builder.CreateExtractValue(adapter, 1, "callback.context");
  llvm::SmallVector<llvm::Value *, 4> arguments;
  for (std::size_t parameter = 0; parameter < layout.parameters.size();
       ++parameter) {
    if (parameter == plan.context_parameter)
      arguments.push_back(context);
    else {
      const auto source =
          parameter < plan.context_parameter ? parameter + 1 : parameter;
      arguments.push_back(state.value(operands[source]));
    }
  }
  return state.emit_foreign_call_values(layout, call_layout, entry, arguments,
                                        builder, function);
}

llvm::Value *LLVMCallbackRegistrationService::makeAdapter(
    LowMakeCallbackAdapter inst, llvm::IRBuilder<> &builder,
    LLVMCallbackState &state) {
  llvm::Value *result = llvm::UndefValue::get(state.lower_value_type(inst.type));
  std::uint32_t index = 0;
  for (const auto field : state.low_ir.valueBlock(inst.arg0))
    result = builder.CreateInsertValue(result, state.value(field), index++);
  return result;
}

llvm::Value *LLVMCallbackRegistrationService::makeRegistration(
    LowMakeCallbackRegistration inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackState &state) {
  const auto &plan = state.low_ir.callbackRegistrationPlan(inst.arg0);
  const auto operands = state.low_ir.valueBlock(inst.arg1);
  const auto registration_fields = state.sem_ir.typeBlock(
      TypeBlockId(state.sem_ir.type(plan.registration_type).arg0));
  const auto fixed_count =
      registration_fields.size() == 5 ? 4U : registration_fields.size() - 1U;
  auto *callback = state.value(operands[0]);
  auto *entry = builder.CreateExtractValue(callback, 0, "registration.entry");
  auto *userdata =
      builder.CreateExtractValue(callback, 1, "registration.userdata");
  auto *release =
      builder.CreateExtractValue(callback, 2, "registration.release");
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.register_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  llvm::SmallVector<llvm::Value *, 8> arguments;
  for (std::size_t index = 0; index < layout.parameters.size(); ++index) {
    if (index == plan.entry_parameter)
      arguments.push_back(entry);
    else if (index == plan.userdata_parameter)
      arguments.push_back(userdata);
    else if (index == plan.release_parameter &&
             plan.authority == CallbackReleaseAuthority::Transferred)
      arguments.push_back(release);
    else {
      const auto binding = std::ranges::find(
          plan.binding_parameters, static_cast<std::uint32_t>(index));
      assert(binding != plan.binding_parameters.end());
      arguments.push_back(state.value(
          operands[fixed_count + static_cast<std::size_t>(
                                     binding - plan.binding_parameters.begin())]));
    }
  }
  llvm::Value *handle = state.emit_foreign_call_values(
      layout, call_layout, state.value(operands[1]), arguments, builder,
      function);
  if (!handle)
    handle = llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(state.context));
  llvm::Value *result = llvm::UndefValue::get(state.lower_value_type(inst.type));
  result = builder.CreateInsertValue(result, callback, 0);
  result = builder.CreateInsertValue(result, handle, 1);
  result = builder.CreateInsertValue(result, state.value(operands[2]), 2);
  result = builder.CreateInsertValue(result, state.value(operands[3]), 3);
  result = builder.CreateInsertValue(result, state.value(operands[1]), 4);
  if (registration_fields.size() >= 7) {
    result = builder.CreateInsertValue(result, state.value(operands[4]), 5);
    result = builder.CreateInsertValue(result, state.value(operands[5]), 6);
    if (registration_fields.size() == 8)
      result = builder.CreateInsertValue(result, state.value(operands[6]), 7);
    else if (registration_fields.size() == 10) {
      result = builder.CreateInsertValue(result, state.value(operands[6]), 7);
      result = builder.CreateInsertValue(result, state.value(operands[7]), 8);
      result = builder.CreateInsertValue(result, state.value(operands[8]), 9);
    }
  }
  return result;
}

llvm::Value *LLVMCallbackRegistrationService::registrationActive(
    LowCallbackRegistrationActive inst, llvm::IRBuilder<> &builder,
    LLVMCallbackState &state) {
  auto *handle = builder.CreateExtractValue(state.value(inst.arg0), 1,
                                            "registration.handle");
  return builder.CreateICmpNE(
      handle, llvm::ConstantPointerNull::get(
                  llvm::cast<llvm::PointerType>(handle->getType())));
}

void LLVMCallbackRegistrationService::releaseAdapter(
    llvm::Value *adapter, TypeId adapter_type, llvm::IRBuilder<> &builder,
    LLVMCallbackState &state) {
  const auto plan_id = state.low_ir.callbackAdapterPlanFor(adapter_type);
  assert(plan_id.hasValue());
  const auto &plan = state.low_ir.callbackAdapterPlan(plan_id);
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.release_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  assert(layout.parameters.size() == 1 &&
         layout.result.kind == ForeignPassKind::Ignore);
  auto *context = builder.CreateExtractValue(adapter, 1, "release.context");
  auto *release = builder.CreateExtractValue(adapter, 2, "release.entry");
  auto *call = builder.CreateCall(state.foreign_function_type(layout), release,
                                  {context});
  state.apply_foreign_attributes(*call, layout);
}

llvm::Value *LLVMCallbackRegistrationService::finishRegistration(
    CallbackRegistrationPlanId plan_id, LowValueBlockId operands, bool cancel,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMCallbackState &state) {
  const auto &plan = state.low_ir.callbackRegistrationPlan(plan_id);
  const auto registration_fields = state.sem_ir.typeBlock(
      TypeBlockId(state.sem_ir.type(plan.registration_type).arg0));
  assert(registration_fields.size() == 5 || registration_fields.size() == 7 ||
         registration_fields.size() == 8 || registration_fields.size() == 10);
  const auto callback_type = registration_fields[0];
  const auto values = state.low_ir.valueBlock(operands);
  auto *registration = state.value(values[0]);
  auto *handle =
      builder.CreateExtractValue(registration, 1, "registration.handle");
  auto *not_null = builder.CreateICmpNE(
      handle, llvm::ConstantPointerNull::get(
                  llvm::cast<llvm::PointerType>(handle->getType())));
  auto *call_block = llvm::BasicBlock::Create(
      state.context, "registration.finish", &function);
  auto *done_block = llvm::BasicBlock::Create(
      state.context, "registration.done", &function);
  builder.CreateCondBr(not_null, call_block, done_block);
  builder.SetInsertPoint(call_block);
  const auto index = cancel ? 3U : 2U;
  auto *terminal = builder.CreateExtractValue(
      registration, index,
      cancel ? "registration.cancel" : "registration.unregister");
  const auto layout_id =
      cancel ? plan.cancel_call_layout : plan.unregister_call_layout;
  const auto &call_layout = state.low_ir.foreignAbiCallLayout(layout_id);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  llvm::SmallVector<llvm::Value *, 1> args{handle};
  (void)state.emit_foreign_call_values(layout, call_layout, terminal, args,
                                       builder, function);
  builder.CreateBr(done_block);
  builder.SetInsertPoint(done_block);
  if (plan.authority == CallbackReleaseAuthority::Retained) {
    auto *callback = builder.CreateExtractValue(
        registration, 0, "registration.callback");
    releaseAdapter(callback, callback_type, builder, state);
  } else {
    auto *callback = builder.CreateExtractValue(
        registration, 0, "registration.callback.null");
    auto *null_block = llvm::BasicBlock::Create(
        state.context, "registration.release.null", &function);
    auto *after_release = llvm::BasicBlock::Create(
        state.context, "registration.release.done", &function);
    builder.SetInsertPoint(done_block);
    builder.CreateCondBr(not_null, after_release, null_block);
    builder.SetInsertPoint(null_block);
    releaseAdapter(callback, callback_type, builder, state);
    builder.CreateBr(after_release);
    builder.SetInsertPoint(after_release);
  }
  return nullptr;
}

llvm::Value *LLVMCallbackRegistrationService::cancelAsync(
    LowCallbackRegistrationCancelAsync inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCallbackState &state) {
  const auto &plan = state.low_ir.callbackRegistrationPlan(inst.arg0);
  assert(plan.cancel_async_call_layout.hasValue() &&
         plan.completion_plan.hasValue());
  const auto values = state.low_ir.valueBlock(inst.arg1);
  auto *registration = state.value(values.front());
  auto *handle =
      builder.CreateExtractValue(registration, 1, "registration.handle");
  auto *not_null = builder.CreateICmpNE(
      handle, llvm::ConstantPointerNull::get(
                  llvm::cast<llvm::PointerType>(handle->getType())));
  auto *request_block = llvm::BasicBlock::Create(
      state.context, "registration.cancel.async", &function);
  auto *done_block = llvm::BasicBlock::Create(
      state.context, "registration.cancel.requested", &function);
  auto *origin_block = builder.GetInsertBlock();
  builder.CreateCondBr(not_null, request_block, done_block);

  builder.SetInsertPoint(request_block);
  auto *cancel_async = builder.CreateExtractValue(
      registration, 5, "registration.cancel.async.entry");
  const auto &call_layout =
      state.low_ir.foreignAbiCallLayout(plan.cancel_async_call_layout);
  const auto &layout =
      state.low_ir.foreignAbiLayout(call_layout.function_layout);
  llvm::SmallVector<llvm::Value *, 1> args{handle};
  auto *requested_token = state.emit_foreign_call_values(
      layout, call_layout, cancel_async, args, builder, function);
  assert(requested_token);
  builder.CreateBr(done_block);

  builder.SetInsertPoint(done_block);
  auto *token = builder.CreatePHI(handle->getType(), 2, "completion.token");
  token->addIncoming(
      llvm::ConstantPointerNull::get(
          llvm::cast<llvm::PointerType>(handle->getType())),
      origin_block);
  token->addIncoming(requested_token, request_block);

  llvm::Value *result = llvm::UndefValue::get(state.lower_value_type(inst.type));
  result = builder.CreateInsertValue(
      result,
      builder.CreateExtractValue(registration, 0, "completion.callback"), 0);
  result = builder.CreateInsertValue(result, handle, 1);
  result = builder.CreateInsertValue(result, token, 2);
  result = builder.CreateInsertValue(
      result, builder.CreateExtractValue(registration, 6, "completion.wait"),
      3);
  const auto completion_fields = state.sem_ir.typeBlock(
      TypeBlockId(state.sem_ir.type(TypeId(inst.type)).arg0));
  if (completion_fields.size() >= 5)
    result = builder.CreateInsertValue(
        result,
        builder.CreateExtractValue(registration, 7, "completion.poll"), 4);
  if (completion_fields.size() == 7) {
    result = builder.CreateInsertValue(
        result,
        builder.CreateExtractValue(registration, 8, "completion.arm"), 5);
    result = builder.CreateInsertValue(
        result,
        builder.CreateExtractValue(registration, 9, "completion.detach"), 6);
  }
  return result;
}

} // namespace chtholly::compiler
