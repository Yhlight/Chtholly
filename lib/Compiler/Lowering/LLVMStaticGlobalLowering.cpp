#include "LLVMInternal.h"

namespace chtholly::compiler {

bool LLVMStaticGlobalLoweringService::emit(LLVMStaticGlobalState &state,
                                           std::string &error) {
  for (std::uint32_t index = 0; index < state.sem_ir.constantEntityCount();
       ++index) {
    const auto id = ConstantEntityId(index);
    const auto &entity = state.sem_ir.constantEntity(id);
    if ((entity.flags & SemConstantStatic) == 0)
      continue;
    const auto imported = (entity.flags & SemConstantImported) != 0;
    const auto package = imported
                             ? state.sem_ir.identifier(entity.canonical_package)
                             : state.package_name;
    const auto module =
        imported ? state.sem_ir.identifier(entity.canonical_module)
                 : state.sem_ir.identifier(state.sem_ir.moduleName());
    const auto name =
        imported ? state.sem_ir.identifier(entity.canonical_name)
                 : state.sem_ir.identifier(state.sem_ir.name(entity.name).text);
    llvm::Constant *initializer = nullptr;
    if (!imported) {
      if (!entity.result.isConcrete()) {
        error = "readonly static has no concrete initializer";
        return false;
      }
      initializer = state.constant_object(entity.result.value, error);
      if (!initializer)
        return false;
    }
    const auto linkage = imported || (entity.flags & SemConstantPublic) != 0
                             ? llvm::GlobalValue::ExternalLinkage
                             : llvm::GlobalValue::InternalLinkage;
    auto *global = new llvm::GlobalVariable(
        state.module, state.lower_object_type(entity.type), true, linkage,
        initializer, state.mangle_static(package, module, name));
    state.static_globals.emplace(index, global);
  }
  return true;
}

} // namespace chtholly::compiler
