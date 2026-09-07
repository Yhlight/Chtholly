// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
struct LLVMIntrinsicState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::string &instruction_error;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      copy_object;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      move_object;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      move_value;
  std::function<llvm::Value *(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      load_value_from_object;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      store_value_to_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
  std::function<void(TypeId, llvm::Value *, llvm::IRBuilder<> &,
                     llvm::Function &)>
      destroy_address;
  std::function<llvm::GlobalVariable *(TypeId)> typed_channel_descriptor;
  std::function<llvm::Value *(TypeId, llvm::Value *, llvm::Value *, unsigned,
                              std::uint32_t, TypeId, llvm::IRBuilder<> &,
                              llvm::Function &)>
      typed_channel_transition;
};

struct LLVMIntrinsicLoweringService {
  [[nodiscard]] static llvm::Value *lower(
      LowCompilerIntrinsicCall inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMIntrinsicState &state);
};

struct LLVMScalarInstructionState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::string &instruction_error;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      store_value_to_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
};

struct LLVMScalarInstructionService {
  static void trap(llvm::Value *condition, std::uint32_t reason,
                   std::string_view name, llvm::IRBuilder<> &builder,
                   llvm::Function &function,
                   LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *unary(
      LowBuiltinUnary inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *binary(
      LowBuiltinBinary inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *add(
      LowAdd inst, llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *convert(
      LowNumericConvert inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *checkedCast(
      LowCheckedNumericCast inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *equal(
      LowEqual inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *logicalNot(
      LowLogicalNot inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *logicalAnd(
      LowLogicalAnd inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
  [[nodiscard]] static llvm::Value *logicalOr(
      LowLogicalOr inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMScalarInstructionState &state);
};

struct LLVMObjectInstructionState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint32_t, llvm::GlobalVariable *> &static_globals;
  std::unordered_map<std::uint32_t, llvm::Value *> &slots;
  std::unordered_map<std::uint32_t, llvm::Value *> &place_flags;
  std::unordered_set<std::uint32_t> &initialized_slots;
  LLVMCoroutineState &coroutine;
  LLVMObjectValueState &object_value;
  std::string &instruction_error;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<bool(TypeId)> uses_pointer_value_representation;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &, llvm::Function &)>
      copy_semantic_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
  std::function<void(llvm::Value *, std::uint32_t, std::string_view,
                     llvm::IRBuilder<> &, llvm::Function &)>
      arithmetic_trap;
  std::function<void(TypeId, llvm::Value *, llvm::IRBuilder<> &, llvm::Function &)> destroy_address;
};

struct LLVMObjectInstructionService {
  [[nodiscard]] static llvm::Value *lowerRaw(
      LowInst inst, llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMObjectInstructionState &state);
  template <typename InstT>
  [[nodiscard]] static llvm::Value *lower(
      InstT inst, llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMObjectInstructionState &state) {
    return lowerRaw(
        {InstT::Kind, inst.type.index, inst.arg0.index, inst.arg1.index},
        builder, function, state);
  }
  static void copy(llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder,
                   LLVMObjectInstructionState &state);
  static void moveObject(llvm::Value *destination, llvm::Value *source,
                         TypeId type, llvm::IRBuilder<> &builder,
                         LLVMObjectInstructionState &state);
  static void moveValue(llvm::Value *destination, llvm::Value *source,
                        TypeId type, llvm::IRBuilder<> &builder,
                        LLVMObjectInstructionState &state);
  [[nodiscard]] static llvm::Value *load(
      llvm::Value *address, TypeId type, llvm::IRBuilder<> &builder,
      LLVMObjectInstructionState &state);
  static void store(llvm::Value *address, llvm::Value *source, TypeId type,
                    llvm::IRBuilder<> &builder,
                    LLVMObjectInstructionState &state);
  static void markMoved(LowPlaceId place, llvm::IRBuilder<> &builder,
                        LLVMObjectInstructionState &state);
  static void markInitialized(LowPlaceId place, llvm::IRBuilder<> &builder,
                              LLVMObjectInstructionState &state);
  [[nodiscard]] static llvm::Value *placeAddress(
      LowPlaceId place, llvm::IRBuilder<> &builder,
      LLVMObjectInstructionState &state);
  static void setSlotPlacesInitialized(
      SlotId slot, bool initialized, llvm::IRBuilder<> &builder,
      LLVMObjectInstructionState &state);
  [[nodiscard]] static llvm::Value *testCoroutineInitializationBit(
      std::uint32_t bit, llvm::IRBuilder<> &builder,
      LLVMObjectInstructionState &state);
  [[nodiscard]] static bool pathPrefix(
      std::span<const LowPlaceProjection> prefix,
      std::span<const LowPlaceProjection> path);
};
