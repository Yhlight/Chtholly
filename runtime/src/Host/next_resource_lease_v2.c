#include "chtholly/next_resource_lease_v2.h"

#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
typedef CRITICAL_SECTION NextPayloadMutex;
static void payload_mutex_init(NextPayloadMutex *m) { InitializeCriticalSection(m); }
static void payload_mutex_lock(NextPayloadMutex *m) { EnterCriticalSection(m); }
static void payload_mutex_unlock(NextPayloadMutex *m) { LeaveCriticalSection(m); }
static void payload_mutex_destroy(NextPayloadMutex *m) { DeleteCriticalSection(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t NextPayloadMutex;
static void payload_mutex_init(NextPayloadMutex *m) { (void)pthread_mutex_init(m, NULL); }
static void payload_mutex_lock(NextPayloadMutex *m) { (void)pthread_mutex_lock(m); }
static void payload_mutex_unlock(NextPayloadMutex *m) { (void)pthread_mutex_unlock(m); }
static void payload_mutex_destroy(NextPayloadMutex *m) { (void)pthread_mutex_destroy(m); }
#endif

#define NEXT_RESOURCE_LEASE_MAGIC 0x4c563231u
#define NEXT_RESOURCE_LEASE_STATE_AVAILABLE 1u
#define NEXT_RESOURCE_LEASE_STATE_LEASED 2u
#define NEXT_RESOURCE_LEASE_STATE_CLOSING 3u
#define NEXT_RESOURCE_LEASE_STATE_CLOSED 4u
#define NEXT_RESOURCE_TOKEN_ACTIVE 1u

typedef struct NextResourceTokenRecord NextResourceTokenRecord;

struct chtholly_next_resource_lease_v2 {
  uint32_t magic;
  NextPayloadMutex mutex;
  uint32_t policy;
  uint32_t state;
  uint32_t active;
  uint32_t transports;
  uint64_t next_generation;
  uint8_t descriptor_digest[32];
  NextResourceTokenRecord *records;
};

struct NextResourceTokenRecord {
  chtholly_next_resource_lease_v2 *lease;
  chtholly_next_resource_token_v2 *token;
  uint64_t generation;
  uint8_t active;
  NextResourceTokenRecord *next;
};

#define NEXT_RESOURCE_OPERATION_MAGIC 0x4f503232u

struct chtholly_next_resource_operation_v2 {
  uint32_t magic;
  NextPayloadMutex mutex;
  uint32_t state;
  uint32_t cancel_requested;
  uint32_t provider_active;
  chtholly_next_resource_token_v2 token;
};

static int valid_digest(const uint8_t *digest) {
  size_t index;
  if (digest == NULL)
    return 0;
  for (index = 0; index < 32; ++index)
    if (digest[index] != 0)
      return 1;
  return 0;
}

static int valid_lease(const chtholly_next_resource_lease_v2 *lease) {
  return lease != NULL && lease->magic == NEXT_RESOURCE_LEASE_MAGIC;
}

int32_t chtholly_next_resource_lease_v2_create(
    uint32_t policy, const uint8_t descriptor_digest[32],
    chtholly_next_resource_lease_v2 **out_lease) {
  chtholly_next_resource_lease_v2 *lease;
  if (out_lease == NULL || (policy != 1u && policy != 2u) ||
      !valid_digest(descriptor_digest))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out_lease = NULL;
  lease = (chtholly_next_resource_lease_v2 *)calloc(1, sizeof(*lease));
  if (lease == NULL)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY;
  payload_mutex_init(&lease->mutex);
  lease->magic = NEXT_RESOURCE_LEASE_MAGIC;
  lease->policy = policy;
  lease->state = NEXT_RESOURCE_LEASE_STATE_AVAILABLE;
  lease->next_generation = 1;
  memcpy(lease->descriptor_digest, descriptor_digest, 32);
  *out_lease = lease;
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

static int32_t chtholly_next_resource_lease_v2_acquire_locked(
    chtholly_next_resource_lease_v2 *lease,
    const uint8_t descriptor_digest[32],
    chtholly_next_resource_token_v2 *out_token) {
  if (!valid_lease(lease) || out_token == NULL ||
      !valid_digest(descriptor_digest))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  memset(out_token, 0, sizeof(*out_token));
  if (memcmp(lease->descriptor_digest, descriptor_digest, 32) != 0)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_STALE;
  if (lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSING ||
      lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSED)
    return lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSED
               ? CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED
               : CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY;
  if (lease->policy == 1u && lease->state != NEXT_RESOURCE_LEASE_STATE_AVAILABLE)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY;
  if (lease->active == UINT32_MAX || lease->next_generation == UINT64_MAX)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY;
  ++lease->active;
  lease->state = NEXT_RESOURCE_LEASE_STATE_LEASED;
  NextResourceTokenRecord *record =
      (NextResourceTokenRecord *)calloc(1, sizeof(*record));
  if (record == NULL) {
    --lease->active;
    if (lease->active == 0)
      lease->state = NEXT_RESOURCE_LEASE_STATE_AVAILABLE;
    return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY;
  }
  out_token->lease = lease;
  out_token->generation = lease->next_generation++;
  out_token->state = NEXT_RESOURCE_TOKEN_ACTIVE;
  memcpy(out_token->descriptor_digest, descriptor_digest, 32);
  record->lease = lease;
  record->token = out_token;
  record->generation = out_token->generation;
  record->active = 1;
  record->next = lease->records;
  lease->records = record;
  out_token->record = record;
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

static int32_t chtholly_next_resource_lease_v2_release_locked(
    chtholly_next_resource_token_v2 *token) {
  chtholly_next_resource_lease_v2 *lease;
  NextResourceTokenRecord *record;
  if (token == NULL || token->state != NEXT_RESOURCE_TOKEN_ACTIVE ||
      !valid_lease(token->lease))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  lease = token->lease;
  NextResourceTokenRecord **entry = &lease->records;
  while (*entry && *entry != token->record) entry = &(*entry)->next;
  record = *entry;
  if (record == NULL || !record->active || record->lease != lease ||
      record->token != token || record->generation != token->generation ||
      token->generation == 0 || token->generation >= lease->next_generation ||
      memcmp(token->descriptor_digest, lease->descriptor_digest, 32) != 0)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_STALE;
  if (lease->active == 0)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  --lease->active;
  if (lease->state == NEXT_RESOURCE_LEASE_STATE_LEASED && lease->active == 0)
    lease->state = NEXT_RESOURCE_LEASE_STATE_AVAILABLE;
  *entry = record->next;
  free(record);
  memset(token, 0, sizeof(*token));
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

static int32_t chtholly_next_resource_lease_v2_begin_close_locked(
    chtholly_next_resource_lease_v2 *lease) {
  if (!valid_lease(lease))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  if (lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSED ||
      lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSING)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_ALREADY_CLOSED;
  lease->state = NEXT_RESOURCE_LEASE_STATE_CLOSING;
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

static int32_t chtholly_next_resource_lease_v2_quiesce_locked(
    chtholly_next_resource_lease_v2 *lease) {
  if (!valid_lease(lease))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  if (lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSED) return 0;
  if (lease->state != NEXT_RESOURCE_LEASE_STATE_CLOSING)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY;
  if (lease->active != 0)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY;
  lease->state = NEXT_RESOURCE_LEASE_STATE_CLOSED;
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

int32_t chtholly_next_resource_lease_v2_destroy(
    chtholly_next_resource_lease_v2 *lease) {
  if (!valid_lease(lease))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  if (lease->state != NEXT_RESOURCE_LEASE_STATE_CLOSED || lease->active != 0 || lease->transports != 0)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY;
  while (lease->records != NULL) {
    NextResourceTokenRecord *record = lease->records;
    lease->records = record->next;
    free(record);
  }
  lease->magic = 0;
  payload_mutex_destroy(&lease->mutex);
  free(lease);
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

uint32_t chtholly_next_resource_lease_v2_active(
    const chtholly_next_resource_lease_v2 *lease) {
  if (!valid_lease(lease)) return 0;
  payload_mutex_lock((NextPayloadMutex *)&lease->mutex);
  uint32_t result = lease->active;
  payload_mutex_unlock((NextPayloadMutex *)&lease->mutex);
  return result;
}

int32_t chtholly_next_resource_lease_v2_acquire(chtholly_next_resource_lease_v2 *lease, const uint8_t digest[32], chtholly_next_resource_token_v2 *token) {
  chtholly_next_resource_lease_v2 *owner = lease;
  if (!valid_lease(owner)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  payload_mutex_lock(&owner->mutex);
  int32_t result = chtholly_next_resource_lease_v2_acquire_locked(lease, digest, token);
  payload_mutex_unlock(&owner->mutex);
  return result;
}

int32_t chtholly_next_resource_lease_v2_release(chtholly_next_resource_token_v2 *token) {
  chtholly_next_resource_lease_v2 *owner = token ? token->lease : NULL;
  if (!valid_lease(owner)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  payload_mutex_lock(&owner->mutex);
  int32_t result = chtholly_next_resource_lease_v2_release_locked(token);
  payload_mutex_unlock(&owner->mutex);
  return result;
}

int32_t chtholly_next_resource_lease_v2_begin_close(chtholly_next_resource_lease_v2 *lease) {
  chtholly_next_resource_lease_v2 *owner = lease;
  if (!valid_lease(owner)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  payload_mutex_lock(&owner->mutex);
  int32_t result = chtholly_next_resource_lease_v2_begin_close_locked(lease);
  payload_mutex_unlock(&owner->mutex);
  return result;
}

int32_t chtholly_next_resource_lease_v2_quiesce(chtholly_next_resource_lease_v2 *lease) {
  chtholly_next_resource_lease_v2 *owner = lease;
  if (!valid_lease(owner)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  payload_mutex_lock(&owner->mutex);
  int32_t result = chtholly_next_resource_lease_v2_quiesce_locked(lease);
  payload_mutex_unlock(&owner->mutex);
  return result;
}

int32_t chtholly_next_resource_operation_v2_begin(
    chtholly_next_resource_lease_v2 *lease,
    const uint8_t descriptor_digest[32],
    chtholly_next_resource_operation_v2 **out_operation) {
  chtholly_next_resource_operation_v2 *operation;
  int32_t status;
  if (out_operation == NULL)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out_operation = NULL;
  operation = (chtholly_next_resource_operation_v2 *)calloc(1, sizeof(*operation));
  if (operation == NULL)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY;
  status = chtholly_next_resource_lease_v2_acquire(lease, descriptor_digest,
                                                    &operation->token);
  if (status != CHTHOLLY_NEXT_RESOURCE_LEASE_OK) {
    free(operation);
    return status;
  }
  payload_mutex_init(&operation->mutex);
  operation->magic = NEXT_RESOURCE_OPERATION_MAGIC;
  operation->state = CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED;
  *out_operation = operation;
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}

int32_t chtholly_next_resource_operation_v2_complete(
    chtholly_next_resource_operation_v2 *operation, uint32_t terminal_state) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  payload_mutex_lock(&operation->mutex);
  int32_t status = CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  if (operation->state == CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED &&
      terminal_state >= CHTHOLLY_NEXT_RESOURCE_OPERATION_COMMITTED &&
      terminal_state <= CHTHOLLY_NEXT_RESOURCE_OPERATION_CANCELLED) {
    operation->state = terminal_state;
    status = 0;
    if (!operation->provider_active)
      status = chtholly_next_resource_lease_v2_release(&operation->token);
  }
  payload_mutex_unlock(&operation->mutex);
  return status;
}
int32_t chtholly_next_resource_operation_v2_provider_enter(chtholly_next_resource_operation_v2 *operation) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  payload_mutex_lock(&operation->mutex);
  int32_t status = CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  if (operation->state == CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED && !operation->provider_active) {
    operation->provider_active = 1; status = 0;
  }
  payload_mutex_unlock(&operation->mutex); return status;
}
int32_t chtholly_next_resource_operation_v2_provider_leave(chtholly_next_resource_operation_v2 *operation) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  payload_mutex_lock(&operation->mutex);
  int32_t status = CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  if (operation->provider_active) {
    operation->provider_active = 0; status = 0;
    if (operation->state != CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED)
      status = chtholly_next_resource_lease_v2_release(&operation->token);
  }
  payload_mutex_unlock(&operation->mutex); return status;
}
int32_t chtholly_next_resource_operation_v2_request_cancel(chtholly_next_resource_operation_v2 *operation) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  payload_mutex_lock(&operation->mutex);
  operation->cancel_requested = 1;
  payload_mutex_unlock(&operation->mutex); return 0;
}
uint32_t chtholly_next_resource_operation_v2_cancel_requested(chtholly_next_resource_operation_v2 *operation) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC) return 0;
  payload_mutex_lock(&operation->mutex);
  uint32_t result = operation->cancel_requested;
  payload_mutex_unlock(&operation->mutex); return result;
}
int32_t chtholly_next_resource_operation_v2_destroy(chtholly_next_resource_operation_v2 *operation) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  payload_mutex_lock(&operation->mutex);
  if (operation->state == CHTHOLLY_NEXT_RESOURCE_OPERATION_ARMED || operation->provider_active) {
    payload_mutex_unlock(&operation->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY;
  }
  operation->magic = 0;
  payload_mutex_unlock(&operation->mutex);
  payload_mutex_destroy(&operation->mutex);
  free(operation); return 0;
}
uint32_t chtholly_next_resource_operation_v2_state(const chtholly_next_resource_operation_v2 *operation) {
  if (!operation || operation->magic != NEXT_RESOURCE_OPERATION_MAGIC) return 0;
  payload_mutex_lock((NextPayloadMutex *)&operation->mutex);
  uint32_t state = operation->state;
  payload_mutex_unlock((NextPayloadMutex *)&operation->mutex); return state;
}

/* Opaque-byte ABI-2 payload transport. This intentionally has no blocking
 * behavior: a host may retry when capacity or quiescence is unavailable. */
#define NEXT_PAYLOAD_MAGIC 0x54525032u
#define NEXT_PAYLOAD_TOKEN_MAGIC 0x54504b32u
#define NEXT_PAYLOAD_SEND 1u
#define NEXT_PAYLOAD_RECEIVE 2u
typedef struct NextPayloadNode NextPayloadNode;
typedef struct NextPayloadToken NextPayloadToken;
struct NextPayloadNode { NextPayloadNode *next; void *bytes; };
struct chtholly_next_payload_transport_v2 {
  uint32_t magic;
  chtholly_next_resource_lease_v2 *lease;
  uint8_t digest[32];
  uint64_t capacity, payload_size, size, reservations;
  uint32_t closed, quiesced;
  NextPayloadMutex mutex;
  chtholly_next_payload_descriptor_v2 descriptor;
  int typed;
  uint64_t callbacks;
  NextPayloadNode *head, *tail;
};
struct NextPayloadToken {
  uint32_t magic, kind, state;
  chtholly_next_payload_transport_v2 *transport;
  chtholly_next_resource_token_v2 lease_token;
  const void *source;
  NextPayloadNode *node;
};
struct chtholly_next_owned_payload_v2 {
  chtholly_next_payload_descriptor_v2 descriptor;
  void *bytes;
};
static void *payload_allocate(uint64_t size, uint64_t alignment) {
#if defined(_WIN32)
  return _aligned_malloc((size_t)size, (size_t)alignment);
#else
  void *out = NULL;
  size_t align = alignment < sizeof(void *) ? sizeof(void *) : (size_t)alignment;
  return posix_memalign(&out, align, (size_t)size) == 0 ? out : NULL;
#endif
}
static void payload_free(void *bytes) {
#if defined(_WIN32)
  _aligned_free(bytes);
#else
  free(bytes);
#endif
}
static void payload_drop_node(chtholly_next_payload_transport_v2 *t, NextPayloadNode *node) {
  if (t->typed) t->descriptor.drop(t->descriptor.context, node->bytes);
  payload_free(node->bytes); free(node);
}
static int payload_valid(const chtholly_next_payload_transport_v2 *t) {
  return t != NULL && t->magic == NEXT_PAYLOAD_MAGIC && valid_lease(t->lease);
}
static int payload_token_valid(const NextPayloadToken *tok, uint32_t kind,
                               uint32_t state) {
  return tok != NULL && tok->magic == NEXT_PAYLOAD_TOKEN_MAGIC &&
         tok->kind == kind && tok->state == state && payload_valid(tok->transport);
}
int32_t chtholly_next_payload_transport_v2_create(
    chtholly_next_resource_lease_v2 *lease, const uint8_t descriptor_digest[32],
    uint64_t capacity, uint64_t payload_size,
    chtholly_next_payload_transport_v2 **out_transport) {
  chtholly_next_payload_transport_v2 *t;
  if (!valid_lease(lease)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  if (!out_transport || !valid_digest(descriptor_digest) || capacity == 0 ||
      payload_size == 0 || payload_size > (uint64_t)SIZE_MAX)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out_transport = NULL;
  t = (chtholly_next_payload_transport_v2 *)calloc(1, sizeof(*t));
  if (!t) return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY;
  t->magic = NEXT_PAYLOAD_MAGIC; t->lease = lease;
  memcpy(t->digest, descriptor_digest, 32); t->capacity = capacity;
  if (memcmp(lease->descriptor_digest, descriptor_digest, 32) != 0) { free(t); return CHTHOLLY_NEXT_RESOURCE_LEASE_STALE; }
  t->payload_size = payload_size;
  t->descriptor.alignment = sizeof(void *);
  payload_mutex_init(&t->mutex);
  payload_mutex_lock(&lease->mutex);
  if (lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSING || lease->state == NEXT_RESOURCE_LEASE_STATE_CLOSED) {
    payload_mutex_unlock(&lease->mutex); payload_mutex_destroy(&t->mutex); free(t);
    return CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED;
  }
  ++lease->transports;
  payload_mutex_unlock(&lease->mutex);
  *out_transport = t;
  return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}
int32_t chtholly_next_payload_transport_v2_create_typed(
    chtholly_next_resource_lease_v2 *lease, const uint8_t digest[32], uint64_t capacity,
    const chtholly_next_payload_descriptor_v2 *descriptor, chtholly_next_payload_transport_v2 **out) {
  if (!out) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out = NULL;
  if (!descriptor || descriptor->struct_size != sizeof(*descriptor) ||
      descriptor->version != CHTHOLLY_NEXT_PAYLOAD_DESCRIPTOR_VERSION ||
      !descriptor->move || !descriptor->drop || !descriptor->alignment ||
      (descriptor->alignment & (descriptor->alignment - 1)) || descriptor->alignment > SIZE_MAX ||
      !valid_digest(descriptor->type_digest) || !valid_digest(descriptor->layout_digest) ||
      !valid_digest(descriptor->lifecycle_digest) ||
      !descriptor->retain_owner || !descriptor->release_owner || !descriptor->owner)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  int32_t status = descriptor->retain_owner(descriptor->owner);
  if (status) return status;
  status = chtholly_next_payload_transport_v2_create(lease, digest, capacity, descriptor->size, out);
  if (status) { descriptor->release_owner(descriptor->owner); return status; }
  (*out)->descriptor = *descriptor;
  (*out)->typed = 1;
  return 0;
}
int32_t chtholly_next_payload_transport_v2_send_prepare(
    chtholly_next_payload_transport_v2 *t, const void *source,
    uint64_t source_size, void **out_token) {
  NextPayloadToken *tok; int32_t status;
  if (!payload_valid(t) || !source || !out_token || source_size != t->payload_size)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out_token = NULL; payload_mutex_lock(&t->mutex);
  if (t->closed) { payload_mutex_unlock(&t->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED; }
  if (t->size + t->reservations >= t->capacity)
    { payload_mutex_unlock(&t->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY; }
  tok = (NextPayloadToken *)calloc(1, sizeof(*tok));
  if (!tok) { payload_mutex_unlock(&t->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY; }
  status = chtholly_next_resource_lease_v2_acquire(t->lease, t->digest,
                                                   &tok->lease_token);
  if (status != CHTHOLLY_NEXT_RESOURCE_LEASE_OK) { free(tok); payload_mutex_unlock(&t->mutex); return status; }
  tok->magic = NEXT_PAYLOAD_TOKEN_MAGIC; tok->kind = NEXT_PAYLOAD_SEND;
  tok->state = CHTHOLLY_NEXT_PAYLOAD_SEND_PREPARED; tok->transport = t;
  tok->source = source; ++t->reservations; *out_token = tok; payload_mutex_unlock(&t->mutex); return 0;
}
int32_t chtholly_next_payload_transport_v2_send_commit(void *opaque) {
  NextPayloadToken *tok = (NextPayloadToken *)opaque; NextPayloadNode *node;
  if (!payload_token_valid(tok, NEXT_PAYLOAD_SEND,
                           CHTHOLLY_NEXT_PAYLOAD_SEND_PREPARED))
    return CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  payload_mutex_lock(&tok->transport->mutex);
  if (tok->transport->closed) { payload_mutex_unlock(&tok->transport->mutex); (void)chtholly_next_payload_transport_v2_send_cancel(tok); return CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED; }
  node = (NextPayloadNode *)calloc(1, sizeof(*node));
  if (!node) { payload_mutex_unlock(&tok->transport->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY; }
  node->bytes = payload_allocate(tok->transport->payload_size, tok->transport->descriptor.alignment);
  if (!node->bytes) { free(node); payload_mutex_unlock(&tok->transport->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY; }
  /* Commit is selected now. The token lease prevents close while user code runs. */
  payload_mutex_unlock(&tok->transport->mutex);
  if (tok->transport->typed)
    tok->transport->descriptor.move(tok->transport->descriptor.context, node->bytes, (void *)tok->source);
  else memcpy(node->bytes, tok->source, (size_t)tok->transport->payload_size);
  payload_mutex_lock(&tok->transport->mutex);
  if (tok->transport->tail) tok->transport->tail->next = node; else tok->transport->head = node;
  tok->transport->tail = node; ++tok->transport->size; --tok->transport->reservations;
  (void)chtholly_next_resource_lease_v2_release(&tok->lease_token);
  tok->magic = 0; payload_mutex_unlock(&tok->transport->mutex); free(tok); return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}
int32_t chtholly_next_payload_transport_v2_send_cancel(void *opaque) {
  NextPayloadToken *tok = (NextPayloadToken *)opaque;
  if (!payload_token_valid(tok, NEXT_PAYLOAD_SEND, CHTHOLLY_NEXT_PAYLOAD_SEND_PREPARED))
    return CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  payload_mutex_lock(&tok->transport->mutex);
  if (tok->transport->reservations) --tok->transport->reservations;
  (void)chtholly_next_resource_lease_v2_release(&tok->lease_token);
  tok->magic = 0; payload_mutex_unlock(&tok->transport->mutex); free(tok); return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}
int32_t chtholly_next_payload_transport_v2_send_fail(void *opaque) {
  return chtholly_next_payload_transport_v2_send_cancel(opaque);
}
int32_t chtholly_next_payload_transport_v2_receive_acquire(
    chtholly_next_payload_transport_v2 *t, void **out_token,
    const void **out_payload, uint64_t *out_size) {
  NextPayloadToken *tok; NextPayloadNode *node; int32_t status;
  if (!payload_valid(t) || !out_token || !out_payload || !out_size)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out_token = NULL; *out_payload = NULL; *out_size = 0; payload_mutex_lock(&t->mutex);
  if (!t->head) { const int32_t status = t->closed ? CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED : CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY; payload_mutex_unlock(&t->mutex); return status; }
  node = t->head; t->head = node->next; if (!t->head) t->tail = NULL; --t->size;
  tok = (NextPayloadToken *)calloc(1, sizeof(*tok));
  if (!tok) { node->next = t->head; t->head = node; if (!t->tail) t->tail = node; ++t->size; payload_mutex_unlock(&t->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY; }
  status = chtholly_next_resource_lease_v2_acquire(t->lease, t->digest, &tok->lease_token);
  if (status != CHTHOLLY_NEXT_RESOURCE_LEASE_OK) { free(tok); node->next=t->head; t->head=node; if (!t->tail) t->tail=node; ++t->size; payload_mutex_unlock(&t->mutex); return status; }
  tok->magic=NEXT_PAYLOAD_TOKEN_MAGIC; tok->kind=NEXT_PAYLOAD_RECEIVE; tok->state=CHTHOLLY_NEXT_PAYLOAD_RECEIVE_ACQUIRED; tok->transport=t; tok->node=node;
  *out_token=tok; *out_payload=node->bytes; *out_size=t->payload_size; payload_mutex_unlock(&t->mutex); return 0;
}
int32_t chtholly_next_payload_transport_v2_receive_commit(void *opaque, void *destination, uint64_t destination_size) {
  NextPayloadToken *tok=(NextPayloadToken *)opaque; NextPayloadNode *node;
  if (!payload_token_valid(tok,NEXT_PAYLOAD_RECEIVE,CHTHOLLY_NEXT_PAYLOAD_RECEIVE_ACQUIRED) || !destination || destination_size != tok->transport->payload_size)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  if (tok->transport->typed) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  payload_mutex_lock(&tok->transport->mutex);
  node=tok->node; memcpy(destination,node->bytes,(size_t)destination_size); payload_free(node->bytes); free(node);
  (void)chtholly_next_resource_lease_v2_release(&tok->lease_token); tok->magic=0; payload_mutex_unlock(&tok->transport->mutex); free(tok); return 0;
}
int32_t chtholly_next_payload_transport_v2_receive_cancel(void *opaque) {
  NextPayloadToken *tok=(NextPayloadToken *)opaque; NextPayloadNode *node;
  if (!payload_token_valid(tok,NEXT_PAYLOAD_RECEIVE,CHTHOLLY_NEXT_PAYLOAD_RECEIVE_ACQUIRED)) return CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  node=tok->node;
  payload_drop_node(tok->transport, node);
  payload_mutex_lock(&tok->transport->mutex);
  (void)chtholly_next_resource_lease_v2_release(&tok->lease_token);
  tok->magic=0; payload_mutex_unlock(&tok->transport->mutex); free(tok); return 0;
}
int32_t chtholly_next_payload_transport_v2_receive_take(void *opaque, chtholly_next_owned_payload_v2 **out) {
  NextPayloadToken *tok = (NextPayloadToken *)opaque;
  if (!out) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  *out = NULL;
  if (!payload_token_valid(tok, NEXT_PAYLOAD_RECEIVE, CHTHOLLY_NEXT_PAYLOAD_RECEIVE_ACQUIRED) || !tok->transport->typed)
    return CHTHOLLY_NEXT_RESOURCE_OPERATION_INVALID_STATE;
  chtholly_next_owned_payload_v2 *value = calloc(1, sizeof(*value));
  if (!value) return CHTHOLLY_NEXT_RESOURCE_LEASE_OUT_OF_MEMORY;
  value->descriptor = tok->transport->descriptor;
  int32_t status = value->descriptor.retain_owner(value->descriptor.owner);
  if (status) { free(value); return status; }
  value->bytes = tok->node->bytes; free(tok->node);
  payload_mutex_lock(&tok->transport->mutex);
  status = chtholly_next_resource_lease_v2_release(&tok->lease_token);
  payload_mutex_unlock(&tok->transport->mutex);
  tok->magic = 0; free(tok); *out = value; return status;
}
const void *chtholly_next_owned_payload_v2_data(const chtholly_next_owned_payload_v2 *value) {
  return value ? value->bytes : NULL;
}
int32_t chtholly_next_owned_payload_v2_destroy(chtholly_next_owned_payload_v2 *value) {
  if (!value) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  value->descriptor.drop(value->descriptor.context, value->bytes);
  payload_free(value->bytes);
  value->descriptor.release_owner(value->descriptor.owner);
  free(value); return 0;
}
int32_t chtholly_next_payload_transport_v2_receive_fail(void *opaque) {
  return chtholly_next_payload_transport_v2_receive_cancel(opaque);
}
int32_t chtholly_next_payload_transport_v2_close(chtholly_next_payload_transport_v2 *t) {
  NextPayloadNode *node, *next;
  if (!payload_valid(t)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  payload_mutex_lock(&t->mutex);
  if (t->quiesced) { payload_mutex_unlock(&t->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_ALREADY_CLOSED; }
  if (!t->closed) {
    t->closed=1;
    (void)chtholly_next_resource_lease_v2_begin_close(t->lease);
  }
  if (t->callbacks != 0 || chtholly_next_resource_lease_v2_active(t->lease) != 0) { payload_mutex_unlock(&t->mutex); return CHTHOLLY_NEXT_RESOURCE_LEASE_NOT_READY; }
  node=t->head; t->head=t->tail=NULL; t->size=0;
  ++t->callbacks; payload_mutex_unlock(&t->mutex);
  while(node){next=node->next; payload_drop_node(t,node); node=next;}
  payload_mutex_lock(&t->mutex); --t->callbacks;
  {
    const int32_t status = chtholly_next_resource_lease_v2_quiesce(t->lease);
    if (status == CHTHOLLY_NEXT_RESOURCE_LEASE_OK) t->quiesced = 1;
    payload_mutex_unlock(&t->mutex); return status;
  }
}
uint64_t chtholly_next_payload_transport_v2_size(const chtholly_next_payload_transport_v2 *t) { if (!payload_valid(t)) return 0; payload_mutex_lock((NextPayloadMutex *)&t->mutex); const uint64_t size=t->size; payload_mutex_unlock((NextPayloadMutex *)&t->mutex); return size; }
uint64_t chtholly_next_payload_transport_v2_payload_size(const chtholly_next_payload_transport_v2 *t) { return payload_valid(t) ? t->payload_size : 0; }
int32_t chtholly_next_payload_transport_v2_check_digest(const chtholly_next_payload_transport_v2 *t, const uint8_t digest[32]) {
  if (!payload_valid(t) || !valid_digest(digest)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_ARGUMENT;
  return memcmp(t->digest, digest, 32) == 0 ? CHTHOLLY_NEXT_RESOURCE_LEASE_OK : CHTHOLLY_NEXT_RESOURCE_LEASE_STALE;
}
int32_t chtholly_next_payload_transport_v2_destroy(chtholly_next_payload_transport_v2 *t) {
  if (!payload_valid(t)) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  if (!t->quiesced || chtholly_next_resource_lease_v2_active(t->lease) != 0 || t->size != 0)
    return CHTHOLLY_NEXT_RESOURCE_LEASE_BUSY;
  if (t->typed) t->descriptor.release_owner(t->descriptor.owner);
  payload_mutex_lock(&t->lease->mutex); --t->lease->transports; payload_mutex_unlock(&t->lease->mutex);
  payload_mutex_destroy(&t->mutex); t->magic = 0; free(t); return CHTHOLLY_NEXT_RESOURCE_LEASE_OK;
}
