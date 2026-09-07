#pragma once

#include <iosfwd>
#include <string>

namespace chtholly {

struct CompilerInvocation;

int runCompilerDoctor(const CompilerInvocation &invocation,
                      std::ostream &output, std::string &error);

} // namespace chtholly
