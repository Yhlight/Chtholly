#include "chtholly/next_resource_lease_v2.h"
#include <array>
#include "test_check.h"
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

int main() {
  std::array<uint8_t, 32> digest{}; digest[0] = 7;
  chtholly_next_resource_lease_v2 *lease = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_create(2, digest.data(), &lease) == 0);
  chtholly_next_payload_transport_v2 *transport = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_create(lease, digest.data(), 2, 4,
                                                     &transport) == 0);
  std::array<uint8_t, 4> source{1,2,3,4};
  void *send = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(
             transport, source.data(), source.size(), &send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_fail(send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_size(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(
             transport, source.data(), source.size(), &send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_size(transport) == 1);
  void *receive = nullptr; const void *payload = nullptr; uint64_t size = 0;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_acquire(
             transport, &receive, &payload, &size) == 0);
  CHTHOLLY_TEST_CHECK(size == 4 && std::memcmp(payload, source.data(), 4) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_fail(receive) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_size(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(
             transport, source.data(), source.size(), &send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_acquire(
             transport, &receive, &payload, &size) == 0);
  std::array<uint8_t, 4> destination{};
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_commit(
             receive, destination.data(), destination.size()) == 0);
  CHTHOLLY_TEST_CHECK(destination == source);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(
             transport, source.data(), source.size(), &send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) ==
         CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_cancel(send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) ==
         CHTHOLLY_NEXT_RESOURCE_LEASE_ALREADY_CLOSED);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_destroy(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == 0);

  // Concurrent shared-lease senders exercise the transport linearization
  // point and exactly-once token release.
  lease = nullptr; transport = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_create(2, digest.data(), &lease) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_create(lease, digest.data(), 64, 4,
                                                    &transport) == 0);
  std::vector<std::thread> senders;
  for (int i = 0; i < 8; ++i) senders.emplace_back([&] {
    void *token = nullptr;
    while (chtholly_next_payload_transport_v2_send_prepare(
               transport, source.data(), source.size(), &token) != 0)
      std::this_thread::yield();
    CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(token) == 0);
  });
  for (auto &thread : senders) thread.join();
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_size(transport) == 8);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_destroy(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == 0);

  // Deterministic receive/close soak: receivers race with a closer while
  // active capabilities keep quiescence pending, then drain cleanly.
  lease = nullptr; transport = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_create(2, digest.data(), &lease) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_create(lease, digest.data(), 16, 4,
                                                    &transport) == 0);
  for (int i = 0; i < 8; ++i) {
    void *token = nullptr;
    CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(
               transport, source.data(), source.size(), &token) == 0);
    CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(token) == 0);
  }
  std::vector<std::thread> receivers;
  for (int i = 0; i < 4; ++i) receivers.emplace_back([&] {
    void *token = nullptr; const void *payload = nullptr; uint64_t payload_size = 0;
    while (chtholly_next_payload_transport_v2_receive_acquire(
               transport, &token, &payload, &payload_size) != 0)
      std::this_thread::yield();
    std::this_thread::yield();
    if ((payload_size & 1u) == 0) {
      std::array<uint8_t, 4> out{};
      CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_commit(
                 token, out.data(), out.size()) == 0);
    } else {
      CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_cancel(token) == 0);
    }
  });
  for (auto &thread : receivers) thread.join();
  while (chtholly_next_payload_transport_v2_size(transport) != 0) {
    void *token = nullptr; const void *payload = nullptr; uint64_t payload_size = 0;
    if (chtholly_next_payload_transport_v2_receive_acquire(
            transport, &token, &payload, &payload_size) == 0)
      CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_cancel(token) == 0);
  }
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_destroy(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == 0);
  return 0;
}
