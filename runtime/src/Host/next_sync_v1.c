#include "chtholly/next_host_v1.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION NextSyncNativeMutex;
typedef CONDITION_VARIABLE NextSyncNativeCondition;
#else
#include <pthread.h>
typedef pthread_mutex_t NextSyncNativeMutex;
typedef pthread_cond_t NextSyncNativeCondition;
#endif

typedef struct NextSyncMutex {
  NextSyncNativeMutex native;
  uint32_t magic;
} NextSyncMutex;

typedef struct NextSyncCondvar {
  NextSyncNativeCondition native;
  uint32_t magic;
} NextSyncCondvar;

#define NEXT_SYNC_MUTEX_MAGIC 0x4d545831u
#define NEXT_SYNC_CONDVAR_MAGIC 0x43564431u

static NextSyncMutex *next_sync_valid(void *opaque) {
  NextSyncMutex *mutex = (NextSyncMutex *)opaque;
  return mutex != NULL && mutex->magic == NEXT_SYNC_MUTEX_MAGIC ? mutex : NULL;
}

int32_t chtholly_next_host_v1_sync_mutex_init(void **out_mutex) {
  NextSyncMutex *mutex;
  if (out_mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_mutex = NULL;
  mutex = (NextSyncMutex *)calloc(1, sizeof(*mutex));
  if (mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
#if defined(_WIN32)
  InitializeCriticalSection(&mutex->native);
#else
  if (pthread_mutex_init(&mutex->native, NULL) != 0) {
    free(mutex);
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  }
#endif
  mutex->magic = NEXT_SYNC_MUTEX_MAGIC;
  *out_mutex = mutex;
  return 0;
}

int32_t chtholly_next_host_v1_sync_mutex_lock(void *opaque) {
  NextSyncMutex *mutex = next_sync_valid(opaque);
  if (mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if defined(_WIN32)
  EnterCriticalSection(&mutex->native);
  return 0;
#else
  return pthread_mutex_lock(&mutex->native) == 0
             ? 0
             : CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#endif
}

int32_t chtholly_next_host_v1_sync_mutex_unlock(void *opaque) {
  NextSyncMutex *mutex = next_sync_valid(opaque);
  if (mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if defined(_WIN32)
  LeaveCriticalSection(&mutex->native);
  return 0;
#else
  return pthread_mutex_unlock(&mutex->native) == 0
             ? 0
             : CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#endif
}

int32_t chtholly_next_host_v1_sync_mutex_close(void *opaque) {
  NextSyncMutex *mutex = next_sync_valid(opaque);
  if (mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if defined(_WIN32)
  DeleteCriticalSection(&mutex->native);
#else
  if (pthread_mutex_destroy(&mutex->native) != 0) {
    mutex->magic = 0;
    free(mutex);
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  }
#endif
  mutex->magic = 0;
  free(mutex);
  return 0;
}

static NextSyncCondvar *next_sync_valid_condvar(void *opaque) {
  NextSyncCondvar *condition = (NextSyncCondvar *)opaque;
  return condition != NULL && condition->magic == NEXT_SYNC_CONDVAR_MAGIC
             ? condition
             : NULL;
}

int32_t chtholly_next_host_v1_sync_condvar_init(void **out_condition) {
  NextSyncCondvar *condition;
  if (out_condition == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_condition = NULL;
  condition = (NextSyncCondvar *)calloc(1, sizeof(*condition));
  if (condition == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
#if defined(_WIN32)
  InitializeConditionVariable(&condition->native);
#else
  if (pthread_cond_init(&condition->native, NULL) != 0) {
    free(condition);
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  }
#endif
  condition->magic = NEXT_SYNC_CONDVAR_MAGIC;
  *out_condition = condition;
  return 0;
}

int32_t chtholly_next_host_v1_sync_condvar_wait(void *condition_opaque,
                                                void *mutex_opaque) {
  NextSyncCondvar *condition = next_sync_valid_condvar(condition_opaque);
  NextSyncMutex *mutex = next_sync_valid(mutex_opaque);
  if (condition == NULL || mutex == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if defined(_WIN32)
  return SleepConditionVariableCS(&condition->native, &mutex->native, INFINITE)
             ? 0
             : CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#else
  return pthread_cond_wait(&condition->native, &mutex->native) == 0
             ? 0
             : CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#endif
}

int32_t chtholly_next_host_v1_sync_condvar_notify_one(void *opaque) {
  NextSyncCondvar *condition = next_sync_valid_condvar(opaque);
  if (condition == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if defined(_WIN32)
  WakeConditionVariable(&condition->native);
#else
  if (pthread_cond_signal(&condition->native) != 0)
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#endif
  return 0;
}

int32_t chtholly_next_host_v1_sync_condvar_notify_all(void *opaque) {
  NextSyncCondvar *condition = next_sync_valid_condvar(opaque);
  if (condition == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if defined(_WIN32)
  WakeAllConditionVariable(&condition->native);
#else
  if (pthread_cond_broadcast(&condition->native) != 0)
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#endif
  return 0;
}

int32_t chtholly_next_host_v1_sync_condvar_close(void *opaque) {
  NextSyncCondvar *condition = next_sync_valid_condvar(opaque);
  if (condition == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
#if !defined(_WIN32)
  if (pthread_cond_destroy(&condition->native) != 0) {
    condition->magic = 0;
    free(condition);
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  }
#endif
  condition->magic = 0;
  free(condition);
  return 0;
}
