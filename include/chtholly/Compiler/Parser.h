#pragma once

#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/ParseTree.h"
#include "chtholly/Compiler/TokenBuffer.h"

namespace chtholly::compiler {

[[nodiscard]] ParseTree parse(const TokenBuffer &tokens,
                              DiagnosticEmitter &diagnostics);

} // namespace chtholly::compiler
