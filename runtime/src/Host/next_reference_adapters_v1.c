#include "chtholly/next_host_v1.h"

/* CFDL references address a handle slot; native APIs consume its bits. */
int32_t chtholly_next_host_v1_task_poll_ref(void **slot) {
  return slot ? chtholly_next_host_v1_task_poll(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_task_cancel_ref(void **slot) {
  return slot ? chtholly_next_host_v1_task_cancel(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_task_wake_ref(void **slot) {
  return slot ? chtholly_next_host_v1_task_wake(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_net_accept_ref(void **slot, void **out) {
  return slot ? chtholly_next_host_v1_net_accept(*slot, out) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int64_t chtholly_next_host_v1_net_read_ref(void **slot, uint8_t *buffer, uint64_t count) {
  return slot ? chtholly_next_host_v1_net_read(*slot, buffer, count) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int64_t chtholly_next_host_v1_net_write_ref(void **slot, const uint8_t *buffer, uint64_t count) {
  return slot ? chtholly_next_host_v1_net_write(*slot, buffer, count) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_sync_mutex_lock_ref(void **slot) {
  return slot ? chtholly_next_host_v1_sync_mutex_lock(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_sync_mutex_unlock_ref(void **slot) {
  return slot ? chtholly_next_host_v1_sync_mutex_unlock(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_sync_condvar_notify_one_ref(void **slot) {
  return slot ? chtholly_next_host_v1_sync_condvar_notify_one(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_sync_condvar_notify_all_ref(void **slot) {
  return slot ? chtholly_next_host_v1_sync_condvar_notify_all(*slot) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_sync_condvar_wait_ref(void **slot, void **mutex) {
  return slot && mutex ? chtholly_next_host_v1_sync_condvar_wait(*slot, *mutex) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_channel_send_ref(void **slot, const uint8_t *buffer, uint64_t count) {
  return slot ? chtholly_next_host_v1_channel_send(*slot, buffer, count) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_channel_receive_ref(void **slot, uint8_t *buffer, uint64_t capacity, uint64_t *size) {
  return slot ? chtholly_next_host_v1_channel_receive(*slot, buffer, capacity, size) : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}
