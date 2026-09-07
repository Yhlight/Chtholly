#pragma once

#include "chtholly/Compiler/LLVM.h"
#include "chtholly/Compiler/TypeLayout.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <optional>
#include <span>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chtholly::compiler {

#include "LLVMInternalModuleValue.inc"
#include "LLVMInternalObjectServices.inc"
#include "LLVMInternalInstructionServices.inc"
#include "LLVMInternalInteropServices.inc"
#include "LLVMInternalCoroutineServices.inc"
#include "LLVMInternalCleanupModule.inc"
#include "LLVMInternalModuleDecls.inc"
} // namespace chtholly::compiler
