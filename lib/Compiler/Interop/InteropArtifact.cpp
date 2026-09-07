#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_set>

namespace chtholly::compiler::interop {
namespace {

#include "InteropArtifactCodec.inc"
} // namespace

#include "InteropArtifactProtocol.inc"

#include "InteropArtifactRegistry.inc"

} // namespace chtholly::compiler::interop
