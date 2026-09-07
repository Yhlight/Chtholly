#include "chtholly/component_loader_v1.h"
#include "chtholly/next_host_v1.h"
#include "chtholly/Driver/ComponentDeployment.h"
#include "deployment_manifest.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t FrameSize = 36;
constexpr std::size_t PayloadSize = 24;
constexpr std::size_t BatchFrames = 128;

struct Frame {
  std::uint64_t timestamp = 0;
  std::int32_t sensor = 0;
  std::int32_t value = 0;
  std::uint64_t sequence = 0;
};

using Sample = Frame;

// Host-side fallback used until the std::typed_channel source facade is
// selected by the compiler. Keeping the descriptor and lifecycle callbacks in
// one generic owner mirrors the compiler ABI and prevents business logic from
// depending on byte transport details.
template <typename T> class Channel {
public:
  ~Channel() { close(); }
  bool init(std::uint64_t capacity) {
    const chtholly_next_typed_channel_descriptor descriptor{
        CHTHOLLY_NEXT_TYPED_CHANNEL_ABI_V1,
        CHTHOLLY_NEXT_TYPED_CHANNEL_SEND | CHTHOLLY_NEXT_TYPED_CHANNEL_SYNC,
        sizeof(T), alignof(T), &move, &drop};
    return chtholly_next_host_v1_typed_channel_init(capacity, &descriptor,
                                                     &channel_) == 0;
  }
  bool send(T& value) {
    chtholly_next_typed_channel_token token{};
    if (chtholly_next_host_v1_typed_channel_send_prepare(channel_, &value,
                                                          &token) != 0)
      return false;
    if (chtholly_next_host_v1_typed_channel_send_commit(&token) != 0) {
      (void)chtholly_next_host_v1_typed_channel_send_cancel(&token);
      return false;
    }
    return true;
  }
  bool receive(T& value) {
    chtholly_next_typed_channel_token token{};
    if (chtholly_next_host_v1_typed_channel_receive_acquire(channel_, &token) !=
        0)
      return false;
    if (chtholly_next_host_v1_typed_channel_receive_commit(&token, &value) !=
        0) {
      (void)chtholly_next_host_v1_typed_channel_receive_cancel(&token);
      return false;
    }
    return true;
  }
  void close() {
    if (channel_ != nullptr) {
      (void)chtholly_next_host_v1_typed_channel_close(channel_);
      channel_ = nullptr;
    }
  }

private:
  static void move(void* destination, void* source) {
    auto* to = static_cast<T*>(destination);
    auto* from = static_cast<T*>(source);
    *to = std::move(*from);
    *from = T{};
  }
  static void drop(void* value) {
    if constexpr (!std::is_trivially_destructible_v<T>)
      static_cast<T*>(value)->~T();
  }
  void* channel_ = nullptr;
};

struct Component {
  chtholly_component_module_v1* module = nullptr;
  std::array<std::uint8_t, 32> export_id{};
  std::array<char, 512> diagnostic{};
  std::uint64_t diagnostic_size = 0;
};

void print_error(std::string_view stage, std::string_view detail) {
  std::fprintf(stderr, "telemetry ingest: %.*s: %.*s\n",
               static_cast<int>(stage.size()), stage.data(),
               static_cast<int>(detail.size()), detail.data());
}

std::string trim_copy(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool read_config(const std::filesystem::path& path,
                 std::map<std::string, std::string>& values,
                 std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "unable to open config";
    return false;
  }
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim_copy(std::move(line));
    if (line.empty() || line.front() == '#')
      continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      error = "config line " + std::to_string(line_number) +
              " is missing '='";
      return false;
    }
    auto key = trim_copy(line.substr(0, separator));
    auto value = trim_copy(line.substr(separator + 1));
    if (key.empty() || value.empty() ||
        (key != "file" && key != "listen" && key != "output")) {
      error = "config line " + std::to_string(line_number) +
              " has an unknown or empty key";
      return false;
    }
    if (!values.emplace(std::move(key), std::move(value)).second) {
      error = "config contains a duplicate key";
      return false;
    }
  }
  return true;
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* bytes) {
  std::uint64_t result = 0;
  for (unsigned index = 0; index < 8; ++index)
    result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  return result;
}

bool parse_frame(const std::uint8_t* bytes, Frame& frame,
                 std::string& error) {
  if (bytes[0] != 67 || bytes[1] != 72 || bytes[2] != 84 || bytes[3] != 77) {
    error = "invalid magic";
    return false;
  }
  if (read_u16(bytes + 4) != 1) {
    error = "unsupported version";
    return false;
  }
  if (read_u16(bytes + 6) != 0 || read_u32(bytes + 8) != PayloadSize) {
    error = "invalid frame header";
    return false;
  }
  frame.timestamp = read_u64(bytes + 12);
  frame.sensor = static_cast<std::int32_t>(read_u32(bytes + 20));
  frame.value = static_cast<std::int32_t>(read_u32(bytes + 24));
  frame.sequence = read_u64(bytes + 28);
  return true;
}

bool read_contract(const std::filesystem::path& path, std::string& identity,
                   std::array<std::uint8_t, 32>& digest) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  const auto read_line = [&](std::string_view key, std::string& value) {
    const auto begin = text.find(key);
    if (begin == std::string::npos)
      return false;
    const auto value_begin = begin + key.size();
    const auto end = text.find('\n', value_begin);
    value = text.substr(value_begin, end == std::string::npos
                                      ? std::string::npos
                                      : end - value_begin);
    return !value.empty();
  };
  std::string hex;
  if (!read_line("identity\t", identity) ||
      !read_line("contract-digest\t", hex) || hex.size() != 64)
    return false;
  const auto hex_value = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < digest.size(); ++index) {
    const int high = hex_value(hex[index * 2]);
    const int low = hex_value(hex[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    digest[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

bool load_component(const char* library, const char* contract,
                    const char* target, const char* runtime,
                    Component& component) {
  std::string identity;
  std::array<std::uint8_t, 32> digest{};
  if (!read_contract(contract, identity, digest)) {
    print_error("contract", "invalid deployment contract");
    return false;
  }
  chtholly_component_requirement_v1 requirement{};
  if (chtholly_component_requirement_init_v1(
          identity.data(), identity.size(), digest.data(), target,
          std::strlen(target), runtime, std::strlen(runtime), &requirement,
          component.diagnostic.data(), component.diagnostic.size(),
          &component.diagnostic_size) != CHTHOLLY_COMPONENT_LOADER_OK_V1) {
    print_error("requirement", component.diagnostic.data());
    return false;
  }
  if (chtholly_component_load_v1(
          library, &requirement, &component.module, component.diagnostic.data(),
          component.diagnostic.size(), &component.diagnostic_size) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1) {
    print_error("load", component.diagnostic.data());
    return false;
  }
  std::uint64_t count = 0;
  chtholly_component_export_info_v1 info{};
  info.struct_size = sizeof(info);
  std::array<char, 256> name{};
  std::uint64_t name_size = 0;
  if (chtholly_component_export_count_v1(component.module, &count) !=
          CHTHOLLY_COMPONENT_LOADER_OK_V1 || count != 1 ||
      chtholly_component_export_info_v1_get(
          component.module, 0, &info, name.data(), name.size(), &name_size) !=
          CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      std::string_view(name.data(), name_size) !=
          "telemetry::component::checksum") {
    print_error("export", "checksum export is unavailable");
    (void)chtholly_component_unload_v1(component.module, nullptr, 0, nullptr);
    component.module = nullptr;
    return false;
  }
  std::memcpy(component.export_id.data(), info.export_id,
              component.export_id.size());
  return true;
}

bool checksum(Component& component, const std::uint8_t* bytes, std::size_t size,
              std::uint64_t& value) {
  chtholly_component_value_v1 argument{};
  argument.struct_size = sizeof(argument);
  argument.kind = CHTHOLLY_COMPONENT_VALUE_BYTES_V1;
  argument.payload.bytes = {bytes, size};
  chtholly_component_value_v1 result{};
  result.struct_size = sizeof(result);
  if (chtholly_component_invoke_v1(
          component.module, component.export_id.data(), &argument, 1, &result,
          component.diagnostic.data(), component.diagnostic.size(),
          &component.diagnostic_size) != CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      result.kind != CHTHOLLY_COMPONENT_VALUE_U64_V1) {
    print_error("checksum", component.diagnostic.data());
    return false;
  }
  value = result.payload.bits;
  return true;
}

bool process_frames(Component& component, const std::vector<std::uint8_t>& data,
                    std::uint64_t& frames, std::uint64_t& batches,
                    std::uint64_t& checksum_total, std::int64_t& value_total,
                    std::uint64_t& first_timestamp,
                    std::uint64_t& last_timestamp, std::uint64_t& gaps) {
  if (data.size() % FrameSize != 0) {
    print_error("input", "truncated frame");
    return false;
  }
  Channel<Sample> typed;
  if (!typed.init(BatchFrames)) {
    print_error("channel", "typed channel initialization failed");
    return false;
  }
  std::vector<std::uint8_t> batch;
  batch.reserve(FrameSize * BatchFrames);
  std::uint64_t prior_sequence = 0;
  bool have_prior = false;
  for (std::size_t offset = 0; offset < data.size(); offset += FrameSize) {
    Frame parsed{};
    std::string error;
    if (!parse_frame(data.data() + offset, parsed, error)) {
      print_error("input", error);
      return false;
    }
    if (!typed.send(parsed)) {
      print_error("channel", "typed send failed");
      return false;
    }
    Frame frame{};
    if (!typed.receive(frame)) {
      print_error("channel", "typed receive failed");
      return false;
    }
    if (have_prior && frame.sequence <= prior_sequence) {
      print_error("input", "sequence regression");
      return false;
    }
    if (have_prior && frame.sequence != prior_sequence + 1)
      gaps += frame.sequence - prior_sequence - 1;
    prior_sequence = frame.sequence;
    have_prior = true;
    if (frames == 0)
      first_timestamp = frame.timestamp;
    last_timestamp = frame.timestamp;
    value_total += frame.value;
    ++frames;
    batch.insert(batch.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                 data.begin() + static_cast<std::ptrdiff_t>(offset + FrameSize));
    if (frames % BatchFrames == 0) {
      std::uint64_t value = 0;
      if (!checksum(component, batch.data(), batch.size(), value))
        return false;
      checksum_total += value;
      ++batches;
      batch.clear();
    }
  }
  if (!batch.empty()) {
    std::uint64_t value = 0;
    if (!checksum(component, batch.data(), batch.size(), value))
      return false;
    checksum_total += value;
    ++batches;
  }
  typed.close();
  return true;
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket InvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket InvalidSocket = -1;
#endif

void close_socket(Socket socket) {
  if (socket == InvalidSocket) return;
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

bool read_tcp(std::uint16_t port, std::vector<std::uint8_t>& data) {
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    print_error("socket", "WSAStartup failed");
    return false;
  }
#endif
  Socket listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener == InvalidSocket) {
    print_error("socket", "listen socket creation failed");
#if defined(_WIN32)
    WSACleanup();
#endif
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
           sizeof(address)) != 0 || listen(listener, 1) != 0) {
    print_error("socket", "bind/listen failed");
    close_socket(listener);
#if defined(_WIN32)
    WSACleanup();
#endif
    return false;
  }
  Socket connection = accept(listener, nullptr, nullptr);
  close_socket(listener);
  if (connection == InvalidSocket) {
    print_error("socket", "accept failed");
#if defined(_WIN32)
    WSACleanup();
#endif
    return false;
  }
  std::array<std::uint8_t, 4096> buffer{};
  for (;;) {
#if defined(_WIN32)
    const int count = recv(connection, reinterpret_cast<char*>(buffer.data()),
                           static_cast<int>(buffer.size()), 0);
#else
    const auto count = recv(connection, buffer.data(), buffer.size(), 0);
#endif
    if (count == 0) break;
    if (count < 0) {
      print_error("socket", "read failed");
      close_socket(connection);
#if defined(_WIN32)
      WSACleanup();
#endif
      return false;
    }
    data.insert(data.end(), buffer.begin(), buffer.begin() + count);
  }
  close_socket(connection);
#if defined(_WIN32)
  WSACleanup();
#endif
  return true;
}

bool buffer_through_channel(const std::vector<std::uint8_t>& input,
                            std::vector<std::uint8_t>& output) {
  constexpr std::uint64_t ChannelCapacity = 64 * 1024;
  constexpr std::size_t ChunkSize = 4096;
  void* channel = nullptr;
  if (chtholly_next_host_v1_channel_init(ChannelCapacity, &channel) != 0)
    return false;
  output.clear();
  output.reserve(input.size());
  std::thread producer([&] {
    for (std::size_t offset = 0; offset < input.size(); offset += ChunkSize) {
      const auto remaining = input.size() - offset;
      const auto size = remaining < ChunkSize ? remaining : ChunkSize;
      if (chtholly_next_host_v1_channel_send(channel, input.data() + offset,
                                             size) != 0)
        return;
    }
  });
  std::array<std::uint8_t, ChunkSize> buffer{};
  while (output.size() < input.size()) {
    std::uint64_t received = 0;
    if (chtholly_next_host_v1_channel_receive(
            channel, buffer.data(), buffer.size(), &received) != 0) {
      producer.join();
      (void)chtholly_next_host_v1_channel_close(channel);
      return false;
    }
    output.insert(output.end(), buffer.begin(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(received));
  }
  producer.join();
  const bool closed = chtholly_next_host_v1_channel_close(channel) == 0;
  return closed && output == input;
}

int usage() {
  std::fprintf(stderr,
               "usage: telemetry-ingest [--deployment MANIFEST | --deployment-root ROOT] "
               "[LIB CONTRACT TARGET RUNTIME] "
               "[--config PATH] (--file PATH | --listen PORT) --output PATH\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  std::string library_value, contract_value, target_value, runtime_value;
  std::string deployment_version;
  int option_start = 5;
  if (std::string_view(argv[1]) == "--deployment" ||
      std::string_view(argv[1]) == "--deployment-root") {
    TelemetryDeploymentManifest deployment;
    std::string error;
    if (std::string_view(argv[1]) == "--deployment") {
      if (!loadTelemetryDeploymentManifest(argv[2], deployment, error)) {
        print_error("deployment", error);
        return 1;
      }
    } else {
      chtholly::ComponentGenerationInfo generation;
      if (!chtholly::activeComponentGeneration(argv[2], generation, error)) {
        print_error("deployment", error);
        return 1;
      }
      deployment = std::move(generation.manifest);
    }
    std::string contract_identity;
    std::array<std::uint8_t, 32> contract_digest{};
    if (!read_contract(deployment.contract, contract_identity,
                       contract_digest) ||
        contract_identity != deployment.identity ||
        telemetryDigestHex(contract_digest) != deployment.contract_digest) {
      print_error("deployment", "contract identity or digest mismatch");
      return 1;
    }
    library_value = deployment.library.string();
    contract_value = deployment.contract.string();
    target_value = deployment.target;
    runtime_value = deployment.runtime;
    deployment_version = deployment.version;
    option_start = 3;
  } else {
    if (argc < 8) return usage();
    library_value = argv[1];
    contract_value = argv[2];
    target_value = argv[3];
    runtime_value = argv[4];
  }
  const char* library = library_value.c_str();
  const char* contract = contract_value.c_str();
  const char* target = target_value.c_str();
  const char* runtime = runtime_value.c_str();
  std::filesystem::path file;
  std::uint16_t port = 0;
  bool use_file = false;
  std::filesystem::path output;
  std::filesystem::path config;
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view(argv[index]) == "--config") {
      config = argv[index + 1];
      break;
    }
  }
  if (!config.empty()) {
    std::map<std::string, std::string> values;
    std::string error;
    if (!read_config(config, values, error)) {
      print_error("config", error);
      return 1;
    }
    if (const auto found = values.find("file"); found != values.end()) {
      file = found->second;
      use_file = true;
    }
    if (const auto found = values.find("listen"); found != values.end()) {
      const auto parsed = std::strtoul(found->second.c_str(), nullptr, 10);
      if (parsed == 0 || parsed > 65535 || use_file) {
        print_error("config", "listen must be a port without file");
        return 1;
      }
      port = static_cast<std::uint16_t>(parsed);
    }
    if (const auto found = values.find("output"); found != values.end())
      output = found->second;
  }
  for (int index = option_start; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--config" && index + 1 < argc) {
      ++index;
    } else if (option == "--file" && index + 1 < argc) {
      file = argv[++index];
      use_file = true;
    } else if (option == "--listen" && index + 1 < argc) {
      const auto parsed = std::strtoul(argv[++index], nullptr, 10);
      if (parsed > 65535) return usage();
      port = static_cast<std::uint16_t>(parsed);
      use_file = false;
    } else if (option == "--output" && index + 1 < argc) {
      output = argv[++index];
    } else {
      return usage();
    }
  }
  if (output.empty() || (use_file == file.empty()) || (!use_file && port == 0))
    return usage();

  const auto started = std::chrono::steady_clock::now();
  std::vector<std::uint8_t> input;
  if (use_file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
      print_error("input", "unable to open file");
      return 1;
    }
    input.assign(std::istreambuf_iterator<char>(stream),
                 std::istreambuf_iterator<char>());
  } else if (!read_tcp(port, input)) {
    return 1;
  }
  std::vector<std::uint8_t> channel_input;
  if (!buffer_through_channel(input, channel_input)) {
    print_error("channel", "input buffering failed");
    return 1;
  }
  input.swap(channel_input);

  Component component;
  if (!load_component(library, contract, target, runtime, component))
    return 1;
  std::uint64_t frames = 0, batches = 0, checksum_total = 0;
  std::int64_t value_total = 0;
  std::uint64_t first_timestamp = 0, last_timestamp = 0, gaps = 0;
  const bool processed = process_frames(
      component, input, frames, batches, checksum_total, value_total,
      first_timestamp, last_timestamp, gaps);
  const auto close = chtholly_component_close_v1(
      component.module, component.diagnostic.data(), component.diagnostic.size(),
      &component.diagnostic_size);
  const auto release = chtholly_component_release_v1(component.module);
  if (!processed || close != CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      release != CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return 1;

  std::ofstream report(output, std::ios::binary | std::ios::trunc);
  if (!report) {
    print_error("output", "unable to write report");
    return 1;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started).count();
  const std::string report_prefix =
      "{\"schema\":\"chtholly-telemetry-ingest-v1\",";
  report << report_prefix
          << "\"deployment_version\":\"" << deployment_version << "\","
          << "\"frames\":" << frames << ",\"batches\":" << batches
          << ",\"checksum\":" << checksum_total
          << ",\"value_sum\":" << value_total
          << ",\"sequence_gaps\":" << gaps
          << ",\"first_timestamp\":" << first_timestamp
          << ",\"last_timestamp\":" << last_timestamp
          << ",\"input_bytes\":" << input.size()
          << ",\"elapsed_ms\":" << elapsed_ms << "}\n";
  std::printf("{\"schema\":\"chtholly-telemetry-ingest-v1\","
              "\"deployment_version\":\"%s\","
              "\"frames\":%llu,\"batches\":%llu,\"checksum\":%llu,"
              "\"value_sum\":%lld,\"sequence_gaps\":%llu,"
              "\"input_bytes\":%llu,\"elapsed_ms\":%lld}\n",
              deployment_version.c_str(),
              static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(batches),
              static_cast<unsigned long long>(checksum_total),
              static_cast<long long>(value_total),
              static_cast<unsigned long long>(gaps),
              static_cast<unsigned long long>(input.size()),
              static_cast<long long>(elapsed_ms));
  return 0;
}
