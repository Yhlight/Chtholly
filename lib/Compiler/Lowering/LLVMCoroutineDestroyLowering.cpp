#include "LLVMInternal.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"

#include <cassert>

namespace chtholly::compiler {

void LLVMCoroutineScaffoldService::destroyAddress(
    TypeId type, llvm::Value *address, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMCoroutineScaffoldState &state) {
  const auto destroy = [&](const auto &self, TypeId current_type,
                           llvm::Value *current_address) -> void {
    const auto kind = state.sem_ir.type(current_type).kind;
    const auto &representation =
        state.low_ir.typeRepresentation(current_type);
    if (kind == SemTypeKind::CallbackAdapter) {
      state.release_callback_adapter(
          builder.CreateLoad(state.lower_value_type(current_type),
                             current_address),
          current_type, builder);
      return;
    }
    if (kind == SemTypeKind::CallbackCompletion) {
      state.finish_callback_completion(
          state.low_ir.callbackCompletionPlanFor(current_type),
          builder.CreateLoad(state.lower_value_type(current_type),
                             current_address),
          builder, function);
      return;
    }
    if (kind == SemTypeKind::CallbackWake) {
      const auto wake = builder.CreateLoad(state.lower_value_type(current_type),
                                           current_address);
      const auto plan = state.low_ir.callbackWakePlanFor(current_type);
      state.finish_callback_completion(
          state.low_ir.callbackWakePlan(plan).completion_plan,
          builder.CreateExtractValue(wake, 0), builder, function);
      return;
    }
    if (kind == SemTypeKind::CoroutineTaskCompletion) {
      builder.CreateCall(
          state.module.getOrInsertFunction(
              "chtholly_next_task_v1_completion_release",
              llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                       false)),
          {builder.CreateLoad(state.lower_value_type(current_type),
                              current_address)});
      return;
    }
    if (kind == SemTypeKind::CoroutineTaskCompletionSet) {
      state.release_completion_set(
          state.low_ir.completionProviderFor(current_type),
          builder.CreateLoad(state.lower_value_type(current_type),
                             current_address),
          state.sem_ir.coroutineTaskCompletionCapacity(current_type), builder,
          function);
      return;
    }
    if (kind == SemTypeKind::CoroutineTaskSelection) {
      auto *selection = builder.CreateLoad(state.lower_value_type(current_type),
                                           current_address);
      state.release_completion_set(
          state.low_ir.completionProviderFor(current_type),
          builder.CreateExtractValue(selection, 1),
          state.sem_ir.coroutineTaskCompletionCapacity(current_type), builder,
          function);
      return;
    }
    if (representation.object_drop_target.hasValue()) {
      builder.CreateCall(state.functions.at(
                             representation.object_drop_target.index),
                         {current_address});
      return;
    }
    if (representation.destroy_target.hasValue()) {
      builder.CreateCall(
          state.functions.at(representation.destroy_target.index),
          {current_address});
      return;
    }
    if (representation.facts.destroy != DestroyReprKind::Trivial)
      return;
    for (std::size_t index = representation.object_fields.size(); index != 0;
         --index) {
      const auto field = static_cast<unsigned>(index - 1);
      const auto field_type = representation.object_fields[field];
      if (state.low_ir.typeRepresentation(field_type).facts.destroy ==
          DestroyReprKind::None)
        continue;
      self(self, field_type,
           builder.CreateStructGEP(state.lower_object_type(current_type),
                                   current_address, field));
    }
  };
  destroy(destroy, type, address);
}

} // namespace chtholly::compiler
