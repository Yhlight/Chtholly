// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
struct LLVMCleanupState {
  // The cleanup service aliases the one ModuleLoweringContext instance.  No
  // LLVM context, IR map, or LowIR store is copied into the service.
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint32_t, llvm::BasicBlock *> &blocks;
  std::unordered_map<std::uint32_t, llvm::Value *> &slots;
  std::unordered_map<std::uint32_t, llvm::Value *> &place_flags;
  std::string &instruction_error;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Value *(LowPlaceId, llvm::IRBuilder<> &)> place_address;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<bool(TypeId)> uses_pointer_value_representation;
  std::function<bool()> current_representation_pack;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      store_value_to_object;
  std::function<void(LowPlaceId, llvm::IRBuilder<> &)> mark_moved;
  std::function<void(LowPlaceId, llvm::IRBuilder<> &)> mark_initialized;
  std::function<void(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      callback_adapter_release;
  std::function<llvm::Value *(TypeId, llvm::Value *, llvm::IRBuilder<> &,
                              llvm::Function &)>
      lower_vec_drop_address;
  std::function<llvm::Value *(TypeId, llvm::Value *, llvm::IRBuilder<> &,
                              llvm::Function &)>
      lower_container_drop_address;

  // Function-level cleanup state is kept as references/pointers so returns and
  // coroutine cleanup do not acquire a second lowering session.
  LowFunctionId &current_function;
  llvm::Value *&current_result_slot;
  llvm::Value *&current_outcome_slot;
};

// Lowering of instructions which mutate place lifetime/CFG state.  This is an
// implementation service only; the public LLVM API remains unchanged.
struct LLVMCleanupLoweringService {
  static llvm::Value *lowerDestroy(LowDestroy inst, llvm::IRBuilder<> &builder,
                                   llvm::Function &function,
                                   LLVMCleanupState &state);
  static llvm::Value *lowerLifecycleDestroy(
      LowLifecycleDestroy inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCleanupState &state);
  static llvm::Value *lowerIsInitialized(LowIsInitialized inst,
                                         llvm::IRBuilder<> &builder,
                                         llvm::Function &function,
                                         LLVMCleanupState &state);
  static llvm::Value *lowerMarkInitialized(LowMarkInitialized inst,
                                           llvm::IRBuilder<> &builder,
                                           llvm::Function &function,
                                           LLVMCleanupState &state);
  static llvm::Value *lowerMarkMoved(LowMarkMoved inst,
                                     llvm::IRBuilder<> &builder,
                                     llvm::Function &function,
                                     LLVMCleanupState &state);
  static llvm::Value *lowerDestroyValue(LowDestroyValue inst,
                                        llvm::IRBuilder<> &builder,
                                        llvm::Function &function,
                                        LLVMCleanupState &state);
  static llvm::Value *lowerLifecycleDestroyValue(
      LowLifecycleDestroyValue inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCleanupState &state);
  static llvm::Value *lowerEndLifetime(LowEndLifetime inst,
                                       llvm::IRBuilder<> &builder,
                                       llvm::Function &function,
                                       LLVMCleanupState &state);
  static llvm::Value *lowerCoroutineCleanupEnd(
      LowCoroutineCleanupEnd inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCleanupState &state);
  static llvm::Value *lowerBranch(LowBranch inst, llvm::IRBuilder<> &builder,
                                  llvm::Function &function,
                                  LLVMCleanupState &state);
  static llvm::Value *lowerBranchIf(LowBranchIf inst,
                                    llvm::IRBuilder<> &builder,
                                    llvm::Function &function,
                                    LLVMCleanupState &state);
  static llvm::Value *lowerReturn(LowReturn inst, llvm::IRBuilder<> &builder,
                                  llvm::Function &function,
                                  LLVMCleanupState &state);
  static llvm::Value *lowerReturnInPlace(LowReturnInPlace inst,
                                         llvm::IRBuilder<> &builder,
                                         llvm::Function &function,
                                         LLVMCleanupState &state);
  static llvm::Value *lowerUnreachable(LowUnreachable inst,
                                       llvm::IRBuilder<> &builder,
                                       llvm::Function &function,
                                       LLVMCleanupState &state);
  static llvm::Value *lowerFatalFailure(LowFatalFailure inst,
                                        llvm::IRBuilder<> &builder,
                                        llvm::Function &function,
                                        LLVMCleanupState &state);
};

// The public LLVMModule owns this implementation object, but its definition
// is shared by the module façade and the lowering translation unit. Keeping it
// in a private header avoids a back-edge through a .cpp include while the
// public LLVM.h remains unchanged.
