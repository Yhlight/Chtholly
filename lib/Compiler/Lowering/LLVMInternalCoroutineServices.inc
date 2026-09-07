// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
  static void emitProtocolTrap(std::uint32_t reason, llvm::Module &module,
                               llvm::IRBuilder<> &builder);
  [[nodiscard]] static llvm::Value *makeChecked(
      llvm::Value *status, llvm::Value *storage, TypeId checked_type,
      llvm::IRBuilder<> &builder,
      const std::function<llvm::Type *(TypeId)> &lower_value_type);
};

// Per-function coroutine instruction state. The runtime frame state remains
// owned by ModuleLoweringContext; this view only provides the module/error
// channels and the already-established cancellation target callback.
struct LLVMCoroutineInstructionState {
  llvm::Module &module;
  llvm::LLVMContext &context;
  LLVMCoroutineState &coroutine;
  std::string &instruction_error;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::BasicBlock *(llvm::IRBuilder<> &, llvm::Function &)>
      cancellation_target;
};

struct LLVMCoroutineInstructionService {
  static llvm::Value *lowerCancellationCheck(
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCoroutineInstructionState &state);
  static llvm::Value *lowerCancellationRequested(
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCoroutineInstructionState &state);
  static llvm::Value *lowerRuntimeFault(
      LowCoroutineRuntimeFault inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, const SemIR &sem_ir,
      LLVMCoroutineInstructionState &state);
  static llvm::Value *lowerTerminalError(std::string_view message,
                                         llvm::IRBuilder<> &builder,
                                         llvm::Function &function,
                                         LLVMCoroutineInstructionState &state);
  static llvm::Value *lowerExecutorSwitch(
      LowCoroutineExecutorSwitch inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineInstructionState &state);
};

struct LLVMCoroutineTaskCreateState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  LLVMCoroutineState &coroutine;
  std::unordered_map<std::uint32_t, llvm::Function *> &constructors;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Value *(llvm::Value *, llvm::Value *, TypeId,
                              llvm::IRBuilder<> &)>
      make_checked;
  std::function<std::string(const PublicEntity &)> constructor_symbol;
};

struct LLVMCoroutineTaskCreateService {
  [[nodiscard]] static llvm::Value *lower(
      LowCoroutineTaskCreate inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineTaskCreateState &state);
};

struct LLVMCoroutineTaskGroupState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  LLVMCoroutineState &coroutine;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<void(std::uint32_t, llvm::IRBuilder<> &)> protocol_trap;
};

struct LLVMCoroutineTaskGroupService {
  [[nodiscard]] static llvm::Value *create(
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCoroutineTaskGroupState &state);
  [[nodiscard]] static llvm::Value *attach(
      LowCoroutineTaskGroupAttach inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineTaskGroupState &state);
  [[nodiscard]] static llvm::Value *requestCancel(
      LowCoroutineTaskGroupRequestCancel inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineTaskGroupState &state);
  [[nodiscard]] static llvm::Value *close(
      LowCoroutineTaskGroupClose inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineTaskGroupState &state);
  [[nodiscard]] static llvm::Value *completionArm(
      LowCoroutineTaskGroupCompletionArm inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineTaskGroupState &state);
};

struct LLVMCoroutineCompletionState {
  LLVMCoroutineTaskGroupState &task_group;
  std::function<llvm::Value *(llvm::Value *, llvm::Value *, TypeId,
                              llvm::IRBuilder<> &)>
      make_checked;
};

struct LLVMCoroutineCompletionService {
  [[nodiscard]] static llvm::Value *arm(
      LowCoroutineTaskCompletionArm inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineCompletionState &state);
  [[nodiscard]] static llvm::Value *ready(
      LowCoroutineTaskCompletionReady inst, llvm::IRBuilder<> &builder,
      LLVMCoroutineCompletionState &state);
  static void finish(LowFinishCoroutineTaskCompletion inst,
                     llvm::IRBuilder<> &builder,
                     LLVMCoroutineCompletionState &state);
};

struct LLVMCompletionCombineProbe {
  llvm::Value *ready = nullptr;
  llvm::Value *result = nullptr;
};

struct LLVMCoroutineCompletionSetState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  LLVMCoroutineState &coroutine;
  LLVMCallbackWakeState &callback_wake;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Value *(llvm::Value *, llvm::Value *, TypeId,
                              llvm::IRBuilder<> &)>
      make_checked;
  std::function<void(const CallbackWakePlan &, llvm::Value *,
                     llvm::IRBuilder<> &, llvm::Function &)>
      detach_completion;
};

struct LLVMCoroutineCompletionSetService {
  static void arm(const CompletionProviderPlan &provider, llvm::Value *set,
                  std::uint32_t count, llvm::IRBuilder<> &builder,
                  llvm::Function &function,
                  LLVMCoroutineCompletionSetState &state);
  static void detach(const CompletionProviderPlan &provider, llvm::Value *set,
                     std::uint32_t count, llvm::IRBuilder<> &builder,
                     llvm::Function &function,
                     LLVMCoroutineCompletionSetState &state);
  static void release(const CompletionProviderPlan &provider, llvm::Value *set,
                      std::uint32_t count, llvm::IRBuilder<> &builder,
                      llvm::Function &function,
                      LLVMCoroutineCompletionSetState &state);
  [[nodiscard]] static LLVMCompletionCombineProbe probe(
      const CoroutineTaskCompletionCombinePlan &plan, llvm::Value *set,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCoroutineCompletionSetState &state);
  [[nodiscard]] static llvm::Value *create(
      LowCoroutineTaskCompletionSetCreate inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineCompletionSetState &state);
  [[nodiscard]] static llvm::Value *combine(
      LowCoroutineTaskCompletionCombine inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineCompletionSetState &state);
  [[nodiscard]] static llvm::Value *selectionWinner(
      LowCoroutineTaskSelectionWinner inst, llvm::IRBuilder<> &builder,
      LLVMCoroutineCompletionSetState &state);
  [[nodiscard]] static llvm::Value *selectionRemaining(
      LowCoroutineTaskSelectionTakeRemaining inst, llvm::IRBuilder<> &builder,
      LLVMCoroutineCompletionSetState &state);
  static void finishSet(LowFinishCoroutineTaskCompletionSet inst,
                        llvm::IRBuilder<> &builder, llvm::Function &function,
                        LLVMCoroutineCompletionSetState &state);
  static void finishSelection(LowFinishCoroutineTaskSelection inst,
                              llvm::IRBuilder<> &builder,
                              llvm::Function &function,
                              LLVMCoroutineCompletionSetState &state);
};

struct LLVMCoroutineTaskResultState {
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Value *(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      load_value_from_object;
  std::function<llvm::Value *(llvm::Value *, llvm::Value *, TypeId,
                              llvm::IRBuilder<> &)>
      make_checked;
};

struct LLVMCoroutineTaskResultService {
  [[nodiscard]] static llvm::Value *join(
      LowCoroutineTaskJoin inst, llvm::IRBuilder<> &builder,
      LLVMCoroutineTaskResultState &state);
  [[nodiscard]] static llvm::Value *query(
      LowCoroutineTaskQuery inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineTaskResultState &state);
  [[nodiscard]] static llvm::Value *take(
      LowInstId task, TypeId checked_type, bool error,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCoroutineTaskResultState &state);
  [[nodiscard]] static llvm::Value *checkedStatus(
      LowCoroutineCheckedStatus inst, llvm::IRBuilder<> &builder,
      LLVMCoroutineTaskResultState &state);
  [[nodiscard]] static llvm::Value *checkedTake(
      LowCoroutineCheckedTake inst, llvm::IRBuilder<> &builder,
      LLVMCoroutineTaskResultState &state);
  [[nodiscard]] static llvm::Value *outcome(
      LowInstId value, std::uint32_t expected, llvm::IRBuilder<> &builder,
      LLVMCoroutineTaskResultState &state);
  static void finishTask(LowFinishCoroutineTask inst,
                         llvm::IRBuilder<> &builder,
                         LLVMCoroutineTaskResultState &state);
};

struct LLVMCoroutineScaffoldState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint32_t, llvm::Function *> &constructors;
  std::unordered_map<std::uint32_t, llvm::Value *> &values;
  std::unordered_map<std::uint32_t, llvm::Value *> &slots;
  std::unordered_map<std::uint32_t, llvm::Value *> &place_flags;
  std::unordered_map<std::uint32_t, llvm::BasicBlock *> &blocks;
  std::unordered_map<std::uint32_t, FunctionRefId> &local_function_refs;
  std::unordered_set<std::uint32_t> &initialized_slots;
  LLVMCoroutineState &coroutine;
  llvm::Value *&current_result_slot;
  llvm::Value *&current_outcome_slot;
  LowFunctionId &current_function;
  ModuleEmissionRole emission_role;
  llvm::Function *&source_entry;
  std::uint32_t &entry_candidate_count;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<llvm::Value *(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      load_value_from_object;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      store_value_to_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
  std::function<void(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      release_callback_adapter;
  std::function<llvm::Value *(CallbackCompletionPlanId, llvm::Value *,
                              llvm::IRBuilder<> &, llvm::Function &)>
      finish_callback_completion;
  std::function<void(llvm::Value *, const CallbackWakePlan &,
                     llvm::IRBuilder<> &)>
      release_wake_adapter;
  std::function<void(const CompletionProviderPlan &, llvm::Value *,
                     std::uint32_t, llvm::IRBuilder<> &, llvm::Function &)>
      arm_completion_set;
  std::function<void(const CompletionProviderPlan &, llvm::Value *,
                     std::uint32_t, llvm::IRBuilder<> &, llvm::Function &)>
      detach_completion_set;
  std::function<void(const CompletionProviderPlan &, llvm::Value *,
                     std::uint32_t, llvm::IRBuilder<> &, llvm::Function &)>
      release_completion_set;
  std::function<LLVMCompletionCombineProbe(
      const CoroutineTaskCompletionCombinePlan &, llvm::Value *,
      llvm::IRBuilder<> &, llvm::Function &)>
      probe_completion_set;
  std::function<bool(std::span<const LowPlaceProjection>,
                     std::span<const LowPlaceProjection>)>
      path_prefix;
  std::function<llvm::Value *(std::uint32_t, llvm::IRBuilder<> &)>
      test_initialization_bit;
  std::function<void(SlotId, bool, llvm::IRBuilder<> &)>
      set_slot_places_initialized;
  std::function<void(LowInstId, llvm::IRBuilder<> &, llvm::Function &)>
      lower_instruction;
  std::function<llvm::Value *(
      const ForeignAbiFunctionLayout &, const ForeignAbiCallLayout &,
      llvm::Value *, std::span<llvm::Value *const>, llvm::IRBuilder<> &,
      llvm::Function &)>
      emit_foreign_call_values;
  std::function<void(std::uint32_t, llvm::IRBuilder<> &)> protocol_trap;
  std::function<llvm::BasicBlock *(llvm::IRBuilder<> &, llvm::Function &)>
      cancellation_target;
  std::function<std::string(const PublicEntity &)> constructor_symbol;
};

struct LLVMCoroutineScaffoldService {
  [[nodiscard]] static bool registerConstructors(
      std::string &error, LLVMCoroutineScaffoldState &state);
  static void finishTaskGroupDrain(
      llvm::Value *group, const CoroutineResumeState &resume_state,
      llvm::BasicBlock *continuation, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCoroutineScaffoldState &state);
  static void cleanupChain(
      llvm::Function &function, llvm::StructType *frame_type,
      llvm::Value *frame, const CoroutineFramePlan &plan,
      const std::unordered_map<std::uint32_t, unsigned> &value_fields,
      llvm::BasicBlock *start, llvm::BasicBlock *terminal,
      std::string_view prefix, LLVMCoroutineScaffoldState &state);
  [[nodiscard]] static llvm::Function *taskDriverHost(
      llvm::Function &driver, LLVMCoroutineScaffoldState &state);
  static void destroyAddress(TypeId type, llvm::Value *address,
                             llvm::IRBuilder<> &builder,
                             llvm::Function &function,
                             LLVMCoroutineScaffoldState &state);
  static void detachCompletion(const CallbackWakePlan &plan,
                               llvm::Value *completion,
                               llvm::IRBuilder<> &builder,
                               llvm::Function &function,
                               LLVMCoroutineScaffoldState &state);
  static void cleanupGraph(llvm::Function &function,
                           llvm::StructType *frame_type, llvm::Value *frame,
                           const CoroutineFramePlan &plan,
                           CoroutineCleanupGraphId graph_id,
                           llvm::BasicBlock *entry,
                           llvm::BasicBlock *continuation,
                           std::string_view prefix, bool preserve_result,
                           LLVMCoroutineScaffoldState &state);
  [[nodiscard]] static bool emit(std::string &error,
                                 LLVMCoroutineScaffoldState &state);
};

