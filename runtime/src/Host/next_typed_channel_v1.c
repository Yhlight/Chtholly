#include "chtholly/next_host_v1.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
typedef SRWLOCK NextTypedMutex;
typedef CONDITION_VARIABLE NextTypedCondition;
static void typed_mutex_init(NextTypedMutex *m) { InitializeSRWLock(m); }
static void typed_lock(NextTypedMutex *m) { AcquireSRWLockExclusive(m); }
static void typed_unlock(NextTypedMutex *m) { ReleaseSRWLockExclusive(m); }
static void typed_condition_init(NextTypedCondition *c) {
  InitializeConditionVariable(c);
}
static void typed_wait(NextTypedCondition *c, NextTypedMutex *m) {
  SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static void typed_wake_all(NextTypedCondition *c) {
  WakeAllConditionVariable(c);
}
#else
#include <pthread.h>
typedef pthread_mutex_t NextTypedMutex;
typedef pthread_cond_t NextTypedCondition;
static void typed_mutex_init(NextTypedMutex *m) { (void)pthread_mutex_init(m, 0); }
static void typed_lock(NextTypedMutex *m) { (void)pthread_mutex_lock(m); }
static void typed_unlock(NextTypedMutex *m) { (void)pthread_mutex_unlock(m); }
static void typed_condition_init(NextTypedCondition *c) {
  (void)pthread_cond_init(c, 0);
}
static void typed_wait(NextTypedCondition *c, NextTypedMutex *m) {
  (void)pthread_cond_wait(c, m);
}
static void typed_wake_all(NextTypedCondition *c) {
  (void)pthread_cond_broadcast(c);
}
#endif

#define NEXT_TYPED_CHANNEL_MAGIC UINT64_C(0x4348545950454431)
#define NEXT_TYPED_TOKEN_MAGIC UINT64_C(0x544f4b454e000001)
#define NEXT_TYPED_SEND 1u
#define NEXT_TYPED_RECEIVE 2u
#define NEXT_TYPED_PREPARED 1u
#define NEXT_TYPED_ACQUIRED 2u

typedef struct NextTypedNode {
  struct NextTypedNode *next;
  void *payload;
} NextTypedNode;

typedef struct NextTypedChannel {
  uint64_t magic;
  uint64_t generation;
  uint64_t capacity;
  uint64_t size;
  uint64_t reservations;
  uint64_t active_receives;
  uint64_t active_calls;
  struct NextTypedChannel *registry_next;
  int closed;
  chtholly_next_typed_channel_descriptor descriptor;
  NextTypedNode *head;
  NextTypedNode *tail;
  NextTypedMutex mutex;
  NextTypedCondition readable;
  NextTypedCondition writable;
  NextTypedCondition quiescent;
} NextTypedChannel;

#if defined(_WIN32)
static NextTypedMutex registry_mutex = SRWLOCK_INIT;
#else
static NextTypedMutex registry_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static NextTypedChannel *registry_head;

/* The registry pins entry calls without dereferencing an already closed handle.
 * Lock order is registry then channel; callbacks never acquire the registry. */
static NextTypedChannel *typed_acquire(void *opaque) {
  typed_lock(&registry_mutex);
  NextTypedChannel *channel = registry_head;
  while (channel != NULL && channel != opaque)
    channel = channel->registry_next;
  if (channel != NULL) {
    typed_lock(&channel->mutex);
    ++channel->active_calls;
    typed_unlock(&channel->mutex);
  }
  typed_unlock(&registry_mutex);
  return channel;
}
static void typed_release(NextTypedChannel *channel) {
  typed_lock(&channel->mutex);
  --channel->active_calls;
  typed_wake_all(&channel->quiescent);
  typed_unlock(&channel->mutex);
}
#ifdef CHTHOLLY_RUNTIME_TESTING
static atomic_int fail_next_allocation;
void chtholly_next_host_v1_typed_channel_test_fail_allocate(void) {
  atomic_store(&fail_next_allocation, 1);
}
uint64_t chtholly_next_host_v1_typed_channel_test_active(void *opaque) {
  NextTypedChannel *channel = typed_acquire(opaque);
  if (!channel) return 0;
  typed_lock(&channel->mutex);
  uint64_t count = channel->active_calls - 1;
  typed_unlock(&channel->mutex);
  typed_release(channel);
  return count;
}
#endif
static void *typed_allocate(uint64_t size, uint64_t alignment) {
#ifdef CHTHOLLY_RUNTIME_TESTING
  if (atomic_exchange(&fail_next_allocation, 0)) return NULL;
#endif
#if defined(_WIN32)
  return _aligned_malloc((size_t)size, (size_t)alignment);
#else
  void *result = NULL;
  const size_t actual_alignment = alignment < sizeof(void *) ? sizeof(void *) : (size_t)alignment;
  return posix_memalign(&result, actual_alignment, (size_t)size) == 0 ? result : NULL;
#endif
}
static void typed_deallocate(void *value) {
#if defined(_WIN32)
  _aligned_free(value);
#else
  free(value);
#endif
}

static void token_clear(chtholly_next_typed_channel_token *token) {
  if (token != NULL)
    memset(token, 0, sizeof(*token));
}

static int descriptor_valid(const chtholly_next_typed_channel_descriptor *d) {
  if (d == NULL || d->abi != CHTHOLLY_NEXT_TYPED_CHANNEL_ABI_V1 ||
      (d->capabilities & CHTHOLLY_NEXT_TYPED_CHANNEL_SEND) == 0 ||
      d->size == 0 || d->size > (uint64_t)SIZE_MAX || d->alignment == 0 ||
      (d->alignment & (d->alignment - 1u)) != 0 ||
      d->alignment > (uint64_t)SIZE_MAX || d->move == NULL ||
      d->drop == NULL)
    return 0;
  return 1;
}

static void free_node(NextTypedChannel *channel, NextTypedNode *node,
                      int destroy) {
  if (node == NULL)
    return;
  if (destroy && node->payload != NULL)
    channel->descriptor.drop(node->payload);
  typed_deallocate(node->payload);
  free(node);
}

static int token_valid(const chtholly_next_typed_channel_token *token,
                       uint32_t kind, uint32_t state,
                       NextTypedChannel **out_channel) {
  if (token == NULL || token->generation != NEXT_TYPED_TOKEN_MAGIC ||
      token->kind != kind || token->state != state || token->destination != token)
    return 0;
  NextTypedChannel *channel = (NextTypedChannel *)token->channel;
  if (channel == NULL || (kind == NEXT_TYPED_RECEIVE && token->node == NULL))
    return 0;
  if (out_channel != NULL)
    *out_channel = channel;
  return 1;
}

int32_t chtholly_next_host_v1_typed_channel_init(
    uint64_t capacity, const chtholly_next_typed_channel_descriptor *descriptor,
    void **out_channel) {
  if (out_channel == NULL || capacity == 0 || !descriptor_valid(descriptor))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_channel = NULL;
  NextTypedChannel *channel = (NextTypedChannel *)calloc(1, sizeof(*channel));
  if (channel == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  channel->capacity = capacity;
  channel->descriptor = *descriptor;
  channel->generation = 1;
  typed_mutex_init(&channel->mutex);
  typed_condition_init(&channel->readable);
  typed_condition_init(&channel->writable);
  typed_condition_init(&channel->quiescent);
  channel->magic = NEXT_TYPED_CHANNEL_MAGIC;
  typed_lock(&registry_mutex);
  channel->registry_next = registry_head;
  registry_head = channel;
  typed_unlock(&registry_mutex);
  *out_channel = channel;
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_send_prepare(
    void *opaque, const void *source,
    chtholly_next_typed_channel_token *out_token) {
  if (source == NULL || out_token == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  NextTypedChannel *channel = typed_acquire(opaque);
  if (channel == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  token_clear(out_token);
  typed_lock(&channel->mutex);
  while (!channel->closed &&
         (channel->size >= channel->capacity ||
          channel->reservations >= channel->capacity - channel->size)) {
    typed_wait(&channel->writable, &channel->mutex);
  }
  if (channel->closed) {
    typed_unlock(&channel->mutex);
    typed_release(channel);
    return CHTHOLLY_NEXT_HOST_STATUS_CLOSED;
  }
  ++channel->reservations;
  out_token->channel = channel;
  out_token->destination = out_token;
  out_token->source = source;
  out_token->generation = NEXT_TYPED_TOKEN_MAGIC;
  out_token->state = NEXT_TYPED_PREPARED;
  out_token->kind = NEXT_TYPED_SEND;
  typed_unlock(&channel->mutex);
  typed_release(channel);
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_send_commit(
    chtholly_next_typed_channel_token *token) {
  NextTypedChannel *channel = NULL;
  if (!token_valid(token, NEXT_TYPED_SEND, NEXT_TYPED_PREPARED, &channel))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  typed_lock(&channel->mutex);
  if (channel->closed) {
    if (channel->reservations != 0)
      --channel->reservations;
    typed_wake_all(&channel->writable);
    if (channel->reservations == 0 && channel->active_receives == 0)
      typed_wake_all(&channel->quiescent);
    typed_unlock(&channel->mutex);
    token_clear(token);
    return CHTHOLLY_NEXT_HOST_STATUS_CLOSED;
  }
  NextTypedNode *node = (NextTypedNode *)calloc(1, sizeof(*node));
  void *payload = node == NULL ? NULL : typed_allocate(channel->descriptor.size, channel->descriptor.alignment);
  if (node == NULL || payload == NULL) {
    typed_deallocate(payload);
    free(node);
    --channel->reservations;
    typed_wake_all(&channel->writable);
    typed_wake_all(&channel->quiescent);
    typed_unlock(&channel->mutex);
    token_clear(token);
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  }
  /* Commit is selected under the lock; the reservation pins storage while
   * the user move callback executes outside it. A later close drains this
   * committed value rather than undoing its ownership transfer. */
  typed_unlock(&channel->mutex);
  channel->descriptor.move(payload, (void *)token->source);
  node->payload = payload;
  typed_lock(&channel->mutex);
  if (channel->tail != NULL)
    channel->tail->next = node;
  else
    channel->head = node;
  channel->tail = node;
  --channel->reservations;
  ++channel->size;
  ++channel->generation;
  typed_wake_all(&channel->readable);
  typed_wake_all(&channel->quiescent);
  typed_unlock(&channel->mutex);
  token_clear(token);
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_send_cancel(
    chtholly_next_typed_channel_token *token) {
  NextTypedChannel *channel = NULL;
  if (!token_valid(token, NEXT_TYPED_SEND, NEXT_TYPED_PREPARED, &channel))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  typed_lock(&channel->mutex);
  if (channel->reservations != 0)
    --channel->reservations;
  typed_wake_all(&channel->writable);
  if (channel->reservations == 0 && channel->active_receives == 0)
    typed_wake_all(&channel->quiescent);
  typed_unlock(&channel->mutex);
  token_clear(token);
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_receive_acquire(
    void *opaque, chtholly_next_typed_channel_token *out_token) {
  if (out_token == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  NextTypedChannel *channel = typed_acquire(opaque);
  if (channel == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  token_clear(out_token);
  typed_lock(&channel->mutex);
  while (!channel->closed && channel->size == 0)
    typed_wait(&channel->readable, &channel->mutex);
  if (channel->size == 0 && channel->closed) {
    typed_unlock(&channel->mutex);
    typed_release(channel);
    return CHTHOLLY_NEXT_HOST_STATUS_CLOSED;
  }
  NextTypedNode *node = channel->head;
  channel->head = node->next;
  if (channel->head == NULL)
    channel->tail = NULL;
  --channel->size;
  ++channel->active_receives;
  out_token->channel = channel;
  out_token->destination = out_token;
  out_token->node = node;
  out_token->generation = NEXT_TYPED_TOKEN_MAGIC;
  out_token->state = NEXT_TYPED_ACQUIRED;
  out_token->kind = NEXT_TYPED_RECEIVE;
  typed_wake_all(&channel->writable);
  typed_unlock(&channel->mutex);
  typed_release(channel);
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_receive_commit(
    chtholly_next_typed_channel_token *token, void *destination) {
  NextTypedChannel *channel = NULL;
  if (!token_valid(token, NEXT_TYPED_RECEIVE, NEXT_TYPED_ACQUIRED, &channel) ||
      destination == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  NextTypedNode *node = (NextTypedNode *)token->node;
  channel->descriptor.move(destination, node->payload);
  free_node(channel, node, 0);
  typed_lock(&channel->mutex);
  if (channel->active_receives != 0)
    --channel->active_receives;
  if (channel->active_receives == 0 && channel->reservations == 0)
    typed_wake_all(&channel->quiescent);
  typed_unlock(&channel->mutex);
  token_clear(token);
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_receive_cancel(
    chtholly_next_typed_channel_token *token) {
  NextTypedChannel *channel = NULL;
  if (!token_valid(token, NEXT_TYPED_RECEIVE, NEXT_TYPED_ACQUIRED, &channel))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  free_node(channel, (NextTypedNode *)token->node, 1);
  typed_lock(&channel->mutex);
  if (channel->active_receives != 0)
    --channel->active_receives;
  if (channel->active_receives == 0 && channel->reservations == 0)
    typed_wake_all(&channel->quiescent);
  typed_unlock(&channel->mutex);
  token_clear(token);
  return 0;
}

int32_t chtholly_next_host_v1_typed_channel_close(void *opaque) {
  typed_lock(&registry_mutex);
  NextTypedChannel **entry = &registry_head;
  while (*entry != NULL && *entry != opaque)
    entry = &(*entry)->registry_next;
  NextTypedChannel *channel = *entry;
  if (channel == NULL) {
    typed_unlock(&registry_mutex);
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  }
  typed_lock(&channel->mutex);
  *entry = channel->registry_next;
  typed_unlock(&registry_mutex);
  channel->closed = 1;
  typed_wake_all(&channel->readable);
  typed_wake_all(&channel->writable);
  while (channel->reservations != 0 || channel->active_receives != 0 || channel->active_calls != 0)
    typed_wait(&channel->quiescent, &channel->mutex);
  NextTypedNode *node = channel->head;
  channel->head = NULL;
  channel->tail = NULL;
  typed_unlock(&channel->mutex);
  while (node != NULL) {
    NextTypedNode *next = node->next;
    free_node(channel, node, 1);
    node = next;
  }
#if !defined(_WIN32)
  (void)pthread_cond_destroy(&channel->readable);
  (void)pthread_cond_destroy(&channel->writable);
  (void)pthread_cond_destroy(&channel->quiescent);
  (void)pthread_mutex_destroy(&channel->mutex);
#endif
  channel->magic = 0;
  free(channel);
  return 0;
}
