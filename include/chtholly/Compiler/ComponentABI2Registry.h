#pragma once

#include "chtholly/Compiler/ComponentABI2Protocol.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chtholly::compiler {

enum class ComponentAbi2RegistryResult : std::uint8_t {
  Inserted,
  AlreadyRegistered,
  Retained,
  Released,
  Erased,
  Conflict,
  Invalid,
  NotFound,
  Busy,
};

// Session-owned registry for verified ABI-2 descriptors. It is intentionally
// single-threaded; a runtime host must linearize calls before entering it.
class ComponentAbi2DescriptorRegistry {
public:
  ComponentAbi2RegistryResult registerDescriptor(
      std::string_view bytes, ComponentAbi2DescriptorError &decode_error,
      std::string &error);
  ComponentAbi2RegistryResult registerDescriptor(
      const ComponentAbi2Descriptor &descriptor, std::string &error);
  ComponentAbi2RegistryResult writeArtifactFile(
      std::string_view path, const ComponentAbi2Descriptor &descriptor,
      std::string &error) const;
  ComponentAbi2RegistryResult replayArtifactFile(
      std::string_view path, ComponentAbi2DescriptorError &decode_error,
      std::string &error);

  [[nodiscard]] std::optional<ComponentAbi2Descriptor> lookup(
      std::string_view component_identity,
      const StableFingerprint &descriptor_digest) const;

  ComponentAbi2RegistryResult retainLease(std::string_view component_identity,
                                          const StableFingerprint &digest);
  ComponentAbi2RegistryResult releaseLease(std::string_view component_identity,
                                           const StableFingerprint &digest);
  ComponentAbi2RegistryResult erase(std::string_view component_identity,
                                    const StableFingerprint &digest);

  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  void clear() { entries_.clear(); }

private:
  struct Entry {
    ComponentAbi2Descriptor descriptor;
    std::uint32_t active_leases = 0;
  };

  [[nodiscard]] static std::string key(std::string_view identity,
                                       const StableFingerprint &digest);
  std::unordered_map<std::string, Entry> entries_;
};

} // namespace chtholly::compiler
