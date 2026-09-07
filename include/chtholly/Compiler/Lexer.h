#pragma once

#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/TokenBuffer.h"

namespace chtholly::compiler {

[[nodiscard]] TokenBuffer lex(const SourceBuffer &source,
                              SharedValueStores &values,
                              DiagnosticEmitter &diagnostics,
                              LanguageVersion language_version =
                                  DefaultLanguageVersion);

} // namespace chtholly::compiler
