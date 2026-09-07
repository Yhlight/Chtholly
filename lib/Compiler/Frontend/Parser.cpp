#include "chtholly/Compiler/Parser.h"

#include "chtholly/Compiler/Grammar.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace chtholly::compiler {
namespace {

class Parser {
public:
#include "ParserEntry.inc"
private:
#include "ParserLexical.inc"

#include "ParserDeclaration.inc"
#include "ParserType.inc"
#include "ParserStatement.inc"
#include "ParserExpression.inc"
#include "ParserRecovery.inc"
};

} // namespace

ParseTree parse(const TokenBuffer &tokens, DiagnosticEmitter &diagnostics) {
  return Parser(tokens, diagnostics).run();
}

} // namespace chtholly::compiler
