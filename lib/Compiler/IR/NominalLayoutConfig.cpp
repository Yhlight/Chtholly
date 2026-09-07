#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <cassert>
#include <limits>

namespace chtholly::compiler {
namespace {

void appendU32(std::string &out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

void appendField(std::string &out, std::string_view value) {
  assert(value.size() <= std::numeric_limits<std::uint32_t>::max());
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

} // namespace

StableFingerprint TargetLayoutConfig::fingerprint() const {
  std::string bytes("chtholly.next.target-layout.v1");
  appendField(bytes, normalized_triple);
  appendU32(bytes, pointer_width);
  appendU32(bytes, abi_epoch);
  return StableFingerprint::fromCanonicalBytes(bytes);
}

bool TargetLayoutConfig::verify(std::string &error) const {
  if (normalized_triple.empty() ||
      (pointer_width != 32 && pointer_width != 64) || abi_epoch == 0) {
    error = "target layout configuration is invalid";
    return false;
  }
  return true;
}

} // namespace chtholly::compiler
