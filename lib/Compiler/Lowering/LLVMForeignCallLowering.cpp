#include "LLVMInternal.h"

#include <cassert>

namespace chtholly::compiler {

llvm::Value *LLVMForeignCallLoweringService::emitValues(
    const ForeignAbiFunctionLayout &layout,
    const ForeignAbiCallLayout &call_layout, llvm::Value *callee,
    std::span<llvm::Value *const> semantic_arguments,
    llvm::IRBuilder<> &builder, llvm::Function &function,
    LLVMForeignCallState &state) {
  llvm::SmallVector<llvm::Value *, 4> arguments;
  llvm::SmallVector<llvm::AllocaInst *, 4> parameter_copies;
  llvm::AllocaInst *result = nullptr;
  if (layout.result.kind == ForeignPassKind::Indirect) {
    result = state.entry_alloca(
        function, state.lower_object_type(layout.result.semantic_type),
        "foreign.result");
    builder.CreateLifetimeStart(result);
    arguments.push_back(result);
  }
  assert(semantic_arguments.size() ==
         layout.parameters.size() + call_layout.suffix.size());
  for (std::size_t index = 0; index < layout.parameters.size(); ++index) {
    const auto &parameter = layout.parameters[index];
    auto *semantic_value = semantic_arguments[index];
    if (parameter.kind == ForeignPassKind::Scalar) {
      arguments.push_back(semantic_value);
      continue;
    }
    if (parameter.kind == ForeignPassKind::Indirect) {
      auto *copy = state.entry_alloca(
          function, state.lower_object_type(parameter.semantic_type),
          "foreign.byval");
      builder.CreateLifetimeStart(copy);
      state.copy_object(copy, semantic_value, parameter.semantic_type, builder);
      parameter_copies.push_back(copy);
      arguments.push_back(copy);
      continue;
    }
    for (const auto &lane : parameter.lanes) {
      auto *address = builder.CreateConstGEP1_64(
          llvm::Type::getInt8Ty(state.context), semantic_value, lane.offset);
      arguments.push_back(builder.CreateAlignedLoad(
          state.lower_foreign_lane(lane), address, llvm::Align(1)));
    }
  }
  for (std::size_t index = 0; index < call_layout.suffix.size(); ++index)
    arguments.push_back(semantic_arguments[layout.parameters.size() + index]);
  auto *call = builder.CreateCall(state.foreign_function_type(layout), callee,
                                  arguments);
  state.apply_foreign_attributes(*call, layout);
  for (auto iterator = parameter_copies.rbegin();
       iterator != parameter_copies.rend(); ++iterator)
    builder.CreateLifetimeEnd(*iterator);
  if (layout.result.kind == ForeignPassKind::Direct) {
    result = state.entry_alloca(
        function, state.lower_object_type(layout.result.semantic_type),
        "foreign.result");
    builder.CreateLifetimeStart(result);
    for (std::size_t lane_index = 0; lane_index < layout.result.lanes.size();
         ++lane_index) {
      const auto &lane = layout.result.lanes[lane_index];
      auto *lane_value = layout.result.lanes.size() == 1
                             ? static_cast<llvm::Value *>(call)
                             : builder.CreateExtractValue(
                                   call, static_cast<unsigned>(lane_index));
      auto *address = builder.CreateConstGEP1_64(
          llvm::Type::getInt8Ty(state.context), result, lane.offset);
      builder.CreateAlignedStore(lane_value, address, llvm::Align(1));
    }
  }
  return result ? static_cast<llvm::Value *>(result)
         : layout.result.kind == ForeignPassKind::Ignore
             ? nullptr
             : static_cast<llvm::Value *>(call);
}

} // namespace chtholly::compiler
