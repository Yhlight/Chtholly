#include "fuzz/chtholly_artifact_fuzz.h"

#include <array>
#include <cstdint>

int main() {
  for (std::uint8_t family = 0; family < 6; ++family) {
    const std::array<std::uint8_t, 2> input = {family, 0};
    if (fuzzNextArtifact(input.data(), input.size()) != 0)
      return 1;
  }
  return 0;
}
