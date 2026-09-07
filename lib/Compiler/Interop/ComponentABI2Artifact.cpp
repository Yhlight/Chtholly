#include "chtholly/Compiler/ComponentABI2Artifact.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>

namespace chtholly::compiler {
namespace {
constexpr std::string_view Magic = "CHNXA2R";
constexpr std::size_t HeaderSize = 7 + 2 + 4 + 32;

bool validPath(std::string_view path) {
  return !path.empty() && path.size() <= 32768 &&
         path.find('\0') == path.npos;
}

struct TemporaryFile {
  std::filesystem::path directory;
  std::filesystem::path file;
  ~TemporaryFile() {
    std::error_code ignored;
    if (!file.empty()) std::filesystem::remove(file, ignored);
    if (!directory.empty()) std::filesystem::remove(directory, ignored);
  }
};

std::optional<ComponentAbi2Descriptor> failure(
    ComponentAbi2DescriptorError value, ComponentAbi2DescriptorError &kind,
    std::string &error, std::string_view message) {
  kind = value;
  error = message;
  return std::nullopt;
}
} // namespace

std::string encodeComponentAbi2Artifact(
    const ComponentAbi2Descriptor &descriptor, std::string &error) {
  error.clear();
  const auto encoded = descriptor.encode(error);
  if (encoded.empty()) return {};
  if (encoded.size() > ComponentAbi2ArtifactMaxBytes - HeaderSize) {
    error = "ABI-2 artifact exceeds its size budget";
    return {};
  }
  std::string result(Magic);
  result.append("\1\0", 2);
  const auto size = static_cast<std::uint32_t>(encoded.size());
  for (unsigned shift = 0; shift < 32; shift += 8)
    result.push_back(static_cast<char>(size >> shift));
  const auto digest = componentAbi2DescriptorDigest(descriptor);
  result.append(reinterpret_cast<const char *>(digest.bytes().data()), 32);
  result += encoded;
  return result;
}

std::optional<ComponentAbi2Descriptor> decodeComponentAbi2Artifact(
    std::string_view bytes, ComponentAbi2DescriptorError &kind,
    std::string &error) {
  kind = ComponentAbi2DescriptorError::None;
  error.clear();
  if (bytes.size() > ComponentAbi2ArtifactMaxBytes)
    return failure(ComponentAbi2DescriptorError::SizeOverflow, kind, error,
                   "ABI-2 artifact exceeds its size budget");
  if (bytes.size() < Magic.size())
    return failure(ComponentAbi2DescriptorError::Truncated, kind, error,
                   "truncated ABI-2 artifact magic");
  if (!bytes.starts_with(Magic))
    return failure(ComponentAbi2DescriptorError::InvalidMagic, kind, error,
                   "invalid ABI-2 artifact magic");
  if (bytes.size() < HeaderSize)
    return failure(ComponentAbi2DescriptorError::Truncated, kind, error,
                   "truncated ABI-2 artifact envelope");
  if (bytes[7] != 1 || bytes[8] != 0)
    return failure(ComponentAbi2DescriptorError::UnsupportedVersion, kind, error,
                   "unsupported ABI-2 artifact version");
  std::uint32_t size = 0;
  for (unsigned index = 0; index < 4; ++index)
    size |= static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(bytes[9 + index])) << (8 * index);
  if (size > ComponentAbi2ArtifactMaxBytes - HeaderSize)
    return failure(ComponentAbi2DescriptorError::SizeOverflow, kind, error,
                   "ABI-2 artifact descriptor size exceeds its budget");
  if (bytes.size() < HeaderSize + size)
    return failure(ComponentAbi2DescriptorError::Truncated, kind, error,
                   "truncated ABI-2 artifact descriptor");
  if (bytes.size() != HeaderSize + size)
    return failure(ComponentAbi2DescriptorError::NonCanonical, kind, error,
                   "trailing bytes in ABI-2 artifact");
  auto descriptor = ComponentAbi2Descriptor::decode(
      bytes.substr(HeaderSize), kind, error);
  if (!descriptor) return std::nullopt;
  const auto digest = descriptor->descriptor_digest.bytes();
  if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 13,
                  [](std::uint8_t a, char b) {
                    return a == static_cast<std::uint8_t>(b);
                  }))
    return failure(ComponentAbi2DescriptorError::DigestMismatch, kind, error,
                   "ABI-2 artifact envelope digest mismatch");
  return descriptor;
}

std::optional<ComponentAbi2Descriptor> readComponentAbi2Artifact(
    std::string_view path, ComponentAbi2DescriptorError &kind,
    std::string &error) {
  kind = ComponentAbi2DescriptorError::None;
  error.clear();
  if (!validPath(path))
    return failure(ComponentAbi2DescriptorError::InvalidField, kind, error,
                   "invalid ABI-2 artifact path");
  std::ifstream input(chtholly::pathForFileSystem(path),
                      std::ios::binary | std::ios::ate);
  if (!input)
    return failure(ComponentAbi2DescriptorError::IoError, kind, error,
                   "cannot open ABI-2 artifact");
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > ComponentAbi2ArtifactMaxBytes)
    return failure(ComponentAbi2DescriptorError::SizeOverflow, kind, error,
                   "ABI-2 artifact file exceeds its size budget");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.seekg(0);
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::char_traits<char>::eof())
    return failure(ComponentAbi2DescriptorError::IoError, kind, error,
                   "ABI-2 artifact changed or could not be read");
  return decodeComponentAbi2Artifact(bytes, kind, error);
}

bool writeComponentAbi2Artifact(std::string_view path,
                                const ComponentAbi2Descriptor &descriptor,
                                std::string &error) {
  if (!validPath(path)) {
    error = "invalid ABI-2 artifact path";
    return false;
  }
  const auto bytes = encodeComponentAbi2Artifact(descriptor, error);
  if (bytes.empty()) return false;
  // Reserve a private same-directory staging area atomically. This avoids
  // clobbering another writer's .tmp or following a pre-existing temp symlink.
  static std::atomic<std::uint64_t> sequence{0};
  TemporaryFile temporary;
  const auto destination = chtholly::pathForFileSystem(path);
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    auto candidate = destination;
    candidate += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(sequence.fetch_add(1));
    std::error_code ec;
    if (std::filesystem::create_directory(candidate, ec)) {
      temporary.directory = candidate;
      temporary.file = candidate / "artifact";
      break;
    }
    if (ec && ec != std::errc::file_exists) {
      error = "cannot stage ABI-2 artifact: " + ec.message();
      return false;
    }
  }
  if (temporary.directory.empty()) {
    error = "cannot reserve ABI-2 artifact staging directory";
    return false;
  }
  std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  output.close();
  if (output.fail()) {
    error = "failed to write ABI-2 artifact";
    return false;
  }
  std::error_code ec;
  const auto encoded_path = temporary.file.u8string();
  if (!chtholly::replaceFile(
          std::string(encoded_path.begin(), encoded_path.end()),
          std::string(path), ec)) {
    error = "failed to publish ABI-2 artifact: " + ec.message();
    return false;
  }
  return true;
}
} // namespace chtholly::compiler
