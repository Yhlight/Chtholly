#include "CompilerArtifactStoreInternal.h"

#include <array>
#include <ranges>
#include <sstream>

namespace chtholly {
namespace {

constexpr std::string_view ReferenceMagic = "CHNXTREF1";
constexpr std::string_view LeaseMagic = "CHNXTLEASE1";
constexpr std::string_view SpecializationReferenceMagic = "CHNXTSPECREF1";
constexpr std::string_view TypeSpecificReferenceMagic = "CHNXTYPEIDX1";
constexpr std::string_view NominalSemanticWitnessReferenceMagic = "CHNXWITIDX1";
constexpr std::string_view TypeLayoutReferenceMagic = "CHNXLAYIDX1";

std::string_view
nominalReferenceMagic(CompilerArtifactCodecService::NominalReferenceKind kind) {
  switch (kind) {
  case CompilerArtifactCodecService::NominalReferenceKind::TypeSpecific:
    return TypeSpecificReferenceMagic;
  case CompilerArtifactCodecService::NominalReferenceKind::SemanticWitness:
    return NominalSemanticWitnessReferenceMagic;
  case CompilerArtifactCodecService::NominalReferenceKind::TypeLayout:
    return TypeLayoutReferenceMagic;
  }
  return {};
}

} // namespace

bool CompilerArtifactCodecService::hasInvalidFieldCharacter(
    std::string_view value) {
  return value.find_first_of("\t\r\n") != std::string_view::npos;
}

bool CompilerArtifactCodecService::validHexKey(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<compiler::StableFingerprint>
CompilerArtifactCodecService::parseFingerprint(std::string_view hex) {
  if (hex.size() != compiler::StableFingerprint::ByteCount * 2)
    return std::nullopt;
  std::array<std::uint8_t, compiler::StableFingerprint::ByteCount> bytes{};
  const auto digit = [](char value) -> int {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto high = digit(hex[index * 2]);
    const auto low = digit(hex[index * 2 + 1]);
    if (high < 0 || low < 0)
      return std::nullopt;
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return compiler::StableFingerprint(bytes);
}

std::string CompilerArtifactCodecService::encodeNominalReference(
    NominalReferenceKind kind,
    const compiler::StableFingerprint &result_fingerprint) {
  return std::string(nominalReferenceMagic(kind)) + "\nresult\t" +
         result_fingerprint.hex() + "\n";
}

std::optional<compiler::StableFingerprint>
CompilerArtifactCodecService::decodeNominalReference(
    std::string_view bytes, NominalReferenceKind kind) {
  std::istringstream input{std::string(bytes)};
  std::string magic;
  std::string result;
  std::string trailing;
  if (!std::getline(input, magic) || magic != nominalReferenceMagic(kind) ||
      !std::getline(input, result) || !result.starts_with("result\t") ||
      std::getline(input, trailing))
    return std::nullopt;
  return parseFingerprint(result.substr(std::string_view("result\t").size()));
}

std::string CompilerArtifactCodecService::encodeSpecializationReference(
    const compiler::StableFingerprint &component_fingerprint) {
  return std::string(SpecializationReferenceMagic) + "\ncomponent\t" +
         component_fingerprint.hex() + "\n";
}

std::optional<compiler::StableFingerprint>
CompilerArtifactCodecService::decodeSpecializationReference(
    std::string_view bytes) {
  std::istringstream input{std::string(bytes)};
  std::string magic;
  std::string component;
  std::string trailing;
  if (!std::getline(input, magic) || magic != SpecializationReferenceMagic ||
      !std::getline(input, component) ||
      !component.starts_with("component\t") || std::getline(input, trailing))
    return std::nullopt;
  return parseFingerprint(
      component.substr(std::string_view("component\t").size()));
}

bool CompilerArtifactCodecService::verifyLease(
    const CompilerArtifactLeaseSnapshot &lease, std::string &error) {
  error.clear();
  if (!validHexKey(lease.session_key) || lease.target_triple.empty() ||
      lease.root_package.empty() ||
      hasInvalidFieldCharacter(lease.target_triple) ||
      hasInvalidFieldCharacter(lease.root_package) ||
      !lease.root_manifest.hasValue()) {
    error = "compiler artifact lease has an invalid identity";
    return false;
  }
  return true;
}

std::string CompilerArtifactCodecService::encodeLease(
    const CompilerArtifactLeaseSnapshot &lease, std::string &error) {
  if (!verifyLease(lease, error))
    return {};
  std::ostringstream out;
  out << LeaseMagic << '\n'
      << "session\t" << lease.session_key << '\n'
      << "target\t" << lease.target_triple << '\n'
      << "root\t" << lease.root_package << '\n'
      << "manifest\t" << lease.root_manifest.hex() << '\n';
  return out.str();
}

std::optional<CompilerArtifactLeaseSnapshot>
CompilerArtifactCodecService::decodeLease(std::string_view bytes,
                                          std::string &error) {
  error.clear();
  std::istringstream input{std::string(bytes)};
  std::string magic;
  std::string session;
  std::string target;
  std::string root;
  std::string manifest;
  std::string trailing;
  if (!std::getline(input, magic) || magic != LeaseMagic ||
      !std::getline(input, session) || !session.starts_with("session\t") ||
      !std::getline(input, target) || !target.starts_with("target\t") ||
      !std::getline(input, root) || !root.starts_with("root\t") ||
      !std::getline(input, manifest) || !manifest.starts_with("manifest\t") ||
      std::getline(input, trailing)) {
    error = "compiler artifact lease has an invalid encoding";
    return std::nullopt;
  }
  const auto fingerprint = parseFingerprint(
      std::string_view(manifest).substr(std::string_view("manifest\t").size()));
  if (!fingerprint) {
    error = "compiler artifact lease has an invalid fingerprint";
    return std::nullopt;
  }
  CompilerArtifactLeaseSnapshot result{
      .session_key = session.substr(std::string_view("session\t").size()),
      .target_triple = target.substr(std::string_view("target\t").size()),
      .root_package = root.substr(std::string_view("root\t").size()),
      .root_manifest = *fingerprint};
  return verifyLease(result, error) ? std::optional(std::move(result))
                                    : std::nullopt;
}

bool CompilerSessionArtifactReference::verify(std::string &error) const {
  error.clear();
  if (target_triple.empty() || root_package.empty() ||
      CompilerArtifactCodecService::hasInvalidFieldCharacter(target_triple) ||
      CompilerArtifactCodecService::hasInvalidFieldCharacter(root_package) ||
      !root_manifest.hasValue()) {
    error = "compiler session artifact reference has an invalid identity";
    return false;
  }
  return true;
}

std::string CompilerSessionArtifactReference::encode(std::string &error) const {
  if (!verify(error))
    return {};
  std::ostringstream out;
  out << ReferenceMagic << '\n'
      << "target\t" << target_triple << '\n'
      << "root\t" << root_package << '\n'
      << "manifest\t" << root_manifest.hex() << '\n';
  return out.str();
}

std::optional<CompilerSessionArtifactReference>
CompilerSessionArtifactReference::decode(std::string_view bytes,
                                         std::string &error) {
  error.clear();
  std::istringstream input{std::string(bytes)};
  std::string magic;
  std::string target;
  std::string root;
  std::string manifest;
  std::string trailing;
  if (!std::getline(input, magic) || magic != ReferenceMagic ||
      !std::getline(input, target) || !target.starts_with("target\t") ||
      !std::getline(input, root) || !root.starts_with("root\t") ||
      !std::getline(input, manifest) || !manifest.starts_with("manifest\t") ||
      std::getline(input, trailing)) {
    error = "compiler session artifact reference has an invalid encoding";
    return std::nullopt;
  }
  const auto fingerprint = CompilerArtifactCodecService::parseFingerprint(
      std::string_view(manifest).substr(std::string_view("manifest\t").size()));
  if (!fingerprint) {
    error = "compiler session artifact reference has an invalid fingerprint";
    return std::nullopt;
  }
  CompilerSessionArtifactReference result{
      target.substr(std::string_view("target\t").size()),
      root.substr(std::string_view("root\t").size()), *fingerprint};
  if (!result.verify(error))
    return std::nullopt;
  return result;
}

} // namespace chtholly
