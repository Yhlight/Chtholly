#include "chtholly/Driver/ManifestDiscovery.h"
#include "chtholly/Driver/CompilerBuildControlSnapshot.h"
#include "chtholly/Driver/CompilerDaemon.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <limits>
#include <llvm/Support/JSON.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr std::size_t MaxMessageBytes = 16U * 1024U * 1024U;

std::string renderJson(const llvm::json::Value &value) {
  std::string result;
  llvm::raw_string_ostream output(result);
  output << value;
  output.flush();
  return result;
}

std::string idKey(const llvm::json::Value &id) {
  if (const auto integer = id.getAsInteger())
    return "i:" + std::to_string(*integer);
  if (const auto text = id.getAsString())
    return "s:" + text->str();
  return {};
}

bool isHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

unsigned hexValue(char value) {
  if (value >= '0' && value <= '9')
    return static_cast<unsigned>(value - '0');
  return static_cast<unsigned>((value >= 'a' ? value - 'a' : value - 'A') + 10);
}

std::optional<std::string> fileUriToPath(std::string_view uri,
                                         std::string &error) {
  error.clear();
  if (!uri.starts_with("file://")) {
    error = "compiler LSP supports only file URIs";
    return std::nullopt;
  }
  auto encoded = uri.substr(7);
  if (!encoded.empty() && encoded.front() != '/') {
    const auto slash = encoded.find('/');
    const auto authority = encoded.substr(0, slash);
    if (authority != "localhost") {
      error = "compiler LSP does not support remote file URI authorities";
      return std::nullopt;
    }
    encoded = slash == std::string_view::npos ? std::string_view{}
                                              : encoded.substr(slash);
  }
  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    if (encoded[index] != '%') {
      decoded.push_back(encoded[index]);
      continue;
    }
    if (index + 2 >= encoded.size() || !isHex(encoded[index + 1]) ||
        !isHex(encoded[index + 2])) {
      error = "compiler LSP received an invalid percent-encoded file URI";
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((hexValue(encoded[index + 1]) << 4U) |
                                        hexValue(encoded[index + 2])));
    index += 2;
  }
#if defined(_WIN32)
  if (decoded.size() >= 3 && decoded[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':')
    decoded.erase(decoded.begin());
#endif
  const auto normalized = chtholly::normalizeCompilerInputPath(decoded);
  if (normalized.empty()) {
    error = "compiler LSP received an empty file URI path";
    return std::nullopt;
  }
  return normalized;
}

std::string pathToFileUri(std::string_view path) {
  auto generic = std::filesystem::path(std::string(path)).generic_string();
  std::string result = "file://";
#if defined(_WIN32)
  if (generic.size() >= 2 && generic[1] == ':')
    result += '/';
#endif
  constexpr std::string_view Hex = "0123456789ABCDEF";
  for (const auto character : generic) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) || character == '/' || character == ':' ||
        character == '-' || character == '_' || character == '.' ||
        character == '~') {
      result.push_back(character);
    } else {
      result.push_back('%');
      result.push_back(Hex[byte >> 4U]);
      result.push_back(Hex[byte & 0x0FU]);
    }
  }
  return result;
}

std::optional<chtholly::CompilerTextPosition>
parsePosition(const llvm::json::Object *object, std::string &error) {
  if (!object) {
    error = "missing LSP position";
    return std::nullopt;
  }
  const auto line = object->getInteger("line");
  const auto character = object->getInteger("character");
  if (!line || !character || *line < 0 || *character < 0 ||
      *line > std::numeric_limits<std::uint32_t>::max() ||
      *character > std::numeric_limits<std::uint32_t>::max()) {
    error = "invalid LSP position";
    return std::nullopt;
  }
  return chtholly::CompilerTextPosition{static_cast<std::uint32_t>(*line),
                                    static_cast<std::uint32_t>(*character)};
}

std::optional<chtholly::CompilerTextRange>
parseRange(const llvm::json::Object *object, std::string &error) {
  if (!object) {
    error = "missing LSP range";
    return std::nullopt;
  }
  const auto start = parsePosition(object->getObject("start"), error);
  const auto end =
      start ? parsePosition(object->getObject("end"), error) : std::nullopt;
  if (!start || !end)
    return std::nullopt;
  return chtholly::CompilerTextRange{*start, *end};
}

llvm::json::Object renderPosition(chtholly::CompilerTextPosition position) {
  return llvm::json::Object{{"line", position.line},
                            {"character", position.character}};
}

llvm::json::Object renderRange(const chtholly::CompilerTextRange &range) {
  return llvm::json::Object{{"start", renderPosition(range.start)},
                            {"end", renderPosition(range.end)}};
}

llvm::json::Object
renderLocation(const chtholly::CompilerSourceLocation &location) {
  return llvm::json::Object{{"uri", pathToFileUri(location.path)},
                            {"range", renderRange(location.range)}};
}

class Server {
public:
  Server(std::string executable_path, std::string resource_dir)
      : executable_path_(std::move(executable_path)),
        resource_dir_(std::move(resource_dir)) {}
  ~Server() {
    service_.reset();
  }

  int run() {
    while (true) {
      std::string message;
      std::string error;
      const auto read = readMessage(message, error);
      if (!read) {
        if (!error.empty())
          std::cerr << "chtholly-lsp: " << error << '\n';
        return saw_shutdown_ ? 0 : 1;
      }
      auto parsed = llvm::json::parse(message);
      if (!parsed) {
        sendError(llvm::json::Value(nullptr), -32700, "Parse error");
        continue;
      }
      const auto *object = parsed->getAsObject();
      if (!object) {
        sendError(llvm::json::Value(nullptr), -32600, "Invalid Request");
        continue;
      }
      if (!handle(*object))
        return saw_shutdown_ ? 0 : 1;
    }
  }

private:
  bool readMessage(std::string &message, std::string &error) {
    std::optional<std::size_t> length;
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        break;
      const auto colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      auto name = line.substr(0, colon);
      std::ranges::transform(name, name.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      if (name != "content-length")
        continue;
      auto value = line.substr(colon + 1);
      const auto first = value.find_first_not_of(" \t");
      if (first == std::string::npos) {
        error = "empty Content-Length header";
        return false;
      }
      try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value.substr(first), &consumed);
        if (consumed != value.substr(first).size() ||
            parsed > MaxMessageBytes) {
          error = "invalid or oversized Content-Length header";
          return false;
        }
        length = static_cast<std::size_t>(parsed);
      } catch (...) {
        error = "invalid Content-Length header";
        return false;
      }
    }
    if (!std::cin && !length)
      return false;
    if (!length) {
      error = "message omitted Content-Length";
      return false;
    }
    message.resize(*length);
    std::cin.read(message.data(), static_cast<std::streamsize>(*length));
    if (static_cast<std::size_t>(std::cin.gcount()) != *length) {
      error = "truncated JSON-RPC message body";
      return false;
    }
    return true;
  }

  void send(llvm::json::Value value) {
    const auto body = renderJson(value);
    std::lock_guard lock(output_mutex_);
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
  }

  void sendResult(llvm::json::Value id, llvm::json::Value result) {
    send(llvm::json::Object{{"jsonrpc", "2.0"},
                            {"id", std::move(id)},
                            {"result", std::move(result)}});
  }

  void sendError(llvm::json::Value id, std::int64_t code, std::string message) {
    send(llvm::json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::move(id)},
        {"error",
         llvm::json::Object{{"code", code}, {"message", std::move(message)}}}});
  }

  void notify(std::string method, llvm::json::Value params) {
    send(llvm::json::Object{{"jsonrpc", "2.0"},
                            {"method", std::move(method)},
                            {"params", std::move(params)}});
  }

  bool configureService(std::string_view path_hint, std::string &error) {
    if (service_)
      return true;
    auto root = root_path_;
    if (root.empty() && !path_hint.empty())
      root =
          std::filesystem::path(std::string(path_hint)).parent_path().string();
    const auto discovery = chtholly::discoverManifests(root);
    chtholly::CompilerInvocation invocation;
    invocation.executable_path = executable_path_;
    invocation.resource_dir = resource_dir_;
    invocation.action = chtholly::DriverAction::EmitLLVM;
    invocation.workflow = chtholly::DriverWorkflow::Compile;
    invocation.output_path = (std::filesystem::path(root.empty() ? "." : root) /
                              ".chtholly" / "lsp-unused.ll")
                                 .string();
    invocation.jobs =
        std::max<std::size_t>(1, std::thread::hardware_concurrency());
    invocation.jobs_specified = true;
    if (discovery.workspace_manifest)
      invocation.workspace_path =
          discovery.workspace_manifest->parent_path().string();
    else if (discovery.project_manifest)
      invocation.project_path =
          discovery.project_manifest->parent_path().string();
    else if (!path_hint.empty())
      invocation.input_path = std::string(path_hint);
    else {
      error = "compiler LSP needs a workspace manifest or an opened .cns file";
      return false;
    }

    service_ = std::make_unique<chtholly::CompilerLanguageService>(
        std::move(invocation), chtholly::makeCompilerRealInputFileSystem(),
        chtholly::CompilerLanguageServiceCallbacks{
            .publish_diagnostics =
                [this](chtholly::CompilerDiagnosticBatch batch) {
                  publishDiagnostics(std::move(batch));
                },
            .publish_workspace_diagnostic =
                [](chtholly::CompilerRequestId, std::string message) {
                  if (!message.empty())
                    std::cerr << "chtholly-lsp: " << message << '\n';
                },
            .complete_query =
                [this](chtholly::CompilerLanguageQueryResult result) {
                  completeQuery(std::move(result));
                }});
    return true;
  }

  void publishDiagnostics(chtholly::CompilerDiagnosticBatch batch) {
    llvm::json::Array diagnostics;
    std::string_view source;
    if (service_) {
      const auto snapshot = service_->latestPublishedSnapshot();
      if (snapshot) {
        if (const auto *entry = snapshot->sources().find(batch.path))
          source = entry->text();
      }
    }
    for (const auto &diagnostic : batch.diagnostics) {
      chtholly::CompilerTextPosition start;
      chtholly::CompilerTextPosition end;
      std::string error;
      if (!chtholly::compilerTextOffsetToPosition(source, diagnostic.offset, start,
                                              error) ||
          !chtholly::compilerTextOffsetToPosition(
              source, diagnostic.offset + diagnostic.length, end, error)) {
        start = {diagnostic.location.line > 0 ? diagnostic.location.line - 1
                                              : 0,
                 diagnostic.location.column > 0 ? diagnostic.location.column - 1
                                                : 0};
        end = {start.line, start.character + 1};
      }
      int severity = 1;
      switch (diagnostic.level) {
      case chtholly::compiler::DiagnosticLevel::Error:
        severity = 1;
        break;
      case chtholly::compiler::DiagnosticLevel::Warning:
        severity = 2;
        break;
      case chtholly::compiler::DiagnosticLevel::Note:
        severity = 3;
        break;
      }
      llvm::json::Object rendered{
          {"range", renderRange({start, end})},
          {"severity", severity},
          {"code", diagnostic.code},
          {"source", "chtholly-next"},
          {"message", diagnostic.message}};
      if (!diagnostic.related.empty()) {
        llvm::json::Array related;
        for (const auto &item : diagnostic.related) {
          std::string related_source;
          if (service_) {
            const auto snapshot = service_->latestPublishedSnapshot();
            if (snapshot) {
              if (const auto *entry = snapshot->sources().find(item.path))
                related_source = entry->text();
            }
          }
          chtholly::CompilerTextPosition related_start{};
          chtholly::CompilerTextPosition related_end{
              related_start.line, related_start.character};
          std::string related_error;
          const bool source_location_available =
              item.location_available && !related_source.empty();
          if (source_location_available &&
              chtholly::compilerTextOffsetToPosition(
                  related_source, item.offset, related_start, related_error) &&
              chtholly::compilerTextOffsetToPosition(
                  related_source, item.offset + item.length, related_end,
                  related_error)) {}
          auto related_message = item.message + " [" + item.code + "]";
          if (!source_location_available)
            related_message += " (source unavailable)";
          related.push_back(llvm::json::Object{
              {"location", llvm::json::Object{
                                {"uri", pathToFileUri(item.path)},
                                {"range", renderRange({related_start,
                                                         related_end})}}},
              {"message", std::move(related_message)}});
        }
        rendered["relatedInformation"] = std::move(related);
      }
      diagnostics.push_back(std::move(rendered));
    }
    llvm::json::Object params{{"uri", pathToFileUri(batch.path)},
                              {"diagnostics", std::move(diagnostics)}};
    if (batch.version)
      params["version"] = *batch.version;
    notify("textDocument/publishDiagnostics", std::move(params));
  }

  void completeQuery(chtholly::CompilerLanguageQueryResult result) {
    llvm::json::Value client_id(nullptr);
    {
      std::lock_guard lock(request_mutex_);
      const auto found = internal_requests_.find(result.request_id);
      if (found == internal_requests_.end()) {
        early_results_.insert_or_assign(result.request_id, std::move(result));
        return;
      }
      client_id = std::move(found->second);
      client_requests_.erase(idKey(client_id));
      internal_requests_.erase(found);
    }
    if (result.status == chtholly::CompilerDaemonRequestStatus::Cancelled) {
      sendError(std::move(client_id), -32800, "Request cancelled");
      return;
    }
    if (result.status == chtholly::CompilerDaemonRequestStatus::Stale) {
      sendError(std::move(client_id), -32801, "Content modified");
      return;
    }
    if (result.status != chtholly::CompilerDaemonRequestStatus::Succeeded) {
      sendError(std::move(client_id), -32603,
                result.error.empty() ? "compiler language query failed"
                                     : result.error);
      return;
    }
    if (result.kind == chtholly::CompilerLanguageQueryKind::Hover) {
      if (!result.hover) {
        sendResult(std::move(client_id), nullptr);
        return;
      }
      sendResult(std::move(client_id),
                 llvm::json::Object{
                     {"contents",
                      llvm::json::Object{{"kind", "markdown"},
                                         {"value", result.hover->markdown}}},
                     {"range", renderRange(result.hover->range)}});
      return;
    }
    if (result.kind == chtholly::CompilerLanguageQueryKind::Completion) {
      llvm::json::Array items;
      for (const auto &item : result.completion_items) {
        auto detail = item.detail;
        if (detail.starts_with("```chtholly\n") &&
            detail.ends_with("\n```"))
          detail = detail.substr(12, detail.size() - 16);
        items.push_back(llvm::json::Object{
            {"label", item.label},
            {"kind",
             item.kind == chtholly::CompilerCompletionItemKind::InstanceMethod ? 2
                                                                           : 3},
            {"detail", detail}});
      }
      sendResult(std::move(client_id),
                 llvm::json::Object{{"isIncomplete", false},
                                    {"items", std::move(items)}});
      return;
    }
    if (result.kind == chtholly::CompilerLanguageQueryKind::DocumentSymbols) {
      llvm::json::Array symbols;
      for (const auto &symbol : result.document_symbols) {
        int kind = 12;
        if (symbol.kind == chtholly::CompilerDocumentSymbolKind::Constant)
          kind = 14;
        else if (symbol.kind == chtholly::CompilerDocumentSymbolKind::Static)
          kind = 13;
        symbols.push_back(llvm::json::Object{
            {"name", symbol.name},
            {"kind", kind},
            {"range", renderRange(symbol.range)},
            {"selectionRange", renderRange(symbol.selection_range)}});
      }
      sendResult(std::move(client_id), std::move(symbols));
      return;
    }
    if (result.kind == chtholly::CompilerLanguageQueryKind::PrepareRename) {
      if (!result.rename) {
        sendResult(std::move(client_id), nullptr);
        return;
      }
      sendResult(std::move(client_id),
                 llvm::json::Object{
                     {"range", renderRange(result.rename->range)},
                     {"placeholder", result.rename->placeholder}});
      return;
    }
    if (result.kind == chtholly::CompilerLanguageQueryKind::Rename) {
      if (!result.rename) {
        sendError(std::move(client_id), -32602,
                  result.error.empty() ? "the selected symbol cannot be renamed"
                                       : result.error);
        return;
      }
      std::map<std::string, llvm::json::Array> edits_by_uri;
      for (const auto &location : result.rename->locations) {
        edits_by_uri[pathToFileUri(location.path)].push_back(
            llvm::json::Object{{"range", renderRange(location.range)},
                               {"newText", result.rename_text}});
      }
      llvm::json::Object changes;
      for (auto &[uri, edits] : edits_by_uri)
        changes.try_emplace(uri, std::move(edits));
      sendResult(std::move(client_id),
                 llvm::json::Object{{"changes", std::move(changes)}});
      return;
    }
    llvm::json::Array locations;
    for (const auto &location : result.locations)
      locations.push_back(renderLocation(location));
    sendResult(std::move(client_id), std::move(locations));
  }

  void registerQuery(llvm::json::Value client_id,
                     chtholly::CompilerRequestId internal_id) {
    std::optional<chtholly::CompilerLanguageQueryResult> early;
    {
      std::lock_guard lock(request_mutex_);
      const auto key = idKey(client_id);
      client_requests_[key] = internal_id;
      internal_requests_.emplace(internal_id, std::move(client_id));
      if (const auto found = early_results_.find(internal_id);
          found != early_results_.end()) {
        early = std::move(found->second);
        early_results_.erase(found);
      }
    }
    if (early)
      completeQuery(std::move(*early));
  }

  bool handle(const llvm::json::Object &message) {
    const auto version = message.getString("jsonrpc");
    const auto method = message.getString("method");
    const auto *id = message.get("id");
    if (!version || *version != "2.0" || !method) {
      sendError(id ? llvm::json::Value(*id) : llvm::json::Value(nullptr),
                -32600, "Invalid Request");
      return true;
    }
    const auto *params = message.getObject("params");
    if (*method == "exit")
      return false;
    if (*method == "initialize") {
      if (!id) {
        std::cerr << "chtholly-lsp: initialize must be a request\n";
        return true;
      }
      if (initialized_) {
        sendError(llvm::json::Value(*id), -32600,
                  "compiler LSP is already initialized");
        return true;
      }
      std::string uri;
      if (params) {
        if (const auto *folders = params->getArray("workspaceFolders");
            folders && !folders->empty()) {
          if (const auto *folder = folders->front().getAsObject()) {
            if (const auto value = folder->getString("uri"))
              uri = value->str();
          }
        }
        if (uri.empty()) {
          if (const auto value = params->getString("rootUri"))
            uri = value->str();
        }
      }
      std::string error;
      if (!uri.empty()) {
        const auto path = fileUriToPath(uri, error);
        if (!path) {
          sendError(llvm::json::Value(*id), -32602, std::move(error));
          return true;
        }
        root_path_ = *path;
      }
      initialized_ = true;
      (void)configureService({}, error);
      sendResult(
          llvm::json::Value(*id),
          llvm::json::Object{
              {"capabilities",
               llvm::json::Object{
                   {"positionEncoding", "utf-16"},
                   {"textDocumentSync",
                    llvm::json::Object{{"openClose", true}, {"change", 2}}},
                   {"hoverProvider", true},
                   {"definitionProvider", true},
                   {"referencesProvider", true},
                   {"documentSymbolProvider", true},
                   {"renameProvider",
                    llvm::json::Object{{"prepareProvider", true}}},
                   {"codeActionProvider",
                    llvm::json::Object{
                        {"codeActionKinds", llvm::json::Array{"quickfix"}},
                        {"resolveProvider", false}}},
                   {"completionProvider",
                    llvm::json::Object{
                        {"triggerCharacters",
                         llvm::json::Array{".", ":"}}}}}},
              {"serverInfo", llvm::json::Object{{"name", "chtholly-lsp"},
                                                {"version", "next"}}}});
      return true;
    }
    if (!initialized_) {
      if (id)
        sendError(llvm::json::Value(*id), -32002,
                  "compiler LSP has not been initialized");
      return true;
    }
    if (saw_shutdown_) {
      if (id)
        sendError(llvm::json::Value(*id), -32600,
                  "compiler LSP has already shut down");
      return true;
    }
    if (*method == "initialized")
      return true;
    if (*method == "shutdown") {
      if (id) {
        saw_shutdown_ = true;
        sendResult(llvm::json::Value(*id), nullptr);
      }
      return true;
    }
    if (*method == "$/cancelRequest") {
      if (!params || !params->get("id"))
        return true;
      const auto key = idKey(*params->get("id"));
      chtholly::CompilerRequestId internal = 0;
      {
        std::lock_guard lock(request_mutex_);
        if (const auto found = client_requests_.find(key);
            found != client_requests_.end())
          internal = found->second;
      }
      if (internal && service_)
        service_->cancelRequest(internal);
      return true;
    }
    if (*method == "workspace/didChangeWatchedFiles") {
      if (service_)
        (void)service_->requestDiagnostics();
      return true;
    }
    if (*method == "textDocument/didOpen") {
      const auto *document =
          params ? params->getObject("textDocument") : nullptr;
      const auto uri = document ? document->getString("uri") : std::nullopt;
      const auto text = document ? document->getString("text") : std::nullopt;
      const auto version_number =
          document ? document->getInteger("version") : std::nullopt;
      std::string error;
      const auto path = uri ? fileUriToPath(*uri, error) : std::nullopt;
      if (!path || !text || !version_number ||
          !configureService(path ? *path : std::string_view{}, error) ||
          !service_->openDocument(*path, *version_number, text->str(), error)) {
        std::cerr << "chtholly-lsp: didOpen rejected: " << error << '\n';
      } else {
        (void)service_->requestDiagnostics();
      }
      return true;
    }
    if (*method == "textDocument/didChange") {
      const auto *document =
          params ? params->getObject("textDocument") : nullptr;
      const auto uri = document ? document->getString("uri") : std::nullopt;
      const auto version_number =
          document ? document->getInteger("version") : std::nullopt;
      const auto *raw_changes =
          params ? params->getArray("contentChanges") : nullptr;
      std::string error;
      const auto path = uri ? fileUriToPath(*uri, error) : std::nullopt;
      std::vector<chtholly::CompilerTextDocumentContentChange> changes;
      if (raw_changes) {
        for (const auto &raw : *raw_changes) {
          const auto *change = raw.getAsObject();
          const auto text = change ? change->getString("text") : std::nullopt;
          if (!change || !text) {
            error = "invalid didChange content change";
            break;
          }
          chtholly::CompilerTextDocumentContentChange parsed{.text = text->str()};
          if (const auto *range = change->getObject("range")) {
            parsed.range = parseRange(range, error);
            if (!parsed.range)
              break;
          }
          if (const auto length = change->getInteger("rangeLength")) {
            if (*length < 0 ||
                *length > std::numeric_limits<std::uint32_t>::max()) {
              error = "invalid didChange rangeLength";
              break;
            }
            parsed.range_length = static_cast<std::uint32_t>(*length);
          }
          changes.push_back(std::move(parsed));
        }
      }
      if (!path || !version_number || !raw_changes || !error.empty() ||
          !service_ ||
          !service_->changeDocument(*path, *version_number, changes, error)) {
        std::cerr << "chtholly-lsp: didChange rejected: " << error << '\n';
      } else {
        (void)service_->requestDiagnostics();
      }
      return true;
    }
    if (*method == "textDocument/didClose") {
      const auto *document =
          params ? params->getObject("textDocument") : nullptr;
      const auto uri = document ? document->getString("uri") : std::nullopt;
      std::string error;
      const auto path = uri ? fileUriToPath(*uri, error) : std::nullopt;
      if (!path || !service_ || !service_->closeDocument(*path, error))
        std::cerr << "chtholly-lsp: didClose rejected: " << error << '\n';
      return true;
    }
    if (*method == "textDocument/codeAction") {
      if (!id) return true;
      const auto *document =
          params ? params->getObject("textDocument") : nullptr;
      const auto uri = document ? document->getString("uri") : std::nullopt;
      const auto *context = params ? params->getObject("context") : nullptr;
      const auto *diagnostics = context ? context->getArray("diagnostics")
                                        : nullptr;
      if (!uri || !diagnostics) {
        sendError(llvm::json::Value(*id), -32602,
                  "codeAction requires a document and diagnostics");
        return true;
      }
      struct ImportFix {
        std::string title;
        std::string text;
        bool at_diagnostic = false;
      };
      const std::map<std::string, ImportFix> fixes = {
          {"chtholly.next.sem.operator.missing-import",
           {"Import std::ops", "import std::ops;\n"}},
          {"chtholly.next.sem.ordering.missing-import",
           {"Import std::compare", "import std::compare;\n"}},
          {"chtholly.next.sem.checked-cast.missing-imports",
           {"Import checked-cast modules",
            "import std::result;\nimport std::convert;\n"}},
          {"chtholly.next.sem.async.missing-result-import",
           {"Import std::result", "import std::result;\n"}},
          {"chtholly.next.sem.cffi.missing-result-import",
           {"Import std::result", "import std::result;\n"}},
          {"chtholly.next.sem.cffi.missing-outcome-import",
           {"Import IO and Result modules",
            "import std::io;\nimport std::result;\n"}},
          {"chtholly.next.sem.method.explicit-move-required",
           {"Insert explicit move", "move ", true}},
      };
      std::set<std::string> emitted;
      llvm::json::Array actions;
      for (const auto &raw : *diagnostics) {
        const auto *diagnostic = raw.getAsObject();
        const auto code = diagnostic ? diagnostic->getString("code")
                                     : std::nullopt;
        if (!code) continue;
        const auto fix = fixes.find(code->str());
        if (fix == fixes.end() || !emitted.insert(code->str()).second)
          continue;
        auto edit_range = chtholly::CompilerTextRange{{1, 0}, {1, 0}};
        if (fix->second.at_diagnostic) {
          std::string range_error;
          const auto diagnostic_range =
              parseRange(diagnostic->getObject("range"), range_error);
          if (!diagnostic_range)
            continue;
          edit_range = *diagnostic_range;
          edit_range.end = edit_range.start;
        }
        llvm::json::Array edits;
        edits.push_back(llvm::json::Object{
            {"range", renderRange(edit_range)},
            {"newText", fix->second.text}});
        llvm::json::Object changes;
        changes.try_emplace(uri->str(), std::move(edits));
        actions.push_back(llvm::json::Object{
            {"title", fix->second.title},
            {"kind", "quickfix"},
            {"isPreferred", true},
            {"edit", llvm::json::Object{{"changes", std::move(changes)}}}});
      }
      sendResult(llvm::json::Value(*id), std::move(actions));
      return true;
    }
    if (*method == "textDocument/hover" ||
        *method == "textDocument/definition" ||
        *method == "textDocument/references" ||
        *method == "textDocument/completion" ||
        *method == "textDocument/documentSymbol" ||
        *method == "textDocument/prepareRename" ||
        *method == "textDocument/rename") {
      if (!id) {
        return true;
      }
      const auto *document =
          params ? params->getObject("textDocument") : nullptr;
      const auto uri = document ? document->getString("uri") : std::nullopt;
      std::string error;
      const auto path = uri ? fileUriToPath(*uri, error) : std::nullopt;
      const bool needs_position =
          *method != "textDocument/documentSymbol";
      const auto position = needs_position && params
                                ? parsePosition(params->getObject("position"),
                                                error)
                                : std::optional(chtholly::CompilerTextPosition{});
      if (!path || !position || !service_) {
        sendError(llvm::json::Value(*id), -32602,
                  error.empty() ? "invalid text document query" : error);
        return true;
      }
      chtholly::CompilerRequestId internal = 0;
      if (*method == "textDocument/hover")
        internal = service_->requestHover(*path, *position);
      else if (*method == "textDocument/definition")
        internal = service_->requestDefinition(*path, *position);
      else if (*method == "textDocument/completion")
        internal = service_->requestCompletion(*path, *position);
      else if (*method == "textDocument/documentSymbol")
        internal = service_->requestDocumentSymbols(*path);
      else if (*method == "textDocument/prepareRename")
        internal = service_->requestPrepareRename(*path, *position);
      else if (*method == "textDocument/rename") {
        const auto new_name = params ? params->getString("newName")
                                     : std::nullopt;
        if (!new_name) {
          sendError(llvm::json::Value(*id), -32602,
                    "rename requires newName");
          return true;
        }
        internal =
            service_->requestRename(*path, *position, new_name->str());
      }
      else {
        bool include_declaration = false;
        if (const auto *context = params->getObject("context"))
          include_declaration =
              context->getBoolean("includeDeclaration").value_or(false);
        internal =
            service_->requestReferences(*path, *position, include_declaration);
      }
      registerQuery(llvm::json::Value(*id), internal);
      return true;
    }
    if (id)
      sendError(llvm::json::Value(*id), -32601, "Method not found");
    else
      std::cerr << "chtholly-lsp: ignored unsupported notification "
                << method->str() << '\n';
    return true;
  }

  std::string executable_path_;
  std::string resource_dir_;
  std::string root_path_;
  bool initialized_ = false;
  bool saw_shutdown_ = false;
  std::unique_ptr<chtholly::CompilerLanguageService> service_;
  std::mutex output_mutex_;
  std::mutex request_mutex_;
  std::unordered_map<std::string, chtholly::CompilerRequestId> client_requests_;
  std::unordered_map<chtholly::CompilerRequestId, llvm::json::Value>
      internal_requests_;
  std::unordered_map<chtholly::CompilerRequestId, chtholly::CompilerLanguageQueryResult>
      early_results_;
};

} // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  std::string resource_dir;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--resource-dir" && index + 1 < argc) {
      resource_dir = argv[++index];
      continue;
    }
    std::cerr << "usage: chtholly-lsp [--resource-dir <path>]\n";
    return 2;
  }
  return Server(argc > 0 ? argv[0] : "chtholly-lsp", std::move(resource_dir))
      .run();
}
