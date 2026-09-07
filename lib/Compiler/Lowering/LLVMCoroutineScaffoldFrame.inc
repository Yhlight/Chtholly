// Included inside LLVMCoroutineScaffoldService::emit; all state references
// intentionally alias the active scaffold state.

    for (std::uint32_t index = 0; index < scaffold_state.low_ir.coroutineFramePlanCount();
         ++index) {
      const auto &plan =
          scaffold_state.low_ir.coroutineFramePlan(CoroutineFramePlanId(index));
      const auto reference = scaffold_state.local_function_refs.at(plan.function.index);
      auto *source_function = scaffold_state.functions.at(reference.index);
      const auto base = source_function->getName().str() + "$task";
      const auto &low_function = [&]() -> const LowFunction & {
        for (std::uint32_t function_index = 0;
             function_index < scaffold_state.low_ir.functionCount(); ++function_index) {
          const auto &candidate =
              scaffold_state.low_ir.function(LowFunctionId(function_index));
          if (candidate.semantic_function == plan.function)
            return candidate;
        }
        llvm_unreachable("verified coroutine has no LowIR function");
      }();
      const auto &frame_values = plan.frame_values;
      const auto bitmap_words =
          static_cast<std::uint32_t>((plan.frame_places.size() + 63) / 64);
      auto *bitmap_type = llvm::ArrayType::get(i64, bitmap_words);
      const auto has_error_payload =
          plan.error_type && *plan.error_type != scaffold_state.sem_ir.voidType();
      llvm::SmallVector<llvm::Type *, 16> frame_fields{
          i32,
          bitmap_type,
          llvm::Type::getInt8Ty(scaffold_state.context),
          pointer_type,
          plan.result_type == scaffold_state.sem_ir.voidType()
              ? static_cast<llvm::Type *>(llvm::Type::getInt8Ty(scaffold_state.context))
              : lowerObjectType(plan.result_type),
          has_error_payload
              ? lowerObjectType(*plan.error_type)
              : static_cast<llvm::Type *>(llvm::Type::getInt8Ty(scaffold_state.context))};
      for (const auto slot : plan.lifted_slots)
        frame_fields.push_back(lowerObjectType(scaffold_state.low_ir.slot(slot).type));
      for (const auto value_id : frame_values)
        frame_fields.push_back(
            lowerObjectType(TypeId(scaffold_state.low_ir.inst(value_id).type)));
      auto *frame_type =
          llvm::StructType::create(scaffold_state.context, frame_fields, base + "$frame");
      std::unordered_map<std::uint32_t, unsigned> value_fields;
      for (std::size_t value_index = 0; value_index < frame_values.size();
           ++value_index)
        value_fields.emplace(
            frame_values[value_index].index,
            static_cast<unsigned>(6 + plan.lifted_slots.size() + value_index));
      auto *resume_type =
          llvm::FunctionType::get(i32, {pointer_type, pointer_type}, false);
      auto *resume =
          llvm::Function::Create(resume_type, llvm::Function::InternalLinkage,
                                 base + "$resume", scaffold_state.module);
      auto *resume_entry =
          llvm::BasicBlock::Create(scaffold_state.context, "task.resume.entry", resume);
      auto *resume_cancel =
          llvm::BasicBlock::Create(scaffold_state.context, "task.cancel", resume);
      auto *resume_dispatch =
          llvm::BasicBlock::Create(scaffold_state.context, "task.resume.dispatch", resume);
      auto *resume_cancelled =
          llvm::BasicBlock::Create(scaffold_state.context, "task.cancelled", resume);
      auto resume_args = resume->arg_begin();
      llvm::Value *resume_frame = &*resume_args++;
      llvm::Value *task = &*resume_args;
      scaffold_state.coroutine.task = task;
      scaffold_state.coroutine.cancel = resume_cancel;
      scaffold_state.coroutine.cancelled = resume_cancelled;
      scaffold_state.coroutine.plan = &plan;
      scaffold_state.coroutine.frame_type = frame_type;
      scaffold_state.coroutine.frame = resume_frame;
      llvm::IRBuilder<> resume_builder(resume_entry);
      auto cancellation = scaffold_state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_cancellation_requested",
          llvm::FunctionType::get(llvm::Type::getInt8Ty(scaffold_state.context),
                                  {pointer_type}, false));
      resume_builder.CreateBr(resume_dispatch);
      llvm::IRBuilder<> dispatch(resume_dispatch);
      auto *state_address = dispatch.CreateStructGEP(frame_type, resume_frame,
                                                     0, "task.state.address");
      auto *state = dispatch.CreateLoad(i32, state_address, "task.state");
      auto *invalid_state =
          llvm::BasicBlock::Create(scaffold_state.context, "task.resume.invalid", resume);
      auto *state_switch = dispatch.CreateSwitch(
          state, invalid_state,
          static_cast<unsigned>(plan.resume_states.size() + 1));
      auto *initial =
          llvm::BasicBlock::Create(scaffold_state.context, "task.resume.initial", resume);
      auto *initial_continue = llvm::BasicBlock::Create(
          scaffold_state.context, "task.resume.initial.continue", resume);
      state_switch->addCase(llvm::ConstantInt::get(i32, 0), initial);
      llvm::IRBuilder<> initial_check(initial);
      auto *initial_cancelled = initial_check.CreateCall(
          cancellation, {task}, "task.initial.cancelled");
      initial_check.CreateCondBr(
          initial_check.CreateICmpNE(initial_cancelled,
                                     initial_check.getInt8(0)),
          resume_cancel, initial_continue);
      std::unordered_map<std::uint32_t, llvm::BasicBlock *> resume_blocks;
      for (const auto &resume_state : plan.resume_states) {
        auto *block = llvm::BasicBlock::Create(
            scaffold_state.context, "task.resume.state." + std::to_string(resume_state.state),
            resume);
        state_switch->addCase(llvm::ConstantInt::get(i32, resume_state.state),
                              block);
        resume_blocks.emplace(resume_state.state, block);
      }
      llvm::IRBuilder<>(invalid_state).CreateBr(resume_cancel);
      llvm::IRBuilder<>(resume_cancelled)
          .CreateRet(llvm::ConstantInt::get(i32, 2));
      emitCoroutineCleanupChain(*resume, frame_type, resume_frame, plan,
                                value_fields, resume_cancel, resume_cancelled,
                                "task.cancel");
      auto *wake_entry = llvm::Function::Create(
          llvm::FunctionType::get(void_type, {pointer_type}, false),
          llvm::Function::InternalLinkage, base + "$wake", scaffold_state.module);
      auto *wake_entry_block =
          llvm::BasicBlock::Create(scaffold_state.context, "entry", wake_entry);
      llvm::IRBuilder<> wake_builder(wake_entry_block);
      (void)wake_builder.CreateCall(
          scaffold_state.module.getOrInsertFunction(
              "chtholly_next_task_v1_task_wake",
              llvm::FunctionType::get(i32, {pointer_type}, false)),
          {wake_entry->getArg(0)});
      wake_builder.CreateRetVoid();
      auto *wake_release = llvm::Function::Create(
          llvm::FunctionType::get(void_type, {pointer_type}, false),
          llvm::Function::InternalLinkage, base + "$wake_release", scaffold_state.module);
      auto *wake_release_block =
          llvm::BasicBlock::Create(scaffold_state.context, "entry", wake_release);
      llvm::IRBuilder<> release_builder(wake_release_block);
      release_builder.CreateCall(
          scaffold_state.module.getOrInsertFunction(
              "chtholly_next_task_v1_task_release",
              llvm::FunctionType::get(void_type, {pointer_type}, false)),
          {wake_release->getArg(0)});
      release_builder.CreateRetVoid();
      scaffold_state.coroutine.wake_entry = wake_entry;
      scaffold_state.coroutine.wake_release = wake_release;
      scaffold_state.current_function = LowFunctionId::invalid();
      for (std::uint32_t function_index = 0;
           function_index < scaffold_state.low_ir.functionCount(); ++function_index)
        if (scaffold_state.low_ir.function(LowFunctionId(function_index)).semantic_function ==
            plan.function)
          scaffold_state.current_function = LowFunctionId(function_index);
      scaffold_state.blocks.clear();
      scaffold_state.slots.clear();
      scaffold_state.values.clear();
      scaffold_state.initialized_slots.clear();
      scaffold_state.place_flags.clear();
      scaffold_state.coroutine.place_bits.clear();
      llvm::IRBuilder<> frame_setup(resume_entry->getTerminator());
      scaffold_state.coroutine.bitmap =
          frame_setup.CreateStructGEP(frame_type, resume_frame, 1);
      scaffold_state.coroutine.terminal =
          frame_setup.CreateStructGEP(frame_type, resume_frame, 2);
      scaffold_state.current_result_slot =
          frame_setup.CreateStructGEP(frame_type, resume_frame, 4);
      scaffold_state.current_outcome_slot = nullptr;
      for (std::size_t slot_index = 0; slot_index < plan.lifted_slots.size();
           ++slot_index) {
        const auto slot = plan.lifted_slots[slot_index];
        scaffold_state.slots.emplace(slot.index, frame_setup.CreateStructGEP(
                                       frame_type, resume_frame,
                                       static_cast<unsigned>(6 + slot_index)));
      }
      for (const auto &frame_place : plan.frame_places)
        scaffold_state.coroutine.place_bits.emplace(frame_place.place.index,
                                              frame_place.initialization_bit);
      for (const auto slot : scaffold_state.low_ir.slotBlock(low_function.slots))
        if (!scaffold_state.slots.contains(slot.index))
          scaffold_state.slots.emplace(slot.index,
                         entryAlloca(*resume,
                                     lowerObjectType(scaffold_state.low_ir.slot(slot).type),
                                     "task.local.slot"));
      std::unordered_map<std::uint32_t, llvm::BasicBlock *> region_blocks;
      for (std::size_t region_index = 0; region_index < plan.regions.size();
           ++region_index) {
        const auto region_id = static_cast<std::uint32_t>(region_index);
        region_blocks.emplace(
            region_id,
            region_id == plan.initial_region.index
                ? initial_continue
                : llvm::BasicBlock::Create(
                      scaffold_state.context, "task.region." + std::to_string(region_id),
                      resume));
      }
      for (const auto &segment : plan.segments)
        if (segment.begin == 0)
          scaffold_state.blocks.emplace(segment.block.index,
                          region_blocks.at(segment.region.index));
      for (std::uint32_t place_index = 0; place_index < scaffold_state.low_ir.placeCount();
           ++place_index) {
        const auto place = LowPlaceId(place_index);
        if (!scaffold_state.slots.contains(scaffold_state.low_ir.place(place).root.index))
          continue;
        auto *flag = entryAlloca(*resume, llvm::Type::getInt1Ty(scaffold_state.context),
                                 "task.place.init");
        llvm::Value *initialized = llvm::ConstantInt::getFalse(scaffold_state.context);
        if (const auto found = scaffold_state.coroutine.place_bits.find(place.index);
            found != scaffold_state.coroutine.place_bits.end())
          initialized =
              testCoroutineInitializationBit(found->second, frame_setup);
        frame_setup.CreateStore(initialized, flag);
        scaffold_state.place_flags.emplace(place_index, flag);
      }
      std::unordered_map<std::uint32_t, const CoroutineResumeState *> states;
      for (const auto &resume_state_plan : plan.resume_states)
        states.emplace(resume_state_plan.suspension.index, &resume_state_plan);
