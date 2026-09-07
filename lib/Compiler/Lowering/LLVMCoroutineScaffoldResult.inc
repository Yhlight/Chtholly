// Included inside LLVMCoroutineScaffoldService::emit; all state references
// intentionally alias the active scaffold state.

      auto *destroy_type =
          llvm::FunctionType::get(void_type, {pointer_type}, false);
      auto *destroy =
          llvm::Function::Create(destroy_type, llvm::Function::InternalLinkage,
                                 base + "$destroy", scaffold_state.module);
      auto *destroy_entry =
          llvm::BasicBlock::Create(scaffold_state.context, "task.destroy.entry", destroy);
      auto *destroy_done =
          llvm::BasicBlock::Create(scaffold_state.context, "task.destroy.done", destroy);
      auto *destroy_result =
          llvm::BasicBlock::Create(scaffold_state.context, "task.destroy.result", destroy);
      auto *destroy_error =
          llvm::BasicBlock::Create(scaffold_state.context, "task.destroy.error", destroy);
      llvm::IRBuilder<> destroy_done_builder(destroy_done);
      destroy_done_builder.CreateCall(
          scaffold_state.module.getOrInsertFunction(
              "chtholly_next_task_v1_frame_deallocate",
              llvm::FunctionType::get(void_type, {pointer_type}, false)),
          {destroy->getArg(0)});
      destroy_done_builder.CreateRetVoid();
      emitCoroutineCleanupChain(*destroy, frame_type, destroy->getArg(0), plan,
                                value_fields, destroy_entry, destroy_result,
                                "task.destroy");
      llvm::IRBuilder<> result_builder(destroy_result);
      if (plan.result_type == scaffold_state.sem_ir.voidType()) {
        result_builder.CreateBr(destroy_error);
      } else {
        auto *destroy_owned_result = llvm::BasicBlock::Create(
            scaffold_state.context, "task.destroy.result.owned", destroy);
        auto *flags_address =
            result_builder.CreateStructGEP(frame_type, destroy->getArg(0), 2);
        auto *flags = result_builder.CreateLoad(result_builder.getInt8Ty(),
                                                flags_address);
        result_builder.CreateCondBr(
            result_builder.CreateICmpNE(
                result_builder.CreateAnd(flags, result_builder.getInt8(1)),
                result_builder.getInt8(0)),
            destroy_owned_result, destroy_error);
        llvm::IRBuilder<> owned_result(destroy_owned_result);
        owned_result.CreateStore(
            owned_result.CreateAnd(flags, owned_result.getInt8(0xfeU)),
            flags_address);
        emitCoroutineDestroyAddress(
            plan.result_type,
            owned_result.CreateStructGEP(frame_type, destroy->getArg(0), 4),
            owned_result, *destroy);
        owned_result.CreateBr(destroy_error);
      }
      llvm::IRBuilder<> error_builder(destroy_error);
      if (!has_error_payload) {
        error_builder.CreateBr(destroy_done);
      } else {
        auto *destroy_owned_error = llvm::BasicBlock::Create(
            scaffold_state.context, "task.destroy.error.owned", destroy);
        auto *flags_address =
            error_builder.CreateStructGEP(frame_type, destroy->getArg(0), 2);
        auto *flags =
            error_builder.CreateLoad(error_builder.getInt8Ty(), flags_address);
        error_builder.CreateCondBr(
            error_builder.CreateICmpNE(
                error_builder.CreateAnd(flags, error_builder.getInt8(2)),
                error_builder.getInt8(0)),
            destroy_owned_error, destroy_done);
        llvm::IRBuilder<> owned_error(destroy_owned_error);
        owned_error.CreateStore(
            owned_error.CreateAnd(flags, owned_error.getInt8(0xfdU)),
            flags_address);
        emitCoroutineDestroyAddress(
            *plan.error_type,
            owned_error.CreateStructGEP(frame_type, destroy->getArg(0), 5),
            owned_error, *destroy);
        owned_error.CreateBr(destroy_done);
      }
      auto *move_result = llvm::Function::Create(
          llvm::FunctionType::get(void_type, {pointer_type, pointer_type},
                                  false),
          llvm::Function::InternalLinkage, base + "$move_result", scaffold_state.module);
      auto *move_entry = llvm::BasicBlock::Create(
          scaffold_state.context, "task.move_result.entry", move_result);
      llvm::IRBuilder<> move_builder(move_entry);
      if (plan.result_type != scaffold_state.sem_ir.voidType()) {
        auto *source = move_builder.CreateStructGEP(
            frame_type, move_result->getArg(0), 4, "task.result.address");
        storeValueToObject(
            move_result->getArg(1),
            loadValueFromObject(source, plan.result_type, move_builder),
            plan.result_type, move_builder);
        auto *flags_address =
            move_builder.CreateStructGEP(frame_type, move_result->getArg(0), 2);
        auto *flags =
            move_builder.CreateLoad(move_builder.getInt8Ty(), flags_address);
        move_builder.CreateStore(
            move_builder.CreateAnd(flags, move_builder.getInt8(0xfeU)),
            flags_address);
      }
      move_builder.CreateRetVoid();
      auto *move_error = llvm::Function::Create(
          llvm::FunctionType::get(void_type, {pointer_type, pointer_type},
                                  false),
          llvm::Function::InternalLinkage, base + "$move_error", scaffold_state.module);
      auto *move_error_entry = llvm::BasicBlock::Create(
          scaffold_state.context, "task.move_error.entry", move_error);
      llvm::IRBuilder<> move_error_builder(move_error_entry);
      if (has_error_payload) {
        auto *source = move_error_builder.CreateStructGEP(
            frame_type, move_error->getArg(0), 5, "task.error.address");
        storeValueToObject(
            move_error->getArg(1),
            loadValueFromObject(source, *plan.error_type, move_error_builder),
            *plan.error_type, move_error_builder);
        auto *flags_address = move_error_builder.CreateStructGEP(
            frame_type, move_error->getArg(0), 2);
        auto *flags = move_error_builder.CreateLoad(
            move_error_builder.getInt8Ty(), flags_address);
        move_error_builder.CreateStore(
            move_error_builder.CreateAnd(flags,
                                         move_error_builder.getInt8(0xfdU)),
            flags_address);
      }
      move_error_builder.CreateRetVoid();
