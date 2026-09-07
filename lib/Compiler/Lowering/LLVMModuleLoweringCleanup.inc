// Included inside ModuleLoweringContext; all methods alias its existing LLVM/LowIR state.
  llvm::Value *lowerInst(LowCallbackAdapterCall inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCallbackRegistrationService::adapterCall(
        inst, builder, function, callback_state_);
  }

  llvm::Value *lowerInst(LowForeignDefaultPromote inst,
                         llvm::IRBuilder<> &builder, llvm::Function &) {
    return LLVMForeignValueLoweringService::defaultPromote(
        inst, value(inst.arg0), low_ir_, sem_ir_, builder,
        [this](TypeId type) { return lowerValueType(type); });
  }

  llvm::Value *lowerInst(LowTransferReturn inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    LLVMTransferReturnState state{
        low_ir_, sem_ir_, context_, current_result_slot_,
        current_outcome_slot_, [this](LowInstId id) { return value(id); },
        [this](TypeId type) { return lowerObjectType(type); },
        [this](llvm::Value *destination, llvm::Value *source, TypeId type,
               llvm::IRBuilder<> &ir_builder) {
          copyObject(destination, source, type, ir_builder);
        },
        [this](llvm::Value *address, TypeId owner, std::uint32_t variant,
               std::uint32_t field, llvm::IRBuilder<> &ir_builder) {
          return enumPayloadAddress(address, owner, variant, field, ir_builder);
        },
        instruction_error_};
    return LLVMTransferReturnLoweringService::lower(inst, builder, function,
                                                    state);
  }
  llvm::Value *lowerInst(LowDestroy inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerDestroy(inst, builder, function,
                                                     cleanup_state_);
  }

  llvm::Value *lowerVecDropAddress(TypeId vec_type, llvm::Value *vec,
                                   llvm::IRBuilder<> &builder,
                                   llvm::Function &function) {
    LLVMVectorDropState state{
        sem_ir_, module_, context_,
        [this](TypeId type) { return lowerObjectType(type); },
        [this](TypeId type, llvm::Value *address,
               llvm::IRBuilder<> &ir_builder, llvm::Function &fn) {
          emitCoroutineDestroyAddress(type, address, ir_builder, fn);
        },
        instruction_error_};
    return LLVMVectorDropLoweringService::lower(vec_type, vec, builder,
                                                function, state);
  }

  llvm::Value *lowerContainerDropAddress(TypeId container_type,
                                         llvm::Value *container,
                                         llvm::IRBuilder<> &builder,
                                         llvm::Function &) {
    if (semanticType(semanticTypeRef(container_type)).kind !=
        SemTypeKind::Nominal) {
      instruction_error_ = "container lifecycle drop has a non-nominal type";
      return nullptr;
    }
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(
        semanticType(semanticTypeRef(container_type)).arg0));
    const auto name = sem_ir_.identifier(sem_ir_.name(nominal.name).text);
    if (name != "HashMap" && name != "HashSet") {
      instruction_error_ = "container lifecycle drop has an invalid owner";
      return nullptr;
    }
    auto *record = llvm::dyn_cast<llvm::StructType>(lowerObjectType(container_type));
    if (!record || record->getNumElements() == 0) {
      instruction_error_ = "container lifecycle drop has an invalid layout";
      return nullptr;
    }
    auto *table = builder.CreateLoad(
        builder.getPtrTy(), builder.CreateStructGEP(record, container, 0),
        "container.drop.table");
    builder.CreateCall(
        module_.getOrInsertFunction(
            "chtholly_next_container_v1_destroy",
            llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                    false)),
        {table});
    builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                        builder.CreateStructGEP(record, container, 0));
    return nullptr;
  }

  llvm::Value *lowerInst(LowLifecycleDestroy inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerLifecycleDestroy(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowIsInitialized inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerIsInitialized(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowMarkInitialized inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerMarkInitialized(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowMarkMoved inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerMarkMoved(inst, builder, function,
                                                      cleanup_state_);
  }

  llvm::Value *lowerInst(LowDestroyValue inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerDestroyValue(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowLifecycleDestroyValue inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerLifecycleDestroyValue(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowEndLifetime inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerEndLifetime(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowCoroutineCleanupEnd inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerCoroutineCleanupEnd(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowBranch inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerBranch(inst, builder, function,
                                                   cleanup_state_);
  }

  llvm::Value *lowerInst(LowBranchIf inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerBranchIf(inst, builder, function,
                                                     cleanup_state_);
  }

  llvm::Value *lowerInst(LowReturn inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerReturn(inst, builder, function,
                                                   cleanup_state_);
  }

  llvm::Value *lowerInst(LowReturnInPlace inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerReturnInPlace(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowUnreachable inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerUnreachable(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowFatalFailure inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMCleanupLoweringService::lowerFatalFailure(
        inst, builder, function, cleanup_state_);
  }

  llvm::Value *lowerInst(LowInvalid, llvm::IRBuilder<> &, llvm::Function &) {
    assert(false && "invalid LowIR instruction reached LLVM lowering");
    return nullptr;
  }
