// Included inside ModuleLoweringContext; all methods alias its existing LLVM/LowIR state.
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return values_.at(id.index);
  }

  llvm::Value *lowerInst(LowIntegerConstant inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return LLVMValueLoweringService::integer(
        inst, sem_ir_, [this](TypeId type) { return lowerObjectType(type); });
  }

  llvm::Value *lowerInst(LowFloatConstant inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return LLVMValueLoweringService::floating(inst, sem_ir_, context_);
  }

  llvm::Value *lowerInst(LowBoolConstant inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return LLVMValueLoweringService::boolean(inst, sem_ir_, context_);
  }

  llvm::Value *lowerInst(LowVoidValue, llvm::IRBuilder<> &, llvm::Function &) {
    return nullptr;
  }

  llvm::Value *lowerInst(LowStringConstant inst, llvm::IRBuilder<> &builder,
                         llvm::Function &) {
    return semanticType(semanticTypeRef(inst.type)).kind ==
                   SemTypeKind::RawPointer
               ? cstringConstant(inst.arg0)
               : stringConstant(inst.arg0, builder);
  }

  llvm::Value *lowerInst(LowNullPointer inst, llvm::IRBuilder<> &,
                         llvm::Function &) {
    return LLVMValueLoweringService::nullPointer(
        inst, [this](TypeId type) { return lowerValueType(type); });
  }

  void copyObject(llvm::Value *destination, llvm::Value *source, TypeId type,
                  llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::copy(
        destination, source, type, builder, object_instruction_state_);
  }
  void moveSemanticObject(llvm::Value *destination, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::moveObject(
        destination, source, type, builder, object_instruction_state_);
  }
  void moveSemanticValueToObject(llvm::Value *destination, llvm::Value *source,
                                 TypeId type, llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::moveValue(
        destination, source, type, builder, object_instruction_state_);
  }
  llvm::Value *loadValueFromObject(llvm::Value *address, TypeId type,
                                   llvm::IRBuilder<> &builder) {
    return LLVMObjectInstructionService::load(
        address, type, builder, object_instruction_state_);
  }
  void storeValueToObject(llvm::Value *address, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::store(
        address, source, type, builder, object_instruction_state_);
  }
  void markMoved(LowPlaceId place, llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::markMoved(
        place, builder, object_instruction_state_);
  }
  void markInitialized(LowPlaceId place, llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::markInitialized(
        place, builder, object_instruction_state_);
  }
  llvm::Value *placeAddress(LowPlaceId place, llvm::IRBuilder<> &builder) {
    return LLVMObjectInstructionService::placeAddress(
        place, builder, object_instruction_state_);
  }
  void setSlotPlacesInitialized(SlotId slot, bool initialized,
                                llvm::IRBuilder<> &builder) {
    LLVMObjectInstructionService::setSlotPlacesInitialized(
        slot, initialized, builder, object_instruction_state_);
  }
  llvm::Value *testCoroutineInitializationBit(
      std::uint32_t bit, llvm::IRBuilder<> &builder) {
    return LLVMObjectInstructionService::testCoroutineInitializationBit(
        bit, builder, object_instruction_state_);
  }
  static bool pathPrefix(std::span<const LowPlaceProjection> prefix,
                         std::span<const LowPlaceProjection> path) {
    return LLVMObjectInstructionService::pathPrefix(prefix, path);
  }

#define CHTHOLLY_LLVM_OBJECT_INST(Name)                                        \
  llvm::Value *lowerInst(Low##Name inst, llvm::IRBuilder<> &builder,           \
                         llvm::Function &function) {                            \
    return LLVMObjectInstructionService::lower(                                \
        inst, builder, function, object_instruction_state_);                    \
  }

  CHTHOLLY_LLVM_OBJECT_INST(StaticLoad)
  CHTHOLLY_LLVM_OBJECT_INST(MakeArray)
  CHTHOLLY_LLVM_OBJECT_INST(MakeTuple)
  CHTHOLLY_LLVM_OBJECT_INST(MakeAggregate)
  CHTHOLLY_LLVM_OBJECT_INST(MakeUnion)
  CHTHOLLY_LLVM_OBJECT_INST(MakeEnum)
  CHTHOLLY_LLVM_OBJECT_INST(MakeObject)
  CHTHOLLY_LLVM_OBJECT_INST(MakeObjectCopy)
  CHTHOLLY_LLVM_OBJECT_INST(MakeObjectMove)
  CHTHOLLY_LLVM_OBJECT_INST(Load)
  CHTHOLLY_LLVM_OBJECT_INST(LoadPlace)
  CHTHOLLY_LLVM_OBJECT_INST(Borrow)
  CHTHOLLY_LLVM_OBJECT_INST(BorrowPlace)
  CHTHOLLY_LLVM_OBJECT_INST(CarrierView)
  CHTHOLLY_LLVM_OBJECT_INST(ObjectAddress)
  CHTHOLLY_LLVM_OBJECT_INST(PlaceAddress)
  CHTHOLLY_LLVM_OBJECT_INST(Dereference)
  CHTHOLLY_LLVM_OBJECT_INST(DereferenceObject)
  CHTHOLLY_LLVM_OBJECT_INST(PackValue)
  CHTHOLLY_LLVM_OBJECT_INST(UnpackValue)
  CHTHOLLY_LLVM_OBJECT_INST(Initialize)
  CHTHOLLY_LLVM_OBJECT_INST(Transfer)
  CHTHOLLY_LLVM_OBJECT_INST(InitializeFromValue)
  CHTHOLLY_LLVM_OBJECT_INST(MoveOut)
  CHTHOLLY_LLVM_OBJECT_INST(CopyValue)
  CHTHOLLY_LLVM_OBJECT_INST(LifecycleCopy)
  CHTHOLLY_LLVM_OBJECT_INST(InitializePlace)
  CHTHOLLY_LLVM_OBJECT_INST(InitializePlaceFromValue)
  CHTHOLLY_LLVM_OBJECT_INST(StringLength)
  CHTHOLLY_LLVM_OBJECT_INST(MakeSlice)
  CHTHOLLY_LLVM_OBJECT_INST(TupleElement)
  CHTHOLLY_LLVM_OBJECT_INST(SliceLength)
  CHTHOLLY_LLVM_OBJECT_INST(StructField)
  CHTHOLLY_LLVM_OBJECT_INST(UnionField)
  CHTHOLLY_LLVM_OBJECT_INST(EnumTag)
  CHTHOLLY_LLVM_OBJECT_INST(EnumPayload)
  CHTHOLLY_LLVM_OBJECT_INST(ProjectionLoad)
  CHTHOLLY_LLVM_OBJECT_INST(ProjectionTake)
  CHTHOLLY_LLVM_OBJECT_INST(ProjectionStore)
  CHTHOLLY_LLVM_OBJECT_INST(ProjectionInit)
  CHTHOLLY_LLVM_OBJECT_INST(ProjectionBorrow)
  CHTHOLLY_LLVM_OBJECT_INST(ProjectionBorrowMut)
  CHTHOLLY_LLVM_OBJECT_INST(IndexStore)
  CHTHOLLY_LLVM_OBJECT_INST(ArrayIndex)
  CHTHOLLY_LLVM_OBJECT_INST(DynamicIndexBorrow)

#undef CHTHOLLY_LLVM_OBJECT_INST
  void emitArithmeticTrap(llvm::Value *condition, std::uint32_t reason,
                          std::string_view name,
                          llvm::IRBuilder<> &builder,
                          llvm::Function &function) {
    LLVMScalarInstructionService::trap(
        condition, reason, name, builder, function, scalar_instruction_state_);
  }

  llvm::Value *lowerInst(LowBuiltinUnary inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::unary(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowBuiltinBinary inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::binary(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowAdd inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::add(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowNumericConvert inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::convert(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowCheckedNumericCast inst,
                         llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::checkedCast(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowEqual inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::equal(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowLogicalNot inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::logicalNot(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowLogicalAnd inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::logicalAnd(
        inst, builder, function, scalar_instruction_state_);
  }
  llvm::Value *lowerInst(LowLogicalOr inst, llvm::IRBuilder<> &builder,
                         llvm::Function &function) {
    return LLVMScalarInstructionService::logicalOr(
        inst, builder, function, scalar_instruction_state_);
  }
