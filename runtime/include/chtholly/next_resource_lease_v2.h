#ifndef CHTHOLLY_NEXT_RESOURCE_LEASE_V2_H
#define CHTHOLLY_NEXT_RESOURCE_LEASE_V2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_RESOURCE_LEASE_ABI_V2 2u
#define CHTHOLLY_NEXT_RESOURCE_LEASE_OK 0
#define CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT (-2701)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY (-2703)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE (-2704)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY (-2706)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_ALREADY_CLOSED (-2707)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED (-2805)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_STALE (-2807)
#define CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY (-2808)
#define CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE (-2809)
#define CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED 1u
#define CHTHOLLY_NEXT_RESOURCE_OPERATION_COMMITTED 2u
#define CHTHOLLY_NEXT_RESOURCE_OPERATION_FAILED 3u
#define CHTHOLLY_NEXT_RESOURCE_OPERATION_CANCELLED 4u
#define CHTHOLLY_NEXT_PAYLOAD_SEND_PREPARED 1u
#define CHTHOLLY_NEXT_PAYLOAD_RECEIVE_ACQUIRED 2u

typedef struct chtholly_next_resource_lease_v2
    chtholly_next_resource_lease_v2;
typedef struct chtholly_next_resource_operation_v2
    chtholly_next_resource_operation_v2;
typedef struct chtholly_next_payload_transport_v2
    chtholly_next_payload_transport_v2;

typedef struct chtholly_next_resource_token_v2 {
  chtholly_next_resource_lease_v2 *lease;
  void *record;
  uint64_t generation;
  uint32_t state;
  uint32_t reserved;
  uint8_t descriptor_digest[32];
} chtholly_next_resource_token_v2;

int32_t chtholly_next_resource_lease_v2_create(
    uint32_t policy, const uint8_t descriptor_digest[32],
    chtholly_next_resource_lease_v2 **out_lease);
int32_t chtholly_next_resource_lease_v2_acquire(
    chtholly_next_resource_lease_v2 *lease,
    const uint8_t descriptor_digest[32],
    chtholly_next_resource_token_v2 *out_token);
int32_t chtholly_next_resource_lease_v2_release(
    chtholly_next_resource_token_v2 *token);
int32_t chtholly_next_resource_lease_v2_begin_close(
    chtholly_next_resource_lease_v2 *lease);
int32_t chtholly_next_resource_lease_v2_quiesce(
    chtholly_next_resource_lease_v2 *lease);
int32_t chtholly_next_resource_lease_v2_destroy(
    chtholly_next_resource_lease_v2 *lease);

uint32_t chtholly_next_resource_lease_v2_active(
    const chtholly_next_resource_lease_v2 *lease);

int32_t chtholly_next_resource_operation_v2_begin(
    chtholly_next_resource_lease_v2 *lease,
    const uint8_t descriptor_digest[32],
    chtholly_next_resource_operation_v2 **out_operation);
int32_t chtholly_next_resource_operation_v2_complete(
    chtholly_next_resource_operation_v2 *operation, uint32_t terminal_state);
int32_t chtholly_next_resource_operation_v2_destroy(
    chtholly_next_resource_operation_v2 *operation);
uint32_t chtholly_next_resource_operation_v2_state(
    const chtholly_next_resource_operation_v2 *operation);

/* Runtime protocol extension: terminal publication does not release the
 * module lease until provider_leave. Destroy requires exclusive ownership;
 * callers join users of an operation before destroying its handle. */
int32_t chtholly_next_resource_operation_v2_provider_enter(chtholly_next_resource_operation_v2 *);
int32_t chtholly_next_resource_operation_v2_provider_leave(chtholly_next_resource_operation_v2 *);
int32_t chtholly_next_resource_operation_v2_request_cancel(chtholly_next_resource_operation_v2 *);
uint32_t chtholly_next_resource_operation_v2_cancel_requested(chtholly_next_resource_operation_v2 *);

#define CHTHOLLY_NEXT_PAYLOAD_DESCRIPTOR_VERSION 1u
typedef struct chtholly_next_payload_descriptor_v2 {
  uint32_t struct_size, version;
  uint64_t size, alignment;
  uint8_t type_digest[32], layout_digest[32], lifecycle_digest[32];
  void (*move)(void *context, void *destination, void *source);
  void (*drop)(void *context, void *value);
  void *context;
  int32_t (*retain_owner)(void *owner);
  void (*release_owner)(void *owner);
  void *owner;
} chtholly_next_payload_descriptor_v2;
typedef struct chtholly_next_owned_payload_v2 chtholly_next_owned_payload_v2;
int32_t chtholly_next_payload_transport_v2_create_typed(
    chtholly_next_resource_lease_v2 *, const uint8_t descriptor_digest[32],
    uint64_t capacity, const chtholly_next_payload_descriptor_v2 *,
    chtholly_next_payload_transport_v2 **);
/* Typed results keep the callback owner alive independently of the queue. */
int32_t chtholly_next_payload_transport_v2_receive_take(void *token, chtholly_next_owned_payload_v2 **out);
const void *chtholly_next_owned_payload_v2_data(const chtholly_next_owned_payload_v2 *);
int32_t chtholly_next_owned_payload_v2_destroy(chtholly_next_owned_payload_v2 *);

int32_t chtholly_next_payload_transport_v2_create(
    chtholly_next_resource_lease_v2 *lease, const uint8_t descriptor_digest[32],
    uint64_t capacity, uint64_t payload_size,
    chtholly_next_payload_transport_v2 **out_transport);
int32_t chtholly_next_payload_transport_v2_send_prepare(
    chtholly_next_payload_transport_v2 *transport, const void *source,
    uint64_t source_size, void **out_token);
int32_t chtholly_next_payload_transport_v2_send_commit(void *token);
int32_t chtholly_next_payload_transport_v2_send_cancel(void *token);
int32_t chtholly_next_payload_transport_v2_send_fail(void *token);
int32_t chtholly_next_payload_transport_v2_receive_acquire(
    chtholly_next_payload_transport_v2 *transport, void **out_token,
    const void **out_payload, uint64_t *out_size);
int32_t chtholly_next_payload_transport_v2_receive_commit(
    void *token, void *destination, uint64_t destination_size);
int32_t chtholly_next_payload_transport_v2_receive_cancel(void *token);
int32_t chtholly_next_payload_transport_v2_receive_fail(void *token);
int32_t chtholly_next_payload_transport_v2_close(
    chtholly_next_payload_transport_v2 *transport);
int32_t chtholly_next_payload_transport_v2_destroy(
    chtholly_next_payload_transport_v2 *transport);
uint64_t chtholly_next_payload_transport_v2_size(
    const chtholly_next_payload_transport_v2 *transport);
uint64_t chtholly_next_payload_transport_v2_payload_size(
    const chtholly_next_payload_transport_v2 *transport);
int32_t chtholly_next_payload_transport_v2_check_digest(
    const chtholly_next_payload_transport_v2 *transport,
    const uint8_t descriptor_digest[32]);

#ifdef __cplusplus
}
#endif

#endif
