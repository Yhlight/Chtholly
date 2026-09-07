#include "chtholly/Driver/RegistryTransparency.h"

#include "RegistryCrypto.h"
#include "chtholly/Support/Digest.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <sstream>

namespace chtholly {
namespace {

bool isHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::optional<std::string> digestBytes(std::string_view hex) {
  if (!isHexDigest(hex))
    return std::nullopt;
  std::string bytes(32, '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto digit = [](char ch) -> unsigned char {
      return ch <= '9' ? static_cast<unsigned char>(ch - '0')
                       : static_cast<unsigned char>(ch - 'a' + 10);
    };
    bytes[i] =
        static_cast<char>((digit(hex[i * 2]) << 4U) | digit(hex[i * 2 + 1]));
  }
  return bytes;
}

std::string nodeHash(std::string_view left, std::string_view right) {
  auto left_bytes = digestBytes(left);
  auto right_bytes = digestBytes(right);
  if (!left_bytes || !right_bytes)
    return {};
  std::string input(1, '\x01');
  input += *left_bytes;
  input += *right_bytes;
  return sha256Hex(input);
}

std::uint64_t largestPowerOfTwoLessThan(std::uint64_t value) {
  std::uint64_t result = 1;
  while (result <= (value - 1) / 2)
    result *= 2;
  return result;
}

std::string merkleRange(const std::vector<std::string> &leaves,
                        std::uint64_t begin, std::uint64_t count) {
  if (count == 0)
    return sha256Hex("");
  if (count == 1)
    return leaves[static_cast<std::size_t>(begin)];
  const auto split = largestPowerOfTwoLessThan(count);
  return nodeHash(merkleRange(leaves, begin, split),
                  merkleRange(leaves, begin + split, count - split));
}

void inclusionRange(const std::vector<std::string> &leaves, std::uint64_t begin,
                    std::uint64_t count, std::uint64_t index,
                    std::vector<std::string> &proof) {
  if (count <= 1)
    return;
  const auto split = largestPowerOfTwoLessThan(count);
  if (index < split) {
    inclusionRange(leaves, begin, split, index, proof);
    proof.push_back(merkleRange(leaves, begin + split, count - split));
  } else {
    inclusionRange(leaves, begin + split, count - split, index - split, proof);
    proof.push_back(merkleRange(leaves, begin, split));
  }
}

void consistencyRange(const std::vector<std::string> &leaves,
                      std::uint64_t begin, std::uint64_t old_size,
                      std::uint64_t new_size, bool complete,
                      std::vector<std::string> &proof) {
  if (old_size == new_size) {
    if (!complete)
      proof.push_back(merkleRange(leaves, begin, new_size));
    return;
  }
  const auto split = largestPowerOfTwoLessThan(new_size);
  if (old_size <= split) {
    consistencyRange(leaves, begin, old_size, split, complete, proof);
    proof.push_back(merkleRange(leaves, begin + split, new_size - split));
  } else {
    consistencyRange(leaves, begin + split, old_size - split, new_size - split,
                     false, proof);
    proof.push_back(merkleRange(leaves, begin, split));
  }
}

void appendField(std::ostringstream &out, std::string_view name,
                 std::string_view value) {
  out << name << '\t' << value.size() << ':' << value << '\n';
}

std::string canonicalCheckpoint(const RegistryAuditCheckpoint &checkpoint) {
  std::ostringstream out;
  out << "chtholly-registry-audit-checkpoint-v1\n";
  appendField(out, "registry", checkpoint.registry_name);
  appendField(out, "tree-size", std::to_string(checkpoint.tree_size));
  appendField(out, "root-hash", checkpoint.root_hash);
  appendField(out, "root-version", std::to_string(checkpoint.root_version));
  appendField(out, "root-sha256", checkpoint.root_sha256);
  appendField(out, "issued-at", checkpoint.issued_at);
  return out.str();
}

std::string canonicalLeaf(const RegistryAuditReceipt &receipt) {
  std::ostringstream out;
  out << "chtholly-registry-audit-leaf-v1\n";
  appendField(out, "event", receipt.event_kind);
  appendField(out, "registry", receipt.registry_name);
  appendField(out, "package", receipt.package_name);
  appendField(out, "version", receipt.version);
  appendField(out, "variant", receipt.variant_name);
  appendField(out, "archive-sha256", receipt.archive_sha256);
  appendField(out, "entry-sha256", receipt.entry_sha256);
  appendField(out, "publisher", receipt.publisher_principal);
  appendField(out, "accepted-at", receipt.accepted_at);
  appendField(out, "root-version",
              std::to_string(receipt.checkpoint.root_version));
  appendField(out, "root-sha256", receipt.checkpoint.root_sha256);
  appendField(out, "targets-threshold",
              std::to_string(receipt.targets_threshold));
  auto signers = receipt.valid_target_signer_ids;
  std::sort(signers.begin(), signers.end());
  out << "target-signer-count\t" << signers.size() << '\n';
  for (const auto &signer : signers)
    appendField(out, "target-signer", signer);
  return out.str();
}

std::string canonicalLeaf(const RegistryLifecycleReceipt &receipt) {
  std::ostringstream out;
  out << "chtholly-registry-lifecycle-leaf-v1\n";
  appendField(out, "registry", receipt.registry_name);
  appendField(out, "package", receipt.package_name);
  appendField(out, "version", receipt.version);
  appendField(out, "state",
              receipt.state == RegistryReleaseState::Yanked ? "yanked"
                                                            : "active");
  appendField(out, "previous-state-leaf-index",
              std::to_string(receipt.previous_state_leaf_index));
  appendField(out, "previous-state-leaf-hash",
              receipt.previous_state_leaf_hash);
  appendField(out, "actor-type", receipt.actor_type);
  appendField(out, "actor-principal", receipt.actor_principal);
  appendField(out, "accepted-at", receipt.accepted_at);
  appendField(out, "reason", receipt.reason_code);
  appendField(out, "root-version",
              std::to_string(receipt.checkpoint.root_version));
  appendField(out, "root-sha256", receipt.checkpoint.root_sha256);
  return out.str();
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

bool verifyCheckpointSignatures(const RegistryAuditCheckpoint &checkpoint,
                                const RegistryRootMetadata &root,
                                std::string &error) {
  if (root.format != RegistryRootFormatV2 || root.audit.threshold == 0) {
    error = "registry root does not authorize an audit role";
    return false;
  }
  if (!registry_crypto::initialize(error))
    return false;
  std::set<std::string> revoked(root.revoked_key_ids.begin(),
                                root.revoked_key_ids.end());
  std::set<std::string> accepted;
  const auto payload = canonicalCheckpoint(checkpoint);
  for (const auto &key_text : root.audit.keys) {
    auto key = registry_crypto::parsePublicKey(key_text);
    if (!key)
      continue;
    const auto id = registry_crypto::publicKeyId(*key);
    if (revoked.contains(id))
      continue;
    const auto found = std::find_if(
        checkpoint.signatures.begin(), checkpoint.signatures.end(),
        [&](const auto &candidate) { return candidate.key_id == id; });
    if (found == checkpoint.signatures.end())
      continue;
    registry_crypto::Signature signature{};
    if (!registry_crypto::decodeSignature(found->signature, signature))
      continue;
    if (crypto_sign_verify_detached(
            signature.data(),
            reinterpret_cast<const unsigned char *>(payload.data()),
            static_cast<unsigned long long>(payload.size()),
            key->data()) == 0) {
      accepted.insert(id);
    }
  }
  if (accepted.size() < root.audit.threshold) {
    error =
        "registry audit checkpoint signature threshold was not satisfied (" +
        std::to_string(accepted.size()) + "/" +
        std::to_string(root.audit.threshold) + ")";
    return false;
  }
  return true;
}

} // namespace

std::string registryAuditLeafHash(std::string_view canonical_leaf) {
  std::string input(1, '\0');
  input += canonical_leaf;
  return sha256Hex(input);
}

std::string registryPublishMutationId(std::string_view registry_name,
                                      const SignedRegistryEntry &entry,
                                      const RegistryArtifactVariant &variant) {
  const auto entry_digest = sha256Hex(renderSignedRegistryEntry(entry));
  std::ostringstream out;
  out << "chtholly-registry-publish-mutation-v1\n";
  appendField(out, "registry", registry_name);
  appendField(out, "package", entry.package_name);
  appendField(out, "version", entry.version);
  appendField(out, "variant", variant.name);
  appendField(out, "archive-sha256", variant.archive_sha256);
  appendField(out, "entry-sha256", entry_digest);
  return "sha256:" + sha256Hex(out.str());
}

std::string registryAuditCanonicalLeaf(const RegistryAuditReceipt &receipt) {
  return canonicalLeaf(receipt);
}

std::string registryAuditCheckpointSignatureInput(
    const RegistryAuditCheckpoint &checkpoint) {
  return canonicalCheckpoint(checkpoint);
}

std::string registryMerkleRoot(const std::vector<std::string> &leaf_hashes) {
  if (!std::all_of(leaf_hashes.begin(), leaf_hashes.end(), isHexDigest))
    return {};
  return merkleRange(leaf_hashes, 0,
                     static_cast<std::uint64_t>(leaf_hashes.size()));
}

std::vector<std::string>
registryMerkleInclusionProof(const std::vector<std::string> &leaf_hashes,
                             std::uint64_t leaf_index) {
  std::vector<std::string> proof;
  if (leaf_index >= leaf_hashes.size() ||
      !std::all_of(leaf_hashes.begin(), leaf_hashes.end(), isHexDigest)) {
    return proof;
  }
  inclusionRange(leaf_hashes, 0, static_cast<std::uint64_t>(leaf_hashes.size()),
                 leaf_index, proof);
  return proof;
}

bool verifyRegistryMerkleInclusion(std::string_view leaf_hash,
                                   std::uint64_t leaf_index,
                                   std::uint64_t tree_size,
                                   const std::vector<std::string> &proof,
                                   std::string_view expected_root) {
  if (!isHexDigest(leaf_hash) || !isHexDigest(expected_root) ||
      tree_size == 0 || leaf_index >= tree_size ||
      !std::all_of(proof.begin(), proof.end(), isHexDigest))
    return false;
  std::size_t proof_index = 0;
  const auto rebuild = [&](const auto &self, std::uint64_t index,
                           std::uint64_t count) -> std::optional<std::string> {
    if (count == 1)
      return std::string(leaf_hash);
    const auto split = largestPowerOfTwoLessThan(count);
    auto child = index < split ? self(self, index, split)
                               : self(self, index - split, count - split);
    if (!child || proof_index >= proof.size())
      return std::nullopt;
    const auto &sibling = proof[proof_index++];
    return index < split ? nodeHash(*child, sibling)
                         : nodeHash(sibling, *child);
  };
  const auto rebuilt = rebuild(rebuild, leaf_index, tree_size);
  return rebuilt && proof_index == proof.size() && *rebuilt == expected_root;
}

std::vector<std::string>
registryMerkleConsistencyProof(const std::vector<std::string> &leaf_hashes,
                               std::uint64_t old_tree_size) {
  std::vector<std::string> proof;
  const auto new_size = static_cast<std::uint64_t>(leaf_hashes.size());
  if (old_tree_size == 0 || old_tree_size > new_size ||
      !std::all_of(leaf_hashes.begin(), leaf_hashes.end(), isHexDigest)) {
    return proof;
  }
  if (old_tree_size != new_size) {
    consistencyRange(leaf_hashes, 0, old_tree_size, new_size, true, proof);
  }
  return proof;
}

bool verifyRegistryMerkleConsistency(std::uint64_t old_tree_size,
                                     std::uint64_t new_tree_size,
                                     std::string_view old_root,
                                     std::string_view new_root,
                                     const std::vector<std::string> &proof) {
  if (old_tree_size == 0 || old_tree_size > new_tree_size ||
      !isHexDigest(old_root) || !isHexDigest(new_root) ||
      !std::all_of(proof.begin(), proof.end(), isHexDigest))
    return false;
  if (old_tree_size == new_tree_size)
    return proof.empty() && old_root == new_root;
  std::uint64_t fn = old_tree_size - 1;
  std::uint64_t sn = new_tree_size - 1;
  while ((fn & 1U) != 0U) {
    fn >>= 1U;
    sn >>= 1U;
  }
  std::size_t proof_index = 0;
  std::string first;
  std::string second;
  if (fn == 0) {
    first = std::string(old_root);
    second = first;
  } else {
    if (proof.empty())
      return false;
    first = proof.front();
    second = first;
    proof_index = 1;
  }
  for (; proof_index < proof.size(); ++proof_index) {
    if (sn == 0)
      return false;
    const auto &hash = proof[proof_index];
    if ((fn & 1U) != 0U || fn == sn) {
      first = nodeHash(hash, first);
      second = nodeHash(hash, second);
      while (fn != 0 && (fn & 1U) == 0U) {
        fn >>= 1U;
        sn >>= 1U;
      }
    } else {
      second = nodeHash(second, hash);
    }
    fn >>= 1U;
    sn >>= 1U;
  }
  return sn == 0 && first == old_root && second == new_root;
}

std::string
renderRegistryAuditCheckpoint(const RegistryAuditCheckpoint &checkpoint) {
  std::ostringstream out;
  out << canonicalCheckpoint(checkpoint);
  auto signatures = checkpoint.signatures;
  std::sort(
      signatures.begin(), signatures.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.key_id < rhs.key_id; });
  for (const auto &signature : signatures) {
    out << "signature\t" << signature.key_id << '\t' << signature.signature
        << '\n';
  }
  return out.str();
}

std::optional<RegistryAuditCheckpoint>
parseRegistryAuditCheckpoint(std::string_view text, std::string &error) {
  if (text.size() > 128u * 1024u) {
    error = "registry audit checkpoint exceeds its size limit";
    return std::nullopt;
  }
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) ||
      line != "chtholly-registry-audit-checkpoint-v1") {
    error = "registry audit checkpoint has an invalid format";
    return std::nullopt;
  }
  const std::array<std::string_view, 6> expected{"registry",    "tree-size",
                                                 "root-hash",   "root-version",
                                                 "root-sha256", "issued-at"};
  std::map<std::string, std::string> fields;
  RegistryAuditCheckpoint checkpoint;
  std::size_t field_index = 0;
  std::string previous_key;
  while (std::getline(input, line)) {
    if (line.empty()) {
      error = "registry audit checkpoint contains an empty field";
      return std::nullopt;
    }
    if (line.starts_with("signature\t")) {
      const auto key_end = line.find('\t', 10);
      if (field_index != expected.size() || key_end == std::string::npos ||
          key_end == 10 || key_end + 1 == line.size()) {
        error = "registry audit checkpoint signature is malformed";
        return std::nullopt;
      }
      RegistryMetadataSignature signature{line.substr(10, key_end - 10),
                                          line.substr(key_end + 1)};
      if ((!previous_key.empty() && previous_key >= signature.key_id) ||
          checkpoint.signatures.size() >= 64) {
        error =
            "registry audit checkpoint signatures are not unique and ordered";
        return std::nullopt;
      }
      previous_key = signature.key_id;
      checkpoint.signatures.push_back(std::move(signature));
      continue;
    }
    if (field_index >= expected.size()) {
      error = "registry audit checkpoint contains an unknown field";
      return std::nullopt;
    }
    const auto tab = line.find('\t');
    const auto colon =
        tab == std::string::npos ? std::string::npos : line.find(':', tab + 1);
    std::size_t size = 0;
    if (tab == std::string::npos || colon == std::string::npos ||
        line.substr(0, tab) != expected[field_index] ||
        !parseUnsigned(std::string_view(line).substr(tab + 1, colon - tab - 1))
             .has_value()) {
      error = "registry audit checkpoint field is malformed";
      return std::nullopt;
    }
    const auto parsed_size =
        parseUnsigned(std::string_view(line).substr(tab + 1, colon - tab - 1));
    if (!parsed_size || *parsed_size > SIZE_MAX ||
        line.size() - colon - 1 != *parsed_size) {
      error = "registry audit checkpoint field length is invalid";
      return std::nullopt;
    }
    size = static_cast<std::size_t>(*parsed_size);
    fields.emplace(std::string(expected[field_index]),
                   line.substr(colon + 1, size));
    ++field_index;
  }
  const auto tree_size = fields.contains("tree-size")
                             ? parseUnsigned(fields["tree-size"])
                             : std::nullopt;
  const auto root_version = fields.contains("root-version")
                                ? parseUnsigned(fields["root-version"])
                                : std::nullopt;
  std::int64_t issued_at = 0;
  if (field_index != expected.size() || checkpoint.signatures.empty() ||
      fields["registry"].empty() || !tree_size || *tree_size == 0 ||
      !root_version || *root_version == 0 ||
      !isHexDigest(fields["root-hash"]) ||
      !isHexDigest(fields["root-sha256"]) ||
      !parseRegistryUtcTimestamp(fields["issued-at"], issued_at)) {
    error = "registry audit checkpoint values are invalid";
    return std::nullopt;
  }
  checkpoint.registry_name = std::move(fields["registry"]);
  checkpoint.tree_size = *tree_size;
  checkpoint.root_hash = std::move(fields["root-hash"]);
  checkpoint.root_version = *root_version;
  checkpoint.root_sha256 = std::move(fields["root-sha256"]);
  checkpoint.issued_at = std::move(fields["issued-at"]);
  return checkpoint;
}

std::string renderRegistryAuditReceipt(const RegistryAuditReceipt &receipt) {
  std::ostringstream out;
  out << RegistryAuditReceiptFormat << '\n'
      << "registry\t" << receipt.registry_name << '\n'
      << "package\t" << receipt.package_name << '\n'
      << "version\t" << receipt.version << '\n'
      << "variant\t" << receipt.variant_name << '\n'
      << "archive-sha256\t" << receipt.archive_sha256 << '\n'
      << "entry-sha256\t" << receipt.entry_sha256 << '\n'
      << "event\t" << receipt.event_kind << '\n'
      << "publisher\t" << receipt.publisher_principal << '\n'
      << "accepted-at\t" << receipt.accepted_at << '\n'
      << "targets-threshold\t" << receipt.targets_threshold << '\n';
  auto signers = receipt.valid_target_signer_ids;
  std::sort(signers.begin(), signers.end());
  for (const auto &signer : signers)
    out << "target-signer\t" << signer << '\n';
  out << "leaf-index\t" << receipt.leaf_index << '\n'
      << "leaf-hash\t" << receipt.leaf_hash << '\n'
      << "tree-size\t" << receipt.checkpoint.tree_size << '\n'
      << "tree-root\t" << receipt.checkpoint.root_hash << '\n'
      << "root-version\t" << receipt.checkpoint.root_version << '\n'
      << "root-sha256\t" << receipt.checkpoint.root_sha256 << '\n'
      << "checkpoint-issued-at\t" << receipt.checkpoint.issued_at << '\n';
  auto signatures = receipt.checkpoint.signatures;
  std::sort(
      signatures.begin(), signatures.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.key_id < rhs.key_id; });
  for (const auto &signature : signatures) {
    out << "audit-signature\t" << signature.key_id << '\t'
        << signature.signature << '\n';
  }
  for (const auto &hash : receipt.inclusion_proof)
    out << "inclusion\t" << hash << '\n';
  return out.str();
}

std::optional<RegistryAuditReceipt>
parseRegistryAuditReceipt(std::string_view text, std::string &error) {
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != RegistryAuditReceiptFormat) {
    error = "registry publish returned an invalid receipt format";
    return std::nullopt;
  }
  std::map<std::string, std::string> fields;
  RegistryAuditReceipt receipt;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto first = line.find('\t');
    if (first == std::string::npos || first == 0 || first + 1 == line.size()) {
      error = "registry publish returned a malformed receipt";
      return std::nullopt;
    }
    const auto name = line.substr(0, first);
    const auto value = line.substr(first + 1);
    if (name == "target-signer") {
      receipt.valid_target_signer_ids.push_back(value);
    } else if (name == "inclusion") {
      receipt.inclusion_proof.push_back(value);
    } else if (name == "audit-signature") {
      const auto split = value.find('\t');
      if (split == std::string::npos || split == 0 ||
          split + 1 == value.size()) {
        error = "registry publish returned a malformed audit signature";
        return std::nullopt;
      }
      receipt.checkpoint.signatures.push_back(
          {value.substr(0, split), value.substr(split + 1)});
    } else if (!fields.emplace(name, value).second) {
      error = "registry publish returned a duplicate receipt field";
      return std::nullopt;
    }
  }
  const std::set<std::string> required{"registry",
                                       "package",
                                       "version",
                                       "variant",
                                       "archive-sha256",
                                       "entry-sha256",
                                       "event",
                                       "publisher",
                                       "accepted-at",
                                       "targets-threshold",
                                       "leaf-index",
                                       "leaf-hash",
                                       "tree-size",
                                       "tree-root",
                                       "root-version",
                                       "root-sha256",
                                       "checkpoint-issued-at"};
  if (!std::all_of(required.begin(), required.end(),
                   [&](const auto &name) { return fields.contains(name); }) ||
      fields.size() != required.size()) {
    error = "registry publish receipt is incomplete";
    return std::nullopt;
  }
  const auto threshold = parseUnsigned(fields["targets-threshold"]);
  const auto leaf_index = parseUnsigned(fields["leaf-index"]);
  const auto tree_size = parseUnsigned(fields["tree-size"]);
  const auto root_version = parseUnsigned(fields["root-version"]);
  if (!threshold || *threshold == 0 || *threshold > UINT32_MAX || !leaf_index ||
      !tree_size || *tree_size == 0 || !root_version || *root_version == 0) {
    error = "registry publish receipt contains invalid numeric fields";
    return std::nullopt;
  }
  receipt.registry_name = fields["registry"];
  receipt.package_name = fields["package"];
  receipt.version = fields["version"];
  receipt.variant_name = fields["variant"];
  receipt.archive_sha256 = fields["archive-sha256"];
  receipt.entry_sha256 = fields["entry-sha256"];
  receipt.event_kind = fields["event"];
  receipt.publisher_principal = fields["publisher"];
  receipt.accepted_at = fields["accepted-at"];
  receipt.targets_threshold = static_cast<std::uint32_t>(*threshold);
  receipt.leaf_index = *leaf_index;
  receipt.leaf_hash = fields["leaf-hash"];
  receipt.checkpoint.registry_name = receipt.registry_name;
  receipt.checkpoint.tree_size = *tree_size;
  receipt.checkpoint.root_hash = fields["tree-root"];
  receipt.checkpoint.root_version = *root_version;
  receipt.checkpoint.root_sha256 = fields["root-sha256"];
  receipt.checkpoint.issued_at = fields["checkpoint-issued-at"];
  std::sort(receipt.valid_target_signer_ids.begin(),
            receipt.valid_target_signer_ids.end());
  std::sort(
      receipt.checkpoint.signatures.begin(),
      receipt.checkpoint.signatures.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.key_id < rhs.key_id; });
  const auto duplicate_strings = [](const auto &values) {
    return std::adjacent_find(values.begin(), values.end()) != values.end();
  };
  if (duplicate_strings(receipt.valid_target_signer_ids) ||
      std::adjacent_find(receipt.checkpoint.signatures.begin(),
                         receipt.checkpoint.signatures.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.key_id == rhs.key_id;
                         }) != receipt.checkpoint.signatures.end()) {
    error = "registry publish receipt contains duplicate signers";
    return std::nullopt;
  }
  if ((receipt.event_kind != "publish" &&
       receipt.event_kind != "countersign") ||
      !isHexDigest(receipt.archive_sha256) ||
      !isHexDigest(receipt.entry_sha256) || !isHexDigest(receipt.leaf_hash) ||
      !isHexDigest(receipt.checkpoint.root_hash) ||
      !isHexDigest(receipt.checkpoint.root_sha256) ||
      receipt.leaf_index >= receipt.checkpoint.tree_size) {
    error = "registry publish receipt contains invalid audit facts";
    return std::nullopt;
  }
  return receipt;
}

bool verifyRegistryAuditReceipt(const RegistryAuditReceipt &receipt,
                                const RegistryRootMetadata &root,
                                std::string &error) {
  if (receipt.registry_name != root.registry_name ||
      receipt.checkpoint.registry_name != root.registry_name ||
      receipt.checkpoint.root_version != root.version ||
      receipt.checkpoint.root_sha256 !=
          sha256Hex(renderRegistryRootMetadata(root))) {
    error = "registry audit receipt does not match the verified root";
    return false;
  }
  if (receipt.targets_threshold == 0 ||
      receipt.targets_threshold != root.targets.threshold ||
      receipt.valid_target_signer_ids.size() < receipt.targets_threshold) {
    error = "registry audit receipt contains an invalid targets threshold";
    return false;
  }
  std::set<std::string> target_ids;
  for (const auto &key_text : root.targets.keys) {
    auto key = registry_crypto::parsePublicKey(key_text);
    if (key)
      target_ids.insert(registry_crypto::publicKeyId(*key));
  }
  if (!std::all_of(receipt.valid_target_signer_ids.begin(),
                   receipt.valid_target_signer_ids.end(),
                   [&](const auto &id) { return target_ids.contains(id); })) {
    error = "registry audit receipt names a signer outside the targets role";
    return false;
  }
  if (registryAuditLeafHash(canonicalLeaf(receipt)) != receipt.leaf_hash ||
      !verifyRegistryMerkleInclusion(
          receipt.leaf_hash, receipt.leaf_index, receipt.checkpoint.tree_size,
          receipt.inclusion_proof, receipt.checkpoint.root_hash)) {
    error = "registry audit receipt inclusion proof is invalid";
    return false;
  }
  return verifyCheckpointSignatures(receipt.checkpoint, root, error);
}

bool verifyRegistryAuditCheckpoint(const RegistryAuditCheckpoint &checkpoint,
                                   const RegistryRootMetadata &root,
                                   std::string &error) {
  if (checkpoint.registry_name != root.registry_name ||
      checkpoint.root_version != root.version ||
      checkpoint.root_sha256 != sha256Hex(renderRegistryRootMetadata(root))) {
    error = "registry audit checkpoint does not match the verified root";
    return false;
  }
  return verifyCheckpointSignatures(checkpoint, root, error);
}

std::string
registryLifecycleCanonicalLeaf(const RegistryLifecycleReceipt &receipt) {
  return canonicalLeaf(receipt);
}

std::string
renderRegistryLifecycleReceipt(const RegistryLifecycleReceipt &receipt) {
  std::ostringstream out;
  out << RegistryLifecycleReceiptFormat << '\n'
      << "registry\t" << receipt.registry_name << '\n'
      << "package\t" << receipt.package_name << '\n'
      << "version\t" << receipt.version << '\n'
      << "state\t"
      << (receipt.state == RegistryReleaseState::Yanked ? "yanked" : "active")
      << '\n'
      << "previous-state-leaf-index\t" << receipt.previous_state_leaf_index
      << '\n'
      << "previous-state-leaf-hash\t" << receipt.previous_state_leaf_hash
      << '\n'
      << "actor-type\t" << receipt.actor_type << '\n'
      << "actor-principal\t" << receipt.actor_principal << '\n'
      << "accepted-at\t" << receipt.accepted_at << '\n'
      << "reason\t" << receipt.reason_code << '\n'
      << "leaf-index\t" << receipt.leaf_index << '\n'
      << "leaf-hash\t" << receipt.leaf_hash << '\n'
      << "tree-size\t" << receipt.checkpoint.tree_size << '\n'
      << "tree-root\t" << receipt.checkpoint.root_hash << '\n'
      << "root-version\t" << receipt.checkpoint.root_version << '\n'
      << "root-sha256\t" << receipt.checkpoint.root_sha256 << '\n'
      << "checkpoint-issued-at\t" << receipt.checkpoint.issued_at << '\n';
  auto signatures = receipt.checkpoint.signatures;
  std::sort(signatures.begin(), signatures.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.key_id < rhs.key_id;
            });
  for (const auto &signature : signatures)
    out << "audit-signature\t" << signature.key_id << '\t'
        << signature.signature << '\n';
  for (const auto &hash : receipt.inclusion_proof)
    out << "inclusion\t" << hash << '\n';
  return out.str();
}

std::optional<RegistryLifecycleReceipt>
parseRegistryLifecycleReceipt(std::string_view text, std::string &error) {
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != RegistryLifecycleReceiptFormat) {
    error = "registry lifecycle response has an invalid receipt format";
    return std::nullopt;
  }
  std::map<std::string, std::string> fields;
  RegistryLifecycleReceipt receipt;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto first = line.find('\t');
    if (first == std::string::npos || first == 0 || first + 1 == line.size()) {
      error = "registry lifecycle response contains a malformed field";
      return std::nullopt;
    }
    const auto name = line.substr(0, first);
    const auto value = line.substr(first + 1);
    if (name == "inclusion") {
      receipt.inclusion_proof.push_back(value);
    } else if (name == "audit-signature") {
      const auto split = value.find('\t');
      if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
        error = "registry lifecycle receipt has a malformed audit signature";
        return std::nullopt;
      }
      receipt.checkpoint.signatures.push_back(
          {value.substr(0, split), value.substr(split + 1)});
    } else if (!fields.emplace(name, value).second) {
      error = "registry lifecycle receipt contains a duplicate field";
      return std::nullopt;
    }
  }
  const std::set<std::string> required{
      "registry", "package", "version", "state",
      "previous-state-leaf-index", "previous-state-leaf-hash", "actor-type",
      "actor-principal", "accepted-at", "reason", "leaf-index", "leaf-hash",
      "tree-size", "tree-root", "root-version", "root-sha256",
      "checkpoint-issued-at"};
  if (fields.size() != required.size() ||
      !std::all_of(required.begin(), required.end(),
                   [&](const auto &name) { return fields.contains(name); })) {
    error = "registry lifecycle receipt is incomplete";
    return std::nullopt;
  }
  const auto previous = parseUnsigned(fields["previous-state-leaf-index"]);
  const auto leaf = parseUnsigned(fields["leaf-index"]);
  const auto tree = parseUnsigned(fields["tree-size"]);
  const auto root_version = parseUnsigned(fields["root-version"]);
  std::int64_t accepted = 0;
  std::int64_t issued = 0;
  if (!previous || !leaf || !tree || *tree == 0 || !root_version ||
      *root_version == 0 || *leaf >= *tree || *previous >= *leaf ||
      (fields["state"] != "active" && fields["state"] != "yanked") ||
      (fields["actor-type"] != "publisher" &&
       fields["actor-type"] != "operator") ||
      fields["registry"].empty() || fields["package"].empty() ||
      fields["version"].empty() || fields["actor-principal"].empty() ||
      !isRegistryReleaseReasonCode(fields["reason"]) ||
      !isHexDigest(fields["previous-state-leaf-hash"]) ||
      !isHexDigest(fields["leaf-hash"]) || !isHexDigest(fields["tree-root"]) ||
      !isHexDigest(fields["root-sha256"]) ||
      !parseRegistryUtcTimestamp(fields["accepted-at"], accepted) ||
      !parseRegistryUtcTimestamp(fields["checkpoint-issued-at"], issued)) {
    error = "registry lifecycle receipt contains invalid facts";
    return std::nullopt;
  }
  receipt.registry_name = fields["registry"];
  receipt.package_name = fields["package"];
  receipt.version = fields["version"];
  receipt.state = fields["state"] == "yanked" ? RegistryReleaseState::Yanked
                                               : RegistryReleaseState::Active;
  receipt.previous_state_leaf_index = *previous;
  receipt.previous_state_leaf_hash = fields["previous-state-leaf-hash"];
  receipt.actor_type = fields["actor-type"];
  receipt.actor_principal = fields["actor-principal"];
  receipt.accepted_at = fields["accepted-at"];
  receipt.reason_code = fields["reason"];
  receipt.leaf_index = *leaf;
  receipt.leaf_hash = fields["leaf-hash"];
  receipt.checkpoint.registry_name = receipt.registry_name;
  receipt.checkpoint.tree_size = *tree;
  receipt.checkpoint.root_hash = fields["tree-root"];
  receipt.checkpoint.root_version = *root_version;
  receipt.checkpoint.root_sha256 = fields["root-sha256"];
  receipt.checkpoint.issued_at = fields["checkpoint-issued-at"];
  std::sort(receipt.checkpoint.signatures.begin(),
            receipt.checkpoint.signatures.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.key_id < rhs.key_id;
            });
  if (receipt.checkpoint.signatures.empty() ||
      std::adjacent_find(receipt.checkpoint.signatures.begin(),
                         receipt.checkpoint.signatures.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.key_id == rhs.key_id;
                         }) != receipt.checkpoint.signatures.end()) {
    error = "registry lifecycle receipt contains invalid audit signatures";
    return std::nullopt;
  }
  return receipt;
}

bool verifyRegistryLifecycleReceipt(const RegistryLifecycleReceipt &receipt,
                                    const RegistryRootMetadata &root,
                                    std::string &error) {
  std::int64_t accepted_at = 0;
  if (receipt.package_name.empty() || receipt.version.empty() ||
      (receipt.actor_type != "publisher" && receipt.actor_type != "operator") ||
      receipt.actor_principal.empty() ||
      !isRegistryReleaseReasonCode(receipt.reason_code) ||
      !isHexDigest(receipt.previous_state_leaf_hash) ||
      receipt.previous_state_leaf_index >= receipt.leaf_index ||
      receipt.leaf_index >= receipt.checkpoint.tree_size ||
      !parseRegistryUtcTimestamp(receipt.accepted_at, accepted_at)) {
    error = "registry lifecycle receipt contains invalid state facts";
    return false;
  }
  if (receipt.registry_name != root.registry_name ||
      receipt.checkpoint.registry_name != root.registry_name ||
      receipt.checkpoint.root_version != root.version ||
      receipt.checkpoint.root_sha256 !=
          sha256Hex(renderRegistryRootMetadata(root))) {
    error = "registry lifecycle receipt does not match the verified root";
    return false;
  }
  if (registryAuditLeafHash(canonicalLeaf(receipt)) != receipt.leaf_hash ||
      !verifyRegistryMerkleInclusion(
          receipt.leaf_hash, receipt.leaf_index, receipt.checkpoint.tree_size,
          receipt.inclusion_proof, receipt.checkpoint.root_hash)) {
    error = "registry lifecycle receipt inclusion proof is invalid";
    return false;
  }
  return verifyCheckpointSignatures(receipt.checkpoint, root, error);
}

} // namespace chtholly
