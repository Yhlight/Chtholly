#include "chtholly/next_host_v1.h"

#include <string.h>

#define NEXT_SYNC_GUARD_MAGIC UINT64_C(0x4755415244310001)
#define NEXT_SYNC_GUARD_ACTIVE 1u

int32_t chtholly_next_host_v1_sync_guard_acquire(
    void *mutex, chtholly_next_sync_guard *out_guard) {
  if (mutex == NULL || out_guard == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  memset(out_guard, 0, sizeof(*out_guard));
  const int32_t status = chtholly_next_host_v1_sync_mutex_lock(mutex);
  if (status != 0)
    return status;
  out_guard->mutex = mutex;
  out_guard->generation = NEXT_SYNC_GUARD_MAGIC;
  out_guard->state = NEXT_SYNC_GUARD_ACTIVE;
  return 0;
}

int32_t chtholly_next_host_v1_sync_guard_release(
    chtholly_next_sync_guard *guard) {
  if (guard == NULL || guard->generation != NEXT_SYNC_GUARD_MAGIC ||
      guard->state != NEXT_SYNC_GUARD_ACTIVE || guard->mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  const int32_t status =
      chtholly_next_host_v1_sync_mutex_unlock(guard->mutex);
  if (status != 0)
    return status;
  memset(guard, 0, sizeof(*guard));
  return 0;
}
