#include "chtholly/Driver/RegistryTransparency.h"

#include "chtholly/Driver/RegistryWitness.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace chtholly {
namespace {

struct HttpResponse {
  long status = 0;
  std::string body;
};

struct CurlBuffer {
  std::string text;
  std::size_t limit = 128 * 1024;
  bool exceeded = false;
};

std::size_t writeCurl(char *data, std::size_t item_size, std::size_t count,
                      void *context) {
  auto &buffer = *static_cast<CurlBuffer *>(context);
  if (item_size != 0 && count > SIZE_MAX / item_size) {
    buffer.exceeded = true;
    return 0;
  }
  const auto size = item_size * count;
  if (buffer.text.size() > buffer.limit ||
      size > buffer.limit - buffer.text.size()) {
    buffer.exceeded = true;
    return 0;
  }
  buffer.text.append(data, size);
  return size;
}

bool initializeCurl(std::string &error) {
  static std::once_flag once;
  static CURLcode result = CURLE_OK;
  std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (result != CURLE_OK) {
    error = "failed to initialize registry transparency HTTPS";
    return false;
  }
  return true;
}

std::optional<HttpResponse>
requestHttps(const std::string &url, const std::string &ca_bundle,
             const std::optional<std::string> &body, std::string &error) {
  if (!initializeCurl(error))
    return std::nullopt;
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "failed to initialize registry transparency request";
    return std::nullopt;
  }
  CurlBuffer buffer;
  curl_slist *headers = nullptr;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurl);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "chthollyc-registry-transparency/1");
  if (!ca_bundle.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
  if (body) {
    headers = curl_slist_append(headers, "Content-Type: text/plain");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body->size()));
  }
  const auto result = curl_easy_perform(curl);
  HttpResponse response;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  response.body = std::move(buffer.text);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || buffer.exceeded) {
    error = buffer.exceeded
                ? "registry transparency response exceeded its size limit"
                : "registry transparency HTTPS request failed: " +
                      std::string(curl_easy_strerror(result));
    return std::nullopt;
  }
  return response;
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::optional<std::vector<std::string>>
parseConsistencyProof(std::string_view text, std::uint64_t old_size,
                      std::uint64_t new_size, std::string &error) {
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) ||
      line != "chtholly-registry-audit-consistency-v1") {
    error = "registry returned an invalid consistency proof format";
    return std::nullopt;
  }
  bool first = false;
  bool second = false;
  std::vector<std::string> proof;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) {
      error = "registry returned a malformed consistency proof";
      return std::nullopt;
    }
    const auto name = std::string_view(line).substr(0, tab);
    const auto value = std::string_view(line).substr(tab + 1);
    if (name == "first")
      first = !first && value == std::to_string(old_size);
    else if (name == "second")
      second = !second && value == std::to_string(new_size);
    else if (name == "hash" && value.size() == 64)
      proof.emplace_back(value);
    else {
      error = "registry returned an invalid consistency proof field";
      return std::nullopt;
    }
  }
  if (!first || !second || proof.size() > 128) {
    error = "registry consistency proof is incomplete";
    return std::nullopt;
  }
  return proof;
}

RegistryCheckpointPin pin(const RegistryAuditCheckpoint &checkpoint) {
  return {checkpoint.tree_size, checkpoint.root_hash,
          checkpoint.root_version, checkpoint.root_sha256};
}

bool same(const RegistryCheckpointPin &lhs, const RegistryCheckpointPin &rhs) {
  return lhs.tree_size == rhs.tree_size && lhs.root_hash == rhs.root_hash &&
         lhs.root_version == rhs.root_version &&
         lhs.root_sha256 == rhs.root_sha256;
}

bool sameRoot(const RegistryCheckpointPin &lhs,
              const RegistryCheckpointPin &rhs) {
  return lhs.root_version == rhs.root_version &&
         lhs.root_sha256 == rhs.root_sha256;
}

std::filesystem::path clientStateDirectory(
    const RegistryTrustVerificationRequest &request) {
  return std::filesystem::path(registryTrustStatePath(
                                  request.identity_store_root,
                                  request.registry_name,
                                  request.registry_index))
      .parent_path();
}

struct CachedTransparencyState {
  RegistryCheckpointPin minimum;
  RegistryCheckpointPin snapshot;
  std::uint32_t threshold = 0;
  std::map<std::string, std::string> statements;
};

std::optional<CachedTransparencyState>
loadCachedState(const std::filesystem::path &path, std::string &error) {
  auto text = readTextFile(path.string(), error);
  if (!text)
    return std::nullopt;
  std::istringstream input(*text);
  std::string line;
  if (!std::getline(input, line) ||
      line != "chtholly-registry-transparency-state-v1") {
    error = "invalid registry transparency state";
    return std::nullopt;
  }
  CachedTransparencyState state;
  bool minimum = false;
  bool snapshot = false;
  bool threshold = false;
  bool end = false;
  while (std::getline(input, line)) {
    if (line == "end") {
      end = true;
      break;
    }
    const auto fields = [&] {
      std::vector<std::string_view> result;
      std::string_view rest(line);
      while (true) {
        const auto tab = rest.find('\t');
        result.push_back(rest.substr(0, tab));
        if (tab == std::string_view::npos)
          break;
        rest.remove_prefix(tab + 1);
      }
      return result;
    }();
    const auto checkpoint = [&](RegistryCheckpointPin &output) {
      if (fields.size() != 5)
        return false;
      const auto size = parseUnsigned(fields[1]);
      const auto version = parseUnsigned(fields[3]);
      if (!size || !version || fields[2].size() != 64 ||
          fields[4].size() != 64)
        return false;
      output = {*size, std::string(fields[2]), *version,
                std::string(fields[4])};
      return true;
    };
    if (!fields.empty() && fields[0] == "minimum" && !minimum)
      minimum = checkpoint(state.minimum);
    else if (!fields.empty() && fields[0] == "snapshot" && !snapshot)
      snapshot = checkpoint(state.snapshot);
    else if (fields.size() == 2 && fields[0] == "threshold" && !threshold) {
      const auto value = parseUnsigned(fields[1]);
      threshold = value && *value > 0 && *value <= UINT32_MAX;
      if (threshold)
        state.threshold = static_cast<std::uint32_t>(*value);
    } else if (fields.size() == 3 && fields[0] == "statement" &&
               fields[1].starts_with("sha256:") && fields[2].size() == 64) {
      if (!state.statements.emplace(std::string(fields[1]),
                                    std::string(fields[2])).second) {
        error = "duplicate registry transparency statement state";
        return std::nullopt;
      }
    } else {
      error = "invalid registry transparency state field";
      return std::nullopt;
    }
  }
  if (!end || !minimum || !snapshot || !threshold) {
    error = "incomplete registry transparency state";
    return std::nullopt;
  }
  return state;
}

bool persistState(const std::filesystem::path &directory,
                  const CachedTransparencyState &state,
                  const std::map<std::string, std::string> &statement_texts,
                  std::string &error) {
  std::error_code ec;
  const auto statements = directory / "witness-statements";
  std::filesystem::create_directories(statements, ec);
  if (ec) {
    error = "failed to create registry transparency state: " + ec.message();
    return false;
  }
  for (const auto &[key, text] : statement_texts) {
    const auto path = statements / (key.substr(7) + ".txt");
    const auto temporary = path.string() + ".tmp";
    if (!writeTextFile(temporary, text, error) ||
        !replaceFile(temporary, path.string(), ec)) {
      if (error.empty())
        error = "failed to persist registry witness statement: " + ec.message();
      removeFile(temporary, ec);
      return false;
    }
  }
  std::ostringstream out;
  const auto append = [&](std::string_view name,
                          const RegistryCheckpointPin &value) {
    out << name << '\t' << value.tree_size << '\t' << value.root_hash << '\t'
        << value.root_version << '\t' << value.root_sha256 << '\n';
  };
  out << "chtholly-registry-transparency-state-v1\n";
  append("minimum", state.minimum);
  append("snapshot", state.snapshot);
  out << "threshold\t" << state.threshold << '\n';
  for (const auto &[key, digest] : state.statements)
    out << "statement\t" << key << '\t' << digest << '\n';
  out << "end\n";
  const auto path = directory / "transparency-state.txt";
  const auto temporary = path.string() + ".tmp";
  if (!writeTextFile(temporary, out.str(), error) ||
      !replaceFile(temporary, path.string(), ec)) {
    if (error.empty())
      error = "failed to publish registry transparency state: " + ec.message();
    removeFile(temporary, ec);
    return false;
  }
  return true;
}

std::optional<std::vector<std::string>> loadRootChain(
    const RegistryTrustVerificationRequest &request, std::uint64_t version,
    std::string &error) {
  std::vector<std::string> result;
  for (std::uint64_t current = 1; current <= version; ++current) {
    const auto path = std::filesystem::path(request.checkout_root) / "trust" /
                      "root" / (std::to_string(current) + ".toml");
    auto text = readTextFile(path.string(), error);
    if (!text)
      return std::nullopt;
    result.push_back(std::move(*text));
  }
  return result;
}

} // namespace

std::optional<VerifiedRegistryClientView>
verifyRegistryClientView(const RegistryClientViewVerificationRequest &request,
                         std::string &error) {
  auto trust_request = request.trust;
  trust_request.persist_state = false;
  auto trust = verifyRegistryTrust(trust_request, error);
  if (!trust)
    return std::nullopt;
  const auto &policy = request.transparency;
  if (policy.witness_threshold == 0) {
    if (!commitVerifiedRegistryTrust(trust_request, *trust, error))
      return std::nullopt;
    RegistryCheckpointPin accepted;
    if (trust->audit_checkpoint)
      accepted = pin(*trust->audit_checkpoint);
    return VerifiedRegistryClientView{std::move(*trust), std::move(accepted),
                                      {}};
  }
  if (!trust->audit_checkpoint || policy.registry_origin.empty() ||
      policy.witness_urls.empty() || policy.witness_keys.empty() ||
      policy.witness_threshold > policy.witness_urls.size() ||
      policy.witness_threshold > policy.witness_keys.size() ||
      policy.witness_threshold <= policy.witness_keys.size() / 2) {
    error = "registry transparency policy or snapshot v2 checkpoint is missing";
    return std::nullopt;
  }
  const auto snapshot = pin(*trust->audit_checkpoint);
  const auto minimum = request.minimum_checkpoint.value_or(snapshot);
  if (!sameRoot(minimum, snapshot) || minimum.tree_size > snapshot.tree_size ||
      (minimum.tree_size == snapshot.tree_size &&
       minimum.root_hash != snapshot.root_hash)) {
    error = "registry snapshot does not contain the lockfile checkpoint floor";
    return std::nullopt;
  }
  const auto state_directory = clientStateDirectory(trust_request);
  if (request.offline) {
    auto cached = loadCachedState(state_directory / "transparency-state.txt",
                                  error);
    if (!cached || cached->threshold < policy.witness_threshold ||
        !same(cached->minimum, minimum) ||
        !same(cached->snapshot, snapshot)) {
      if (error.empty())
        error = "offline registry transparency quorum is unavailable";
      return std::nullopt;
    }
    std::vector<std::string> keys;
    for (const auto &[key, digest] : cached->statements) {
      const auto path = state_directory / "witness-statements" /
                        (key.substr(7) + ".txt");
      auto text = readTextFile(path.string(), error);
      auto statement = text && sha256Hex(*text) == digest
                           ? parseRegistryWitnessStatement(*text, error)
                           : std::optional<RegistryWitnessStatement>{};
      if (statement && statement->signature.key_id == key &&
          statement->registry_name == trust_request.registry_name &&
          statement->registry_origin == policy.registry_origin &&
          statement->root_version == snapshot.root_version &&
          statement->root_sha256 == snapshot.root_sha256 &&
          statement->head_tree_size >= snapshot.tree_size &&
          verifyRegistryWitnessStatementSignature(*statement,
                                                  policy.witness_keys, error))
        keys.push_back(key);
      error.clear();
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.size() < policy.witness_threshold) {
      error = "offline registry transparency cache lacks a valid witness quorum";
      return std::nullopt;
    }
    if (!commitVerifiedRegistryTrust(trust_request, *trust, error))
      return std::nullopt;
    return VerifiedRegistryClientView{std::move(*trust), snapshot,
                                      std::move(keys)};
  }

  auto registry = requestHttps(policy.registry_origin + "/v1/audit/checkpoint",
                               policy.registry_ca_bundle_path, std::nullopt,
                               error);
  auto registry_checkpoint =
      registry && registry->status == 200
          ? parseRegistryAuditCheckpoint(registry->body, error)
          : std::optional<RegistryAuditCheckpoint>{};
  if (!registry_checkpoint ||
      !verifyRegistryAuditCheckpoint(*registry_checkpoint, trust->root, error) ||
      registry_checkpoint->tree_size < snapshot.tree_size) {
    if (error.empty())
      error = "registry live checkpoint is unavailable or precedes snapshot";
    return std::nullopt;
  }

  auto root_chain = loadRootChain(trust_request, trust->root.version, error);
  if (!root_chain)
    return std::nullopt;
  RegistryWitnessObservation observation{policy.registry_origin,
                                         *trust->audit_checkpoint,
                                         std::move(*root_chain)};
  const auto observation_text = renderRegistryWitnessObservation(observation);
  struct WitnessReply {
    std::string url;
    std::optional<HttpResponse> response;
    std::string error;
  };
  std::vector<std::future<WitnessReply>> pending;
  for (const auto &base : policy.witness_urls) {
    pending.push_back(std::async(std::launch::async, [&, base] {
      WitnessReply reply;
      reply.url = base;
      reply.response = requestHttps(base + "/v1/checkpoint",
                                    policy.witness_ca_bundle_path,
                                    std::nullopt, reply.error);
      return reply;
    }));
  }
  std::map<std::string, RegistryWitnessStatement> statements;
  std::map<std::string, std::string> statement_texts;
  std::string first_failure;
  for (auto &future : pending) {
    auto reply = future.get();
    auto parsed = reply.response && reply.response->status == 200
                      ? parseRegistryWitnessStatement(reply.response->body,
                                                      reply.error)
                      : std::optional<RegistryWitnessStatement>{};
    if (!parsed || parsed->head_tree_size < snapshot.tree_size) {
      reply.response = requestHttps(reply.url + "/v1/observations",
                                    policy.witness_ca_bundle_path,
                                    observation_text, reply.error);
      parsed = reply.response && reply.response->status == 200
                   ? parseRegistryWitnessStatement(reply.response->body,
                                                   reply.error)
                   : std::optional<RegistryWitnessStatement>{};
    }
    if (parsed && parsed->registry_name == trust_request.registry_name &&
        parsed->registry_origin == policy.registry_origin &&
        parsed->root_version == snapshot.root_version &&
        parsed->root_sha256 == snapshot.root_sha256 &&
        parsed->head_tree_size >= snapshot.tree_size &&
        verifyRegistryWitnessStatementSignature(*parsed, policy.witness_keys,
                                                reply.error)) {
      const auto key = parsed->signature.key_id;
      if (statements.emplace(key, *parsed).second)
        statement_texts.emplace(key, reply.response->body);
    } else if (first_failure.empty()) {
      first_failure = reply.error.empty() ? "witness checkpoint is unavailable"
                                          : std::move(reply.error);
    }
  }
  if (statements.size() < policy.witness_threshold) {
    error = "registry transparency witness quorum reached " +
            std::to_string(statements.size()) + " of " +
            std::to_string(policy.witness_threshold);
    if (!first_failure.empty())
      error += ": " + first_failure;
    return std::nullopt;
  }

  std::vector<RegistryCheckpointPin> chain{minimum, snapshot,
                                           pin(*registry_checkpoint)};
  for (const auto &[_, statement] : statements)
    chain.push_back({statement.head_tree_size, statement.head_root_hash,
                     statement.root_version, statement.root_sha256});
  std::sort(chain.begin(), chain.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.tree_size < rhs.tree_size;
  });
  chain.erase(std::unique(chain.begin(), chain.end(), same), chain.end());
  for (std::size_t index = 0; index < chain.size(); ++index) {
    if (!sameRoot(chain.front(), chain[index])) {
      error = "registry transparency views use different trust roots";
      return std::nullopt;
    }
    if (index == 0)
      continue;
    const auto &previous = chain[index - 1];
    const auto &current = chain[index];
    if (previous.tree_size == current.tree_size) {
      error = "registry transparency detected an equal-size view fork";
      return std::nullopt;
    }
    auto proof_response = requestHttps(
        policy.registry_origin + "/v1/audit/consistency?old_size=" +
            std::to_string(previous.tree_size) + "&new_size=" +
            std::to_string(current.tree_size),
        policy.registry_ca_bundle_path, std::nullopt, error);
    auto proof = proof_response && proof_response->status == 200
                     ? parseConsistencyProof(proof_response->body,
                                             previous.tree_size,
                                             current.tree_size, error)
                     : std::optional<std::vector<std::string>>{};
    if (!proof || !verifyRegistryMerkleConsistency(
                      previous.tree_size, current.tree_size,
                      previous.root_hash, current.root_hash, *proof)) {
      if (error.empty())
        error = "registry transparency detected inconsistent checkpoint growth";
      return std::nullopt;
    }
  }

  CachedTransparencyState state;
  state.minimum = minimum;
  state.snapshot = snapshot;
  state.threshold = policy.witness_threshold;
  for (const auto &[key, text] : statement_texts)
    state.statements.emplace(key, sha256Hex(text));
  if (!persistState(state_directory, state, statement_texts, error) ||
      !commitVerifiedRegistryTrust(trust_request, *trust, error))
    return std::nullopt;
  std::vector<std::string> keys;
  for (const auto &[key, _] : statements)
    keys.push_back(key);
  return VerifiedRegistryClientView{std::move(*trust), snapshot,
                                    std::move(keys)};
}

} // namespace chtholly
