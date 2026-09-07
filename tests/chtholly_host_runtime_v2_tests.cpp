#include "chtholly/next_host_v2.h"
#include "chtholly/next_runtime_v2.h"

#include "test_check.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <thread>

int main() {
  void *allocation = nullptr;
  if (chtholly_next_runtime_v2_allocate(64, 16, &allocation) !=
          CHTHOLLY_NEXT_RUNTIME_V2_OK ||
      allocation == nullptr)
    return 5;
  chtholly_next_runtime_v2_deallocate(allocation, 64, 16);
  allocation = reinterpret_cast<void *>(1);
  if (chtholly_next_runtime_v2_allocate(64, 3, &allocation) !=
          CHTHOLLY_NEXT_RUNTIME_V2_INVALID_ARGUMENT ||
      allocation != nullptr)
    return 6;
  const auto host = reinterpret_cast<const std::uint8_t *>("127.0.0.1");
  chtholly_next_host_v2_endpoint endpoint{};
  const auto resolve_status =
      chtholly_next_host_v2_resolve(host, 9, 1, &endpoint);
  if (resolve_status != 0)
    return 1;
  CHTHOLLY_TEST_CHECK(resolve_status == 0);
  CHTHOLLY_TEST_CHECK(endpoint.family == 4);
  CHTHOLLY_TEST_CHECK(endpoint.port == 1);
  chtholly_next_host_v2_endpoint listen_endpoint{};
  listen_endpoint.family = 4;
  listen_endpoint.address[0] = 127;
  listen_endpoint.address[1] = 0;
  listen_endpoint.address[2] = 0;
  listen_endpoint.address[3] = 1;
  listen_endpoint.port = 39123;
  void *listener = nullptr;
  if (chtholly_next_host_v2_bind(&listen_endpoint, &listener) != 0)
    return 2;
  std::thread client([&] {
    void *stream = nullptr;
    if (chtholly_next_host_v2_connect(&listen_endpoint, 2000, &stream) != 0)
      return;
    const std::uint8_t payload[] = {'o', 'k'};
    (void)chtholly_next_host_v2_write(stream, payload, sizeof(payload), 2000);
    (void)chtholly_next_host_v2_close(stream);
  });
  void *stream = nullptr;
  const auto accept_status =
      chtholly_next_host_v2_accept(listener, 2000, &stream);
  if (accept_status != 0) {
    client.join();
    (void)chtholly_next_host_v2_close(listener);
    return 3;
  }
  std::uint8_t payload[2] = {};
  const auto read_status =
      chtholly_next_host_v2_read(stream, payload, sizeof(payload), 2000);
  client.join();
  (void)chtholly_next_host_v2_close(stream);
  (void)chtholly_next_host_v2_close(listener);
  if (read_status != 2 || std::memcmp(payload, "ok", 2) != 0)
    return 4;
  CHTHOLLY_TEST_CHECK(chtholly_next_host_v2_close(nullptr) ==
         CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT);
  return 0;
}
