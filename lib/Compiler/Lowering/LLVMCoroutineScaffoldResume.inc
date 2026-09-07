// Included inside LLVMCoroutineScaffoldService::emit; all state references
// intentionally alias the active scaffold state.

      for (const auto &segment : plan.segments) {
        llvm::IRBuilder<> builder(region_blocks.at(segment.region.index));
        const auto instructions = scaffold_state.low_ir.block(segment.block);
        for (std::size_t position = segment.begin; position < segment.end;
             ++position) {
          const auto instruction = instructions[position];
          const auto &raw = scaffold_state.low_ir.inst(instruction);
          if (raw.kind == LowInstKind::Parameter)
            continue;
          if (raw.kind == LowInstKind::ParameterValue) {
            auto *address = builder.CreateStructGEP(
                frame_type, resume_frame, value_fields.at(instruction.index));
            scaffold_state.values.emplace(
                instruction.index,
                loadValueFromObject(address, TypeId(raw.type), builder));
            continue;
          }
          if (const auto found = states.find(instruction.index);
              found != states.end()) {
            const auto &resume_state_plan = *found->second;
            const auto callback_wait =
                resume_state_plan.suspension_kind ==
                CoroutineResumeState::SuspensionKind::CallbackWake;
            const auto completion_set_wait =
                resume_state_plan.suspension_kind ==
                CoroutineResumeState::SuspensionKind::TaskCompletionSet;
            const auto task_group_drain =
                resume_state_plan.suspension_kind ==
                CoroutineResumeState::SuspensionKind::TaskGroupDrain;
            const auto defer_cancellation =
                resume_state_plan.cancellation_acknowledgement ==
                CoroutineCancellationAcknowledgement::EnclosingTaskScope;
            const auto wait_operands =
                callback_wait
                    ? scaffold_state.low_ir.valueBlock(
                          scaffold_state.low_ir.getAs<LowCallbackWakeWait>(instruction).arg1)
                : task_group_drain
                    ? scaffold_state.low_ir.valueBlock(
                          scaffold_state.low_ir.inst(instruction).kind ==
                                  LowInstKind::CoroutineTaskGroupDrain
                              ? scaffold_state.low_ir
                                    .getAs<LowCoroutineTaskGroupDrain>(
                                        instruction)
                                    .arg0
                          : scaffold_state.low_ir.inst(instruction).kind ==
                                  LowInstKind::CoroutineTaskGroupErrorDrain
                              ? scaffold_state.low_ir
                                    .getAs<LowCoroutineTaskGroupErrorDrain>(
                                        instruction)
                                    .arg0
                              : scaffold_state.low_ir
                                    .getAs<LowCoroutineTaskGroupCancelDrain>(
                                        instruction)
                                    .arg0)
                : !completion_set_wait
                    ? scaffold_state.low_ir.valueBlock(
                          scaffold_state.low_ir
                              .getAs<LowCoroutineTaskCompletionWait>(
                                  instruction)
                              .arg0)
                    : std::span<const LowInstId>{};
            const auto operand =
                completion_set_wait
                    ? scaffold_state.low_ir
                          .getAs<LowCoroutineTaskCompletionCombine>(instruction)
                          .arg1
                    : wait_operands.front();
            auto *wake = value(operand);
            for (const auto live : resume_state_plan.live_values) {
              auto *address = builder.CreateStructGEP(
                  frame_type, resume_frame, value_fields.at(live.index));
              storeValueToObject(address, value(live),
                                 TypeId(scaffold_state.low_ir.inst(live).type), builder);
            }
            if (task_group_drain &&
                std::ranges::find(resume_state_plan.live_values,
                                  resume_state_plan.task_group) ==
                    resume_state_plan.live_values.end()) {
              auto *address = builder.CreateStructGEP(
                  frame_type, resume_frame,
                  value_fields.at(resume_state_plan.task_group.index));
              storeValueToObject(
                  address, value(resume_state_plan.task_group),
                  TypeId(scaffold_state.low_ir.inst(resume_state_plan.task_group).type),
                  builder);
            }
            LLVMCompletionCombineProbe combine_probe;
            const CoroutineTaskCompletionCombinePlan *combine_plan = nullptr;
            if (completion_set_wait) {
              combine_plan = &scaffold_state.low_ir.coroutineTaskCompletionCombinePlan(
                  scaffold_state.low_ir.getAs<LowCoroutineTaskCompletionCombine>(instruction)
                      .arg0);
              armCompletionSet(combine_plan->provider, wake,
                               combine_plan->operand_count, builder, *resume);
              combine_probe =
                  probeCompletionSet(*combine_plan, wake, builder, *resume);
            }
            auto *ready =
                completion_set_wait ? combine_probe.ready
                : callback_wait
                    ? builder.CreateExtractValue(wake, 1, "task.wake.ready")
                    : builder.CreateICmpNE(
                          builder.CreateCall(
                              scaffold_state.module.getOrInsertFunction(
                                  "chtholly_next_task_v1_"
                                  "completion_ready",
                                  llvm::FunctionType::get(builder.getInt8Ty(),
                                                          {builder.getPtrTy()},
                                                          false)),
                              {wake}, "task.completion.ready"),
                          builder.getInt8(0));
            auto *ready_block = llvm::BasicBlock::Create(
                scaffold_state.context, "task.suspend.ready", resume);
            auto *pending_block = llvm::BasicBlock::Create(
                scaffold_state.context, "task.suspend.pending", resume);
            auto *continuation =
                region_blocks.at(resume_state_plan.continuation_region.index);
            builder.CreateCondBr(ready, ready_block, pending_block);
            llvm::IRBuilder<> pending(pending_block);
            const auto slot_position =
                std::ranges::find(plan.lifted_slots,
                                  resume_state_plan.wake_slot) -
                plan.lifted_slots.begin();
            storeValueToObject(
                pending.CreateStructGEP(
                    frame_type, resume_frame,
                    static_cast<unsigned>(6 + slot_position)),
                wake, scaffold_state.low_ir.slot(resume_state_plan.wake_slot).type, pending);
            setSlotPlacesInitialized(resume_state_plan.wake_slot, true,
                                     pending);
            auto *pending_tail = pending_block;
            if (resume_state_plan.pre_commit_cleanup.hasValue()) {
              pending_tail = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.suspend.pre_commit.done", resume);
              emitCoroutineCleanupGraph(*resume, frame_type, resume_frame, plan,
                                        resume_state_plan.pre_commit_cleanup,
                                        pending_block, pending_tail,
                                        "task.suspend.pre_commit", false);
            }
            llvm::IRBuilder<> pending_commit(pending_tail);
            pending_commit.CreateStore(
                pending_commit.getInt32(resume_state_plan.state),
                state_address);
            auto *suspend_committed = llvm::BasicBlock::Create(
                scaffold_state.context, "task.suspend.committed", resume);
            if (task_group_drain) {
              auto *pending_cancelled = pending_commit.CreateCall(
                  cancellation, {task}, "task.group.drain.cancelled");
              auto *request_cancel = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.group.drain.request_cancel", resume);
              pending_commit.CreateCondBr(
                  pending_commit.CreateICmpNE(pending_cancelled,
                                              pending_commit.getInt8(0)),
                  request_cancel, suspend_committed);
              llvm::IRBuilder<> request(request_cancel);
              auto *request_status = request.CreateCall(
                  scaffold_state.module.getOrInsertFunction(
                      "chtholly_next_task_v1_task_group_request_cancel",
                      llvm::FunctionType::get(request.getInt32Ty(),
                                              {request.getPtrTy()}, false)),
                  {value(resume_state_plan.task_group)},
                  "task.group.drain.cancel.status");
              auto *requested = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.group.drain.cancel.requested", resume);
              auto *request_failed = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.group.drain.cancel.failed", resume);
              request.CreateCondBr(
                  request.CreateICmpEQ(request_status, request.getInt32(0)),
                  requested, request_failed);
              llvm::IRBuilder<>(requested).CreateBr(suspend_committed);
              llvm::IRBuilder<> failed(request_failed);
              emitCoroutineProtocolTrap(
                  static_cast<std::uint32_t>(
                      CoroutineRuntimeFaultReason::TaskGroup),
                  failed);
            } else if (defer_cancellation) {
              pending_commit.CreateBr(suspend_committed);
            } else {
              auto *pending_cancelled = pending_commit.CreateCall(
                  cancellation, {task}, "task.suspend.cancelled");
              pending_commit.CreateCondBr(
                  pending_commit.CreateICmpNE(pending_cancelled,
                                              pending_commit.getInt8(0)),
                  resume_cancel, suspend_committed);
            }
            llvm::IRBuilder<>(suspend_committed)
                .CreateRet(llvm::ConstantInt::get(i32, 0));
            llvm::IRBuilder<> ready_builder(ready_block);
            if (completion_set_wait) {
              if (combine_plan->operation ==
                  CoroutineTaskCompletionCombineKind::WaitAll)
                releaseCompletionSet(combine_plan->provider, wake,
                                     combine_plan->operand_count, ready_builder,
                                     *resume);
              else if (combine_plan->operation ==
                       CoroutineTaskCompletionCombineKind::Race)
                detachCompletionSet(combine_plan->provider, wake,
                                    combine_plan->operand_count, ready_builder,
                                    *resume);
              if (resume_state_plan.result_value.hasValue())
                storeValueToObject(
                    ready_builder.CreateStructGEP(
                        frame_type, resume_frame,
                        value_fields.at(resume_state_plan.result_value.index)),
                    combine_probe.result,
                    TypeId(scaffold_state.low_ir.inst(resume_state_plan.result_value).type),
                    ready_builder);
            } else if (callback_wait) {
              auto *ready_completion =
                  ready_builder.CreateExtractValue(wake, 0);
              finishCallbackCompletionValue(
                  scaffold_state.low_ir.callbackWakePlan(resume_state_plan.wake_plan)
                      .completion_plan,
                  ready_completion, ready_builder, *resume);
            } else {
              ready_builder.CreateCall(
                  scaffold_state.module.getOrInsertFunction(
                      "chtholly_next_task_v1_completion_release",
                      llvm::FunctionType::get(ready_builder.getVoidTy(),
                                              {ready_builder.getPtrTy()},
                                              false)),
                  {wake});
            }
            setSlotPlacesInitialized(resume_state_plan.wake_slot, false,
                                     ready_builder);
            if (task_group_drain) {
              finishCoroutineTaskGroupDrain(value(resume_state_plan.task_group),
                                            resume_state_plan, continuation,
                                            ready_builder, *resume);
            } else {
              auto *ready_tail = ready_builder.GetInsertBlock();
              if (resume_state_plan.pre_commit_cleanup.hasValue()) {
                ready_tail = llvm::BasicBlock::Create(
                    scaffold_state.context, "task.ready.pre_commit.done", resume);
                emitCoroutineCleanupGraph(
                    *resume, frame_type, resume_frame, plan,
                    resume_state_plan.pre_commit_cleanup,
                    ready_builder.GetInsertBlock(), ready_tail,
                    "task.ready.pre_commit", false);
              }
              llvm::IRBuilder<>(ready_tail).CreateBr(continuation);
            }
            llvm::IRBuilder<> resumed(
                resume_blocks.at(resume_state_plan.state));
            auto *stored_wake = loadValueFromObject(
                resumed.CreateStructGEP(
                    frame_type, resume_frame,
                    static_cast<unsigned>(6 + slot_position)),
                scaffold_state.low_ir.slot(resume_state_plan.wake_slot).type, resumed);
            if (completion_set_wait) {
              const auto resumed_probe = probeCompletionSet(
                  *combine_plan, stored_wake, resumed, *resume);
              auto *resume_ready = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.combination.resume.ready", resume);
              auto *resume_pending = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.combination.resume.pending", resume);
              resumed.CreateCondBr(resumed_probe.ready, resume_ready,
                                   resume_pending);
              llvm::IRBuilder<> still_pending(resume_pending);
              auto *combination_cancelled = still_pending.CreateCall(
                  cancellation, {task}, "task.combination.cancelled");
              auto *pending_suspend = llvm::BasicBlock::Create(
                  scaffold_state.context, "task.combination.resuspended", resume);
              if (defer_cancellation)
                still_pending.CreateBr(pending_suspend);
              else
                still_pending.CreateCondBr(
                    still_pending.CreateICmpNE(combination_cancelled,
                                               still_pending.getInt8(0)),
                    resume_cancel, pending_suspend);
              llvm::IRBuilder<>(pending_suspend)
                  .CreateRet(llvm::ConstantInt::get(i32, 0));
              resumed.SetInsertPoint(resume_ready);
              if (combine_plan->operation ==
                  CoroutineTaskCompletionCombineKind::WaitAll)
                releaseCompletionSet(combine_plan->provider, stored_wake,
                                     combine_plan->operand_count, resumed,
                                     *resume);
              else if (combine_plan->operation ==
                       CoroutineTaskCompletionCombineKind::Race)
                detachCompletionSet(combine_plan->provider, stored_wake,
                                    combine_plan->operand_count, resumed,
                                    *resume);
              if (resume_state_plan.result_value.hasValue())
                storeValueToObject(
                    resumed.CreateStructGEP(
                        frame_type, resume_frame,
                        value_fields.at(resume_state_plan.result_value.index)),
                    resumed_probe.result,
                    TypeId(scaffold_state.low_ir.inst(resume_state_plan.result_value).type),
                    resumed);
            } else if (callback_wait) {
              finishCallbackCompletionValue(
                  scaffold_state.low_ir.callbackWakePlan(resume_state_plan.wake_plan)
                      .completion_plan,
                  resumed.CreateExtractValue(stored_wake, 0), resumed, *resume);
            } else {
              resumed.CreateCall(
                  scaffold_state.module.getOrInsertFunction(
                      "chtholly_next_task_v1_completion_release",
                      llvm::FunctionType::get(resumed.getVoidTy(),
                                              {resumed.getPtrTy()}, false)),
                  {stored_wake});
            }
            setSlotPlacesInitialized(resume_state_plan.wake_slot, false,
                                     resumed);
            if (task_group_drain) {
              resumed.CreateStore(resumed.getInt32(0), state_address);
              auto *stored_group = resumed.CreateLoad(
                  resumed.getPtrTy(),
                  resumed.CreateStructGEP(
                      frame_type, resume_frame,
                      value_fields.at(resume_state_plan.task_group.index)));
              finishCoroutineTaskGroupDrain(stored_group, resume_state_plan,
                                            continuation, resumed, *resume);
              builder.SetInsertPoint(continuation);
              for (const auto live : resume_state_plan.live_values) {
                if (live == resume_state_plan.task_group)
                  continue;
                auto *address = builder.CreateStructGEP(
                    frame_type, resume_frame, value_fields.at(live.index));
                scaffold_state.values.insert_or_assign(
                    live.index,
                    loadValueFromObject(
                        address, TypeId(scaffold_state.low_ir.inst(live).type), builder));
              }
              continue;
            }
            auto *post_resume_cancelled = resumed.CreateCall(
                cancellation, {task}, "task.post_resume.cancelled");
            auto *post_resume_continue = llvm::BasicBlock::Create(
                scaffold_state.context, "task.post_resume.continue", resume);
            if (defer_cancellation)
              resumed.CreateBr(post_resume_continue);
            else
              resumed.CreateCondBr(
                  resumed.CreateICmpNE(post_resume_cancelled,
                                       llvm::ConstantInt::get(
                                           llvm::Type::getInt8Ty(scaffold_state.context), 0)),
                  resume_cancel, post_resume_continue);
            llvm::IRBuilder<> post_resume(post_resume_continue);
            post_resume.CreateStore(post_resume.getInt32(0), state_address);
            post_resume.CreateBr(continuation);
            builder.SetInsertPoint(continuation);
            if (resume_state_plan.result_value.hasValue())
              scaffold_state.values.insert_or_assign(
                  resume_state_plan.result_value.index,
                  loadValueFromObject(
                      builder.CreateStructGEP(
                          frame_type, resume_frame,
                          value_fields.at(
                              resume_state_plan.result_value.index)),
                      TypeId(scaffold_state.low_ir.inst(resume_state_plan.result_value).type),
                      builder));
            for (const auto live : resume_state_plan.live_values) {
              auto *address = builder.CreateStructGEP(
                  frame_type, resume_frame, value_fields.at(live.index));
              scaffold_state.values.insert_or_assign(
                  live.index,
                  loadValueFromObject(address, TypeId(scaffold_state.low_ir.inst(live).type),
                                      builder));
            }
            continue;
          }
          if (raw.kind == LowInstKind::CoroutineReturnSuccess) {
            const auto returned = LowInstId(raw.arg0);
            const auto return_type = TypeId(scaffold_state.low_ir.inst(returned).type);
            if (return_type != scaffold_state.sem_ir.voidType()) {
              storeValueToObject(scaffold_state.current_result_slot, value(returned),
                                 return_type, builder);
              auto *terminal = builder.CreateLoad(builder.getInt8Ty(),
                                                  scaffold_state.coroutine.terminal);
              builder.CreateStore(
                  builder.CreateOr(terminal, builder.getInt8(1)),
                  scaffold_state.coroutine.terminal);
            }
            builder.CreateRet(builder.getInt32(3));
            continue;
          }
          if (raw.kind == LowInstKind::CoroutineReturnError) {
            const auto returned = LowInstId(raw.arg0);
            const auto return_type = TypeId(scaffold_state.low_ir.inst(returned).type);
            if (return_type != scaffold_state.sem_ir.voidType()) {
              storeValueToObject(
                  builder.CreateStructGEP(frame_type, resume_frame, 5),
                  value(returned), return_type, builder);
            }
            auto *terminal = builder.CreateLoad(builder.getInt8Ty(),
                                                scaffold_state.coroutine.terminal);
            builder.CreateStore(builder.CreateOr(terminal, builder.getInt8(2)),
                                scaffold_state.coroutine.terminal);
            builder.CreateRet(builder.getInt32(4));
            continue;
          }
          if (raw.kind == LowInstKind::CoroutineReturnCancelled) {
            builder.CreateRet(builder.getInt32(2));
            continue;
          }
          lowerInst(instruction, builder, *resume);
        }
      }
