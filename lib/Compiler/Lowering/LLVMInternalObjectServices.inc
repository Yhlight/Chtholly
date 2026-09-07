// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
struct LLVMObjectLoweringService {
  [[nodiscard]] static llvm::Type *lowerObjectType(
      TypeId id, const LowIR &low_ir, const SemIR &sem_ir,
      llvm::Module &module, llvm::LLVMContext &context,
      std::unordered_map<std::uint32_t, llvm::Type *> &types,
      std::string &lowering_error,
      const std::function<llvm::Type *(TypeId)> &recursive);
  using ObjectTypeFn = std::function<llvm::Type *(TypeId)>;
  using CopyObjectFn = std::function<void(llvm::Value *, llvm::Value *, TypeId,
                                         llvm::IRBuilder<> &)>;
  using PayloadAddressFn = std::function<llvm::Value *(
      llvm::Value *, TypeId, std::uint32_t, std::uint32_t,
      llvm::IRBuilder<> &)>;
  using RecursiveCopyFn = std::function<void(
      llvm::Value *, llvm::Value *, TypeId, llvm::IRBuilder<> &,
      llvm::Function &)>;

  static void copySemanticObject(
      llvm::Value *destination, llvm::Value *source, TypeId type,
      llvm::IRBuilder<> &builder, llvm::Function &function,
      const SemIR &sem_ir, const LowIR &low_ir, llvm::LLVMContext &context,
      const std::unordered_map<std::uint32_t, llvm::Function *> &functions,
      const ObjectTypeFn &lower_object_type, const CopyObjectFn &copy_object,
      const PayloadAddressFn &payload_address,
      const RecursiveCopyFn &recursive_copy);
  [[nodiscard]] static llvm::Constant *constantObject(
      ConstantId id, std::string &error, const SemIR &sem_ir,
      llvm::Module &module, llvm::LLVMContext &context,
      const ObjectTypeFn &lower_object_type,
      const std::function<llvm::Constant *(ConstantId, std::string &)> &recursive);
  [[nodiscard]] static llvm::Value *enumPayloadAddress(
      llvm::Value *owner_address, TypeId owner, std::uint32_t variant,
      std::uint32_t field, llvm::IRBuilder<> &builder, const LowIR &low_ir,
      llvm::LLVMContext &context,
      const ObjectTypeFn &lower_object_type);
};

struct LLVMStaticGlobalState {
  const SemIR &sem_ir;
  llvm::Module &module;
  std::string_view package_name;
  std::unordered_map<std::uint32_t, llvm::GlobalVariable *> &static_globals;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Constant *(ConstantId, std::string &)> constant_object;
  std::function<std::string(std::string_view, std::string_view,
                            std::string_view)> mangle_static;
};

struct LLVMStaticGlobalLoweringService {
  [[nodiscard]] static bool emit(LLVMStaticGlobalState &state,
                                 std::string &error);
};

struct LLVMEntryPointState {
  llvm::Module &module;
  llvm::LLVMContext &context;
};

struct LLVMEntryPointLoweringService {
  static void emit(llvm::Function &source, std::string_view hosted_symbol,
                   bool receives_args, bool utf16,
                   LLVMEntryPointState &state);
  static void emitAll(llvm::Function &source, LLVMEntryPointState &state);
};

struct LLVMCallLoweringState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::LLVMContext &context;
  const std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::unordered_map<std::uint32_t, llvm::Value *> &slots;
  std::unordered_set<std::uint32_t> &initialized_slots;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<bool(LowInstId)> has_value;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                   std::string_view)> entry_alloca;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)> copy_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
  std::function<llvm::Value *(LowPlaceId, llvm::IRBuilder<> &)> place_address;
  std::function<void(LowPlaceId, llvm::IRBuilder<> &)> mark_initialized;
  std::function<std::optional<CanonicalResultShape>(FunctionRefId)>
      fallible_constructor_shape;
  std::string &instruction_error;
};

struct LLVMCallLoweringService {
  [[nodiscard]] static llvm::Value *lower(LowCall inst,
                                           llvm::IRBuilder<> &builder,
                                           llvm::Function &function,
                                           LLVMCallLoweringState &state);
  [[nodiscard]] static llvm::Value *indirect(
      LowIndirectCall inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallLoweringState &state);
  [[nodiscard]] static llvm::Value *construct(
      LowConstruct inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMCallLoweringState &state);
};

struct LLVMDebugTypeState {
  const SemIR &sem_ir;
  const LowIR &low_ir;
  llvm::Module &module;
  llvm::DIBuilder &builder;
  llvm::DIFile *file = nullptr;
  std::unordered_map<std::uint32_t, llvm::DIType *> &types;
  std::function<llvm::DIType *(TypeId)> recursive;
};

struct LLVMDebugFieldInfo {
  bool bit_field = false;
  std::uint64_t size_bits = 0;
  std::uint64_t offset_bits = 0;
  std::uint64_t storage_offset_bits = 0;
};

[[nodiscard]] std::optional<LLVMDebugFieldInfo>
debugFieldInfo(const SemNominalField &field,
               const LowNominalFieldLayout &layout);

struct LLVMDebugTypeLoweringService {
  [[nodiscard]] static llvm::DIType *lower(TypeId id,
                                           LLVMDebugTypeState &state);
};

struct LLVMFunctionSetupState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::LLVMContext &context;
  llvm::DIBuilder *debug_builder;
  llvm::DIFile *debug_file;
  DebugInfoMode debug_info;
  std::unordered_map<std::uint32_t, llvm::BasicBlock *> &blocks;
  std::unordered_map<std::uint32_t, llvm::Value *> &slots;
  std::unordered_map<std::uint32_t, llvm::Value *> &place_flags;
  std::unordered_set<std::uint32_t> &initialized_slots;
  llvm::Value *&current_result_slot;
  llvm::Value *&current_outcome_slot;
  std::function<llvm::AllocaInst *(llvm::Function &, llvm::Type *,
                                   std::string_view)> entry_alloca;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::DIType *(TypeId)> debug_type;
};

struct LLVMFunctionSetupResult {
  llvm::DISubprogram *debug_subprogram = nullptr;
  LineColumn debug_location;
};

struct LLVMFunctionSetupService {
  [[nodiscard]] static std::optional<LLVMFunctionSetupResult> prepare(
      LowFunctionId id, llvm::Function &llvm_function,
      LLVMFunctionSetupState &state, std::string &error);
};

struct LLVMFunctionBodyState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::LLVMContext &context;
  const std::unordered_map<std::uint32_t, llvm::BasicBlock *> &blocks;
  std::string &instruction_error;
  std::function<void(LowInstId, llvm::IRBuilder<> &, llvm::Function &)>
      lower_inst;
};

struct LLVMFunctionBodyService {
  [[nodiscard]] static bool lower(
      LowFunctionId id, llvm::Function &function,
      llvm::DISubprogram *debug_subprogram, LineColumn debug_location,
      LLVMFunctionBodyState &state, std::string &error);
};

struct LLVMVectorDropState {
  const SemIR &sem_ir;
  llvm::Module &module;
  llvm::LLVMContext &context;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<void(TypeId, llvm::Value *, llvm::IRBuilder<> &,
                     llvm::Function &)> destroy_address;
  std::string &instruction_error;
};

struct LLVMVectorDropLoweringService {
  [[nodiscard]] static llvm::Value *lower(TypeId vec_type, llvm::Value *vec,
                                           llvm::IRBuilder<> &builder,
                                           llvm::Function &function,
                                           LLVMVectorDropState &state);
};

struct LLVMTransferReturnState {
  const LowIR &low_ir;
  const SemIR &sem_ir;
  llvm::LLVMContext &context;
  llvm::Value *current_result_slot;
  llvm::Value *current_outcome_slot;
  std::function<llvm::Value *(LowInstId)> value;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<void(llvm::Value *, llvm::Value *, TypeId,
                     llvm::IRBuilder<> &)> copy_object;
  std::function<llvm::Value *(llvm::Value *, TypeId, std::uint32_t,
                              std::uint32_t, llvm::IRBuilder<> &)>
      enum_payload_address;
  std::string &instruction_error;
};

struct LLVMTransferReturnLoweringService {
  [[nodiscard]] static llvm::Value *lower(
      LowTransferReturn inst, llvm::IRBuilder<> &builder,
      llvm::Function &function, LLVMTransferReturnState &state);
};

struct LLVMObjectValueState {
  const LowIR &low_ir;
  llvm::Module &module;
  std::unordered_map<std::uint32_t, llvm::Function *> &functions;
  std::string &instruction_error;
  std::function<llvm::Type *(TypeId)> lower_object_type;
  std::function<llvm::Type *(TypeId)> lower_value_type;
  std::function<bool(TypeId)> uses_pointer_value_representation;
};

struct LLVMObjectValueLoweringService {
  static void copy(llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder, LLVMObjectValueState &state);
  static void moveObject(llvm::Value *destination,
                         llvm::Value *source_address, TypeId type,
                         llvm::IRBuilder<> &builder,
                         LLVMObjectValueState &state);
  static void moveValue(llvm::Value *destination, llvm::Value *source,
                        TypeId type, llvm::IRBuilder<> &builder,
                        LLVMObjectValueState &state);
  [[nodiscard]] static llvm::Value *load(llvm::Value *address, TypeId type,
                                         llvm::IRBuilder<> &builder,
                                         LLVMObjectValueState &state);
  static void store(llvm::Value *address, llvm::Value *source, TypeId type,
                    llvm::IRBuilder<> &builder, LLVMObjectValueState &state);
};
