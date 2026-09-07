#include "chtholly/Driver/TargetConfig.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/SemIR.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
bool writeSeed(const std::filesystem::path &directory, std::string_view name,
               std::uint8_t family, std::string_view bytes) {
  std::ofstream out(directory / name, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.put(static_cast<char>(family));
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: next_artifact_seed_generator <corpus-directory>\n";
    return 2;
  }
  const std::filesystem::path directory(argv[1]);
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    std::cerr << filesystem_error.message() << '\n';
    return 2;
  }

  chtholly::compiler::CompilationSession provider(chtholly::hostTargetTriple(),
                                              "artifact-fuzz-seeds");
  const auto unit_id = provider.addUnit(chtholly::compiler::SourceInput(
      "seeds.cns", R"(
module seeds;
pub struct Box<T> { pub value: T; }
pub fn identity<T>(value: T): T { return value; }
fn seed(): i32 {
  let box: Box<i32> = Box<i32> { .value = 7 };
  return identity(box.value);
}
)"));
  std::string error;
  if (!provider.compile(error)) {
    std::cerr << error << '\n';
    return 3;
  }

  chtholly::compiler::CompilationSession consumer(chtholly::hostTargetTriple(),
                                              "artifact-fuzz-consumer");
  const auto consumer_id = consumer.addUnit(chtholly::compiler::SourceInput(
      "consumer.cns", R"(
module consumer;
import seeds;
fn main(): i32 { return seeds::identity(7); }
)"));
  chtholly::compiler::CompilationRequest consumer_request;
  consumer_request.dependency_manifests = {&provider.packageManifest()};
  consumer_request.unit_emission_roles = {
      chtholly::compiler::ModuleEmissionRole::ExecutableEntry};
  if (!consumer.compile(error, consumer_request)) {
    std::cerr << error << '\n';
    return 3;
  }

  const auto &unit = provider.unit(unit_id);
  const auto *sem_ir = consumer.unit(consumer_id).semIR();
  const auto *module = provider.packageManifest().findModule("seeds");
  if (!sem_ir || !module || sem_ir->specializationComponents().empty() ||
      module->public_interface.nominalTypes().empty() ||
      unit.nominalTypeSpecificArtifacts().empty() ||
      unit.nominalSemanticWitnessArtifacts().empty() ||
      unit.nominalTypeLayoutArtifacts().empty()) {
    std::cerr << "seed compilation did not produce every artifact family: "
              << "semir=" << (sem_ir != nullptr)
              << " module=" << (module != nullptr)
              << " components="
              << (sem_ir ? sem_ir->specializationComponents().size() : 0)
              << " definitions="
              << (module ? module->public_interface.nominalTypes().size() : 0)
              << " specifics=" << unit.nominalTypeSpecificArtifacts().size()
              << " witnesses="
              << unit.nominalSemanticWitnessArtifacts().size()
              << " layouts=" << unit.nominalTypeLayoutArtifacts().size()
              << '\n';
    return 4;
  }

  const auto manifest = provider.packageManifest().encode(error);
  const auto component = sem_ir->specializationComponents().front().encode(error);
  const std::array seeds = {
      std::pair{"package-manifest", manifest},
      std::pair{"specialization-component", component},
      std::pair{"nominal-definition",
                module->public_interface.nominalTypes().front().encode()},
      std::pair{"nominal-specific",
                unit.nominalTypeSpecificArtifacts().front().encode()},
      std::pair{"nominal-witness",
                unit.nominalSemanticWitnessArtifacts().front().encode()},
      std::pair{"nominal-layout",
                unit.nominalTypeLayoutArtifacts().front().encode()},
  };
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 5;
  }
  for (std::uint8_t family = 0; family < seeds.size(); ++family)
    if (!writeSeed(directory, seeds[family].first, family,
                   seeds[family].second))
      return 6;
  return 0;
}
