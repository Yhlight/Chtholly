#include "chtholly/Compiler/ComponentABI2Protocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace chtholly::compiler {

namespace {

constexpr std::string_view Magic = "CHTHAB2";
constexpr std::size_t HeaderSize = Magic.size() + 2 + 2 + 4 +
                                   StableFingerprint::ByteCount;
constexpr std::size_t MaxDescriptorBytes = 1024 * 1024;
constexpr std::size_t MaxStringBytes = 64 * 1024;

void appendU16(std::string &out, std::uint16_t value) {
  out.push_back(static_cast<char>(value));
  out.push_back(static_cast<char>(value >> 8));
}

void appendU32(std::string &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<char>(value >> shift));
}

void appendString(std::string &out, std::string_view value) {
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendFingerprint(std::string &out, const StableFingerprint &value) {
  const auto bytes = value.bytes();
  out.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::string canonicalBody(const ComponentAbi2Descriptor &descriptor) {
  std::string body;
  body.reserve(256 + descriptor.component_identity.size() +
               descriptor.entity_identity.size() +
               descriptor.resource_identity.size());
  appendU32(body, descriptor.abi_epoch);
  appendU16(body, descriptor.descriptor_version);
  body.push_back(static_cast<char>(descriptor.operation_kind));
  body.push_back(static_cast<char>(descriptor.terminal_cardinality));
  body.push_back(static_cast<char>(descriptor.lease_policy));
  body.push_back(static_cast<char>(descriptor.ownership_flags));
  appendString(body, descriptor.component_identity);
  appendString(body, descriptor.entity_identity);
  appendString(body, descriptor.resource_identity);
  appendFingerprint(body, descriptor.payload_type_digest);
  appendFingerprint(body, descriptor.layout_digest);
  appendFingerprint(body, descriptor.lifecycle_digest);
  appendFingerprint(body, descriptor.contract_digest);
  appendFingerprint(body, descriptor.runtime_abi_digest);
  return body;
}

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  bool u8(std::uint8_t &value) {
    if (remaining() < 1)
      return false;
    value = static_cast<std::uint8_t>(bytes_[offset_++]);
    return true;
  }
  bool u16(std::uint16_t &value) {
    if (remaining() < 2)
      return false;
    value = static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes_[offset_])) |
            static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes_[offset_ + 1])) << 8;
    offset_ += 2;
    return true;
  }
  bool u32(std::uint32_t &value) {
    if (remaining() < 4)
      return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8)
      value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes_[offset_++])) << shift;
    return true;
  }
  bool string(std::string &value) {
    std::uint32_t size = 0;
    if (!u32(size) || size > MaxStringBytes || remaining() < size)
      return false;
    const auto view = bytes_.substr(offset_, size);
    if (view.find('\0') != view.npos || view.find('\n') != view.npos ||
        view.find('\r') != view.npos)
      return false;
    value.assign(view);
    offset_ += size;
    return true;
  }
  bool fingerprint(StableFingerprint &value) {
    if (remaining() < StableFingerprint::ByteCount)
      return false;
    std::array<std::uint8_t, StableFingerprint::ByteCount> bytes{};
    std::memcpy(bytes.data(), bytes_.data() + offset_, bytes.size());
    value = StableFingerprint(bytes);
    offset_ += bytes.size();
    return true;
  }
  std::size_t remaining() const { return bytes_.size() - offset_; }

private:
  std::string_view bytes_;
  std::size_t offset_ = 0;
};

bool validEnum(const ComponentAbi2Descriptor &descriptor) {
  return descriptor.operation_kind >= ComponentAbi2OperationKind::Send &&
         descriptor.operation_kind <= ComponentAbi2OperationKind::Invoke &&
         descriptor.terminal_cardinality >=
             ComponentAbi2TerminalCardinality::OneShot &&
         descriptor.terminal_cardinality <=
             ComponentAbi2TerminalCardinality::MultiSubmit &&
         descriptor.lease_policy >= ComponentAbi2LeasePolicy::Exclusive &&
         descriptor.lease_policy <= ComponentAbi2LeasePolicy::Shared;
}

bool validIdentity(std::string_view value) {
  return !value.empty() && value.size() <= MaxStringBytes &&
         value.find('\0') == value.npos && value.find('\n') == value.npos &&
         value.find('\r') == value.npos;
}

} // namespace

StableFingerprint
componentAbi2DescriptorDigest(const ComponentAbi2Descriptor &descriptor) {
  const std::string body = canonicalBody(descriptor);
  return StableFingerprint::fromCanonicalBytes(
      std::string("chtholly.component.abi2.descriptor.v1\n") + body);
}

StableFingerprint componentAbi2PayloadPlanDigest(
    const ComponentAbi2Descriptor &descriptor,
    const StableFingerprint &outcome_fingerprint, std::uint32_t source_lane,
    std::uint32_t destination_lane, std::uint32_t token_lane,
    bool source_preserved_until_commit, bool destination_initializes_on_commit) {
  std::string canonical("chtholly.component.abi2.payload-plan.v1\n");
  const auto append_u32 = [&](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
      canonical.push_back(static_cast<char>((value >> shift) & 0xffU));
  };
  const auto append_fp = [&](const StableFingerprint &value) {
    canonical.append(reinterpret_cast<const char *>(value.bytes().data()),
                     value.bytes().size());
  };
  append_fp(componentAbi2DescriptorDigest(descriptor));
  append_fp(outcome_fingerprint);
  append_u32(source_lane); append_u32(destination_lane); append_u32(token_lane);
  canonical.push_back(source_preserved_until_commit ? 1 : 0);
  canonical.push_back(destination_initializes_on_commit ? 1 : 0);
  canonical.push_back(static_cast<char>(descriptor.operation_kind));
  canonical.push_back(static_cast<char>(descriptor.lease_policy));
  canonical.push_back(static_cast<char>(descriptor.ownership_flags));
  return StableFingerprint::fromCanonicalBytes(canonical);
}

const char *componentAbi2DescriptorErrorText(ComponentAbi2DescriptorError error) {
  switch (error) {
  case ComponentAbi2DescriptorError::None: return "none";
  case ComponentAbi2DescriptorError::InvalidMagic: return "invalid magic";
  case ComponentAbi2DescriptorError::UnsupportedVersion: return "unsupported version";
  case ComponentAbi2DescriptorError::Truncated: return "truncated descriptor";
  case ComponentAbi2DescriptorError::SizeOverflow: return "descriptor size overflow";
  case ComponentAbi2DescriptorError::InvalidField: return "invalid descriptor field";
  case ComponentAbi2DescriptorError::NonCanonical: return "non-canonical descriptor";
  case ComponentAbi2DescriptorError::DigestMismatch: return "descriptor digest mismatch";
  case ComponentAbi2DescriptorError::AbiMismatch: return "component ABI mismatch";
  case ComponentAbi2DescriptorError::IoError: return "artifact I/O error";
  }
  return "unknown descriptor error";
}

bool ComponentAbi2Descriptor::canonicalize(std::string &error) {
  error.clear();
  if (abi_epoch != ComponentAbi2Epoch || descriptor_version != ComponentAbi2DescriptorVersion ||
      !validEnum(*this) || !validIdentity(component_identity) ||
      !validIdentity(entity_identity) || !validIdentity(resource_identity) ||
      !payload_type_digest.hasValue() || !layout_digest.hasValue() ||
      !lifecycle_digest.hasValue() || !contract_digest.hasValue() ||
      !runtime_abi_digest.hasValue()) {
    error = "invalid ABI-2 descriptor fields";
    return false;
  }
  descriptor_digest = componentAbi2DescriptorDigest(*this);
  return true;
}

bool ComponentAbi2Descriptor::verify(std::string &error) const {
  error.clear();
  if (abi_epoch != ComponentAbi2Epoch ||
      descriptor_version != ComponentAbi2DescriptorVersion) {
    error = "unsupported ABI-2 descriptor version";
    return false;
  }
  if (!validEnum(*this) || !validIdentity(component_identity) ||
      !validIdentity(entity_identity) || !validIdentity(resource_identity) ||
      !payload_type_digest.hasValue() || !layout_digest.hasValue() ||
      !lifecycle_digest.hasValue() || !contract_digest.hasValue() ||
      !runtime_abi_digest.hasValue()) {
    error = "invalid ABI-2 descriptor fields";
    return false;
  }
  if (descriptor_digest != componentAbi2DescriptorDigest(*this)) {
    error = "ABI-2 descriptor digest mismatch";
    return false;
  }
  return true;
}

std::string ComponentAbi2Descriptor::encode(std::string &error) const {
  ComponentAbi2Descriptor canonical = *this;
  if (!canonical.canonicalize(error))
    return {};
  std::string body = canonicalBody(canonical);
  if (body.size() > std::numeric_limits<std::uint32_t>::max() ||
      HeaderSize + body.size() > MaxDescriptorBytes) {
    error = "ABI-2 descriptor exceeds its size budget";
    return {};
  }
  std::string result;
  result.reserve(HeaderSize + body.size());
  result.append(Magic);
  appendU16(result, ComponentAbi2DescriptorVersion);
  appendU16(result, 0);
  appendU32(result, static_cast<std::uint32_t>(body.size()));
  appendFingerprint(result, canonical.descriptor_digest);
  result.append(body);
  return result;
}

std::optional<ComponentAbi2Descriptor>
ComponentAbi2Descriptor::decode(std::string_view bytes,
                                ComponentAbi2DescriptorError &kind,
                                std::string &error) {
  kind = ComponentAbi2DescriptorError::None;
  error.clear();
  if (bytes.size() > MaxDescriptorBytes) {
    kind = ComponentAbi2DescriptorError::SizeOverflow;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  if (bytes.size() < Magic.size() || bytes.substr(0, Magic.size()) != Magic) {
    kind = ComponentAbi2DescriptorError::InvalidMagic;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  Reader header(bytes.substr(Magic.size()));
  std::uint16_t version = 0, flags = 0;
  std::uint32_t body_size = 0;
  StableFingerprint digest;
  if (!header.u16(version) || !header.u16(flags) || !header.u32(body_size) ||
      !header.fingerprint(digest)) {
    kind = ComponentAbi2DescriptorError::Truncated;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  if (version != ComponentAbi2DescriptorVersion || flags != 0) {
    kind = version != ComponentAbi2DescriptorVersion
               ? ComponentAbi2DescriptorError::UnsupportedVersion
               : ComponentAbi2DescriptorError::InvalidField;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  if (body_size > MaxDescriptorBytes - HeaderSize ||
      bytes.size() != HeaderSize + body_size) {
    kind = bytes.size() < HeaderSize + body_size
               ? ComponentAbi2DescriptorError::Truncated
               : ComponentAbi2DescriptorError::NonCanonical;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  Reader reader(bytes.substr(HeaderSize, body_size));
  ComponentAbi2Descriptor result;
  std::uint8_t operation = 0, cardinality = 0, lease = 0, ownership = 0;
  if (!reader.u32(result.abi_epoch) || !reader.u16(result.descriptor_version) ||
      !reader.u8(operation) || !reader.u8(cardinality) || !reader.u8(lease) ||
      !reader.u8(ownership) || !reader.string(result.component_identity) ||
      !reader.string(result.entity_identity) || !reader.string(result.resource_identity) ||
      !reader.fingerprint(result.payload_type_digest) ||
      !reader.fingerprint(result.layout_digest) ||
      !reader.fingerprint(result.lifecycle_digest) ||
      !reader.fingerprint(result.contract_digest) ||
      !reader.fingerprint(result.runtime_abi_digest) || reader.remaining() != 0) {
    kind = ComponentAbi2DescriptorError::InvalidField;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  result.operation_kind = static_cast<ComponentAbi2OperationKind>(operation);
  result.terminal_cardinality =
      static_cast<ComponentAbi2TerminalCardinality>(cardinality);
  result.lease_policy = static_cast<ComponentAbi2LeasePolicy>(lease);
  result.ownership_flags = ownership;
  result.descriptor_digest = digest;
  if (result.abi_epoch != ComponentAbi2Epoch) {
    kind = ComponentAbi2DescriptorError::AbiMismatch;
    error = componentAbi2DescriptorErrorText(kind);
    return std::nullopt;
  }
  if (!result.verify(error)) {
    kind = error.find("digest") != std::string::npos
               ? ComponentAbi2DescriptorError::DigestMismatch
               : ComponentAbi2DescriptorError::NonCanonical;
    return std::nullopt;
  }
  return result;
}

bool componentAbi2IsTerminal(ComponentAbi2OperationState state) {
  return state == ComponentAbi2OperationState::Committed ||
         state == ComponentAbi2OperationState::Failed ||
         state == ComponentAbi2OperationState::Cancelled ||
         state == ComponentAbi2OperationState::Released;
}

bool componentAbi2Advance(ComponentAbi2OperationState &state,
                          ComponentAbi2OperationEvent event) {
  ComponentAbi2OperationState next = state;
  switch (state) {
  case ComponentAbi2OperationState::Created:
    if (event == ComponentAbi2OperationEvent::Arm)
      next = ComponentAbi2OperationState::Armed;
    else if (event == ComponentAbi2OperationEvent::Fail)
      next = ComponentAbi2OperationState::Failed;
    else if (event == ComponentAbi2OperationEvent::Cancel)
      next = ComponentAbi2OperationState::Cancelled;
    else
      return false;
    break;
  case ComponentAbi2OperationState::Armed:
    if (event == ComponentAbi2OperationEvent::Commit)
      next = ComponentAbi2OperationState::Committed;
    else if (event == ComponentAbi2OperationEvent::Fail)
      next = ComponentAbi2OperationState::Failed;
    else if (event == ComponentAbi2OperationEvent::Cancel)
      next = ComponentAbi2OperationState::Cancelled;
    else
      return false;
    break;
  case ComponentAbi2OperationState::Committed:
  case ComponentAbi2OperationState::Failed:
  case ComponentAbi2OperationState::Cancelled:
    if (event != ComponentAbi2OperationEvent::Release)
      return false;
    next = ComponentAbi2OperationState::Released;
    break;
  case ComponentAbi2OperationState::Released:
    return false;
  }
  state = next;
  return true;
}

bool componentAbi2LeaseIsClosed(ComponentAbi2LeaseState state) {
  return state == ComponentAbi2LeaseState::Closed;
}

bool componentAbi2AdvanceLease(ComponentAbi2LeaseState &state,
                               ComponentAbi2LeaseEvent event) {
  ComponentAbi2LeaseState next = state;
  switch (state) {
  case ComponentAbi2LeaseState::Available:
    if (event == ComponentAbi2LeaseEvent::Acquire)
      next = ComponentAbi2LeaseState::Leased;
    else if (event == ComponentAbi2LeaseEvent::BeginClose)
      next = ComponentAbi2LeaseState::Closing;
    else
      return false;
    break;
  case ComponentAbi2LeaseState::Leased:
    if (event == ComponentAbi2LeaseEvent::Release)
      next = ComponentAbi2LeaseState::Available;
    else if (event == ComponentAbi2LeaseEvent::BeginClose)
      next = ComponentAbi2LeaseState::Closing;
    else
      return false;
    break;
  case ComponentAbi2LeaseState::Closing:
    if (event != ComponentAbi2LeaseEvent::Quiesce)
      return false;
    next = ComponentAbi2LeaseState::Closed;
    break;
  case ComponentAbi2LeaseState::Closed:
    return false;
  }
  state = next;
  return true;
}

bool componentAbi2LeaseAcquire(ComponentAbi2ResourceLease &lease) {
  if (lease.state != ComponentAbi2LeaseState::Available ||
      lease.active_operations == std::numeric_limits<std::uint32_t>::max())
    return false;
  ++lease.active_operations;
  lease.state = ComponentAbi2LeaseState::Leased;
  return true;
}

bool componentAbi2LeaseRelease(ComponentAbi2ResourceLease &lease) {
  if (lease.state == ComponentAbi2LeaseState::Closed ||
      lease.active_operations == 0)
    return false;
  --lease.active_operations;
  if (lease.state == ComponentAbi2LeaseState::Leased &&
      lease.active_operations == 0)
    lease.state = ComponentAbi2LeaseState::Available;
  return true;
}

bool componentAbi2LeaseBeginClose(ComponentAbi2ResourceLease &lease) {
  if (lease.state == ComponentAbi2LeaseState::Closing ||
      lease.state == ComponentAbi2LeaseState::Closed)
    return false;
  lease.state = ComponentAbi2LeaseState::Closing;
  return true;
}

bool componentAbi2LeaseQuiesce(ComponentAbi2ResourceLease &lease) {
  if (lease.state != ComponentAbi2LeaseState::Closing ||
      lease.active_operations != 0)
    return false;
  lease.state = ComponentAbi2LeaseState::Closed;
  return true;
}

} // namespace chtholly::compiler
