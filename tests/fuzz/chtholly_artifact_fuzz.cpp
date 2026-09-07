#include "chtholly_artifact_fuzz.h"

#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Compiler/IncrementalDependencies.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace {
template <typename Artifact, typename Decode, typename Encode>
void roundTrip(std::string_view bytes, Decode decode, Encode encode) {
  std::string error;
  auto artifact = decode(bytes, error);
  if (!artifact)
    return;
  if (!artifact->verify(error))
    std::abort();
  const auto encoded = encode(*artifact, error);
  if (!error.empty())
    std::abort();
  auto decoded = decode(encoded, error);
  if (!decoded || !error.empty())
    std::abort();
  const auto reencoded = encode(*decoded, error);
  if (!error.empty() || reencoded != encoded)
    std::abort();
}
} // namespace

int fuzzNextArtifact(const std::uint8_t *data, std::size_t size) {
  if (!data || size == 0)
    return 0;
  const auto bytes = std::string_view(
      reinterpret_cast<const char *>(data + 1), size - 1);
  switch (data[0] % 6) {
  case 0:
    roundTrip<chtholly::compiler::CompilerPackageArtifactManifest>(
        bytes, chtholly::compiler::CompilerPackageArtifactManifest::decode,
        [](const auto &artifact, std::string &error) {
          return artifact.encode(error);
        });
    break;
  case 1:
    roundTrip<chtholly::compiler::ConcreteSpecializationComponentArtifact>(
        bytes,
        chtholly::compiler::ConcreteSpecializationComponentArtifact::decode,
        [](const auto &artifact, std::string &error) {
          return artifact.encode(error);
        });
    break;
  case 2:
    roundTrip<chtholly::compiler::PublicNominalTypeArtifact>(
        bytes, chtholly::compiler::PublicNominalTypeArtifact::decode,
        [](const auto &artifact, std::string &error) {
          error.clear();
          return artifact.encode();
        });
    break;
  case 3:
    roundTrip<chtholly::compiler::NominalTypeSpecificArtifact>(
        bytes, chtholly::compiler::NominalTypeSpecificArtifact::decode,
        [](const auto &artifact, std::string &error) {
          error.clear();
          return artifact.encode();
        });
    break;
  case 4:
    roundTrip<chtholly::compiler::NominalSemanticWitnessArtifact>(
        bytes, chtholly::compiler::NominalSemanticWitnessArtifact::decode,
        [](const auto &artifact, std::string &error) {
          error.clear();
          return artifact.encode();
        });
    break;
  case 5:
    roundTrip<chtholly::compiler::NominalTypeLayoutArtifact>(
        bytes, chtholly::compiler::NominalTypeLayoutArtifact::decode,
        [](const auto &artifact, std::string &error) {
          error.clear();
          return artifact.encode();
        });
    break;
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  return fuzzNextArtifact(data, size);
}
