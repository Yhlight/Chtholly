#include "chtholly/Compiler/LLVM.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/ProgramModel.h"

#include "LLVMInternal.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler {

} // namespace chtholly::compiler

namespace chtholly::compiler {

LLVMModule
lowerToLLVM(const LowIR &low_ir, std::string_view package_name,
            std::string_view module_name, std::string_view target_triple,
            std::string &error, ModuleEmissionRole emission_role,
            std::span<const std::uint32_t> native_definition_exports,
            std::span<const std::pair<std::string, std::string>>
                runtime_symbol_mappings,
            DebugInfoMode debug_info,
            std::span<const ComponentExportLoweringPlan> component_exports,
            LLVMOptimizationLevel optimization) {
  error.clear();
  auto impl = std::make_unique<LLVMModule::Impl>();
  impl->context = std::make_unique<llvm::LLVMContext>();
  impl->module = std::make_unique<llvm::Module>(llvm::StringRef(module_name),
                                                *impl->context);
  impl->position_independent =
      emission_role == ModuleEmissionRole::ComponentLibrary;
  impl->optimization = optimization;
  if (!low_ir.verify(error))
    return LLVMModule(std::move(impl));
  const auto triple = llvm::Triple::normalize(
      target_triple.empty() ? defaultLLVMTriple() : target_triple);
  if (!low_ir.targetTriple().empty() && low_ir.targetTriple() != triple) {
    error = "LLVM target does not match the verified LowIR ABI target";
    return LLVMModule(std::move(impl));
  }
  impl->module->setTargetTriple(triple);
  if (auto machine = internal::createLLVMTargetMachine(
          triple, error, impl->position_independent))
    impl->module->setDataLayout(machine->createDataLayout());
  else
    return LLVMModule(std::move(impl));
  if (emission_role >= ModuleEmissionRole::Count) {
    error = "LLVM lowering received an invalid module emission role";
    return LLVMModule(std::move(impl));
  }
  if (!internal::lowerLLVMModule(
          low_ir, package_name, emission_role, *impl->module,
          native_definition_exports, runtime_symbol_mappings, debug_info,
          component_exports, error))
    return LLVMModule(std::move(impl));
  LLVMModule result(std::move(impl));
  if (!result.verify(error))
    return result;
  return result;
}

} // namespace chtholly::compiler
