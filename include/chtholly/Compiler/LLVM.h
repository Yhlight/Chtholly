#pragma once

#include "chtholly/Compiler/ComponentABI.h"
#include "chtholly/Compiler/LowIR.h"
#include "chtholly/Compiler/ModuleEmission.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace chtholly::compiler {

// Kept in the compiler layer so LLVM users do not depend on Driver options.
enum class LLVMOptimizationLevel : std::uint8_t {
  O0,
  O1,
  O2,
  O3,
  Os,
  Oz,
};

class LLVMModule {
public:
  LLVMModule(LLVMModule &&) noexcept;
  LLVMModule &operator=(LLVMModule &&) noexcept;
  ~LLVMModule();

  [[nodiscard]] std::string print() const;
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string emitObject(std::string &error) const;
  [[nodiscard]] std::string_view targetTriple() const;

private:
  struct Impl;
  explicit LLVMModule(std::unique_ptr<Impl> impl);
  friend LLVMModule
  lowerToLLVM(const LowIR &, std::string_view, std::string_view,
              std::string_view, std::string &, ModuleEmissionRole,
              std::span<const std::uint32_t>,
              std::span<const std::pair<std::string, std::string>>,
              DebugInfoMode, std::span<const ComponentExportLoweringPlan>,
              LLVMOptimizationLevel);
  friend std::string
  emitComponentDescriptorObject(const ComponentContractArtifact &,
                                std::string_view, std::string_view,
                                std::string &);
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] LLVMModule lowerToLLVM(
    const LowIR &low_ir, std::string_view package_name,
    std::string_view module_name, std::string_view target_triple,
    std::string &error,
    ModuleEmissionRole emission_role = ModuleEmissionRole::Library,
    std::span<const std::uint32_t> native_definition_exports = {},
    std::span<const std::pair<std::string, std::string>>
        runtime_symbol_mappings = {},
    DebugInfoMode debug_info = DebugInfoMode::None,
    std::span<const ComponentExportLoweringPlan> component_exports = {},
    LLVMOptimizationLevel optimization = LLVMOptimizationLevel::O0);
[[nodiscard]] std::string defaultLLVMTriple();
[[nodiscard]] std::string
emitComponentDescriptorObject(const ComponentContractArtifact &contract,
                              std::string_view target_triple,
                              std::string_view runtime_abi, std::string &error);

} // namespace chtholly::compiler
