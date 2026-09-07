#include "chtholly/next_resource_lease_v2.h"
#include "test_check.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {
struct Counters { std::atomic<int> owners{0}, moves{0}, drops{0}; };
struct alignas(64) Resource { int *value; };
int32_t retain(void *context) { ++static_cast<Counters *>(context)->owners; return 0; }
void release(void *context) { --static_cast<Counters *>(context)->owners; }
void move(void *context, void *out, void *in) {
  CHTHOLLY_TEST_CHECK(reinterpret_cast<std::uintptr_t>(out) % alignof(Resource) == 0);
  auto *from = static_cast<Resource *>(in);
  static_cast<Resource *>(out)->value = from->value;
  from->value = nullptr;
  ++static_cast<Counters *>(context)->moves;
}
void drop(void *context, void *value) {
  auto *resource = static_cast<Resource *>(value);
  CHTHOLLY_TEST_CHECK(resource->value != nullptr);
  delete resource->value;
  resource->value = nullptr;
  ++static_cast<Counters *>(context)->drops;
}
}
int main() {
  Counters counts;
  std::array<uint8_t, 32> digest{}; digest[0] = 1;
  chtholly_next_payload_descriptor_v2 descriptor{};
  descriptor.struct_size = sizeof(descriptor);
  descriptor.version = CHTHOLLY_NEXT_PAYLOAD_DESCRIPTOR_VERSION;
  descriptor.size = sizeof(Resource); descriptor.alignment = alignof(Resource);
  descriptor.type_digest[0] = descriptor.layout_digest[0] = descriptor.lifecycle_digest[0] = 1;
  descriptor.move = move; descriptor.drop = drop; descriptor.context = &counts;
  descriptor.owner = &counts; descriptor.retain_owner = retain; descriptor.release_owner = release;
  chtholly_next_resource_lease_v2 *lease = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_create(2, digest.data(), &lease) == 0);
  chtholly_next_payload_transport_v2 *transport = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_create_typed(lease, digest.data(), 4, &descriptor, &transport) == 0);
  CHTHOLLY_TEST_CHECK(counts.owners == 1);
  Resource first{new int(42)};
  void *token = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(transport, &first, sizeof(first), &token) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_cancel(token) == 0);
  CHTHOLLY_TEST_CHECK(*first.value == 42 && counts.moves == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(transport, &first, sizeof(first), &token) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(token) == 0);
  CHTHOLLY_TEST_CHECK(first.value == nullptr && counts.moves == 1);
  const void *view = nullptr; uint64_t size = 0;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_acquire(transport, &token, &view, &size) == 0);
  chtholly_next_owned_payload_v2 *owned = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_take(token, &owned) == 0);
  CHTHOLLY_TEST_CHECK(*static_cast<const Resource *>(chtholly_next_owned_payload_v2_data(owned))->value == 42);
  CHTHOLLY_TEST_CHECK(counts.owners == 2);
  Resource second{new int(7)};
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(transport, &second, sizeof(second), &token) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(token) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_acquire(transport, &token, &view, &size) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_cancel(token) == 0);
  CHTHOLLY_TEST_CHECK(counts.drops == 1);
  Resource third{new int(8)};
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(transport, &third, sizeof(third), &token) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(token) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) == 0);
  CHTHOLLY_TEST_CHECK(counts.drops == 2);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_destroy(transport) == 0);
  CHTHOLLY_TEST_CHECK(counts.owners == 1);
  CHTHOLLY_TEST_CHECK(chtholly_next_owned_payload_v2_destroy(owned) == 0);
  CHTHOLLY_TEST_CHECK(counts.owners == 0 && counts.drops == 3 && counts.moves == 3);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == 0);

  // Terminal publication races, but the provider pin survives the winner.
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_create(2, digest.data(), &lease) == 0);
  chtholly_next_resource_operation_v2 *operation = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_begin(lease, digest.data(), &operation) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_provider_enter(operation) == 0);
  std::atomic<int> winners{0}; std::vector<std::thread> workers;
  for (uint32_t state = 2; state <= 4; ++state)
    workers.emplace_back([&, state] { if (!chtholly_next_resource_operation_v2_complete(operation, state)) ++winners; });
  for (auto &worker : workers) worker.join();
  CHTHOLLY_TEST_CHECK(winners == 1);
  const auto terminal = chtholly_next_resource_operation_v2_state(operation);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_request_cancel(operation) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_state(operation) == terminal);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_begin_close(lease) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_quiesce(lease) == CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_destroy(operation) == CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_provider_leave(operation) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_quiesce(lease) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_quiesce(lease) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_destroy(operation) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == 0);
}
