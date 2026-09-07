// Included inside LLVMCoroutineScaffoldService::emit; all state references
// intentionally alias the active scaffold state.

      auto *descriptor_type = llvm::StructType::get(
          scaffold_state.context, {i64, i32, i32, pointer_type, pointer_type, pointer_type,
                     pointer_type});
      const auto descriptor_size = scaffold_state.module.getDataLayout()
                                       .getTypeAllocSize(descriptor_type)
                                       .getFixedValue();
      llvm::Constant *move_result_entry =
          plan.result_type == scaffold_state.sem_ir.voidType()
              ? static_cast<llvm::Constant *>(
                    llvm::ConstantPointerNull::get(pointer_type))
              : static_cast<llvm::Constant *>(move_result);
      llvm::Constant *move_error_entry_value =
          has_error_payload ? static_cast<llvm::Constant *>(move_error)
                            : static_cast<llvm::Constant *>(
                                  llvm::ConstantPointerNull::get(pointer_type));
      auto *descriptor = new llvm::GlobalVariable(
          scaffold_state.module, descriptor_type, true, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantStruct::get(
              descriptor_type,
              {llvm::ConstantInt::get(i64, descriptor_size),
               llvm::ConstantInt::get(i32, 1), llvm::ConstantInt::get(i32, 0),
               resume, destroy, move_result_entry, move_error_entry_value}),
          base + "$descriptor");
      descriptor->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      // The internal constructor owns allocation and initializes the complete
      // frame before task_create can enqueue the first resume.
      auto *constructor =
          scaffold_state.constructors.at(plan.constructor_entity.index);
      const auto constructor_parameters =
          constructor->getFunctionType()->params();
      auto *constructor_entry =
          llvm::BasicBlock::Create(scaffold_state.context, "task.create.entry", constructor);
      auto *constructor_failed =
          llvm::BasicBlock::Create(scaffold_state.context, "task.create.failed", constructor);
      auto *constructor_allocate_failed = llvm::BasicBlock::Create(
          scaffold_state.context, "task.create.allocate.failed", constructor);
      auto *constructor_submit =
          llvm::BasicBlock::Create(scaffold_state.context, "task.create.submit", constructor);
      auto *constructor_done =
          llvm::BasicBlock::Create(scaffold_state.context, "task.create.done", constructor);
      llvm::IRBuilder<> constructor_builder(constructor_entry);
      const auto frame_size =
          scaffold_state.module.getDataLayout().getTypeAllocSize(frame_type).getFixedValue();
      const auto frame_alignment =
          scaffold_state.module.getDataLayout().getABITypeAlign(frame_type).value();
      auto *allocated = constructor_builder.CreateCall(
          scaffold_state.module.getOrInsertFunction(
              "chtholly_next_task_v1_frame_allocate",
              llvm::FunctionType::get(pointer_type, {i64, i64}, false)),
          {constructor_builder.getInt64(frame_size),
           constructor_builder.getInt64(frame_alignment)},
          "task.frame");
      constructor_builder.CreateCondBr(
          constructor_builder.CreateIsNull(allocated),
          constructor_allocate_failed, constructor_submit);
      llvm::IRBuilder<> submit(constructor_submit);
      submit.CreateStore(llvm::Constant::getNullValue(frame_type), allocated);
      std::unordered_map<std::uint32_t, LowParameter> parameter_slots;
      std::unordered_map<std::uint32_t, LowParameterValue> parameter_values;
      for (const auto body_block : scaffold_state.low_ir.blockList(low_function.blocks))
        for (const auto instruction : scaffold_state.low_ir.block(body_block)) {
          if (scaffold_state.low_ir.inst(instruction).kind == LowInstKind::Parameter) {
            const auto parameter = scaffold_state.low_ir.getAs<LowParameter>(instruction);
            parameter_slots.emplace(parameter.arg1.index, parameter);
          } else if (scaffold_state.low_ir.inst(instruction).kind ==
                     LowInstKind::ParameterValue) {
            const auto parameter =
                scaffold_state.low_ir.getAs<LowParameterValue>(instruction);
            parameter_values.emplace(parameter.arg0.index, parameter);
          }
        }
      auto *constructor_flags =
          submit.CreateStructGEP(frame_type, allocated, 1);
      for (std::uint32_t parameter = 0;
           parameter < constructor_parameters.size() - 3; ++parameter) {
        auto *argument = constructor->getArg(parameter + 1);
        if (const auto found = parameter_slots.find(parameter);
            found != parameter_slots.end()) {
          const auto slot = found->second.arg0;
          const auto lifted_slot = std::ranges::find(plan.lifted_slots, slot);
          if (lifted_slot == plan.lifted_slots.end()) {
            error = "coroutine parameter is not lifted into its frame";
            return false;
          }
          const auto position =
              static_cast<std::size_t>(lifted_slot - plan.lifted_slots.begin());
          storeValueToObject(
              submit.CreateStructGEP(frame_type, allocated,
                                     static_cast<unsigned>(6 + position)),
              argument, scaffold_state.low_ir.slot(slot).type, submit);
          for (const auto &frame_place : plan.frame_places)
            if (scaffold_state.low_ir.place(frame_place.place).root == slot) {
              auto *word_address = submit.CreateInBoundsGEP(
                  i64, constructor_flags,
                  submit.getInt32(frame_place.initialization_bit / 64));
              auto *word = submit.CreateLoad(i64, word_address);
              submit.CreateStore(
                  submit.CreateOr(
                      word, submit.getInt64(
                                UINT64_C(1)
                                << (frame_place.initialization_bit % 64))),
                  word_address);
            }
        } else if (parameter_values.contains(parameter)) {
          LowInstId parameter_instruction;
          for (const auto id : frame_values)
            if (scaffold_state.low_ir.inst(id).kind == LowInstKind::ParameterValue &&
                scaffold_state.low_ir.getAs<LowParameterValue>(id).arg0.index == parameter)
              parameter_instruction = id;
          storeValueToObject(submit.CreateStructGEP(
                                 frame_type, allocated,
                                 value_fields.at(parameter_instruction.index)),
                             argument,
                             TypeId(scaffold_state.low_ir.inst(parameter_instruction).type),
                             submit);
        }
      }
      auto create_root = scaffold_state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_create",
          llvm::FunctionType::get(
              i32, {pointer_type, pointer_type, pointer_type, pointer_type},
              false));
      auto create_child = scaffold_state.module.getOrInsertFunction(
          "chtholly_next_task_v1_task_create_child",
          llvm::FunctionType::get(
              i32, {pointer_type, pointer_type, pointer_type, pointer_type},
              false));
      auto *root_submit =
          llvm::BasicBlock::Create(scaffold_state.context, "task.create.root", constructor);
      auto *child_submit =
          llvm::BasicBlock::Create(scaffold_state.context, "task.create.child", constructor);
      auto *submitted = llvm::BasicBlock::Create(
          scaffold_state.context, "task.create.submitted", constructor);
      const auto out_index = static_cast<unsigned>(constructor->arg_size() - 2);
      const auto mode_index =
          static_cast<unsigned>(constructor->arg_size() - 1);
      submit.CreateCondBr(constructor->getArg(mode_index), child_submit,
                          root_submit);
      llvm::IRBuilder<> root_builder(root_submit);
      auto *root_status = root_builder.CreateCall(
          create_root, {constructor->getArg(0), descriptor, allocated,
                        constructor->getArg(out_index)});
      root_builder.CreateBr(submitted);
      llvm::IRBuilder<> child_builder(child_submit);
      auto *child_status = child_builder.CreateCall(
          create_child, {constructor->getArg(0), descriptor, allocated,
                         constructor->getArg(out_index)});
      child_builder.CreateBr(submitted);
      llvm::IRBuilder<> submitted_builder(submitted);
      auto *status = submitted_builder.CreatePHI(i32, 2, "task.create.status");
      status->addIncoming(root_status, root_submit);
      status->addIncoming(child_status, child_submit);
      submitted_builder.CreateCondBr(
          submitted_builder.CreateICmpEQ(status, submitted_builder.getInt32(0)),
          constructor_done, constructor_failed);
      llvm::IRBuilder<>(constructor_done)
          .CreateRet(llvm::ConstantInt::get(i32, 0));
      llvm::IRBuilder<> allocation_failure(constructor_allocate_failed);
      allocation_failure.CreateRet(llvm::ConstantInt::getSigned(i32, -2602));
      auto *failed_cleanup_done = llvm::BasicBlock::Create(
          scaffold_state.context, "task.create.failed.cleaned", constructor);
      llvm::IRBuilder<> failed_done(failed_cleanup_done);
      failed_done.CreateCall(
          scaffold_state.module.getOrInsertFunction(
              "chtholly_next_task_v1_frame_deallocate",
              llvm::FunctionType::get(void_type, {pointer_type}, false)),
          {allocated});
      failed_done.CreateRet(status);
      emitCoroutineCleanupChain(*constructor, frame_type, allocated, plan,
                                value_fields, constructor_failed,
                                failed_cleanup_done, "task.create.failure");
      if (plan.execution_entry &&
          (scaffold_state.emission_role == ModuleEmissionRole::ExecutableEntry ||
           scaffold_state.emission_role == ModuleEmissionRole::CoroutineExecutionEntry)) {
        auto *runner = llvm::Function::Create(
            llvm::FunctionType::get(i32, false),
            llvm::Function::InternalLinkage, base + "$run", scaffold_state.module);
        auto *runner_entry =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.entry", runner);
        auto *runner_scope =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.scope", runner);
        auto *runner_create =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.create", runner);
        auto *runner_join =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.join", runner);
        auto *runner_query =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.query", runner);
        auto *runner_completed =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.completed", runner);
        auto *runner_cancelled =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.cancelled", runner);
        auto *runner_failed =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.failed", runner);
        auto *runner_join_failed =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.join.failed", runner);
        auto *runner_take_failed =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.take.failed", runner);
        auto *runner_take_succeeded = llvm::BasicBlock::Create(
            scaffold_state.context, "task.run.take.succeeded", runner);
        auto *runner_take_error_failed = llvm::BasicBlock::Create(
            scaffold_state.context, "task.run.take_error.failed", runner);
        auto *runner_take_error_succeeded = llvm::BasicBlock::Create(
            scaffold_state.context, "task.run.take_error.succeeded", runner);
        auto *runner_release =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.release", runner);
        auto *runner_scope_failed =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.scope.failed", runner);
        auto *runner_task_failed =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.task.failed", runner);
        auto *runner_finish =
            llvm::BasicBlock::Create(scaffold_state.context, "task.run.finish", runner);
        llvm::IRBuilder<> runner_builder(runner_entry);
        auto *executor_out = runner_builder.CreateAlloca(pointer_type);
        auto *scope_out = runner_builder.CreateAlloca(pointer_type);
        auto *task_out = runner_builder.CreateAlloca(pointer_type);
        auto *config_type = llvm::StructType::get(scaffold_state.context, {i64, i32, i32});
        auto *config = runner_builder.CreateAlloca(config_type);
        const auto config_size = scaffold_state.module.getDataLayout()
                                     .getTypeAllocSize(config_type)
                                     .getFixedValue();
        runner_builder.CreateStore(
            llvm::ConstantStruct::get(config_type,
                                      {runner_builder.getInt64(config_size),
                                       runner_builder.getInt32(1),
                                       runner_builder.getInt32(0)}),
            config);
        auto *executor_status = runner_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_executor_create",
                llvm::FunctionType::get(i32, {pointer_type, pointer_type},
                                        false)),
            {config, executor_out});
        runner_builder.CreateCondBr(
            runner_builder.CreateICmpEQ(executor_status,
                                        runner_builder.getInt32(0)),
            runner_scope, runner_finish);
        llvm::IRBuilder<> scope_builder(runner_scope);
        auto *executor = scope_builder.CreateLoad(pointer_type, executor_out);
        auto *scope_status = scope_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_scope_create",
                llvm::FunctionType::get(i32, {pointer_type, pointer_type},
                                        false)),
            {executor, scope_out});
        scope_builder.CreateCondBr(
            scope_builder.CreateICmpEQ(scope_status, scope_builder.getInt32(0)),
            runner_create, runner_scope_failed);
        llvm::IRBuilder<> create_builder(runner_create);
        auto *scope = create_builder.CreateLoad(pointer_type, scope_out);
        auto *create_status = create_builder.CreateCall(
            constructor,
            {scope, task_out, llvm::ConstantInt::getFalse(scaffold_state.context)});
        create_builder.CreateCondBr(
            create_builder.CreateICmpEQ(create_status,
                                        create_builder.getInt32(0)),
            runner_join, runner_task_failed);
        llvm::IRBuilder<> join_builder(runner_join);
        auto *created_task = join_builder.CreateLoad(pointer_type, task_out);
        auto *join_status = join_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_task_join",
                llvm::FunctionType::get(i32, {pointer_type}, false)),
            {created_task});
        join_builder.CreateCondBr(
            join_builder.CreateICmpEQ(join_status, join_builder.getInt32(0)),
            runner_query, runner_join_failed);
        llvm::IRBuilder<> query_builder(runner_query);
        auto *info_type = llvm::StructType::get(
            scaffold_state.context,
            {i64, i32, query_builder.getInt8Ty(), query_builder.getInt8Ty(),
             query_builder.getInt8Ty(), query_builder.getInt8Ty(),
             query_builder.getInt8Ty(), query_builder.getInt8Ty(),
             llvm::ArrayType::get(query_builder.getInt8Ty(), 6)});
        auto *info = query_builder.CreateAlloca(info_type);
        const auto info_size =
            scaffold_state.module.getDataLayout().getTypeAllocSize(info_type).getFixedValue();
        query_builder.CreateStore(
            query_builder.getInt64(info_size),
            query_builder.CreateStructGEP(info_type, info, 0));
        auto *query_status = query_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_task_query",
                llvm::FunctionType::get(i32, {pointer_type, pointer_type},
                                        false)),
            {created_task, info});
        auto *runner_state = query_builder.CreateLoad(
            i32, query_builder.CreateStructGEP(info_type, info, 1),
            "task.run.state");
        auto *query_valid =
            query_builder.CreateICmpEQ(query_status, query_builder.getInt32(0));
        auto *query_valid_block = llvm::BasicBlock::Create(
            scaffold_state.context, "task.run.query.valid", runner, runner_join_failed);
        query_builder.CreateCondBr(query_valid, query_valid_block,
                                   runner_join_failed);
        llvm::IRBuilder<> query_valid_builder(query_valid_block);
        auto *valid_switch = query_valid_builder.CreateSwitch(
            runner_state, runner_join_failed, 3);
        valid_switch->addCase(query_valid_builder.getInt32(3),
                              runner_cancelled);
        valid_switch->addCase(query_valid_builder.getInt32(4),
                              runner_completed);
        valid_switch->addCase(query_valid_builder.getInt32(5), runner_failed);
        llvm::Value *completed_result = nullptr;
        llvm::BasicBlock *completed_predecessor = runner_completed;
        llvm::IRBuilder<> completed_builder(runner_completed);
        if (plan.result_type != scaffold_state.sem_ir.voidType()) {
          auto *result_storage =
              completed_builder.CreateAlloca(lowerObjectType(plan.result_type));
          auto *take_status = completed_builder.CreateCall(
              scaffold_state.module.getOrInsertFunction(
                  "chtholly_next_task_v1_task_take_result",
                  llvm::FunctionType::get(i32, {pointer_type, pointer_type},
                                          false)),
              {created_task, result_storage});
          completed_builder.CreateCondBr(
              completed_builder.CreateICmpEQ(take_status,
                                             completed_builder.getInt32(0)),
              runner_take_succeeded, runner_take_failed);
          llvm::IRBuilder<> take_succeeded(runner_take_succeeded);
          if (lowerValueType(plan.result_type)->isIntegerTy(32))
            completed_result = take_succeeded.CreateLoad(
                take_succeeded.getInt32Ty(), result_storage);
          take_succeeded.CreateBr(runner_release);
          completed_predecessor = runner_take_succeeded;
        } else {
          completed_builder.CreateBr(runner_release);
          llvm::IRBuilder<>(runner_take_succeeded).CreateUnreachable();
        }
        llvm::IRBuilder<>(runner_cancelled).CreateBr(runner_release);
        llvm::IRBuilder<> failed_builder(runner_failed);
        if (has_error_payload) {
          auto *error_storage =
              failed_builder.CreateAlloca(lowerObjectType(*plan.error_type));
          auto *take_error_status = failed_builder.CreateCall(
              scaffold_state.module.getOrInsertFunction(
                  "chtholly_next_task_v1_task_take_error",
                  llvm::FunctionType::get(i32, {pointer_type, pointer_type},
                                          false)),
              {created_task, error_storage});
          failed_builder.CreateCondBr(
              failed_builder.CreateICmpEQ(take_error_status,
                                          failed_builder.getInt32(0)),
              runner_take_error_succeeded, runner_take_error_failed);
          llvm::IRBuilder<>(runner_take_error_succeeded)
              .CreateBr(runner_release);
        } else if (plan.error_type) {
          failed_builder.CreateBr(runner_release);
          llvm::IRBuilder<>(runner_take_error_succeeded).CreateUnreachable();
        } else {
          failed_builder.CreateBr(runner_take_error_failed);
          llvm::IRBuilder<>(runner_take_error_succeeded).CreateUnreachable();
        }
        llvm::IRBuilder<>(runner_join_failed).CreateBr(runner_release);
        llvm::IRBuilder<>(runner_take_failed).CreateBr(runner_release);
        llvm::IRBuilder<>(runner_take_error_failed).CreateBr(runner_release);
        llvm::IRBuilder<> runner_release_builder(runner_release);
        auto *run_result = runner_release_builder.CreatePHI(i32, 6);
        run_result->addIncoming(runner_release_builder.getInt32(3),
                                runner_cancelled);
        run_result->addIncoming(runner_release_builder.getInt32(120),
                                runner_join_failed);
        run_result->addIncoming(runner_release_builder.getInt32(121),
                                runner_take_failed);
        run_result->addIncoming(runner_release_builder.getInt32(122),
                                runner_take_error_failed);
        if (has_error_payload)
          run_result->addIncoming(runner_release_builder.getInt32(4),
                                  runner_take_error_succeeded);
        else if (plan.error_type)
          run_result->addIncoming(runner_release_builder.getInt32(4),
                                  runner_failed);
        run_result->addIncoming(completed_result
                                    ? completed_result
                                    : runner_release_builder.getInt32(0),
                                completed_predecessor);
        runner_release_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_task_release",
                llvm::FunctionType::get(void_type, {pointer_type}, false)),
            {created_task});
        runner_release_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_scope_release",
                llvm::FunctionType::get(void_type, {pointer_type}, false)),
            {scope});
        runner_release_builder.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_executor_release",
                llvm::FunctionType::get(void_type, {pointer_type}, false)),
            {executor});
        runner_release_builder.CreateRet(run_result);
        llvm::IRBuilder<> task_failed(runner_task_failed);
        task_failed.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_scope_release",
                llvm::FunctionType::get(void_type, {pointer_type}, false)),
            {scope});
        task_failed.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_executor_release",
                llvm::FunctionType::get(void_type, {pointer_type}, false)),
            {executor});
        task_failed.CreateRet(task_failed.getInt32(1));
        llvm::IRBuilder<> scope_failed(runner_scope_failed);
        scope_failed.CreateCall(
            scaffold_state.module.getOrInsertFunction(
                "chtholly_next_task_v1_executor_release",
                llvm::FunctionType::get(void_type, {pointer_type}, false)),
            {executor});
        scope_failed.CreateRet(scope_failed.getInt32(1));
        llvm::IRBuilder<>(runner_finish)
            .CreateRet(llvm::ConstantInt::get(i32, 1));
        scaffold_state.source_entry = runner;
        ++scaffold_state.entry_candidate_count;
      }
      if (source_function->empty()) {
        auto *unused =
            llvm::BasicBlock::Create(scaffold_state.context, "unavailable", source_function);
        llvm::IRBuilder<>(unused).CreateUnreachable();
      }
      llvm::raw_string_ostream resume_stream(error);
      if (llvm::verifyFunction(*resume, &resume_stream) ||
          llvm::verifyFunction(*destroy, &resume_stream) ||
          llvm::verifyFunction(*constructor, &resume_stream)) {
        resume_stream.flush();
        return false;
      }
      scaffold_state.coroutine.task = nullptr;
      scaffold_state.coroutine.cancel = nullptr;
      scaffold_state.coroutine.cancelled = nullptr;
      scaffold_state.coroutine.plan = nullptr;
      scaffold_state.coroutine.frame_type = nullptr;
      scaffold_state.coroutine.frame = nullptr;
      scaffold_state.coroutine.bitmap = nullptr;
      scaffold_state.coroutine.terminal = nullptr;
      scaffold_state.coroutine.wake_entry = nullptr;
      scaffold_state.coroutine.wake_release = nullptr;
    }
    return true;
  }

