#include "LLVMInternal.h"

#include <algorithm>

namespace chtholly::compiler {

void LLVMCoroutineScaffoldService::cleanupChain(
      llvm::Function &function, llvm::StructType *frame_type,
      llvm::Value *frame, const CoroutineFramePlan &plan,
      const std::unordered_map<std::uint32_t, unsigned> &value_fields,
      llvm::BasicBlock *start, llvm::BasicBlock *terminal,
      std::string_view prefix, LLVMCoroutineScaffoldState &scaffold_state) {
  const auto detach_value = [&](const CallbackWakePlan &wake_plan,
                                llvm::Value *wake,
                                llvm::IRBuilder<> &detach_builder,
                                llvm::Function &detach_function) {
    LLVMCoroutineScaffoldService::detachCompletion(
        wake_plan, detach_builder.CreateExtractValue(wake, 0),
        detach_builder, detach_function, scaffold_state);
  };
    auto *state_address = llvm::IRBuilder<>(start).CreateStructGEP(
        frame_type, frame, 0, "task.cleanup.state.address");
    llvm::IRBuilder<> dispatch(start);
    auto *state = dispatch.CreateLoad(dispatch.getInt32Ty(), state_address);
    auto *after_detach = llvm::BasicBlock::Create(
        scaffold_state.context, std::string(prefix) + ".cleanup.begin", &function);
    const auto bit_address = [&](llvm::IRBuilder<> &builder,
                                 std::uint32_t bit) {
      auto *bitmap = builder.CreateStructGEP(frame_type, frame, 1);
      return builder.CreateInBoundsGEP(builder.getInt64Ty(), bitmap,
                                       builder.getInt32(bit / 64));
    };
    const auto test_bit = [&](llvm::IRBuilder<> &builder, std::uint32_t bit) {
      auto *address = bit_address(builder, bit);
      auto *word = builder.CreateLoad(builder.getInt64Ty(), address);
      return builder.CreateICmpNE(
          builder.CreateAnd(word, builder.getInt64(UINT64_C(1) << (bit % 64))),
          builder.getInt64(0));
    };
    const auto clear_bit = [&](llvm::IRBuilder<> &builder, std::uint32_t bit) {
      auto *address = bit_address(builder, bit);
      auto *word = builder.CreateLoad(builder.getInt64Ty(), address);
      builder.CreateStore(
          builder.CreateAnd(word,
                            builder.getInt64(~(UINT64_C(1) << (bit % 64)))),
          address);
    };
    const auto frame_place_address = [&](llvm::IRBuilder<> &builder,
                                         CoroutineFramePlaceId id) {
      const auto place_id = plan.frame_places[id.index].place;
      const auto &place = scaffold_state.low_ir.place(place_id);
      const auto slot_position =
          std::ranges::find(plan.lifted_slots, place.root) -
          plan.lifted_slots.begin();
      llvm::Value *address = builder.CreateStructGEP(
          frame_type, frame, static_cast<unsigned>(6 + slot_position));
      for (const auto projection :
           scaffold_state.low_ir.placeProjections(place.projections)) {
        auto *aggregate = scaffold_state.lower_object_type(projection.aggregate_type);
        if (projection.kind == LowPlaceProjectionKind::StructField) {
          address =
              builder.CreateStructGEP(aggregate, address, projection.index);
        } else if (projection.kind == LowPlaceProjectionKind::EnumPayload) {
          address = scaffold_state.enum_payload_address(
              address, projection.aggregate_type, projection.variant,
              projection.index, builder);
        } else if (projection.kind == LowPlaceProjectionKind::ArrayElement) {
          address = builder.CreateInBoundsGEP(
              aggregate, address,
              {builder.getInt32(0), builder.getInt32(projection.index)});
        } else {
          llvm_unreachable("frame places cannot contain dereference");
        }
      }
      return address;
    };
    const auto emit_place_cleanup =
        [&](llvm::BasicBlock *begin,
            std::span<const CoroutineFramePlaceId> cleanup_order,
            std::string_view suffix) {
          auto *current = begin;
          for (std::size_t index = 0; index < cleanup_order.size(); ++index) {
            auto *test = current;
            const auto label = std::string(prefix) + ".cleanup" +
                               std::string(suffix) + "." +
                               std::to_string(index);
            auto *destroy = llvm::BasicBlock::Create(
                scaffold_state.context, label + ".destroy", &function);
            auto *next = index + 1 == cleanup_order.size()
                             ? terminal
                             : llvm::BasicBlock::Create(
                                   scaffold_state.context, label + ".next", &function);
            llvm::IRBuilder<> test_builder(test);
            const auto frame_place = cleanup_order[index];
            const auto bit =
                plan.frame_places[frame_place.index].initialization_bit;
            test_builder.CreateCondBr(test_bit(test_builder, bit), destroy,
                                      next);
            llvm::IRBuilder<> destroy_builder(destroy);
            const auto path = scaffold_state.low_ir.logicalPlaceProjections(
                plan.frame_places[frame_place.index].place);
            for (const auto &candidate : plan.frame_places) {
              const auto candidate_place = scaffold_state.low_ir.place(candidate.place);
              const auto candidate_path =
                  scaffold_state.low_ir.logicalPlaceProjections(candidate.place);
              if (candidate_place.root ==
                      scaffold_state.low_ir.place(plan.frame_places[frame_place.index].place)
                          .root &&
                  scaffold_state.path_prefix(path, candidate_path))
                clear_bit(destroy_builder, candidate.initialization_bit);
            }
            LLVMCoroutineScaffoldService::destroyAddress(
                scaffold_state.low_ir.place(plan.frame_places[frame_place.index].place).type,
                frame_place_address(destroy_builder, frame_place),
                destroy_builder, function, scaffold_state);
            destroy_builder.CreateBr(next);
            current = next;
          }
          if (cleanup_order.empty())
            llvm::IRBuilder<>(begin).CreateBr(terminal);
        };
    const auto emit_language_cleanup =
        [&](llvm::BasicBlock *begin, CoroutineCleanupGraphId graph_id,
            std::span<const CoroutineFramePlaceId> fallback,
            std::string_view suffix) {
          if (graph_id.hasValue()) {
            LLVMCoroutineScaffoldService::cleanupGraph(
                function, frame_type, frame, plan, graph_id, begin, terminal,
                std::string(prefix) + std::string(suffix), true, scaffold_state);
          } else {
            emit_place_cleanup(begin, fallback, suffix);
          }
        };
    auto *state_switch = dispatch.CreateSwitch(
        state, after_detach, static_cast<unsigned>(plan.resume_states.size()));
    for (const auto &resume_state : plan.resume_states) {
      auto *detach = llvm::BasicBlock::Create(
          scaffold_state.context,
          std::string(prefix) + ".detach." + std::to_string(resume_state.state),
          &function);
      state_switch->addCase(dispatch.getInt32(resume_state.state), detach);
      llvm::IRBuilder<> detach_builder(detach);
      const auto found =
          std::ranges::find(plan.lifted_slots, resume_state.wake_slot) -
          plan.lifted_slots.begin();
      const auto root_place = std::ranges::find_if(
          plan.frame_places, [&](const CoroutineFramePlace &candidate) {
            return scaffold_state.low_ir.place(candidate.place).root ==
                       resume_state.wake_slot &&
                   scaffold_state.low_ir.logicalPlaceProjections(candidate.place).empty();
          });
      const auto root_bit = root_place->initialization_bit;
      auto *owned = test_bit(detach_builder, root_bit);
      auto *call =
          llvm::BasicBlock::Create(scaffold_state.context,
                                   std::string(prefix) + ".detach.call." +
                                       std::to_string(resume_state.state),
                                   &function);
      auto *done =
          llvm::BasicBlock::Create(scaffold_state.context,
                                   std::string(prefix) + ".detach.done." +
                                       std::to_string(resume_state.state),
                                   &function);
      detach_builder.CreateCondBr(owned, call, done);
      llvm::IRBuilder<> call_builder(call);
      auto *wake_address = call_builder.CreateStructGEP(
          frame_type, frame, static_cast<unsigned>(6 + found));
      auto *wake = call_builder.CreateLoad(
          scaffold_state.lower_value_type(scaffold_state.low_ir.slot(resume_state.wake_slot).type),
          wake_address);
      if (resume_state.suspension_kind ==
          CoroutineResumeState::SuspensionKind::CallbackWake) {
        detach_value(
            scaffold_state.low_ir.callbackWakePlan(resume_state.wake_plan), wake,
            call_builder, function);
      } else if (resume_state.suspension_kind ==
                 CoroutineResumeState::SuspensionKind::TaskCompletion) {
        call_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_completion_release",
                llvm::FunctionType::get(call_builder.getVoidTy(),
                                        {call_builder.getPtrTy()}, false)),
            {wake});
      } else if (resume_state.suspension_kind ==
                 CoroutineResumeState::SuspensionKind::TaskCompletionSet) {
        const auto set_type = scaffold_state.low_ir.slot(resume_state.wake_slot).type;
        scaffold_state.detach_completion_set(scaffold_state.low_ir.completionProviderFor(set_type), wake,
                            scaffold_state.sem_ir.coroutineTaskCompletionCapacity(set_type),
                            call_builder, function);
      } else {
        call_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_completion_release",
                llvm::FunctionType::get(call_builder.getVoidTy(),
                                        {call_builder.getPtrTy()}, false)),
            {wake});
      }
      clear_bit(call_builder, root_bit);
      call_builder.CreateBr(done);
      llvm::IRBuilder<> state_cleanup(done);
      if (resume_state.suspension_kind ==
          CoroutineResumeState::SuspensionKind::TaskGroupDrain) {
        auto *group = state_cleanup.CreateLoad(
            state_cleanup.getPtrTy(),
            state_cleanup.CreateStructGEP(
                frame_type, frame,
                value_fields.at(resume_state.task_group.index)));
        state_cleanup.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_task_group_release",
                llvm::FunctionType::get(state_cleanup.getVoidTy(),
                                        {state_cleanup.getPtrTy()}, false)),
            {group});
      }
      for (auto value = resume_state.live_values.rbegin();
           value != resume_state.live_values.rend(); ++value) {
        LLVMCoroutineScaffoldService::destroyAddress(
            TypeId(scaffold_state.low_ir.inst(*value).type),
            state_cleanup.CreateStructGEP(frame_type, frame,
                                          value_fields.at(value->index)),
            state_cleanup, function, scaffold_state);
      }
      state_cleanup.CreateStore(state_cleanup.getInt32(0), state_address);
      auto *owned_cleanup =
          llvm::BasicBlock::Create(scaffold_state.context,
                                   std::string(prefix) + ".cleanup.state." +
                                       std::to_string(resume_state.state),
                                   &function);
      state_cleanup.CreateBr(owned_cleanup);
      emit_language_cleanup(owned_cleanup, resume_state.transferred_cleanup,
                            resume_state.cleanup_order,
                            ".state." + std::to_string(resume_state.state));
    }
    emit_place_cleanup(after_detach, plan.cleanup_order, ".initial");
  }

} // namespace chtholly::compiler
