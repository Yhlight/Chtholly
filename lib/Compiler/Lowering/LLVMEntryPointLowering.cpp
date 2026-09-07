#include "LLVMInternal.h"

#include "chtholly/Compiler/ProgramModel.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"

namespace chtholly::compiler {

void LLVMEntryPointLoweringService::emit(
    llvm::Function &source, std::string_view hosted_symbol, bool receives_args,
    bool utf16, LLVMEntryPointState &state) {
  auto &context = state.context;
  auto &module = state.module;
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *pointer = llvm::PointerType::getUnqual(context);
  llvm::SmallVector<llvm::Type *, 2> parameters;
  if (receives_args)
    parameters = {i32, pointer};
  auto *entry = llvm::Function::Create(
      llvm::FunctionType::get(i32, parameters, false),
      llvm::Function::ExternalLinkage, hosted_symbol, module);
  entry->addFnAttr(llvm::Attribute::NoUnwind);
  auto *begin = llvm::BasicBlock::Create(context, "entry", entry);
  auto *initialized =
      llvm::BasicBlock::Create(context, "runtime.initialized", entry);
  auto *failed =
      llvm::BasicBlock::Create(context, "runtime.init.failed", entry);
  llvm::IRBuilder<> builder(begin);
  auto *void_function = llvm::FunctionType::get(builder.getVoidTy(), false);
  builder.CreateCall(module.getOrInsertFunction(
      "chtholly_next_runtime_v1_init", void_function));
  llvm::Value *argc = builder.getInt32(0);
  llvm::Value *argv = llvm::ConstantPointerNull::get(pointer);
  if (receives_args) {
    auto argument = entry->arg_begin();
    argc = &*argument++;
    argv = &*argument;
  }
  const auto setter_name =
      utf16 ? "chtholly_next_runtime_v1_set_process_args_utf16"
            : "chtholly_next_runtime_v1_set_process_args_utf8";
  auto setter = module.getOrInsertFunction(
      setter_name, llvm::FunctionType::get(i32, {i32, pointer}, false));
  auto *status = builder.CreateCall(setter, {argc, argv});
  builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                       initialized, failed);

  builder.SetInsertPoint(failed);
  builder.CreateCall(module.getOrInsertFunction(
      "chtholly_next_runtime_v1_shutdown", void_function));
  builder.CreateRet(builder.getInt32(1));

  builder.SetInsertPoint(initialized);
  auto *result = builder.CreateCall(&source);
  builder.CreateCall(module.getOrInsertFunction(
      "chtholly_next_runtime_v1_drain_thread_static_drops", void_function));
  builder.CreateCall(module.getOrInsertFunction(
      "chtholly_next_runtime_v1_drain_program_static_drops", void_function));
  builder.CreateCall(module.getOrInsertFunction(
      "chtholly_next_runtime_v1_shutdown", void_function));
  builder.CreateRet(result);
}

void LLVMEntryPointLoweringService::emitAll(llvm::Function &source,
                                            LLVMEntryPointState &state) {
  const auto windows =
      llvm::Triple(state.module.getTargetTriple()).isOSWindows();
  emit(source, v1HostedEntrySymbol(windows), true, windows, state);
  emit(source, V1EmbeddedEntrySymbol, false, false, state);
}

} // namespace chtholly::compiler
