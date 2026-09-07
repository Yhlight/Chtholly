// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
struct LLVMInteropState {
  std::unordered_map<std::uint64_t, llvm::Function *> &callback_thunks;
  std::span<const std::pair<std::string, std::string>> runtime_symbol_mappings;
  std::span<const ComponentExportLoweringPlan> component_exports;
};

// Component wrappers are emitted from the already-lowered function table.
// This state is a non-owning view over the active module lowering session;
// the service must not rebuild LLVM contexts, function maps, or ABI facts.
struct LLVMComponentExportState {
  llvm::Module &module;
  llvm::LLVMContext &context;
  const std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::span<const ComponentExportLoweringPlan> exports;
};

struct LLVMComponentExportLoweringService {
  [[nodiscard]] static bool emit(LLVMComponentExportState &state,
                                 std::string &error);
};

struct LLVMInteropLoweringService {
  [[nodiscard]] static llvm::Type *lowerForeignLane(
      const ForeignAbiLane &lane, llvm::LLVMContext &context);
  [[nodiscard]] static std::vector<llvm::Type *> foreignPhysicalTypes(
      const ForeignAbiValueLayout &layout, llvm::LLVMContext &context,
      const std::function<llvm::Type *(TypeId)> &lower_value_type,
      const std::function<llvm::Type *(const ForeignAbiLane &)> &lower_lane);
  [[nodiscard]] static llvm::FunctionType *foreignFunctionType(
      const ForeignAbiFunctionLayout &layout, llvm::LLVMContext &context,
      const std::function<std::vector<llvm::Type *>(
          const ForeignAbiValueLayout &)> &physical_types,
      const std::function<llvm::Type *(TypeId)> &lower_object_type);
  static void applyForeignAttributes(
      llvm::Function &function, const ForeignAbiFunctionLayout &layout,
      llvm::LLVMContext &context,
      const std::function<llvm::Type *(TypeId)> &lower_object_type,
      const std::function<std::size_t(const ForeignAbiValueLayout &)>
          &physical_count);
  static void applyForeignAttributes(
      llvm::CallBase &call, const ForeignAbiFunctionLayout &layout,
      llvm::LLVMContext &context,
      const std::function<llvm::Type *(TypeId)> &lower_object_type,
      const std::function<std::size_t(const ForeignAbiValueLayout &)>
          &physical_count);
};

struct LLVMForeignCallState {
  llvm::LLVMContext &context;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      copy_object;
  std::function<llvm::Type *(const ForeignAbiLane &)> lower_foreign_lane;
  std::function<llvm::FunctionType *(const ForeignAbiFunctionLayout &)>
      foreign_function_type;
  std::function<void(llvm::CallBase &, const ForeignAbiFunctionLayout &)>
      apply_foreign_attributes;
};

struct LLVMForeignCallLoweringService {
  [[nodiscard]] static llvm::Value *emitValues(
      const ForeignAbiFunctionLayout &layout,
      const ForeignAbiCallLayout &call_layout, llvm::Value *callee,
      std::span<llvm::Value *const> semantic_arguments,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMForeignCallState &state);
};

struct LLVMForeignResultState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Value *(
      const ForeignAbiFunctionLayout &, const ForeignAbiCallLayout &,
      llvm::Value *, std::span<llvm::Value *const>, llvm::IRBuilder<> &,
      llvm::Function &)>
      emit_foreign_call_values;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      store_value_to_object;
};

struct LLVMForeignResultLoweringService {
  [[nodiscard]] static llvm::Value *lower(
      LowForeignResultCall inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMForeignResultState &state);
};

struct LLVMForeignValueState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<bool(TypeId)> uses_pointer_value_representation;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                    llvm::StringRef)> entry_alloca;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      store_value_to_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      load_value_from_object;
};

struct LLVMForeignValueLoweringService {
  [[nodiscard]] static llvm::Value *defaultPromote(
      LowForeignDefaultPromote inst, llvm::Value *source,
      const LowIR &low_ir, const SemIR &sem_ir, llvm::IRBuilder<> &builder,
      const std::function<llvm::Type *(TypeId)> &lower_value_type);
  [[nodiscard]] static llvm::Constant *invalidSentinel(
      const SemNominalType &resource, TypeId value_type,
      LLVMForeignValueState &state);
  [[nodiscard]] static llvm::Value *valid(LowForeignResourceValid inst,
                                          llvm::IRBuilder<> &builder,
                                          LLVMForeignValueState &state);
  [[nodiscard]] static llvm::Value *wrap(LowForeignResourceWrap inst,
                                         llvm::IRBuilder<> &builder,
                                         llvm::Function &function,
                                         LLVMForeignValueState &state);
  [[nodiscard]] static llvm::Value *unwrap(LowForeignResourceUnwrap inst,
                                           llvm::IRBuilder<> &builder,
                                           LLVMForeignValueState &state);
  [[nodiscard]] static llvm::Value *finish(TypeId source_type,
                                           llvm::Value *handle,
                                           llvm::IRBuilder<> &builder,
                                           LLVMForeignValueState &state);
};

struct LLVMCallbackState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::FunctionType *(const ForeignAbiFunctionLayout &)>
      foreign_function_type;
  std::function<void(llvm::CallBase &, const ForeignAbiFunctionLayout &)>
      apply_foreign_attributes;
  std::function<llvm::Function *(ForeignAbiThunkPlanId)> callback_thunk;
  std::function<llvm::Value *(
      const ForeignAbiFunctionLayout &, const ForeignAbiCallLayout &,
      llvm::Value *, std::span<llvm::Value *const>, llvm::IRBuilder<> &,
      llvm::Function &)>
      emit_foreign_call_values;
};

// Callback thunk construction is a module-level interop operation, but it
// must share the owning lowering context's maps and ABI/type callbacks. Keep
// that state explicit so the independently compiled service cannot create a
// second LLVM module or cache.
struct LLVMCallbackThunkState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint64_t, llvm::Function *> &callback_thunks;
  std::function<llvm::FunctionType *(const ForeignAbiFunctionLayout &)>
      foreign_function_type;
  std::function<std::vector<llvm::Type *>(const ForeignAbiValueLayout &)>
      foreign_physical_types;
  std::function<void(llvm::Function &, const ForeignAbiFunctionLayout &)>
      apply_foreign_attributes;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                   std::string_view)>
      entry_alloca;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)>
      copy_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, llvm::IRBuilder<> &)>
      load_value_from_object;
};

struct LLVMCallbackThunkService {
  [[nodiscard]] static llvm::Function *lower(ForeignAbiThunkPlanId plan_id,
                                              LLVMCallbackThunkState &state);
};

struct LLVMCallbackRegistrationService {
  [[nodiscard]] static llvm::Value *callbackAdapter(
      LowCallbackAdapter inst, LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *foreignFunctionRef(
      LowForeignFunctionRef inst, LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *indirectCall(
      LowIndirectForeignCall inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *adapterCall(
      LowCallbackAdapterCall inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *makeAdapter(
      LowMakeCallbackAdapter inst, llvm::IRBuilder<> &builder,
      LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *makeRegistration(
      LowMakeCallbackRegistration inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *registrationActive(
      LowCallbackRegistrationActive inst, llvm::IRBuilder<> &builder,
      LLVMCallbackState &state);
  static void releaseAdapter(llvm::Value *adapter, TypeId adapter_type,
                             llvm::IRBuilder<> &builder,
                             LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *finishRegistration(
      CallbackRegistrationPlanId plan_id, LowValueBlockId operands,
      bool cancel, llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *cancelAsync(
      LowCallbackRegistrationCancelAsync inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackState &state);
};

struct LLVMCallbackCompletionService {
  [[nodiscard]] static llvm::Value *pending(
      LowCallbackCompletionPending inst, llvm::IRBuilder<> &builder,
      LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *poll(
      LowCallbackCompletionPoll inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *finishValue(
      CallbackCompletionPlanId plan_id, llvm::Value *completion,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCallbackState &state);
  [[nodiscard]] static llvm::Value *finish(
      CallbackCompletionPlanId plan_id, LowValueBlockId operands,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCallbackState &state);
};

struct LLVMCallbackWakeState {
  LLVMCallbackState &callback;
  LLVMCoroutineState &coroutine;
};

struct LLVMCallbackWakeService {
  static void releaseAdapter(llvm::Value *adapter, const CallbackWakePlan &plan,
                             llvm::IRBuilder<> &builder,
                             LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *makeWake(
      LowMakeCallbackWake inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *projectCompletion(
      LowForeignOperationProjectCompletion inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *projectWake(
      LowForeignOperationProjectWake inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *projectPort(
      LowForeignOperationPortProject inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *ready(
      LowCallbackWakeReady inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *finish(
      CallbackWakePlanId plan_id, LowValueBlockId operands,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      LLVMCallbackWakeState &state);
  [[nodiscard]] static llvm::Value *detach(
      LowCallbackDetach inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallbackWakeState &state);
};

struct LLVMCoroutineLoweringService {
