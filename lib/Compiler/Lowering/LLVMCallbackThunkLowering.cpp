#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace chtholly::compiler {

llvm::Function *LLVMCallbackThunkService::lower(
    ForeignAbiThunkPlanId plan_id, LLVMCallbackThunkState &state) {
  const auto key = static_cast<std::uint64_t>(plan_id.index);
  if (const auto found = state.callback_thunks.find(key);
      found != state.callback_thunks.end())
    return found->second;

  const auto &plan = state.low_ir.foreignAbiThunkPlan(plan_id);
  const auto &layout = state.low_ir.foreignAbiLayout(plan.callback_layout);
  assert(!layout.is_variadic && layout.callback_type == plan.callback_type &&
         !layout.target.hasValue() &&
         plan.parameters.size() == layout.parameters.size());
  auto *thunk = llvm::Function::Create(
      state.foreign_function_type(layout), llvm::Function::InternalLinkage,
      "$callback$" + std::to_string(plan.source.index) + "$" +
          std::to_string(plan.callback_type.index),
      state.module);
  state.apply_foreign_attributes(*thunk, layout);
  auto *entry = llvm::BasicBlock::Create(state.context, "entry", thunk);
  llvm::IRBuilder<> builder(entry);
  llvm::SmallVector<llvm::Value *, 4> semantic_arguments;
  llvm::SmallVector<llvm::AllocaInst *, 4> temporaries;
  std::uint32_t physical_index = 0;
  llvm::Value *foreign_result_slot = nullptr;
  if (plan.result.kind == ForeignAbiThunkResultKind::IndirectReturnSlot)
    foreign_result_slot = thunk->getArg(physical_index++);
  llvm::Value *semantic_result_slot = nullptr;
  if (plan.result.semantic_uses_return_slot) {
    if (foreign_result_slot)
      semantic_result_slot = foreign_result_slot;
    else {
      auto *storage = state.entry_alloca(
          *thunk, state.lower_object_type(plan.result.semantic_type),
          "callback.result");
      builder.CreateLifetimeStart(storage);
      temporaries.push_back(storage);
      semantic_result_slot = storage;
    }
    semantic_arguments.push_back(semantic_result_slot);
  }
  for (std::size_t index = 0; index < plan.parameters.size(); ++index) {
    const auto &parameter_plan = plan.parameters[index];
    const auto &parameter_layout = layout.parameters[index];
    if (parameter_plan.kind == ForeignAbiThunkParameterKind::Scalar) {
      semantic_arguments.push_back(thunk->getArg(physical_index++));
      continue;
    }
    auto *storage = state.entry_alloca(
        *thunk, state.lower_object_type(parameter_plan.semantic_type),
        "callback.parameter");
    builder.CreateLifetimeStart(storage);
    temporaries.push_back(storage);
    if (parameter_plan.kind == ForeignAbiThunkParameterKind::DirectLanes) {
      for (const auto &lane : parameter_layout.lanes) {
        auto *address = builder.CreateConstGEP1_64(
            llvm::Type::getInt8Ty(state.context), storage, lane.offset);
        builder.CreateAlignedStore(thunk->getArg(physical_index++), address,
                                   llvm::Align(1));
      }
    } else {
      state.copy_object(storage, thunk->getArg(physical_index++),
                        parameter_plan.semantic_type, builder);
    }
    semantic_arguments.push_back(
        parameter_plan.semantic_uses_object_pointer
            ? static_cast<llvm::Value *>(storage)
            : state.load_value_from_object(storage,
                                           parameter_plan.semantic_type,
                                           builder));
  }
  assert(physical_index == thunk->arg_size());
  auto *call = builder.CreateCall(
      state.functions.at(plan.source.index), semantic_arguments);
  llvm::Value *physical_result = nullptr;
  if (plan.result.kind == ForeignAbiThunkResultKind::Scalar) {
    physical_result = call;
  } else if (plan.result.kind == ForeignAbiThunkResultKind::DirectLanes) {
    llvm::SmallVector<llvm::Value *, 2> lane_values;
    const auto physical_types = state.foreign_physical_types(layout.result);
    for (std::size_t index = 0; index < layout.result.lanes.size(); ++index) {
      const auto &lane = layout.result.lanes[index];
      auto *address = builder.CreateConstGEP1_64(
          llvm::Type::getInt8Ty(state.context), semantic_result_slot,
          lane.offset);
      lane_values.push_back(
          builder.CreateAlignedLoad(physical_types[index], address,
                                    llvm::Align(1)));
    }
    if (lane_values.size() == 1)
      physical_result = lane_values.front();
    else {
      auto *aggregate = llvm::UndefValue::get(
          llvm::StructType::get(state.context, physical_types));
      physical_result = aggregate;
      for (std::size_t index = 0; index < lane_values.size(); ++index)
        physical_result = builder.CreateInsertValue(
            physical_result, lane_values[index], static_cast<unsigned>(index));
    }
  }
  for (auto iterator = temporaries.rbegin(); iterator != temporaries.rend();
       ++iterator)
    builder.CreateLifetimeEnd(*iterator);
  if (physical_result)
    builder.CreateRet(physical_result);
  else
    builder.CreateRetVoid();
  state.callback_thunks.emplace(key, thunk);
  return thunk;
}

} // namespace chtholly::compiler
