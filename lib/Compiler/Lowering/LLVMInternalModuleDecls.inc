// Included by LLVMInternal.h; this fragment keeps the non-owning LLVM
// lowering state/service declarations in the shared private namespace.
struct LLVMModule::Impl {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  bool position_independent = false;
  LLVMOptimizationLevel optimization = LLVMOptimizationLevel::O0;
  bool optimization_applied = false;
};

namespace internal {

void initializeLLVMTargets();

[[nodiscard]] std::unique_ptr<llvm::TargetMachine>
createLLVMTargetMachine(std::string_view triple, std::string &error,
                        bool position_independent = false,
                        LLVMOptimizationLevel optimization =
                            LLVMOptimizationLevel::O0);

[[nodiscard]] bool lowerLLVMModule(
    const LowIR &low_ir, std::string_view package_name,
    ModuleEmissionRole emission_role, llvm::Module &module,
    std::span<const std::uint32_t> native_definition_exports,
    std::span<const std::pair<std::string, std::string>> runtime_symbol_mappings,
    DebugInfoMode debug_info,
    std::span<const ComponentExportLoweringPlan> component_exports,
    std::string &error);

} // namespace internal
