#include "chtholly/Driver/RegistryServer.h"
#include "chtholly/Driver/RegistryBackup.h"
#include "chtholly/Support/FileSystem.h"

#include <httplib.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage(std::ostream &out) {
  out << "usage:\n"
      << "  chtholly-registryd serve --config <server.toml>\n"
      << "  chtholly-registryd token create --config <server.toml> "
         "--principal <name> (--package <name> ... | --all-packages)\n"
      << "  chtholly-registryd token revoke --config <server.toml> <token-id>\n"
      << "  chtholly-registryd token list --config <server.toml>\n"
      << "  chtholly-registryd operator create --config <server.toml> "
         "--principal <name> --capability backup:create\n"
      << "  chtholly-registryd operator revoke --config <server.toml> <token-id>\n"
      << "  chtholly-registryd operator list --config <server.toml>\n"
      << "  chtholly-registryd backup create --url <https-origin> "
         "(--token-env <name> | --token-file <path>) "
         "[--ca-bundle <path>] -o <backup.zip>\n"
      << "  chtholly-registryd backup verify --config <server.toml> <backup.zip>\n"
      << "  chtholly-registryd backup restore --config <server.toml> <backup.zip>\n";
  out << "  chtholly-registryd recovery list --config <server.toml>\n"
      << "  chtholly-registryd recovery extend --config <server.toml> "
         "<recovery-id> --retain-until <unix-seconds>\n"
      << "  chtholly-registryd recovery release --config <server.toml> "
         "<recovery-id>\n"
      << "  chtholly-registryd gc plan --config <server.toml>\n"
      << "  chtholly-registryd gc apply --config <server.toml> <plan-id>\n"
      << "  chtholly-registryd index reseal --config <server.toml>\n";
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::optional<std::pair<std::string, std::uint16_t>>
parseHttpsOrigin(std::string_view value) {
  constexpr std::string_view prefix = "https://";
  if (!value.starts_with(prefix))
    return std::nullopt;
  value.remove_prefix(prefix.size());
  if (value.empty() || value.find_first_of("/?#@") != std::string_view::npos)
    return std::nullopt;
  std::string host;
  std::uint16_t port = 443;
  if (value.front() == '[') {
    const auto close = value.find(']');
    if (close == std::string_view::npos)
      return std::nullopt;
    host = std::string(value.substr(1, close - 1));
    if (close + 1 < value.size()) {
      if (value[close + 1] != ':')
        return std::nullopt;
      auto parsed = parseUnsigned(value.substr(close + 2));
      if (!parsed || *parsed == 0 || *parsed > UINT16_MAX)
        return std::nullopt;
      port = static_cast<std::uint16_t>(*parsed);
    }
  } else {
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos) {
      host = std::string(value);
    } else {
      host = std::string(value.substr(0, colon));
      auto parsed = parseUnsigned(value.substr(colon + 1));
      if (!parsed || *parsed == 0 || *parsed > UINT16_MAX)
        return std::nullopt;
      port = static_cast<std::uint16_t>(*parsed);
    }
  }
  if (host == "localhost")
    host = "127.0.0.1";
  return host.empty() ? std::nullopt
                      : std::optional{std::pair{std::move(host), port}};
}

template <typename RequestT>
bool validPublishMultipart(const RequestT &request) {
  if constexpr (requires { request.form.files; request.form.fields; }) {
    return request.form.fields.empty() && request.form.files.size() == 2 &&
           request.form.get_file_count("archive") == 1 &&
           request.form.get_file_count("entry") == 1;
  } else {
    return request.params.empty() && request.files.size() == 2 &&
           request.files.count("archive") == 1 &&
           request.files.count("entry") == 1;
  }
}

template <typename RequestT>
auto publishMultipartFile(const RequestT &request, const char *name) {
  if constexpr (requires { request.form.get_file(name); })
    return request.form.get_file(name);
  else
    return request.get_file_value(name);
}

struct Arguments {
  std::string config_path;
  std::string principal;
  std::vector<std::string> packages;
  std::string token_id;
  std::vector<std::string> capabilities;
  std::string url;
  std::string token_environment;
  std::string token_file;
  std::string ca_bundle;
  std::string output_path;
  std::vector<std::string> positional;
  std::uint64_t retain_until = 0;
  bool all_packages = false;
};

bool parseArguments(int argc, char **argv, int start, Arguments &result,
                    std::string &error) {
  for (int index = start; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    const auto value = [&](std::string &output) {
      if (++index >= argc) {
        error = "missing value after '" + std::string(argument) + "'";
        return false;
      }
      output = argv[index];
      return !output.empty();
    };
    if (argument == "--config") {
      if (!value(result.config_path))
        return false;
    } else if (argument == "--principal") {
      if (!value(result.principal))
        return false;
    } else if (argument == "--package") {
      std::string package;
      if (!value(package))
        return false;
      result.packages.push_back(std::move(package));
    } else if (argument == "--capability") {
      std::string capability;
      if (!value(capability))
        return false;
      result.capabilities.push_back(std::move(capability));
    } else if (argument == "--url") {
      if (!value(result.url))
        return false;
    } else if (argument == "--token-env") {
      if (!value(result.token_environment))
        return false;
    } else if (argument == "--token-file") {
      if (!value(result.token_file))
        return false;
    } else if (argument == "--ca-bundle") {
      if (!value(result.ca_bundle))
        return false;
    } else if (argument == "-o") {
      if (!value(result.output_path))
        return false;
    } else if (argument == "--retain-until") {
      std::string text;
      if (!value(text))
        return false;
      auto parsed = parseUnsigned(text);
      if (!parsed || *parsed > static_cast<std::uint64_t>(
                                   (std::numeric_limits<std::int64_t>::max)())) {
        error = "--retain-until requires a positive signed timestamp";
        return false;
      }
      result.retain_until = *parsed;
    } else if (argument == "--all-packages") {
      result.all_packages = true;
    } else if (!argument.starts_with('-')) {
      result.positional.emplace_back(argument);
    } else {
      error =
          "unknown registry server argument '" + std::string(argument) + "'";
      return false;
    }
  }
  return true;
}

void textResponse(httplib::Response &response, int status, std::string body) {
  response.status = status;
  response.set_content(std::move(body), "text/plain; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.set_header("X-Content-Type-Options", "nosniff");
}

std::string proofText(std::string_view format, std::uint64_t first,
                      std::uint64_t second,
                      const std::vector<std::string> &proof) {
  std::string output(format);
  output += "\nfirst\t" + std::to_string(first) + "\nsecond\t" +
            std::to_string(second) + "\n";
  for (const auto &hash : proof)
    output += "hash\t" + hash + "\n";
  return output;
}

int serve(const chtholly::RegistryDaemonConfig &config,
          chtholly::RegistryPublicationStore &store, std::string &error) {
  httplib::SSLServer server(config.tls_certificate_path.c_str(),
                            config.tls_private_key_path.c_str());
  if (!server.is_valid()) {
    error = "failed to initialize registry HTTPS certificate and private key";
    return 1;
  }
  const auto stale_backup_root =
      std::filesystem::path(config.publication.state_directory) / "backups";
  std::error_code stale_error;
  if (std::filesystem::exists(stale_backup_root, stale_error)) {
    for (std::filesystem::directory_iterator it(stale_backup_root, stale_error),
         end;
         !stale_error && it != end; it.increment(stale_error)) {
      const auto name = it->path().filename().string();
      if (it->is_regular_file(stale_error) && name.starts_with("online-") &&
          (name.ends_with(".zip") || name.ends_with(".zip.tmp")))
        std::filesystem::remove(it->path(), stale_error);
    }
  }
  if (stale_error) {
    error = "failed to clean stale registry backup files: " +
            stale_error.message();
    return 1;
  }
  constexpr std::uint64_t upload_overhead = 1024 * 1024;
  const auto payload_limit =
      config.publication.max_archive_bytes > UINT64_MAX - upload_overhead
          ? UINT64_MAX
          : config.publication.max_archive_bytes + upload_overhead;
  server.set_payload_max_length(static_cast<std::size_t>((std::min)(
      payload_limit,
      static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))));

  server.Post("/v1/publish", [&](const httplib::Request &request,
                                 httplib::Response &response) {
    if (!request.is_multipart_form_data() || !validPublishMultipart(request)) {
      textResponse(
          response, 400,
          "registry publish requires exactly archive and entry files\n");
      return;
    }
    const auto authorization = request.get_header_value("Authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (!std::string_view(authorization).starts_with(prefix) ||
        authorization.size() == prefix.size()) {
      textResponse(response, 401, "registry publisher authentication failed\n");
      return;
    }
    const auto idempotency = request.get_header_value("Idempotency-Key");
    if (idempotency.empty()) {
      textResponse(response, 400,
                   "registry publish requires Idempotency-Key\n");
      return;
    }
    const auto archive = publishMultipartFile(request, "archive");
    const auto entry = publishMultipartFile(request, "entry");
    if (archive.content.empty() || entry.content.empty() ||
        archive.content.size() > config.publication.max_archive_bytes ||
        entry.content.size() > 1024 * 1024) {
      textResponse(response, 413,
                   "registry publish body exceeds configured limits\n");
      return;
    }
    static std::atomic<std::uint64_t> counter{0};
    const auto nonce =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(counter.fetch_add(1));
    const auto upload_root =
        std::filesystem::path(config.publication.state_directory) / "uploads";
    std::error_code file_error;
    std::filesystem::create_directories(upload_root, file_error);
    const auto archive_path = upload_root / (nonce + ".cpa");
    const auto entry_path = upload_root / (nonce + ".toml");
    std::string write_error;
    if (file_error ||
        !chtholly::writeTextFile(archive_path.string(), archive.content,
                                 write_error) ||
        !chtholly::writeTextFile(entry_path.string(), entry.content,
                                 write_error)) {
      std::filesystem::remove(archive_path, file_error);
      std::filesystem::remove(entry_path, file_error);
      textResponse(response, 500, "failed to stage registry upload\n");
      return;
    }
    chtholly::RegistryServerPublishRequest publication;
    publication.bearer_token = authorization.substr(prefix.size());
    publication.archive_path = archive_path.string();
    publication.entry_path = entry_path.string();
    publication.idempotency_key = idempotency;
    auto result = store.publish(publication);
    std::filesystem::remove(archive_path, file_error);
    std::filesystem::remove(entry_path, file_error);
    if (!result.receipt) {
      textResponse(response, static_cast<int>(result.status),
                   result.message + "\n");
      return;
    }
    textResponse(response, static_cast<int>(result.status),
                 chtholly::renderRegistryAuditReceipt(*result.receipt));
  });

  server.Post(
      R"(/v1/releases/([A-Za-z0-9_.-]+)/([^/]+)/(yank|unyank))",
      [&](const httplib::Request &request, httplib::Response &response) {
        if (request.is_multipart_form_data() || request.body.empty() ||
            request.body.size() > 32) {
          textResponse(response, 400,
                       "registry lifecycle request requires a reason code\n");
          return;
        }
        const auto authorization = request.get_header_value("Authorization");
        constexpr std::string_view prefix = "Bearer ";
        const auto idempotency = request.get_header_value("Idempotency-Key");
        if (!std::string_view(authorization).starts_with(prefix) ||
            authorization.size() == prefix.size() || idempotency.empty()) {
          textResponse(response, 401,
                       "registry lifecycle authentication failed\n");
          return;
        }
        chtholly::RegistryServerLifecycleRequest lifecycle;
        lifecycle.bearer_token = authorization.substr(prefix.size());
        lifecycle.package_name = request.matches[1].str();
        lifecycle.version = request.matches[2].str();
        lifecycle.state = request.matches[3].str() == "yank"
                              ? chtholly::RegistryReleaseState::Yanked
                              : chtholly::RegistryReleaseState::Active;
        lifecycle.reason_code = request.body;
        lifecycle.idempotency_key = idempotency;
        auto result = store.changeReleaseState(lifecycle);
        if (!result.receipt) {
          textResponse(response, static_cast<int>(result.status),
                       result.message + "\n");
          return;
        }
        textResponse(response, static_cast<int>(result.status),
                     chtholly::renderRegistryLifecycleReceipt(*result.receipt));
      });

  server.Post("/v1/admin/backups", [&](const httplib::Request &request,
                                        httplib::Response &response) {
    if (!request.body.empty() || request.is_multipart_form_data()) {
      textResponse(response, 400, "registry backup request body must be empty\n");
      return;
    }
    const auto authorization = request.get_header_value("Authorization");
    constexpr std::string_view prefix = "Bearer ";
    std::string auth_error;
    if (!std::string_view(authorization).starts_with(prefix) ||
        !store.authenticateOperatorToken(
            std::string_view(authorization).substr(prefix.size()),
            "backup:create", auth_error)) {
      textResponse(response, 401, "registry operator authentication failed\n");
      return;
    }
    static std::atomic<std::uint64_t> backup_counter{0};
    const auto backup_root =
        std::filesystem::path(config.publication.state_directory) / "backups";
    std::error_code ec;
    std::filesystem::create_directories(backup_root, ec);
    const auto backup_path =
        backup_root /
        ("online-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(backup_counter.fetch_add(1)) + ".zip");
    std::string backup_error;
    std::optional<chtholly::RegistryRecoveryPointInfo> recovery;
    const bool retention_enabled =
        config.publication.recovery_point_seconds != 0;
    const bool created = retention_enabled
                             ? (recovery = store.createRecoveryPoint(
                                    backup_path.string(), -1, backup_error))
                                   .has_value()
                             : store.createBackup(backup_path.string(), -1,
                                                  backup_error);
    if (ec || !created) {
      textResponse(response, 503,
                   (backup_error.empty() ? "failed to create registry backup"
                                         : backup_error) +
                       "\n");
      return;
    }
    const auto size = std::filesystem::file_size(backup_path, ec);
    if (ec) {
      std::filesystem::remove(backup_path, ec);
      textResponse(response, 500, "failed to inspect registry backup\n");
      return;
    }
    response.status = 200;
    if (recovery)
      response.set_header("X-Chtholly-Recovery-Point", recovery->id);
    response.set_header("Cache-Control", "no-store");
    response.set_header("Content-Disposition",
                        "attachment; filename=registry-backup.zip");
    response.set_content_provider(
        static_cast<std::size_t>(size), "application/zip",
        [backup_path](std::size_t offset, std::size_t length,
                      httplib::DataSink &sink) {
          std::ifstream input(backup_path, std::ios::binary);
          if (!input)
            return false;
          input.seekg(static_cast<std::streamoff>(offset));
          std::vector<char> buffer(length);
          input.read(buffer.data(), static_cast<std::streamsize>(length));
          const auto read = static_cast<std::size_t>(input.gcount());
          return read == length && sink.write(buffer.data(), read);
        },
        [backup_path](bool) {
          std::error_code ignored;
          std::filesystem::remove(backup_path, ignored);
        });
  });

  server.Get(
      R"(/v1/artifacts/sha256/([0-9a-f]{64})\.cpa)",
      [&](const httplib::Request &request, httplib::Response &response) {
        std::string lookup_error;
        auto path = store.archivePath(request.matches[1].str(), lookup_error);
        auto bytes = path ? chtholly::readTextFile(*path, lookup_error)
                          : std::optional<std::string>{};
        if (!bytes) {
          textResponse(response, 404, "registry archive was not found\n");
          return;
        }
        response.status = 200;
        response.set_content(std::move(*bytes), "application/octet-stream");
        response.set_header("Cache-Control",
                            "public, immutable, max-age=31536000");
        response.set_header("X-Content-Type-Options", "nosniff");
      });

  server.Get(
      R"(/v1/trust/root/([1-9][0-9]*))",
      [&](const httplib::Request &request, httplib::Response &response) {
        const auto root_path =
            std::filesystem::path(config.publication.index_worktree) / "trust" /
            "root" / (request.matches[1].str() + ".toml");
        std::string read_error;
        auto root = chtholly::readTextFile(root_path.string(), read_error);
        if (!root) {
          textResponse(response, 404, "registry root version was not found\n");
          return;
        }
        textResponse(response, 200, std::move(*root));
        response.set_header("Cache-Control",
                            "public, immutable, max-age=31536000");
      });

  server.Get("/v1/audit/checkpoint", [&](const httplib::Request &,
                                         httplib::Response &response) {
    std::string lookup_error;
    auto checkpoint = store.latestCheckpoint(lookup_error);
    if (!checkpoint) {
      textResponse(response, 404, lookup_error + "\n");
      return;
    }
    textResponse(response, 200,
                 chtholly::renderRegistryAuditCheckpoint(*checkpoint));
  });

  server.Get(R"(/v1/audit/entries/([0-9]+))",
             [&](const httplib::Request &request, httplib::Response &response) {
               const auto index = parseUnsigned(request.matches[1].str());
               std::string lookup_error;
               auto leaf = index ? store.auditLeaf(*index, lookup_error)
                                 : std::optional<std::string>{};
               if (!leaf) {
                 textResponse(response, 404,
                              "registry audit leaf was not found\n");
                 return;
               }
               textResponse(response, 200, std::move(*leaf));
               response.set_header("Cache-Control",
                                   "public, immutable, max-age=31536000");
             });

  server.Get("/v1/audit/inclusion", [&](const httplib::Request &request,
                                        httplib::Response &response) {
    const auto index = parseUnsigned(request.get_param_value("leaf_index"));
    const auto size = parseUnsigned(request.get_param_value("tree_size"));
    if (!index || !size) {
      textResponse(response, 400, "invalid audit inclusion proof request\n");
      return;
    }
    std::string proof_error;
    auto proof = store.inclusionProof(*index, *size, proof_error);
    if (!proof_error.empty()) {
      textResponse(response, 400, proof_error + "\n");
      return;
    }
    textResponse(response, 200,
                 proofText("chtholly-registry-audit-inclusion-v1", *index,
                           *size, proof));
  });

  server.Get("/v1/audit/consistency", [&](const httplib::Request &request,
                                          httplib::Response &response) {
    const auto old_size = parseUnsigned(request.get_param_value("old_size"));
    const auto new_size = parseUnsigned(request.get_param_value("new_size"));
    if (!old_size || !new_size) {
      textResponse(response, 400, "invalid audit consistency proof request\n");
      return;
    }
    std::string proof_error;
    auto proof = store.consistencyProof(*old_size, *new_size, proof_error);
    if (!proof_error.empty()) {
      textResponse(response, 400, proof_error + "\n");
      return;
    }
    textResponse(response, 200,
                 proofText("chtholly-registry-audit-consistency-v1", *old_size,
                           *new_size, proof));
  });

  std::cout << "listening\thttps://" << config.listen_address << ':'
            << config.listen_port << '\n';
  std::cout.flush();
  if (!server.listen(config.listen_address, config.listen_port)) {
    error = "registry HTTPS server failed to listen";
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(std::cerr);
    return 2;
  }
  const std::string_view command(argv[1]);
  std::string action;
  int argument_start = 2;
  std::string command_group;
  if (command == "token" || command == "operator" || command == "backup" ||
      command == "recovery" || command == "gc" || command == "index") {
    if (argc < 3) {
      usage(std::cerr);
      return 2;
    }
    command_group = std::string(command);
    action = argv[2];
    argument_start = 3;
  } else if (command == "serve") {
    action = "serve";
  } else if (command == "--help" || command == "-h") {
    usage(std::cout);
    return 0;
  } else {
    usage(std::cerr);
    return 2;
  }

  Arguments arguments;
  std::string error;
  if (!parseArguments(argc, argv, argument_start, arguments, error)) {
    std::cerr << "chtholly-registryd: " << error << '\n';
    return 2;
  }
  if (!arguments.positional.empty())
    arguments.token_id = arguments.positional.front();

  if (command_group == "backup" && action == "create") {
    auto token = chtholly::loadRegistryBearerToken(
        arguments.token_environment, arguments.token_file, error);
    auto origin = parseHttpsOrigin(arguments.url);
    if (!token || !origin || arguments.output_path.empty() ||
        !arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: "
                << (error.empty() ? "backup create requires --url and -o"
                                  : error)
                << '\n';
      return 2;
    }
    httplib::SSLClient client(origin->first, origin->second);
    client.enable_server_certificate_verification(true);
    if (!arguments.ca_bundle.empty())
      client.set_ca_cert_path(arguments.ca_bundle);
    httplib::Headers headers{{"Authorization", "Bearer " + *token}};
    const auto temporary = arguments.output_path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      std::cerr << "chtholly-registryd: failed to open backup output\n";
      return 1;
    }
    httplib::Request request;
    request.method = "POST";
    request.path = "/v1/admin/backups";
    request.headers = std::move(headers);
    request.set_header("Content-Type", "application/octet-stream");
    request.content_receiver =
        [&](const char *data, std::size_t size, std::uint64_t,
            std::uint64_t) {
          output.write(data, static_cast<std::streamsize>(size));
          return static_cast<bool>(output);
        };
    auto response = client.send(request);
    output.flush();
    output.close();
    if (!response || response->status != 200) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      std::cerr << "chtholly-registryd: registry backup request failed"
                << (response ? ": " + response->body
                             : ": " + httplib::to_string(response.error()))
                << '\n';
      return 1;
    }
    std::error_code replace_error;
    if (!chtholly::replaceFile(temporary, arguments.output_path,
                               replace_error)) {
      const auto message = replace_error.message();
      std::filesystem::remove(temporary, replace_error);
      std::cerr << "chtholly-registryd: failed to publish backup output: "
                << message << '\n';
      return 1;
    }
    return 0;
  }
  if (arguments.config_path.empty()) {
    std::cerr << "chtholly-registryd: command requires --config\n";
    return 2;
  }
  auto config =
      chtholly::loadRegistryDaemonConfig(arguments.config_path, error);
  if (command_group == "backup" &&
      (action == "verify" || action == "restore")) {
    if (!config || arguments.positional.size() != 1) {
      std::cerr << "chtholly-registryd: "
                << (error.empty() ? "backup command requires one archive"
                                  : error)
                << '\n';
      return error.empty() ? 2 : 1;
    }
    chtholly::RegistryBackupRestoreRequest request;
    request.archive_path = arguments.positional.front();
    request.registry_name = config->publication.registry_name;
    request.state_directory = config->publication.state_directory;
    request.index_worktree = config->publication.index_worktree;
    request.bootstrap_root_keys = config->publication.bootstrap_root_keys;
    request.bootstrap_root_threshold =
        config->publication.bootstrap_root_threshold;
    auto info = action == "verify"
                    ? chtholly::verifyRegistryBackupArchive(request, error)
                    : chtholly::restoreRegistryBackupArchive(request, error);
    if (!info) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    std::cout << "registry\t" << info->registry_name << '\n'
              << "git-head\t" << info->git_head << '\n'
              << "tree-size\t" << info->tree_size << '\n'
              << "tree-root\t" << info->tree_root << '\n'
              << "files\t" << info->files.size() << '\n';
    return 0;
  }
  auto store =
      config
          ? chtholly::RegistryPublicationStore::open(config->publication, error)
          : std::optional<chtholly::RegistryPublicationStore>{};
  if (!store) {
    std::cerr << "chtholly-registryd: " << error << '\n';
    return 1;
  }
  if (action == "serve") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: serve does not accept positional arguments\n";
      return 2;
    }
    const auto result = serve(*config, *store, error);
    if (result != 0 && !error.empty())
      std::cerr << "chtholly-registryd: " << error << '\n';
    return result;
  }
  if (command_group == "recovery" && action == "list") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: recovery list takes no arguments\n";
      return 2;
    }
    auto points = store->listRecoveryPoints(error);
    if (!points) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    for (const auto &point : *points)
      std::cout << point.id << '\t' << point.created_at << '\t'
                << point.retain_until << '\t'
                << (point.released ? "released" : "active") << '\t'
                << point.blob_count << '\n';
    return 0;
  }
  if (command_group == "recovery" && action == "extend") {
    if (arguments.positional.size() != 1 || arguments.retain_until == 0) {
      std::cerr << "chtholly-registryd: recovery extend requires an ID and "
                   "--retain-until\n";
      return 2;
    }
    if (!store->extendRecoveryPoint(
            arguments.positional.front(),
            static_cast<std::int64_t>(arguments.retain_until), error)) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    return 0;
  }
  if (command_group == "recovery" && action == "release") {
    if (arguments.positional.size() != 1 ||
        !store->releaseRecoveryPoint(arguments.positional.front(), error)) {
      std::cerr << "chtholly-registryd: "
                << (error.empty() ? "recovery release requires one ID" : error)
                << '\n';
      return error.empty() ? 2 : 1;
    }
    return 0;
  }
  if (command_group == "gc" && action == "plan") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: gc plan takes no arguments\n";
      return 2;
    }
    auto plan = store->planGarbageCollection(-1, error);
    if (!plan) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    std::cout << "plan\t" << plan->id << '\n'
              << "created-at\t" << plan->created_at << '\n'
              << "candidates\t" << plan->blob_sha256.size() << '\n';
    for (const auto &digest : plan->blob_sha256)
      std::cout << "blob\t" << digest << '\n';
    return 0;
  }
  if (command_group == "gc" && action == "apply") {
    if (arguments.positional.size() != 1 ||
        !store->applyGarbageCollection(arguments.positional.front(), -1,
                                       error)) {
      std::cerr << "chtholly-registryd: "
                << (error.empty() ? "gc apply requires one plan ID" : error)
                << '\n';
      return error.empty() ? 2 : 1;
    }
    return 0;
  }
  if (command_group == "index" && action == "reseal") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: index reseal takes no arguments\n";
      return 2;
    }
    if (!store->resealIndex(-1, error)) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    return 0;
  }
  if (action == "create" && command_group == "token") {
    if (!arguments.positional.empty() || arguments.principal.empty() ||
        (arguments.all_packages == !arguments.packages.empty())) {
      std::cerr << "chtholly-registryd: token create requires --principal and "
                   "exactly one scope form\n";
      return 2;
    }
    chtholly::RegistryPublisherTokenRequest request;
    request.principal = arguments.principal;
    request.packages = std::move(arguments.packages);
    request.all_packages = arguments.all_packages;
    auto token = store->createPublisherToken(request, error);
    if (!token) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    std::cout << *token << '\n';
    return 0;
  }
  if (action == "revoke" && command_group == "token") {
    if (arguments.positional.size() != 1 || arguments.token_id.empty()) {
      std::cerr << "chtholly-registryd: token revoke requires one token ID\n";
      return 2;
    }
    if (!store->revokePublisherToken(arguments.token_id, error)) {
      std::cerr << "chtholly-registryd: "
                << error << '\n';
      return 1;
    }
    return 0;
  }
  if (action == "list" && command_group == "token") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: token list takes no positional arguments\n";
      return 2;
    }
    auto tokens = store->listPublisherTokens(error);
    if (!tokens) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    for (const auto &token : *tokens) {
      std::cout << token.token_id << '\t' << token.principal << '\t'
                << (token.revoked ? "revoked" : "active") << '\t';
      if (token.all_packages) {
        std::cout << '*';
      } else {
        for (std::size_t index = 0; index < token.packages.size(); ++index) {
          if (index != 0)
            std::cout << ',';
          std::cout << token.packages[index];
        }
      }
      std::cout << '\n';
    }
    return 0;
  }
  if (action == "create" && command_group == "operator") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: operator create takes no positional arguments\n";
      return 2;
    }
    chtholly::RegistryOperatorTokenRequest request;
    request.principal = arguments.principal;
    request.capabilities = arguments.capabilities;
    auto token = store->createOperatorToken(request, error);
    if (!token) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    std::cout << *token << '\n';
    return 0;
  }
  if (action == "revoke" && command_group == "operator") {
    if (arguments.positional.size() != 1 || arguments.token_id.empty()) {
      std::cerr
          << "chtholly-registryd: operator revoke requires one token ID\n";
      return 2;
    }
    if (!store->revokeOperatorToken(arguments.token_id, error)) {
      std::cerr << "chtholly-registryd: "
                << error << '\n';
      return 1;
    }
    return 0;
  }
  if (action == "list" && command_group == "operator") {
    if (!arguments.positional.empty()) {
      std::cerr << "chtholly-registryd: operator list takes no positional arguments\n";
      return 2;
    }
    auto tokens = store->listOperatorTokens(error);
    if (!tokens) {
      std::cerr << "chtholly-registryd: " << error << '\n';
      return 1;
    }
    for (const auto &token : *tokens) {
      std::cout << token.token_id << '\t' << token.principal << '\t'
                << (token.revoked ? "revoked" : "active") << '\t';
      for (std::size_t index = 0; index < token.capabilities.size(); ++index) {
        if (index != 0)
          std::cout << ',';
        std::cout << token.capabilities[index];
      }
      std::cout << '\n';
    }
    return 0;
  }
  std::cerr << "chtholly-registryd: unknown " << command_group << " action '"
            << action << "'\n";
  return 2;
}
