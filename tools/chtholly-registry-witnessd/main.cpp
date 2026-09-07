#include "chtholly/Driver/RegistryWitness.h"

#include <httplib.h>

#include <charconv>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, status] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return status == std::errc{} && end == text.data() + text.size()
             ? std::optional(value)
             : std::nullopt;
}

std::optional<std::pair<std::string, std::uint16_t>>
parseOrigin(std::string_view origin) {
  constexpr std::string_view prefix = "https://";
  if (!origin.starts_with(prefix))
    return std::nullopt;
  origin.remove_prefix(prefix.size());
  const auto colon = origin.rfind(':');
  std::string host;
  std::uint16_t port = 443;
  if (colon == std::string_view::npos) {
    host = std::string(origin);
  } else {
    auto parsed = parseUnsigned(origin.substr(colon + 1));
    if (!parsed || *parsed == 0 || *parsed > UINT16_MAX)
      return std::nullopt;
    host = std::string(origin.substr(0, colon));
    port = static_cast<std::uint16_t>(*parsed);
  }
  if (host == "localhost")
    host = "127.0.0.1";
  return host.empty() ? std::nullopt
                      : std::optional(std::pair{std::move(host), port});
}

std::optional<std::vector<std::string>> parseProof(std::string_view text,
                                                   std::uint64_t old_size,
                                                   std::uint64_t new_size,
                                                   std::string &error) {
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

void response(httplib::Response &output, int status, std::string text) {
  output.status = status;
  output.set_content(std::move(text), "text/plain; charset=utf-8");
  output.set_header("Cache-Control", "no-store");
  output.set_header("X-Content-Type-Options", "nosniff");
}

int serve(const chtholly::RegistryWitnessDaemonConfig &config,
          chtholly::RegistryWitnessStore &store, std::string &error) {
  const auto registry = parseOrigin(config.witness.registry_origin);
  if (!registry) {
    error = "registry witness origin is invalid";
    return 1;
  }
  httplib::SSLServer server(config.tls_certificate_path.c_str(),
                            config.tls_private_key_path.c_str());
  if (!server.is_valid()) {
    error = "failed to initialize registry witness TLS server";
    return 1;
  }
  server.set_payload_max_length(8u * 1024u * 1024u);
  server.Post("/v1/observations", [&](const httplib::Request &request,
                                      httplib::Response &output) {
    std::string observation_error;
    auto observation = chtholly::parseRegistryWitnessObservation(
        request.body, observation_error);
    if (!observation) {
      response(output, 400, observation_error + "\n");
      return;
    }
    const auto fetch = [&](std::uint64_t old_size, std::uint64_t new_size,
                           std::string &proof_error)
        -> std::optional<std::vector<std::string>> {
      httplib::SSLClient client(registry->first, registry->second);
      client.enable_server_certificate_verification(true);
      client.set_connection_timeout(30);
      client.set_read_timeout(60);
      if (!config.registry_ca_bundle_path.empty())
        client.set_ca_cert_path(config.registry_ca_bundle_path);
      const auto path =
          "/v1/audit/consistency?old_size=" + std::to_string(old_size) +
          "&new_size=" + std::to_string(new_size);
      auto result = client.Get(path);
      if (!result || result->status != 200 ||
          result->body.size() > 1024u * 1024u) {
        proof_error =
            !result ? "registry consistency proof request failed: " +
                          httplib::to_string(result.error())
                    : "registry consistency proof request returned HTTP " +
                          std::to_string(result->status) + ": " + result->body;
        return std::nullopt;
      }
      return parseProof(result->body, old_size, new_size, proof_error);
    };
    auto statement = store.observe(*observation, -1, fetch, observation_error);
    if (!statement) {
      response(output, 409, observation_error + "\n");
      return;
    }
    response(output, 200, chtholly::renderRegistryWitnessStatement(*statement));
  });
  server.Get("/v1/checkpoint", [&](const httplib::Request &,
                                   httplib::Response &output) {
    std::string lookup_error;
    auto statement = store.latest(lookup_error);
    if (!statement) {
      response(output, 404, lookup_error + "\n");
      return;
    }
    response(output, 200, chtholly::renderRegistryWitnessStatement(*statement));
  });
  server.Get("/v1/incidents", [&](const httplib::Request &,
                                  httplib::Response &output) {
    std::string lookup_error;
    auto incidents = store.incidents(lookup_error);
    if (!incidents) {
      response(output, 500, lookup_error + "\n");
      return;
    }
    std::ostringstream body;
    body << "chtholly-registry-witness-incidents-v1\n";
    for (const auto &incident : *incidents)
      body << "incident\t" << incident.sequence << '\t' << incident.kind << '\t'
           << incident.known_tree_size << '\t' << incident.known_root_hash
           << '\t' << incident.observed_tree_size << '\t'
           << incident.observed_root_hash << '\t' << incident.detected_at
           << '\n';
    response(output, 200, body.str());
  });
  std::cout << "listening\thttps://" << config.listen_address << ':'
            << config.listen_port << '\n';
  std::cout.flush();
  if (!server.listen(config.listen_address, config.listen_port)) {
    error = "registry witness HTTPS server failed to listen";
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h")) {
    std::cout
        << "usage: chtholly-registry-witnessd serve --config <witness.toml>\n";
    return 0;
  }
  if (argc != 4 || std::string_view(argv[1]) != "serve" ||
      std::string_view(argv[2]) != "--config" ||
      std::string_view(argv[3]).empty()) {
    std::cerr
        << "usage: chtholly-registry-witnessd serve --config <witness.toml>\n";
    return 2;
  }
  std::string error;
  auto config = chtholly::loadRegistryWitnessDaemonConfig(argv[3], error);
  auto store =
      config ? chtholly::RegistryWitnessStore::open(config->witness, error)
             : std::optional<chtholly::RegistryWitnessStore>{};
  if (!config || !store) {
    std::cerr << "chtholly-registry-witnessd: " << error << '\n';
    return 1;
  }
  const auto status = serve(*config, *store, error);
  if (status != 0)
    std::cerr << "chtholly-registry-witnessd: " << error << '\n';
  return status;
}
