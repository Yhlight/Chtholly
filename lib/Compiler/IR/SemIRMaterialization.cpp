#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"
#include "chtholly/Compiler/CarrierView.h"

#include <array>
#include <cassert>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler {
namespace {

constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
constexpr std::uint32_t UnionFieldIndexMask = ~UnionFieldUnsafeBit;
constexpr std::uint32_t ProjectionIndexMask = 0x7fffffffU;
constexpr std::uint32_t ProjectionKindShift = 31U;

} // namespace

#include "SemIRMaterializeTemplate.inc"

#include "SemIRMaterializeConcrete.inc"


} // namespace chtholly::compiler
