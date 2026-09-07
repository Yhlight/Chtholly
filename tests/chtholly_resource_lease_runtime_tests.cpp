#include "chtholly/next_resource_lease_v2.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "lease runtime check failed at line %d: %s\n", line,
                 expression);
    std::abort();
  }
}
#define CHECK(condition) check((condition), #condition, __LINE__)

int main() {
  std::array<std::uint8_t, 32> digest{};
  digest[0] = 0x42;
  std::array<std::uint8_t, 32> wrong = digest;
  wrong[0] = 0x43;
  chtholly_next_resource_lease_v2 *lease = nullptr;
  chtholly_next_resource_token_v2 first{}, second{};
  CHECK(chtholly_next_resource_lease_v2_create(
            2, digest.data(), &lease) == CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_acquire(lease, wrong.data(), &first) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_STALE);
  CHECK(chtholly_next_resource_lease_v2_acquire(lease, digest.data(), &first) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_acquire(lease, digest.data(), &second) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_active(lease) == 2);
  CHECK(chtholly_next_resource_lease_v2_begin_close(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  chtholly_next_resource_token_v2 copied = first;
  chtholly_next_resource_token_v2 rejected{};
  CHECK(chtholly_next_resource_lease_v2_acquire(lease, digest.data(), &rejected) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY);
  CHECK(chtholly_next_resource_lease_v2_quiesce(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY);
  CHECK(chtholly_next_resource_lease_v2_release(&first) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_release(&first) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT);
  CHECK(chtholly_next_resource_lease_v2_release(&copied) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_STALE);
  CHECK(chtholly_next_resource_lease_v2_quiesce(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY);
  CHECK(chtholly_next_resource_lease_v2_release(&second) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_quiesce(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_destroy(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);

  lease = nullptr;
  CHECK(chtholly_next_resource_lease_v2_create(
            1, digest.data(), &lease) == CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  chtholly_next_resource_operation_v2 *operation = nullptr;
  CHECK(chtholly_next_resource_operation_v2_begin(
            lease, digest.data(), &operation) == CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_operation_v2_state(operation) ==
        CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED);
  CHECK(chtholly_next_resource_lease_v2_begin_close(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_quiesce(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY);
  CHECK(chtholly_next_resource_operation_v2_complete(
            operation, CHTHOLLY_NEXT_RESOURCE_OPERATION_COMMITTED) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_quiesce(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_operation_v2_complete(
            operation, CHTHOLLY_NEXT_RESOURCE_OPERATION_FAILED) ==
        CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE);
  CHECK(chtholly_next_resource_operation_v2_destroy(operation) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_lease_v2_destroy(lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  return 0;
}
