// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
struct LLVMCoroutineState {
  llvm::Value *task = nullptr;
  llvm::BasicBlock *cancel = nullptr;
  llvm::BasicBlock *cancelled = nullptr;
  const CoroutineFramePlan *plan = nullptr;
  llvm::StructType *frame_type = nullptr;
  llvm::Value *frame = nullptr;
  LowInstId low_instruction;
  llvm::Value *bitmap = nullptr;
  llvm::Value *terminal = nullptr;
  std::unordered_map<std::uint32_t, std::uint32_t> place_bits;
  llvm::Function *wake_entry = nullptr;
  llvm::Function *wake_release = nullptr;
};

// Module-level orchestration view for LLVM lowering. The service coordinates
// registration, globals, coroutine scaffolds, ordinary functions, entry-point
// policy, exports, and debug finalization without owning any LLVM/LowIR state.
struct LLVMModuleOrchestrationState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  ModuleEmissionRole emission_role;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  const std::unordered_map<std::uint32_t, FunctionRefId>
      &local_function_refs;
  llvm::Function *&source_entry;
  std::uint32_t &entry_candidate_count;
  std::function<std::uint32_t(FunctionId)> function_flags;
  std::function<bool(std::string &)> register_functions;
  std::function<bool(std::string &)> emit_static_globals;
  std::function<bool(std::string &)> emit_coroutine_scaffolds;
  std::function<bool(LowFunctionId, std::string &)> lower_function;
  std::function<llvm::Function *(llvm::Function &)> emit_task_driver_host;
  std::function<void(llvm::Function &)> emit_entry_points;
  std::function<bool(std::string &)> emit_component_exports;
  std::function<void()> finalize_debug;
};

struct LLVMModuleOrchestrationService {
  [[nodiscard]] static bool run(LLVMModuleOrchestrationState &state,
                                std::string &error);
};

// Non-owning lowering-domain views. Each view aliases the single
// ModuleLoweringContext stores; extracted services must receive these views
// rather than constructing a second map or LLVM context.
struct LLVMTypeState {
  std::unordered_map<std::uint32_t, llvm::DIType *> &debug_types;
  std::unordered_map<std::uint32_t, llvm::Type *> &types;
};

struct LLVMValueState {
  std::unordered_map<std::uint32_t, llvm::Value *> &values;
  std::unordered_map<std::uint32_t, llvm::Value *> &slots;
  std::unordered_map<std::uint32_t, llvm::Value *> &place_flags;
  std::unordered_set<std::uint32_t> &initialized_slots;
};

struct LLVMValueLoweringService {
  [[nodiscard]] static llvm::Value *integer(
      LowIntegerConstant inst, const SemIR &sem_ir,
      const std::function<llvm::Type *(TypeId)> &lower_object_type);
  [[nodiscard]] static llvm::Value *floating(LowFloatConstant inst,
                                              const SemIR &sem_ir,
                                              llvm::LLVMContext &context);
  [[nodiscard]] static llvm::Value *boolean(LowBoolConstant inst,
                                             const SemIR &sem_ir,
                                             llvm::LLVMContext &context);
  [[nodiscard]] static llvm::Value *nullPointer(
      LowNullPointer inst, const std::function<llvm::Type *(TypeId)> &lower_value_type);
  [[nodiscard]] static llvm::Type *valueType(
      const LowIR &low_ir, TypeId id, llvm::LLVMContext &context,
      const std::function<llvm::Type *(TypeId)> &lower_object_type,
      const std::function<llvm::Type *(TypeId)> &lower_value_type);
  [[nodiscard]] static llvm::Value *stringConstant(
      StringLiteralId id, const SemIR &sem_ir, llvm::Module &module,
      llvm::LLVMContext &context, llvm::IRBuilder<> &builder,
      const std::function<llvm::Type *(TypeId)> &lower_object_type);
  [[nodiscard]] static llvm::Value *cstringConstant(
      StringLiteralId id, const SemIR &sem_ir, llvm::Module &module,
      llvm::LLVMContext &context);
};

struct LLVMObjectState {
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint32_t, llvm::GlobalVariable *> &static_globals;
  std::unordered_map<std::uint32_t, FunctionRefId> &local_function_refs;
  std::unordered_map<std::uint32_t, llvm::Function *> &coroutine_constructors;
};

// Stable object/linkage identity helpers are kept separate from module
// emission. They are pure functions over canonical ABI inputs and never own
// LLVM or lowering state.
struct LLVMObjectIdentityService {
  [[nodiscard]] static std::string mangleFunction(
      std::string_view package, std::string_view module,
      std::string_view function, std::string_view abi_fingerprint);
  [[nodiscard]] static std::string mangleStatic(
      std::string_view package, std::string_view module,
      std::string_view name);
  [[nodiscard]] static std::string coroutineConstructorSymbol(
      const PublicEntity &entity);
};

// Canonical ABI suffix construction is kept independent from LLVM module
// emission. The service reads SemIR's canonical generic-value store and emits
// the established source/concrete fingerprints without owning compiler state.
struct LLVMAbiIdentityState {
  const SemIR &sem_ir;
  std::string_view package_name;
};

struct LLVMAbiIdentityService {
  [[nodiscard]] static std::string callableAbiSuffix(
      const SemFunctionRef &reference, std::string_view package,
      IdentifierId module, std::string_view name,
      bool canonical_semantic_target, LLVMAbiIdentityState &state);
  [[nodiscard]] static std::string concreteTypeSuffix(
      TypeId type, LLVMAbiIdentityState &state);
};

struct LLVMFunctionRegistrationState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::string_view package_name;
  ModuleEmissionRole emission_role;
  std::span<const std::pair<std::string, std::string>> runtime_symbol_mappings;
  const std::unordered_set<std::uint32_t> &native_definition_exports;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint32_t, FunctionRefId> &local_function_refs;
  llvm::Function *&source_entry;
  std::uint32_t &entry_candidate_count;
  std::function<std::vector<llvm::Type *>(const ForeignAbiValueLayout &)>
      foreign_physical_types;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<void(llvm::Function &, const ForeignAbiFunctionLayout &)>
      apply_foreign_attributes;
  std::function<std::string(const SemFunctionRef &, std::string_view,
                            IdentifierId, std::string_view, bool)>
      callable_abi_suffix;
  std::function<std::string(TypeId)> concrete_type_suffix;
};

struct LLVMFunctionRegistrationService {
  [[nodiscard]] static bool registerAll(LLVMFunctionRegistrationState &state,
                                        std::string &error);
};

