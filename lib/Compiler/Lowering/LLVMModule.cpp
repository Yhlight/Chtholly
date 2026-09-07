#include "LLVMInternal.h"

#include "chtholly/Compiler/ComponentABI.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace chtholly::compiler {
LLVMModule::LLVMModule(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LLVMModule::LLVMModule(LLVMModule &&) noexcept = default;
LLVMModule &LLVMModule::operator=(LLVMModule &&) noexcept = default;
LLVMModule::~LLVMModule() = default;

std::string LLVMModule::print() const {
  std::string output;
  llvm::raw_string_ostream stream(output);
  impl_->module->print(stream, nullptr);
  return output;
}
bool LLVMModule::verify(std::string &error) const {
  error.clear();
  llvm::raw_string_ostream stream(error);
  const auto failed = llvm::verifyModule(*impl_->module, &stream);
  stream.flush();
  return !failed;
}
std::string LLVMModule::emitObject(std::string &error) const {
  error.clear();
  auto machine = internal::createLLVMTargetMachine(
      impl_->module->getTargetTriple(), error, impl_->position_independent,
      impl_->optimization);
  if (!machine)
    return {};
  if (!impl_->optimization_applied) {
    if (impl_->optimization != LLVMOptimizationLevel::O0) {
      llvm::PassBuilder pass_builder(machine.get());
      llvm::LoopAnalysisManager loop_analysis;
      llvm::FunctionAnalysisManager function_analysis;
      llvm::CGSCCAnalysisManager cgscc_analysis;
      llvm::ModuleAnalysisManager module_analysis;
      pass_builder.registerModuleAnalyses(module_analysis);
      pass_builder.registerCGSCCAnalyses(cgscc_analysis);
      pass_builder.registerFunctionAnalyses(function_analysis);
      pass_builder.registerLoopAnalyses(loop_analysis);
      pass_builder.crossRegisterProxies(loop_analysis, function_analysis,
                                        cgscc_analysis, module_analysis);
      llvm::OptimizationLevel level = llvm::OptimizationLevel::O1;
      switch (impl_->optimization) {
      case LLVMOptimizationLevel::O1:
        level = llvm::OptimizationLevel::O1;
        break;
      case LLVMOptimizationLevel::O2:
        level = llvm::OptimizationLevel::O2;
        break;
      case LLVMOptimizationLevel::O3:
        level = llvm::OptimizationLevel::O3;
        break;
      case LLVMOptimizationLevel::Os:
        level = llvm::OptimizationLevel::Os;
        break;
      case LLVMOptimizationLevel::Oz:
        level = llvm::OptimizationLevel::Oz;
        break;
      case LLVMOptimizationLevel::O0:
        break;
      }
      auto module_passes = pass_builder.buildPerModuleDefaultPipeline(level);
      module_passes.run(*impl_->module, module_analysis);
      if (!verify(error))
        return {};
    }
    impl_->optimization_applied = true;
  }
  llvm::SmallVector<char, 0> buffer;
  llvm::raw_svector_ostream stream(buffer);
  llvm::legacy::PassManager passes;
  if (machine->addPassesToEmitFile(passes, stream, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
    error = "LLVM target cannot emit object files";
    return {};
  }
  passes.run(*impl_->module);
  return std::string(buffer.begin(), buffer.end());
}
std::string_view LLVMModule::targetTriple() const {
  return impl_->module->getTargetTriple();
}

std::string defaultLLVMTriple() {
  return llvm::sys::getDefaultTargetTriple();
}

std::string emitComponentDescriptorObject(
    const ComponentContractArtifact &contract, std::string_view target_triple,
    std::string_view runtime_abi, std::string &error) {
  error.clear();
  if (!contract.verify(error))
    return {};
  const auto triple = llvm::Triple::normalize(target_triple);
  LLVMModule result(std::make_unique<LLVMModule::Impl>());
  result.impl_->context = std::make_unique<llvm::LLVMContext>();
  auto &context = *result.impl_->context;
  result.impl_->module =
      std::make_unique<llvm::Module>("chtholly.component.v1", context);
  result.impl_->module->setTargetTriple(triple);
  result.impl_->position_independent = true;
  auto machine = internal::createLLVMTargetMachine(triple, error, true);
  if (!machine)
    return {};
  result.impl_->module->setDataLayout(machine->createDataLayout());
  auto &module = *result.impl_->module;
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *digest_type = llvm::ArrayType::get(i8, StableFingerprint::ByteCount);
  auto digest = [&](const StableFingerprint &fingerprint) -> llvm::Constant * {
    const auto bytes = fingerprint.bytes();
    return llvm::ConstantDataArray::get(
        context, llvm::ArrayRef<std::uint8_t>(bytes.data(), bytes.size()));
  };
  auto string_global = [&](std::string_view name, std::string_view value) {
    auto *constant = llvm::ConstantDataArray::getString(context, value, false);
    return new llvm::GlobalVariable(module, constant->getType(), true,
                                    llvm::GlobalValue::PrivateLinkage, constant,
                                    name);
  };
  auto *invoke_type =
      llvm::FunctionType::get(i32, {pointer, i32, pointer}, false);
  auto *export_type = llvm::StructType::get(
      context, {i32, i32, digest_type, digest_type, pointer, i64, pointer,
                llvm::ArrayType::get(i64, 2)});
  std::vector<llvm::Constant *> export_values;
  export_values.reserve(contract.exports.size());
  for (std::size_t index = 0; index < contract.exports.size(); ++index) {
    const auto &entry = contract.exports[index];
    auto *name = string_global("component.export.name." + std::to_string(index),
                               entry.canonical_name);
    auto *wrapper =
        llvm::Function::Create(invoke_type, llvm::Function::ExternalLinkage,
                               componentWrapperSymbol(entry.export_id), module);
    wrapper->setVisibility(llvm::GlobalValue::HiddenVisibility);
    export_values.push_back(llvm::ConstantStruct::get(
        export_type,
        {llvm::ConstantInt::get(i32, 112), llvm::ConstantInt::get(i32, 0),
         digest(entry.export_id), digest(entry.signature_digest), name,
         llvm::ConstantInt::get(i64, entry.canonical_name.size()), wrapper,
         llvm::ConstantAggregateZero::get(llvm::ArrayType::get(i64, 2))}));
  }
  auto *exports_array_type =
      llvm::ArrayType::get(export_type, export_values.size());
  auto *exports = new llvm::GlobalVariable(
      module, exports_array_type, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(exports_array_type, export_values),
      "component.exports");
  auto *identity = string_global("component.identity", contract.identity);
  const auto target_digest = StableFingerprint::fromCanonicalBytes(
      "chtholly.component.target.v1\n" + triple);
  const auto runtime_digest = StableFingerprint::fromCanonicalBytes(
      "chtholly.component.runtime.v1\n" + std::string(runtime_abi));
  auto *descriptor_type = llvm::StructType::get(
      context,
      {i32, i32, i32, i32, digest_type, digest_type, digest_type, digest_type,
       pointer, i64, pointer, i64, llvm::ArrayType::get(i64, 4)});
  auto *descriptor = new llvm::GlobalVariable(
      module, descriptor_type, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          descriptor_type,
          {llvm::ConstantInt::get(i32, 208),
           llvm::ConstantInt::get(i32, ComponentAbiEpoch),
           llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, 0),
           digest(contract.identity_digest), digest(contract.contract_digest),
           digest(target_digest), digest(runtime_digest), identity,
           llvm::ConstantInt::get(i64, contract.identity.size()), exports,
           llvm::ConstantInt::get(i64, contract.exports.size()),
           llvm::ConstantAggregateZero::get(llvm::ArrayType::get(i64, 4))}),
      "component.descriptor");
  auto *query = llvm::Function::Create(llvm::FunctionType::get(pointer, false),
                                       llvm::Function::ExternalLinkage,
                                       "chtholly_component_query_v1", module);
  query->setVisibility(llvm::GlobalValue::DefaultVisibility);
  if (llvm::Triple(triple).isOSBinFormatCOFF())
    query->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
  auto *entry = llvm::BasicBlock::Create(context, "entry", query);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRet(descriptor);
  if (!result.verify(error))
    return {};
  return result.emitObject(error);
}


} // namespace chtholly::compiler
