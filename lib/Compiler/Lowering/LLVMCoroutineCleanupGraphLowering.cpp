#include "LLVMInternal.h"

#include <string>

namespace chtholly::compiler {

void LLVMCoroutineScaffoldService::cleanupGraph(llvm::Function &function,
                                 llvm::StructType *frame_type,
                                 llvm::Value *frame,
                                 const CoroutineFramePlan &plan,
                                 CoroutineCleanupGraphId graph_id,
                                 llvm::BasicBlock *start,
                                 llvm::BasicBlock *terminal, std::string_view prefix,
                                 bool preserve_result, LLVMCoroutineScaffoldState &state) {
    const auto saved_blocks = state.blocks;
    const auto saved_slots = state.slots;
    const auto saved_values = state.values;
    const auto saved_place_flags = state.place_flags;
    const auto saved_bitmap = state.coroutine.bitmap;
    const auto saved_place_bits = state.coroutine.place_bits;
    state.blocks.clear();
    state.values.clear();
    llvm::IRBuilder<> setup(start);
    const auto &graph = state.low_ir.coroutineCleanupGraph(graph_id);
    if (preserve_result) {
      state.slots.clear();
      state.place_flags.clear();
      state.coroutine.place_bits.clear();
      state.coroutine.bitmap = setup.CreateStructGEP(frame_type, frame, 1);
      for (std::size_t slot_index = 0; slot_index < plan.lifted_slots.size();
           ++slot_index)
        state.slots.emplace(
            plan.lifted_slots[slot_index].index,
            setup.CreateStructGEP(frame_type, frame,
                                  static_cast<unsigned>(6 + slot_index)));
      for (const auto &frame_place : plan.frame_places)
        state.coroutine.place_bits.emplace(frame_place.place.index,
                                              frame_place.initialization_bit);
      for (const auto slot : graph.local_slots)
        state.slots.emplace(slot.index,
                       state.entry_alloca(function,
                                   state.lower_object_type(state.low_ir.slot(slot).type),
                                   std::string(prefix) + ".local"));
      for (std::uint32_t place_index = 0; place_index < state.low_ir.placeCount();
           ++place_index) {
        const auto place = LowPlaceId(place_index);
        if (!state.slots.contains(state.low_ir.place(place).root.index))
          continue;
        auto *flag = state.entry_alloca(function, llvm::Type::getInt1Ty(state.context),
                                 std::string(prefix) + ".place.init");
        llvm::Value *initialized = llvm::ConstantInt::getFalse(state.context);
        if (const auto found = state.coroutine.place_bits.find(place.index);
            found != state.coroutine.place_bits.end())
          initialized = state.test_initialization_bit(found->second, setup);
        setup.CreateStore(initialized, flag);
        state.place_flags.emplace(place_index, flag);
      }
    }
    for (const auto block : state.low_ir.blockList(graph.blocks))
      state.blocks.emplace(
          block.index,
          block == graph.entry
              ? start
              : llvm::BasicBlock::Create(
                    state.context, std::string(prefix) + ".block." + std::to_string(block.index),
                    &function));
    for (const auto block : state.low_ir.blockList(graph.blocks)) {
      llvm::IRBuilder<> builder(state.blocks.at(block.index));
      for (const auto instruction : state.low_ir.block(block)) {
        if (state.low_ir.inst(instruction).kind ==
            LowInstKind::CoroutineCleanupEnd) {
          builder.CreateBr(terminal);
          continue;
        }
        state.lower_instruction(instruction, builder, function);
      }
    }
    state.blocks = saved_blocks;
    state.slots = saved_slots;
    state.values = saved_values;
    state.place_flags = saved_place_flags;
    state.coroutine.bitmap = saved_bitmap;
    state.coroutine.place_bits = saved_place_bits;
  }

} // namespace chtholly::compiler
