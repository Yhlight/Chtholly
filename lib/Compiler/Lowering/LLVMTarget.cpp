#include "LLVMInternal.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

namespace chtholly::compiler::internal {

void initializeLLVMTargets() {
  static const bool initialized = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
#ifdef CHTHOLLY_COMPILER_LLVM_HAS_AARCH64
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
#endif
    return true;
  }();
  (void)initialized;
}

std::unique_ptr<llvm::TargetMachine>
createLLVMTargetMachine(std::string_view triple, std::string &error,
                        bool position_independent,
                        LLVMOptimizationLevel optimization) {
  initializeLLVMTargets();
  std::string target_error;
  const auto triple_text = std::string(triple);
  const auto *target =
      llvm::TargetRegistry::lookupTarget(triple_text, target_error);
  if (!target) {
    error = "unable to select LLVM target '" + triple_text + "': " +
            target_error;
    return nullptr;
  }
  llvm::TargetOptions options;
  llvm::CodeGenOptLevel codegen_level = llvm::CodeGenOptLevel::None;
  switch (optimization) {
  case LLVMOptimizationLevel::O0:
    codegen_level = llvm::CodeGenOptLevel::None;
    break;
  case LLVMOptimizationLevel::O1:
    codegen_level = llvm::CodeGenOptLevel::Less;
    break;
  case LLVMOptimizationLevel::O2:
  case LLVMOptimizationLevel::Os:
  case LLVMOptimizationLevel::Oz:
    codegen_level = llvm::CodeGenOptLevel::Default;
    break;
  case LLVMOptimizationLevel::O3:
    codegen_level = llvm::CodeGenOptLevel::Aggressive;
    break;
  }
  auto machine = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple_text, "generic", "", options,
      position_independent ? std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_)
                           : std::nullopt,
      std::nullopt, codegen_level));
  if (!machine)
    error = "unable to create LLVM target machine for '" + triple_text + "'";
  return machine;
}

} // namespace chtholly::compiler::internal
