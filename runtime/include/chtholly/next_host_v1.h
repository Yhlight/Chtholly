#ifndef CHTHOLLY_NEXT_HOST_V1_H
#define CHTHOLLY_NEXT_HOST_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_HOST_ABI_V1 1u
#define CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT (-2701)
#define CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE (-2702)
#define CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY (-2703)
#define CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE (-2704)
#define CHTHOLLY_NEXT_HOST_STATUS_CANCELLED (-2705)
#define CHTHOLLY_NEXT_HOST_STATUS_NOT_READY (-2706)
#define CHTHOLLY_NEXT_HOST_STATUS_ALREADY_CLOSED (-2707)
#define CHTHOLLY_NEXT_HOST_STATUS_TIMEOUT (-2804)
#define CHTHOLLY_NEXT_HOST_STATUS_CLOSED (-2805)
#define CHTHOLLY_NEXT_HOST_STATUS_RESET (-2806)

typedef struct chtholly_next_host_v1_instant {
  uint64_t seconds;
  uint32_t nanoseconds;
  uint32_t reserved;
} chtholly_next_host_v1_instant;

int32_t chtholly_next_host_v1_open(const uint8_t *path, uint64_t path_size,
                                   void **out_handle);
int32_t chtholly_next_host_v1_open_read(const uint8_t *, uint64_t, void **);
int32_t chtholly_next_host_v1_create(const uint8_t *, uint64_t, void **);
int64_t chtholly_next_host_v1_read(void *handle, uint8_t *buffer,
                                   uint64_t count);
int64_t chtholly_next_host_v1_write(void *handle, const uint8_t *buffer,
                                    uint64_t count);
int64_t chtholly_next_host_v1_read_ref(void **, uint8_t *, uint64_t);
int64_t chtholly_next_host_v1_write_ref(void **, const uint8_t *, uint64_t);
int32_t chtholly_next_host_v1_close(void *handle);
int32_t chtholly_next_host_v1_monotonic_now(
    chtholly_next_host_v1_instant *out_value);

int32_t chtholly_next_host_v1_task_spawn(const void *entry, void **out_task);
int32_t chtholly_next_host_v1_task_poll(void *task);
int32_t chtholly_next_host_v1_task_cancel(void *task);
int32_t chtholly_next_host_v1_task_wake(void *task);
int32_t chtholly_next_host_v1_task_join(void *task);

/* Blocking loopback TCP operations. Socket handles are move-only foreign
   resources and must be discharged with net_close. A read returning zero is
   EOF; negative values are one of the status constants above. */
int32_t chtholly_next_host_v1_net_listen(uint16_t port, void **out_listener);
int32_t chtholly_next_host_v1_net_accept(void *listener, void **out_stream);
int64_t chtholly_next_host_v1_net_read(void *stream, uint8_t *buffer,
                                       uint64_t capacity);
int64_t chtholly_next_host_v1_net_write(void *stream,
                                        const uint8_t *buffer,
                                        uint64_t size);
int32_t chtholly_next_host_v1_net_close(void *handle);

int32_t chtholly_next_host_v1_sync_mutex_init(void **out_mutex);
int32_t chtholly_next_host_v1_sync_mutex_lock(void *mutex);
int32_t chtholly_next_host_v1_sync_mutex_unlock(void *mutex);
int32_t chtholly_next_host_v1_sync_mutex_close(void *mutex);
int32_t chtholly_next_host_v1_sync_condvar_init(void **out_condition);
int32_t chtholly_next_host_v1_sync_condvar_wait(void *condition, void *mutex);
int32_t chtholly_next_host_v1_sync_condvar_notify_one(void *condition);
int32_t chtholly_next_host_v1_sync_condvar_notify_all(void *condition);
int32_t chtholly_next_host_v1_sync_condvar_close(void *condition);

/* Caller-owned, move-only-by-convention guard token. The runtime validates
   the token state so a guard can be released at most once. */
typedef struct chtholly_next_sync_guard {
  void *mutex;
  uint64_t generation;
  uint32_t state;
  uint32_t reserved;
} chtholly_next_sync_guard;

int32_t chtholly_next_host_v1_sync_guard_acquire(
    void *mutex, chtholly_next_sync_guard *out_guard);
int32_t chtholly_next_host_v1_sync_guard_release(
    chtholly_next_sync_guard *guard);

int32_t chtholly_next_host_v1_channel_init(uint64_t capacity,
                                            void **out_channel);
int32_t chtholly_next_host_v1_channel_send(void *channel,
                                           const uint8_t *data,
                                           uint64_t size);
int32_t chtholly_next_host_v1_channel_receive(void *channel, uint8_t *buffer,
                                              uint64_t capacity,
                                              uint64_t *out_size);
int32_t chtholly_next_host_v1_channel_close(void *channel);

int32_t chtholly_next_host_v1_task_poll_ref(void **slot);
int32_t chtholly_next_host_v1_task_cancel_ref(void **slot);
int32_t chtholly_next_host_v1_task_wake_ref(void **slot);
int32_t chtholly_next_host_v1_net_accept_ref(void **slot, void **out);
int64_t chtholly_next_host_v1_net_read_ref(void **slot, uint8_t *buffer, uint64_t count);
int64_t chtholly_next_host_v1_net_write_ref(void **slot, const uint8_t *buffer, uint64_t count);
int32_t chtholly_next_host_v1_sync_mutex_lock_ref(void **slot);
int32_t chtholly_next_host_v1_sync_mutex_unlock_ref(void **slot);
int32_t chtholly_next_host_v1_sync_condvar_notify_one_ref(void **slot);
int32_t chtholly_next_host_v1_sync_condvar_notify_all_ref(void **slot);
int32_t chtholly_next_host_v1_sync_condvar_wait_ref(void **slot, void **mutex);
int32_t chtholly_next_host_v1_channel_send_ref(void **slot, const uint8_t *buffer, uint64_t count);
int32_t chtholly_next_host_v1_channel_receive_ref(void **slot, uint8_t *buffer, uint64_t capacity, uint64_t *size);

/* Typed channel prototype. The descriptor is compiler-produced metadata; the
   runtime never infers move/drop behavior from a byte size. A send is split
   into prepare/commit so a full or closed channel leaves the source owner
   unchanged. */
#define CHTHOLLY_NEXT_TYPED_CHANNEL_ABI_V1 1u
#define CHTHOLLY_NEXT_TYPED_CHANNEL_SEND 1u
#define CHTHOLLY_NEXT_TYPED_CHANNEL_SYNC 2u

typedef void (*chtholly_next_typed_move_fn)(void *destination,
                                             void *source);
typedef void (*chtholly_next_typed_drop_fn)(void *value);

typedef struct chtholly_next_typed_channel_descriptor {
  uint32_t abi;
  uint32_t capabilities;
  uint64_t size;
  uint64_t alignment;
  chtholly_next_typed_move_fn move;
  chtholly_next_typed_drop_fn drop;
} chtholly_next_typed_channel_descriptor;

/* Tokens are intentionally caller-owned and opaque apart from their stable
   storage shape. They must be completed or cancelled exactly once. */
typedef struct chtholly_next_typed_channel_token {
  void *channel;
  void *node;
  const void *source;
  void *destination;
  uint64_t generation;
  uint32_t state;
  uint32_t kind;
} chtholly_next_typed_channel_token;

int32_t chtholly_next_host_v1_typed_channel_init(
    uint64_t capacity, const chtholly_next_typed_channel_descriptor *descriptor,
    void **out_channel);
int32_t chtholly_next_host_v1_typed_channel_send_prepare(
    void *channel, const void *source,
    chtholly_next_typed_channel_token *out_token);
int32_t chtholly_next_host_v1_typed_channel_send_commit(
    chtholly_next_typed_channel_token *token);
int32_t chtholly_next_host_v1_typed_channel_send_cancel(
    chtholly_next_typed_channel_token *token);
int32_t chtholly_next_host_v1_typed_channel_receive_acquire(
    void *channel, chtholly_next_typed_channel_token *out_token);
int32_t chtholly_next_host_v1_typed_channel_receive_commit(
    chtholly_next_typed_channel_token *token, void *destination);
int32_t chtholly_next_host_v1_typed_channel_receive_cancel(
    chtholly_next_typed_channel_token *token);
int32_t chtholly_next_host_v1_typed_channel_close(void *channel);
#ifdef CHTHOLLY_RUNTIME_TESTING
void chtholly_next_host_v1_typed_channel_test_fail_allocate(void);
uint64_t chtholly_next_host_v1_typed_channel_test_active(void *channel);
#endif

#ifdef __cplusplus
}
#endif

#endif
