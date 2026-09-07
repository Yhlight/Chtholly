#include "chtholly/Compiler/CFDL.h"

#include "chtholly/Compiler/Diagnostics.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace chtholly::compiler {
namespace {

#include "CFDLParser.inc"

#include "CFDLValidation.inc"
} // namespace

#include "CFDLRendering.inc"

#include "CFDLSemIRBuilder.inc"

} // namespace chtholly::compiler
