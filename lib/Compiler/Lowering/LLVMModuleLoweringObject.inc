// Included inside ModuleLoweringContext; all methods alias its existing LLVM/LowIR state.
  llvm::Value *lowerInst(LowParameter inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    const auto offset = currentHiddenResultCount();
    auto *argument = function.getArg(inst.arg1.index + offset);
    const auto slot_type = low_ir_.slot(inst.arg0).type;
    if (initialized_slots_.insert(inst.arg0.index).second)
      builder.CreateLifetimeStart(slots_.at(inst.arg0.index));
    storeValueToObject(slots_.at(inst.arg0.index), argument, slot_type,
                       builder);
    setSlotPlacesInitialized(inst.arg0, true, builder);
    return nullptr;
  }

  llvm::Value *lowerInst(LowParameterValue inst, llvm::IRBuilder<> &,
                         llvm::Function &function) {
    const auto offset = currentHiddenResultCount();
    return function.getArg(inst.arg0.index + offset);
  }

  llvm::Value *lowerInst(LowCall inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    LLVMCallLoweringState state{
        low_ir_, sem_ir_, context_, functions_,
        slots_, initialized_slots_,
        [this](LowInstId id) { return value(id); },
        [this](LowInstId id) { return values_.contains(id.index); },
        [this](TypeId type) { return lowerObjectType(type); },
        [this](TypeId type) { return lowerValueType(type); },
        [this](llvm::Function &fn, llvm::Type *type, std::string_view name) {
          return entryAlloca(fn, type, name);
        },
        [this](llvm::Value *destination, llvm::Value *source, TypeId type,
               llvm::IRBuilder<> &ir_builder) {
          copyObject(destination, source, type, ir_builder);
        },
        [this](llvm::Value *address, TypeId owner, std::uint32_t variant,
               std::uint32_t field, llvm::IRBuilder<> &ir_builder) {
          return enumPayloadAddress(address, owner, variant, field, ir_builder);
        },
        [this](LowPlaceId place, llvm::IRBuilder<> &ir_builder) {
          return placeAddress(place, ir_builder);
        },
        [this](LowPlaceId place, llvm::IRBuilder<> &ir_builder) {
          markInitialized(place, ir_builder);
        },
        [this](FunctionRefId target) { return fallibleConstructorShape(target); },
        instruction_error_};
    return LLVMCallLoweringService::lower(inst, builder, function, state);
  }

  llvm::Value *lowerInst(LowCompilerIntrinsicCall inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMIntrinsicLoweringService::lower(
        inst, builder, function, intrinsic_state_);
  }

  const SemTypedChannelDescriptor *typedChannelDescriptor(TypeId payload) const {
    if (!current_function_.hasValue())
      return nullptr;
    const auto owner = low_ir_.function(current_function_).semantic_function;
    for (const auto &descriptor : sem_ir_.typedChannelDescriptors())
      if (descriptor.owner_function == owner &&
          sem_ir_.canonicalType(descriptor.payload_type) ==
              sem_ir_.canonicalType(payload))
        return &descriptor;
    return nullptr;
  }

  std::pair<llvm::Function *, llvm::Function *>
  typedLifecycleThunks(TypeId payload,
                       const SemTypedChannelDescriptor &descriptor) {
    const auto key = descriptor.payload_type_fingerprint.hex() + ":" +
                     descriptor.lifecycle_fingerprint.hex();
    if (const auto found = typed_channel_thunks_.find(key);
        found != typed_channel_thunks_.end())
      return found->second;
    auto *pointer = llvm::PointerType::getUnqual(context_);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context_),
                                         {pointer, pointer}, false);
    auto *move = llvm::Function::Create(
        type, llvm::GlobalValue::InternalLinkage,
        "__chtholly_typed_move_" + descriptor.payload_type_fingerprint.hex() +
            "_" + descriptor.lifecycle_fingerprint.hex(),
        module_);
    auto *move_entry = llvm::BasicBlock::Create(context_, "entry", move);
    llvm::IRBuilder<> move_builder(move_entry);
    moveSemanticObject(move->getArg(0), move->getArg(1), payload, move_builder);
    move_builder.CreateRetVoid();
    auto *drop = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {pointer}, false),
        llvm::GlobalValue::InternalLinkage,
        "__chtholly_typed_drop_" + descriptor.payload_type_fingerprint.hex() +
            "_" + descriptor.lifecycle_fingerprint.hex(),
        module_);
    auto *drop_entry = llvm::BasicBlock::Create(context_, "entry", drop);
    llvm::IRBuilder<> drop_builder(drop_entry);
    emitCoroutineDestroyAddress(payload, drop->getArg(0), drop_builder, *drop);
    drop_builder.CreateRetVoid();
    return typed_channel_thunks_.emplace(key, std::make_pair(move, drop)).first->second;
  }

  llvm::GlobalVariable *typedChannelRuntimeDescriptor(
      TypeId payload, const SemTypedChannelDescriptor &descriptor) {
    const auto key = descriptor.payload_type_fingerprint.hex() + ":" +
                     descriptor.lifecycle_fingerprint.hex();
    if (const auto found = typed_channel_descriptors_.find(key);
        found != typed_channel_descriptors_.end())
      return found->second;
    const auto thunks = typedLifecycleThunks(payload, descriptor);
    auto *pointer = llvm::PointerType::getUnqual(context_);
    auto *record = llvm::StructType::get(
        context_, {llvm::Type::getInt32Ty(context_),
                   llvm::Type::getInt32Ty(context_),
                   llvm::Type::getInt64Ty(context_),
                   llvm::Type::getInt64Ty(context_), pointer, pointer});
    auto *layout = lowerObjectType(payload);
    const auto size = module_.getDataLayout().getTypeAllocSize(layout);
    const auto alignment = module_.getDataLayout().getABITypeAlign(layout).value();
    auto *initializer = llvm::ConstantStruct::get(
        record,
        {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 1),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 3),
         llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), size),
         llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), alignment),
         llvm::ConstantExpr::getBitCast(thunks.first, pointer),
         llvm::ConstantExpr::getBitCast(thunks.second, pointer)});
    auto *global = new llvm::GlobalVariable(
        module_, record, true, llvm::GlobalValue::InternalLinkage, initializer,
        "__chtholly_typed_descriptor_" + descriptor.payload_type_fingerprint.hex() +
            "_" + descriptor.lifecycle_fingerprint.hex());
    return typed_channel_descriptors_.emplace(key, global).first->second;
  }

  llvm::Value *lowerTypedChannelTransition(TypeId payload, llvm::Value *channel,
                                           llvm::Value *source,
                                           unsigned operation,
                                           std::uint32_t channel_key,
                                           TypeId channel_type,
                                           llvm::IRBuilder<> &builder,
                                           llvm::Function &function) {
    if (!payload.hasValue()) {
      const auto found = typed_payloads_.find(channel_key);
      if (found != typed_payloads_.end())
        payload = found->second;
    } else {
      typed_payloads_.insert_or_assign(channel_key, payload);
    }
    const auto *descriptor = typedChannelDescriptor(payload);
    if (!descriptor) {
      instruction_error_ = "typed channel transition has no verified descriptor";
      return nullptr;
    }
    auto *runtime_descriptor = typedChannelRuntimeDescriptor(payload, *descriptor);
    auto *pointer = llvm::PointerType::getUnqual(context_);
    auto *token_type = llvm::StructType::get(
        context_, {pointer, pointer, pointer, pointer,
                   llvm::Type::getInt64Ty(context_),
                   llvm::Type::getInt32Ty(context_),
                   llvm::Type::getInt32Ty(context_)});
    const auto token_for = [&] {
      if (const auto found = typed_tokens_.find(channel_key);
          found != typed_tokens_.end())
        return found->second;
      auto *slot = entryAlloca(function, token_type, "typed.channel.token");
      typed_tokens_.emplace(channel_key, slot);
      return static_cast<llvm::Value *>(slot);
    };
    if (channel_type.hasValue() &&
        semanticType(semanticTypeRef(channel_type)).kind ==
            SemTypeKind::Reference) {
      const auto pointee = sem_ir_.referencePointee(channel_type);
      if (semanticType(semanticTypeRef(pointee)).kind == SemTypeKind::Nominal) {
        auto *field = builder.CreateStructGEP(lowerObjectType(pointee), channel, 0,
                                              "typed.channel.handle");
        channel = builder.CreateLoad(llvm::PointerType::getUnqual(context_),
                                     field, "typed.channel.handle.value");
      } else {
        channel = loadValueFromObject(channel, pointee, builder);
      }
    } else if (channel_type.hasValue() &&
               semanticType(semanticTypeRef(channel_type)).kind ==
                   SemTypeKind::Nominal) {
      auto *field = builder.CreateStructGEP(lowerObjectType(channel_type), channel,
                                            0, "typed.channel.handle");
      channel = builder.CreateLoad(llvm::PointerType::getUnqual(context_), field,
                                   "typed.channel.handle.value");
    }
    auto *channel_ptr = builder.CreateBitCast(channel, pointer);
    llvm::FunctionCallee callee;
    llvm::SmallVector<llvm::Value *, 3> args;
    if (operation == 0) {
      if (source && !source->getType()->isPointerTy()) {
        auto *storage = entryAlloca(function, lowerObjectType(payload),
                                     "typed.channel.send.value");
        storeValueToObject(storage, source, payload, builder);
        source = storage;
      }
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_send_prepare",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer, pointer, pointer}, false));
      args = {channel_ptr, builder.CreateBitCast(source, pointer), token_for()};
    } else if (operation == 1) {
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_send_commit",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer}, false));
      args = {token_for()};
    } else if (operation == 2) {
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_send_cancel",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer}, false));
      args = {token_for()};
    } else if (operation == 3) {
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_receive_acquire",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer, pointer}, false));
      args = {channel_ptr, token_for()};
    } else if (operation == 4) {
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_receive_commit",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer, pointer}, false));
      args = {token_for(), builder.CreateBitCast(source, pointer)};
    } else if (operation == 5) {
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_receive_cancel",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer}, false));
      args = {token_for()};
    } else {
      callee = module_.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_close",
          llvm::FunctionType::get(builder.getInt32Ty(), {pointer}, false));
      args = {channel_ptr};
    }
    (void)runtime_descriptor;
    return builder.CreateCall(callee, args);
  }

  TypeId typedChannelPayload(TypeId channel_type) const {
    if (channel_type.hasValue() &&
        semanticType(semanticTypeRef(channel_type)).kind ==
            SemTypeKind::Reference)
      channel_type = sem_ir_.referencePointee(channel_type);
    if (!channel_type.hasValue() ||
        semanticType(semanticTypeRef(channel_type)).kind !=
            SemTypeKind::Nominal)
      return TypeId::invalid();
    const auto arguments = sem_ir_.typeBlock(
        TypeBlockId(semanticType(semanticTypeRef(channel_type)).arg1));
    return arguments.size() == 1 ? arguments.front() : TypeId::invalid();
  }

  llvm::Value *lowerInst(LowTypedChannelSendPrepare inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    auto payload = TypeId(low_ir_.inst(inst.arg1).type);
    if (semanticType(semanticTypeRef(payload)).kind == SemTypeKind::Reference)
      payload = sem_ir_.referencePointee(payload);
    return lowerTypedChannelTransition(payload, value(inst.arg0), value(inst.arg1),
                                        0, inst.arg0.index,
                                        TypeId(low_ir_.inst(inst.arg0).type),
                                        builder, function);
  }
  llvm::Value *lowerInst(LowTypedChannelSendCommit inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return lowerTypedChannelTransition(TypeId(low_ir_.inst(inst.arg1).type), value(inst.arg0), value(inst.arg1),
                                       1, inst.arg0.index,
                                       TypeId(low_ir_.inst(inst.arg0).type),
                                       builder, function);
  }
  llvm::Value *lowerInst(LowTypedChannelSendCancel inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return lowerTypedChannelTransition(typedChannelPayload(TypeId(low_ir_.inst(inst.arg0).type)), value(inst.arg0), nullptr, 2, inst.arg0.index,
                                       TypeId(low_ir_.inst(inst.arg0).type),
                                       builder, function);
  }
  llvm::Value *lowerInst(LowTypedChannelReceiveAcquire inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return lowerTypedChannelTransition(typedChannelPayload(TypeId(low_ir_.inst(inst.arg0).type)), value(inst.arg0), nullptr, 3, inst.arg0.index,
                                       TypeId(low_ir_.inst(inst.arg0).type),
                                       builder, function);
  }
  llvm::Value *lowerInst(LowTypedChannelReceiveCommit inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return lowerTypedChannelTransition(TypeId(low_ir_.inst(inst.arg1).type), value(inst.arg0), value(inst.arg1),
                                       4, inst.arg0.index,
                                       TypeId(low_ir_.inst(inst.arg0).type),
                                       builder, function);
  }
  llvm::Value *lowerInst(LowTypedChannelReceiveCancel inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return lowerTypedChannelTransition(typedChannelPayload(TypeId(low_ir_.inst(inst.arg0).type)), value(inst.arg0), nullptr, 5, inst.arg0.index,
                                       TypeId(low_ir_.inst(inst.arg0).type),
                                       builder, function);
  }
  llvm::Value *lowerInst(LowTypedChannelClose inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    return lowerTypedChannelTransition(typedChannelPayload(TypeId(low_ir_.inst(inst.arg0).type)), value(inst.arg0), nullptr, 6, inst.arg0.index,
                                       TypeId(low_ir_.inst(inst.arg0).type),
                                       builder, function);
  }
  llvm::Value *lowerInst(LowConstruct inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    LLVMCallLoweringState state{
        low_ir_, sem_ir_, context_, functions_, slots_, initialized_slots_,
        [this](LowInstId id) { return value(id); },
        [this](LowInstId id) { return values_.contains(id.index); },
        [this](TypeId type) { return lowerObjectType(type); },
        [this](TypeId type) { return lowerValueType(type); },
        [this](llvm::Function &fn, llvm::Type *type, std::string_view name) {
          return entryAlloca(fn, type, name);
        },
        [this](llvm::Value *destination, llvm::Value *source, TypeId type,
               llvm::IRBuilder<> &ir_builder) {
          copyObject(destination, source, type, ir_builder);
        },
        [this](llvm::Value *address, TypeId owner, std::uint32_t variant,
               std::uint32_t field, llvm::IRBuilder<> &ir_builder) {
          return enumPayloadAddress(address, owner, variant, field, ir_builder);
        },
        [this](LowPlaceId place, llvm::IRBuilder<> &ir_builder) {
          return placeAddress(place, ir_builder);
        },
        [this](LowPlaceId place, llvm::IRBuilder<> &ir_builder) {
          markInitialized(place, ir_builder);
        },
        [this](FunctionRefId target) { return fallibleConstructorShape(target); },
        instruction_error_};
    return LLVMCallLoweringService::construct(inst, builder, function, state);
  }

  llvm::Value *lowerInst(LowFunctionValue inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    const auto found = functions_.find(inst.arg0.index);
    if (found != functions_.end())
      return found->second;
    instruction_error_ = "function value target has no LLVM declaration";
    return llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(context_));
  }

  llvm::Value *lowerInst(LowIndirectCall inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    LLVMCallLoweringState state{
        low_ir_, sem_ir_, context_, functions_,
        slots_, initialized_slots_,
        [this](LowInstId id) { return value(id); },
        [this](LowInstId id) { return values_.contains(id.index); },
        [this](TypeId type) { return lowerObjectType(type); },
        [this](TypeId type) { return lowerValueType(type); },
        [this](llvm::Function &fn, llvm::Type *type, std::string_view name) {
          return entryAlloca(fn, type, name);
        },
        [this](llvm::Value *destination, llvm::Value *source, TypeId type,
               llvm::IRBuilder<> &ir_builder) {
          copyObject(destination, source, type, ir_builder);
        },
        [this](llvm::Value *address, TypeId owner, std::uint32_t variant,
               std::uint32_t field, llvm::IRBuilder<> &ir_builder) {
          return enumPayloadAddress(address, owner, variant, field, ir_builder);
        },
        [this](LowPlaceId place, llvm::IRBuilder<> &ir_builder) {
          return placeAddress(place, ir_builder);
        },
        [this](LowPlaceId place, llvm::IRBuilder<> &ir_builder) {
          markInitialized(place, ir_builder);
        },
        [this](FunctionRefId target) { return fallibleConstructorShape(target); },
        instruction_error_};
    return LLVMCallLoweringService::indirect(inst, builder, function, state);
  }
