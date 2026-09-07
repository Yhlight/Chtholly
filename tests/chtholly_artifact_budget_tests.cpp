#include "ArtifactDecodeInternal.h"
#include "chtholly/Compiler/ConcreteSpecialization.h"
#include "chtholly/Compiler/IncrementalDependencies.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

bool hasCategory(std::string_view error, std::string_view category) {
  return error.starts_with(category) && error.size() > category.size();
}

void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendString(std::string &out, std::string_view value) {
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

} // namespace

int main() {
  using chtholly::compiler::internal::ArtifactDecodeContext;
  using chtholly::compiler::internal::ArtifactDecodeLimits;
  using chtholly::compiler::internal::ArtifactDecodeRecursionScope;

  bool ok = true;
  ArtifactDecodeLimits limits;
  limits.max_input_bytes = 8;
  limits.max_nodes = 3;
  limits.max_string_bytes = 5;
  limits.max_single_string_bytes = 4;
  limits.max_recursion_depth = 2;

  ArtifactDecodeContext exact_input(8, limits);
  ok &= expect(!exact_input.failed(), "input at the limit must be accepted");
  ArtifactDecodeContext excess_input(9, limits);
  ok &= expect(hasCategory(excess_input.error(), "input-limit"),
               "input above the limit must have a stable category");

  ArtifactDecodeContext node_context(0, limits);
  ok &= expect(node_context.consumeNodes(3),
               "nodes at the limit must be accepted");
  ok &= expect(!node_context.consumeNodes(1) &&
                   hasCategory(node_context.error(), "node-budget"),
               "nodes above the limit must be rejected");

  ArtifactDecodeContext string_context(0, limits);
  ok &=
      expect(string_context.consumeString(4) && string_context.consumeString(1),
             "cumulative strings at the limit must be accepted");
  ok &= expect(!string_context.consumeString(1) &&
                   hasCategory(string_context.error(), "string-budget"),
               "cumulative strings above the limit must be rejected");

  ArtifactDecodeLimits recursion_limits = limits;
  recursion_limits.max_nodes = 16;
  ArtifactDecodeContext recursion_context(0, recursion_limits);
  {
    ArtifactDecodeRecursionScope first(recursion_context);
    ArtifactDecodeRecursionScope second(recursion_context);
    ok &= expect(first.entered() && second.entered(),
                 "recursion at the limit must be accepted");
    ArtifactDecodeRecursionScope third(recursion_context);
    ok &= expect(!third.entered() &&
                     hasCategory(recursion_context.error(), "recursion-budget"),
                 "recursion above the limit must be rejected");
  }

  ArtifactDecodeLimits nested_limits;
  nested_limits.max_input_bytes = 1024;
  nested_limits.max_nodes = 64;
  nested_limits.max_string_bytes = 3;
  nested_limits.max_single_string_bytes = 3;
  nested_limits.max_recursion_depth = 8;
  ArtifactDecodeContext nested_context(0, nested_limits);
  std::string nominal("CHNXTYPE30");
  nominal.push_back(1);
  appendString(nominal, "four");
  std::string nested_error;
  const auto nested = chtholly::compiler::internal::decodePublicNominalTypeArtifact(
      nominal, nested_error, nested_context);
  ok &= expect(!nested && hasCategory(nested_error, "string-budget"),
               "nested nominal decode must share the parent string budget");

  const auto nested_type = [](std::size_t levels) {
    chtholly::compiler::PublicType type =
        chtholly::compiler::PublicType::integer(32, true);
    for (std::size_t index = 1; index < levels; ++index)
      type = chtholly::compiler::PublicType::rawPointer(std::move(type), true);
    return type;
  };
  const auto nominal_with_type = [&](std::size_t levels) {
    chtholly::compiler::PublicNominalFieldArtifact field;
    field.name = "value";
    field.type = nested_type(levels);
    auto artifact = chtholly::compiler::buildPublicNominalTypeArtifact(
        "budget", "depth", "Box", 0, {std::move(field)});
    chtholly::compiler::finalizePublicNominalTypeArtifact(artifact);
    return artifact.encode();
  };
  std::string depth_error;
  ok &= expect(chtholly::compiler::PublicNominalTypeArtifact::decode(
                   nominal_with_type(128), depth_error)
                   .has_value(),
               "wire recursion at depth 128 must be accepted");
  depth_error.clear();
  ok &= expect(!chtholly::compiler::PublicNominalTypeArtifact::decode(
                   nominal_with_type(129), depth_error) &&
                   hasCategory(depth_error, "recursion-budget"),
               "wire recursion at depth 129 must be rejected");

  std::string oversized(64U * 1024U * 1024U + 1U, '\0');
  const auto reject_input = [&](auto decode, std::string_view family) {
    std::string error;
    const auto decoded = decode(oversized, error);
    ok &= expect(!decoded && hasCategory(error, "input-limit"), family);
  };
  reject_input(chtholly::compiler::CompilerPackageArtifactManifest::decode,
               "manifest input limit");
  reject_input(chtholly::compiler::ConcreteSpecializationComponentArtifact::decode,
               "specialization input limit");
  reject_input(chtholly::compiler::PublicNominalTypeArtifact::decode,
               "nominal definition input limit");
  reject_input(chtholly::compiler::NominalTypeSpecificArtifact::decode,
               "nominal specific input limit");
  reject_input(chtholly::compiler::NominalSemanticWitnessArtifact::decode,
               "nominal witness input limit");
  reject_input(chtholly::compiler::NominalTypeLayoutArtifact::decode,
               "nominal layout input limit");

  return ok ? 0 : 1;
}
