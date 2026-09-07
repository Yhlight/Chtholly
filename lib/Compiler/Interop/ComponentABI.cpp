#include "chtholly/Compiler/ComponentABI.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

namespace chtholly::compiler {
static_assert(static_cast<std::uint8_t>(ComponentValueKind::Void) == 0);
static_assert(static_cast<std::uint8_t>(ComponentValueKind::Bytes) == 12);
namespace {

constexpr std::string_view Magic = "CHNXCMP1";
constexpr std::size_t MaxBytes = 1024 * 1024;
constexpr std::size_t MaxExports = 1024;
constexpr std::size_t MaxParameters = 64;

bool validText(std::string_view value) {
  return !value.empty() && value.find_first_of("\t\r\n") == value.npos;
}

std::optional<StableFingerprint> parseFingerprint(std::string_view text) {
  if (text.size() != StableFingerprint::ByteCount * 2)
    return std::nullopt;
  std::array<std::uint8_t, StableFingerprint::ByteCount> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    unsigned value = 0;
    const auto part = text.substr(index * 2, 2);
    const auto parsed =
        std::from_chars(part.data(), part.data() + part.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
      return std::nullopt;
    bytes[index] = static_cast<std::uint8_t>(value);
  }
  return StableFingerprint(bytes);
}

std::vector<std::string_view> fields(std::string_view line) {
  std::vector<std::string_view> result;
  while (true) {
    const auto split = line.find('\t');
    result.push_back(line.substr(0, split));
    if (split == line.npos)
      return result;
    line.remove_prefix(split + 1);
  }
}

std::string signatureBytes(std::span<const ComponentValueKind> parameters,
                           ComponentValueKind result) {
  std::string value = "chtholly.component.signature.v1\n";
  value += std::to_string(static_cast<unsigned>(result)) + "\n";
  for (const auto parameter : parameters)
    value += std::to_string(static_cast<unsigned>(parameter)) + "\n";
  return value;
}

} // namespace

std::optional<ComponentValueKind> componentValueKind(const PublicType &type,
                                                     bool result) {
  switch (type.kind) {
  case PublicTypeKind::Void:
    return result ? std::optional(ComponentValueKind::Void) : std::nullopt;
  case PublicTypeKind::Bool:
    return ComponentValueKind::Bool;
  case PublicTypeKind::Integer:
    if (type.scalar_width != 8 && type.scalar_width != 16 &&
        type.scalar_width != 32 && type.scalar_width != 64)
      return std::nullopt;
    if (type.integer_signed)
      return type.scalar_width == 8    ? ComponentValueKind::I8
             : type.scalar_width == 16 ? ComponentValueKind::I16
             : type.scalar_width == 32 ? ComponentValueKind::I32
                                       : ComponentValueKind::I64;
    return type.scalar_width == 8    ? ComponentValueKind::U8
           : type.scalar_width == 16 ? ComponentValueKind::U16
           : type.scalar_width == 32 ? ComponentValueKind::U32
                                     : ComponentValueKind::U64;
  case PublicTypeKind::Float:
    return type.scalar_width == 32   ? std::optional(ComponentValueKind::F32)
           : type.scalar_width == 64 ? std::optional(ComponentValueKind::F64)
                                     : std::nullopt;
  case PublicTypeKind::Slice:
    if (result || type.slice_mutable || type.arguments.size() != 1)
      return std::nullopt;
    if (const auto &element = type.arguments.front();
        element.kind == PublicTypeKind::Integer && element.scalar_width == 8 &&
        !element.integer_signed)
      return ComponentValueKind::Bytes;
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

StableFingerprint componentExportId(std::string_view identity,
                                    std::string_view canonical_name) {
  return StableFingerprint::fromCanonicalBytes(
      "chtholly.component.export.v1\n" + std::string(identity) + "\n" +
      std::string(canonical_name));
}

StableFingerprint
componentSignatureDigest(std::span<const ComponentValueKind> parameters,
                         ComponentValueKind result) {
  return StableFingerprint::fromCanonicalBytes(
      signatureBytes(parameters, result));
}

StableFingerprint
componentContractDigest(std::string_view identity,
                        std::span<const ComponentExportArtifact> exports) {
  std::string bytes = "chtholly.component.contract.v1\n";
  bytes += std::string(identity) + "\n";
  for (const auto &entry : exports)
    bytes += entry.export_id.hex() + "\n" + entry.signature_digest.hex() +
             "\n" + entry.canonical_name + "\n";
  return StableFingerprint::fromCanonicalBytes(bytes);
}

std::string componentWrapperSymbol(const StableFingerprint &id) {
  return "__chtholly_component_export_v1_" + id.hex();
}

void ComponentContractArtifact::canonicalize() {
  identity_digest = StableFingerprint::fromCanonicalBytes(
      "chtholly.component.identity.v1\n" + identity);
  for (auto &entry : exports) {
    entry.export_id = componentExportId(identity, entry.canonical_name);
    entry.signature_digest =
        componentSignatureDigest(entry.parameters, entry.result);
  }
  std::ranges::sort(exports, [](const auto &left, const auto &right) {
    return left.export_id.hex() < right.export_id.hex();
  });
  contract_digest = componentContractDigest(identity, exports);
}

bool ComponentContractArtifact::verify(std::string &error) const {
  error.clear();
  if (abi_epoch != ComponentAbiEpoch || !validText(identity) ||
      exports.empty() || exports.size() > MaxExports) {
    error = "component contract has invalid epoch, identity, or export count";
    return false;
  }
  ComponentContractArtifact expected = *this;
  expected.canonicalize();
  if (expected.identity_digest != identity_digest ||
      expected.contract_digest != contract_digest ||
      expected.exports != exports) {
    error = "component contract identity, ordering, or digest mismatch";
    return false;
  }
  for (std::size_t index = 0; index < exports.size(); ++index) {
    const auto &entry = exports[index];
    if (!validText(entry.canonical_name) ||
        entry.result >= ComponentValueKind::Count ||
        entry.parameters.size() > MaxParameters ||
        (index != 0 &&
         exports[index - 1].export_id.hex() >= entry.export_id.hex()) ||
        std::ranges::any_of(entry.parameters, [](auto kind) {
          return kind == ComponentValueKind::Void ||
                 kind >= ComponentValueKind::Count;
        })) {
      error = "component contract contains an invalid export signature";
      return false;
    }
  }
  return true;
}

std::string ComponentContractArtifact::encode(std::string &error) const {
  if (!verify(error))
    return {};
  std::ostringstream out;
  out << Magic << '\n'
      << "epoch\t" << abi_epoch << '\n'
      << "identity\t" << identity << '\n'
      << "identity-digest\t" << identity_digest.hex() << '\n'
      << "contract-digest\t" << contract_digest.hex() << '\n';
  for (const auto &entry : exports) {
    out << "export\t" << entry.canonical_name << '\t' << entry.export_id.hex()
        << '\t' << entry.signature_digest.hex() << '\t'
        << static_cast<unsigned>(entry.result);
    for (const auto parameter : entry.parameters)
      out << '\t' << static_cast<unsigned>(parameter);
    out << '\n';
  }
  return out.str();
}

std::optional<ComponentContractArtifact>
ComponentContractArtifact::decode(std::string_view bytes, std::string &error) {
  error.clear();
  if (bytes.size() > MaxBytes || !bytes.starts_with(Magic) ||
      (bytes.size() > Magic.size() && bytes[Magic.size()] != '\n')) {
    error = "component contract has invalid magic or exceeds its budget";
    return std::nullopt;
  }
  ComponentContractArtifact result;
  bool epoch = false, identity = false, identity_digest = false,
       contract_digest = false;
  bool malformed = false;
  bytes.remove_prefix(std::min(bytes.size(), Magic.size() + 1));
  while (!bytes.empty()) {
    const auto end = bytes.find('\n');
    const auto line = bytes.substr(0, end);
    bytes.remove_prefix(end == bytes.npos ? bytes.size() : end + 1);
    if (line.empty())
      continue;
    const auto values = fields(line);
    if (values.size() == 2 && values[0] == "epoch") {
      unsigned parsed = 0;
      const auto conversion = std::from_chars(
          values[1].data(), values[1].data() + values[1].size(), parsed);
      if (epoch || conversion.ec != std::errc{} ||
          conversion.ptr != values[1].data() + values[1].size()) {
        malformed = true;
        break;
      }
      result.abi_epoch = parsed;
      epoch = true;
    } else if (values.size() == 2 && values[0] == "identity") {
      if (identity) {
        malformed = true;
        break;
      }
      result.identity = values[1];
      identity = true;
    } else if (values.size() == 2 && values[0] == "identity-digest") {
      const auto parsed = parseFingerprint(values[1]);
      if (identity_digest || !parsed) {
        malformed = true;
        break;
      }
      result.identity_digest = *parsed;
      identity_digest = true;
    } else if (values.size() == 2 && values[0] == "contract-digest") {
      const auto parsed = parseFingerprint(values[1]);
      if (contract_digest || !parsed) {
        malformed = true;
        break;
      }
      result.contract_digest = *parsed;
      contract_digest = true;
    } else if (values.size() >= 5 && values[0] == "export") {
      ComponentExportArtifact entry;
      entry.canonical_name = values[1];
      const auto export_id = parseFingerprint(values[2]);
      const auto signature = parseFingerprint(values[3]);
      unsigned result_kind = 0;
      const auto conversion = std::from_chars(
          values[4].data(), values[4].data() + values[4].size(), result_kind);
      if (!export_id || !signature || conversion.ec != std::errc{} ||
          conversion.ptr != values[4].data() + values[4].size() ||
          result_kind >= static_cast<unsigned>(ComponentValueKind::Count)) {
        malformed = true;
        break;
      }
      entry.export_id = *export_id;
      entry.signature_digest = *signature;
      entry.result = static_cast<ComponentValueKind>(result_kind);
      bool valid = true;
      for (std::size_t index = 5; index < values.size(); ++index) {
        unsigned kind = 0;
        const auto parsed =
            std::from_chars(values[index].data(),
                            values[index].data() + values[index].size(), kind);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != values[index].data() + values[index].size() ||
            kind >= static_cast<unsigned>(ComponentValueKind::Count)) {
          valid = false;
          break;
        }
        entry.parameters.push_back(static_cast<ComponentValueKind>(kind));
      }
      if (!valid || result.exports.size() >= MaxExports) {
        malformed = true;
        break;
      }
      result.exports.push_back(std::move(entry));
    } else {
      malformed = true;
      break;
    }
  }
  if (malformed || !epoch || !identity || !identity_digest ||
      !contract_digest || !bytes.empty() || !result.verify(error)) {
    if (error.empty())
      error = "component contract contains malformed or duplicate records";
    return std::nullopt;
  }
  return result;
}

} // namespace chtholly::compiler
