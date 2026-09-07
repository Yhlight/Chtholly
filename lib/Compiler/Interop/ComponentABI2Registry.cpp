#include "chtholly/Compiler/ComponentABI2Registry.h"
#include "chtholly/Compiler/ComponentABI2Artifact.h"
#include <limits>

namespace chtholly::compiler {

std::string ComponentAbi2DescriptorRegistry::key(
    std::string_view identity, const StableFingerprint &digest) {
  std::string result(identity);
  result.push_back('\0');
  result += digest.hex();
  return result;
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::registerDescriptor(
    std::string_view bytes, ComponentAbi2DescriptorError &decode_error,
    std::string &error) {
  auto descriptor = ComponentAbi2Descriptor::decode(bytes, decode_error, error);
  if (!descriptor)
    return ComponentAbi2RegistryResult::Invalid;
  return registerDescriptor(*descriptor, error);
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::writeArtifactFile(
    std::string_view path, const ComponentAbi2Descriptor &descriptor,
    std::string &error) const {
  return writeComponentAbi2Artifact(path, descriptor, error)
             ? ComponentAbi2RegistryResult::Inserted
             : ComponentAbi2RegistryResult::Invalid;
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::replayArtifactFile(
    std::string_view path, ComponentAbi2DescriptorError &decode_error,
    std::string &error) {
  auto descriptor = readComponentAbi2Artifact(path, decode_error, error);
  if (!descriptor) return ComponentAbi2RegistryResult::Invalid;
  return registerDescriptor(*descriptor, error);
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::registerDescriptor(
    const ComponentAbi2Descriptor &descriptor, std::string &error) {
  error.clear();
  if (!descriptor.verify(error))
    return ComponentAbi2RegistryResult::Invalid;
  const auto descriptor_key = key(descriptor.component_identity,
                                  descriptor.descriptor_digest);
  if (entries_.contains(descriptor_key))
    return ComponentAbi2RegistryResult::AlreadyRegistered;
  for (const auto &[existing_key, entry] : entries_) {
    (void)existing_key;
    if (entry.descriptor.component_identity == descriptor.component_identity) {
      error = "ABI-2 descriptor identity is already registered with another digest";
      return ComponentAbi2RegistryResult::Conflict;
    }
  }
  entries_.emplace(descriptor_key, Entry{descriptor, 0});
  return ComponentAbi2RegistryResult::Inserted;
}

std::optional<ComponentAbi2Descriptor> ComponentAbi2DescriptorRegistry::lookup(
    std::string_view component_identity,
    const StableFingerprint &descriptor_digest) const {
  const auto found = entries_.find(key(component_identity, descriptor_digest));
  if (found == entries_.end())
    return std::nullopt;
  return found->second.descriptor;
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::retainLease(
    std::string_view component_identity, const StableFingerprint &digest) {
  const auto found = entries_.find(key(component_identity, digest));
  if (found == entries_.end())
    return ComponentAbi2RegistryResult::NotFound;
  if (found->second.active_leases == std::numeric_limits<std::uint32_t>::max())
    return ComponentAbi2RegistryResult::Busy;
  ++found->second.active_leases;
  return ComponentAbi2RegistryResult::Retained;
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::releaseLease(
    std::string_view component_identity, const StableFingerprint &digest) {
  const auto found = entries_.find(key(component_identity, digest));
  if (found == entries_.end())
    return ComponentAbi2RegistryResult::NotFound;
  if (found->second.active_leases == 0)
    return ComponentAbi2RegistryResult::Invalid;
  --found->second.active_leases;
  return ComponentAbi2RegistryResult::Released;
}

ComponentAbi2RegistryResult ComponentAbi2DescriptorRegistry::erase(
    std::string_view component_identity, const StableFingerprint &digest) {
  const auto found = entries_.find(key(component_identity, digest));
  if (found == entries_.end())
    return ComponentAbi2RegistryResult::NotFound;
  if (found->second.active_leases != 0)
    return ComponentAbi2RegistryResult::Busy;
  entries_.erase(found);
  return ComponentAbi2RegistryResult::Erased;
}

} // namespace chtholly::compiler
