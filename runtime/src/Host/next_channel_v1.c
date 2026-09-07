#include "chtholly/next_host_v1.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef SRWLOCK NextChannelMutex;
typedef CONDITION_VARIABLE NextChannelCondition;
static void next_channel_mutex_init(NextChannelMutex *mutex) {
  InitializeSRWLock(mutex);
}
static void next_channel_condition_init(NextChannelCondition *condition) {
  InitializeConditionVariable(condition);
}
static void next_channel_lock(NextChannelMutex *mutex) {
  AcquireSRWLockExclusive(mutex);
}
static void next_channel_unlock(NextChannelMutex *mutex) {
  ReleaseSRWLockExclusive(mutex);
}
static void next_channel_wait(NextChannelCondition *condition,
                              NextChannelMutex *mutex) {
  (void)SleepConditionVariableSRW(condition, mutex, INFINITE, 0);
}
static void next_channel_wake_all(NextChannelCondition *condition) {
  WakeAllConditionVariable(condition);
}
#else
#include <pthread.h>
typedef pthread_mutex_t NextChannelMutex;
typedef pthread_cond_t NextChannelCondition;
static void next_channel_mutex_init(NextChannelMutex *mutex) {
  (void)pthread_mutex_init(mutex, NULL);
}
static void next_channel_condition_init(NextChannelCondition *condition) {
  (void)pthread_cond_init(condition, NULL);
}
static void next_channel_lock(NextChannelMutex *mutex) {
  (void)pthread_mutex_lock(mutex);
}
static void next_channel_unlock(NextChannelMutex *mutex) {
  (void)pthread_mutex_unlock(mutex);
}
static void next_channel_wait(NextChannelCondition *condition,
                              NextChannelMutex *mutex) {
  (void)pthread_cond_wait(condition, mutex);
}
static void next_channel_wake_all(NextChannelCondition *condition) {
  (void)pthread_cond_broadcast(condition);
}
#endif

typedef struct NextChannel {
  uint8_t *buffer;
  uint64_t capacity;
  uint64_t head;
  uint64_t size;
  uint32_t waiters;
  uint8_t closed;
  uint32_t magic;
  NextChannelMutex mutex;
  NextChannelCondition readable;
  NextChannelCondition writable;
  NextChannelCondition quiescent;
} NextChannel;

#define NEXT_CHANNEL_MAGIC 0x43484e31u

static NextChannel *next_channel_valid(void *opaque) {
  NextChannel *channel = (NextChannel *)opaque;
  return channel != NULL && channel->magic == NEXT_CHANNEL_MAGIC ? channel
                                                                  : NULL;
}

int32_t chtholly_next_host_v1_channel_init(uint64_t capacity,
                                           void **out_channel) {
  NextChannel *channel;
  if (out_channel == NULL || capacity == 0 || capacity > (uint64_t)SIZE_MAX)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_channel = NULL;
  channel = (NextChannel *)calloc(1, sizeof(*channel));
  if (channel == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  channel->buffer = (uint8_t *)malloc((size_t)capacity);
  if (channel->buffer == NULL) {
    free(channel);
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  }
  channel->capacity = capacity;
  next_channel_mutex_init(&channel->mutex);
  next_channel_condition_init(&channel->readable);
  next_channel_condition_init(&channel->writable);
  next_channel_condition_init(&channel->quiescent);
  channel->magic = NEXT_CHANNEL_MAGIC;
  *out_channel = channel;
  return 0;
}

int32_t chtholly_next_host_v1_channel_send(void *opaque, const uint8_t *data,
                                           uint64_t size) {
  NextChannel *channel = next_channel_valid(opaque);
  uint64_t tail;
  uint64_t first;
  if (channel == NULL || (data == NULL && size != 0) ||
      size > channel->capacity)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  next_channel_lock(&channel->mutex);
  while (!channel->closed && channel->capacity - channel->size < size) {
    ++channel->waiters;
    next_channel_wait(&channel->writable, &channel->mutex);
    --channel->waiters;
    if (channel->waiters == 0)
      next_channel_wake_all(&channel->quiescent);
  }
  if (channel->closed) {
    next_channel_unlock(&channel->mutex);
    return CHTHOLLY_NEXT_HOST_STATUS_CLOSED;
  }
  tail = (channel->head + channel->size) % channel->capacity;
  first = channel->capacity - tail;
  if (first > size)
    first = size;
  if (first != 0)
    memcpy(channel->buffer + tail, data, (size_t)first);
  if (size > first)
    memcpy(channel->buffer, data + first, (size_t)(size - first));
  channel->size += size;
  next_channel_wake_all(&channel->readable);
  next_channel_unlock(&channel->mutex);
  return 0;
}

int32_t chtholly_next_host_v1_channel_receive(void *opaque, uint8_t *buffer,
                                               uint64_t capacity,
                                               uint64_t *out_size) {
  NextChannel *channel = next_channel_valid(opaque);
  uint64_t first;
  if (channel == NULL || out_size == NULL ||
      (buffer == NULL && capacity != 0) || capacity > (uint64_t)SIZE_MAX)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_size = 0;
  if (capacity == 0)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  next_channel_lock(&channel->mutex);
  while (!channel->closed && channel->size == 0) {
    ++channel->waiters;
    next_channel_wait(&channel->readable, &channel->mutex);
    --channel->waiters;
    if (channel->waiters == 0)
      next_channel_wake_all(&channel->quiescent);
  }
  if (channel->size == 0 && channel->closed) {
    next_channel_unlock(&channel->mutex);
    return CHTHOLLY_NEXT_HOST_STATUS_CLOSED;
  }
  *out_size = channel->size < capacity ? channel->size : capacity;
  first = channel->capacity - channel->head;
  if (first > *out_size)
    first = *out_size;
  memcpy(buffer, channel->buffer + channel->head, (size_t)first);
  if (*out_size > first)
    memcpy(buffer + first, channel->buffer,
           (size_t)(*out_size - first));
  channel->head = (channel->head + *out_size) % channel->capacity;
  channel->size -= *out_size;
  next_channel_wake_all(&channel->writable);
  next_channel_unlock(&channel->mutex);
  return 0;
}

int32_t chtholly_next_host_v1_channel_close(void *opaque) {
  NextChannel *channel = next_channel_valid(opaque);
  if (channel == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  next_channel_lock(&channel->mutex);
  channel->closed = 1;
  next_channel_wake_all(&channel->readable);
  next_channel_wake_all(&channel->writable);
  while (channel->waiters != 0)
    next_channel_wait(&channel->quiescent, &channel->mutex);
  next_channel_unlock(&channel->mutex);
#if !defined(_WIN32)
  (void)pthread_cond_destroy(&channel->readable);
  (void)pthread_cond_destroy(&channel->writable);
  (void)pthread_cond_destroy(&channel->quiescent);
  (void)pthread_mutex_destroy(&channel->mutex);
#endif
  channel->magic = 0;
  free(channel->buffer);
  free(channel);
  return 0;
}
