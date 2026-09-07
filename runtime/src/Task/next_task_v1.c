#include "chtholly/next_task_v1.h"
#include "chtholly/next_runtime_v1.h"

#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <malloc.h>
#include <windows.h>
typedef HANDLE NextTaskThread;
typedef SRWLOCK NextTaskMutex;
typedef CONDITION_VARIABLE NextTaskCondition;
static int next_task_sync_initialize(NextTaskMutex *mutex,
                                     NextTaskCondition *condition) {
  InitializeSRWLock(mutex);
  InitializeConditionVariable(condition);
  return 1;
}
static void next_task_sync_destroy(NextTaskMutex *mutex,
                                   NextTaskCondition *condition) {
  (void)mutex;
  (void)condition;
}
static void next_task_lock(NextTaskMutex *mutex) {
  AcquireSRWLockExclusive(mutex);
}
static void next_task_unlock(NextTaskMutex *mutex) {
  ReleaseSRWLockExclusive(mutex);
}
static void next_task_notify_all(NextTaskCondition *condition) {
  WakeAllConditionVariable(condition);
}
static void next_task_wait(NextTaskCondition *condition, NextTaskMutex *mutex) {
  (void)SleepConditionVariableSRW(condition, mutex, INFINITE, 0);
}
static void next_task_wait_milliseconds(NextTaskCondition *condition,
                                        NextTaskMutex *mutex,
                                        uint32_t milliseconds) {
  (void)SleepConditionVariableSRW(condition, mutex, milliseconds, 0);
}
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t NextTaskThread;
typedef pthread_mutex_t NextTaskMutex;
typedef pthread_cond_t NextTaskCondition;
static int next_task_sync_initialize(NextTaskMutex *mutex,
                                     NextTaskCondition *condition) {
#if defined(__APPLE__)
  if (pthread_mutex_init(mutex, NULL) != 0)
    return 0;
  if (pthread_cond_init(condition, NULL) != 0) {
    (void)pthread_mutex_destroy(mutex);
    return 0;
  }
#else
  pthread_condattr_t attributes;
  if (pthread_mutex_init(mutex, NULL) != 0)
    return 0;
  if (pthread_condattr_init(&attributes) != 0) {
    (void)pthread_mutex_destroy(mutex);
    return 0;
  }
  if (pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) != 0 ||
      pthread_cond_init(condition, &attributes) != 0) {
    (void)pthread_condattr_destroy(&attributes);
    (void)pthread_mutex_destroy(mutex);
    return 0;
  }
  (void)pthread_condattr_destroy(&attributes);
#endif
  return 1;
}
static void next_task_sync_destroy(NextTaskMutex *mutex,
                                   NextTaskCondition *condition) {
  (void)pthread_cond_destroy(condition);
  (void)pthread_mutex_destroy(mutex);
}
static void next_task_lock(NextTaskMutex *mutex) {
  if (pthread_mutex_lock(mutex) != 0)
    abort();
}
static void next_task_unlock(NextTaskMutex *mutex) {
  if (pthread_mutex_unlock(mutex) != 0)
    abort();
}
static void next_task_notify_all(NextTaskCondition *condition) {
  if (pthread_cond_broadcast(condition) != 0)
    abort();
}
static void next_task_wait(NextTaskCondition *condition, NextTaskMutex *mutex) {
  if (pthread_cond_wait(condition, mutex) != 0)
    abort();
}
#if !defined(__APPLE__)
static void next_task_wait_absolute(NextTaskCondition *condition,
                                    NextTaskMutex *mutex, uint64_t seconds,
                                    uint32_t nanoseconds) {
  struct timespec deadline;
  deadline.tv_sec =
      seconds > (uint64_t)INT64_MAX ? (time_t)INT64_MAX : (time_t)seconds;
  deadline.tv_nsec = (long)nanoseconds;
  (void)pthread_cond_timedwait(condition, mutex, &deadline);
}
#else
static void next_task_wait_relative(NextTaskCondition *condition,
                                    NextTaskMutex *mutex, uint64_t seconds,
                                    uint32_t nanoseconds) {
  struct timespec duration;
  duration.tv_sec =
      seconds > (uint64_t)INT64_MAX ? (time_t)INT64_MAX : (time_t)seconds;
  duration.tv_nsec = (long)nanoseconds;
  (void)pthread_cond_timedwait_relative_np(condition, mutex, &duration);
}
#endif
#endif

#define CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND 1000000000u

typedef struct NextTaskInstant {
  uint64_t seconds;
  uint32_t nanoseconds;
} NextTaskInstant;

typedef struct NextTaskCancellationNode {
  atomic_uint references;
  struct NextTaskCancellationNode *parent;
} NextTaskCancellationNode;

typedef struct NextTaskGroupMember NextTaskGroupMember;

struct chtholly_next_task_v1_task {
  atomic_uint references;
  atomic_uchar cancellation_requested;
  atomic_uint cancellation_cause;
  atomic_uchar published_terminal;
  atomic_uchar result_taken;
  atomic_uchar error_taken;
  atomic_uint scheduler_state;
  uint32_t terminal_step;
  int rerun_requested;
  NextTaskMutex mutex;
  NextTaskCondition condition;
  chtholly_next_task_v1_frame_descriptor descriptor;
  void *frame;
  chtholly_next_task_v1_scope *scope;
  chtholly_next_task_v1_executor *executor;
  NextTaskCancellationNode *cancellation_node;
  chtholly_next_task_v1_task *queue_next;
  chtholly_next_task_v1_task *scope_next;
  chtholly_next_task_v1_completion *completions;
  NextTaskGroupMember *group_members;
};

enum {
  NextTaskCompletionArmed,
  NextTaskCompletionReady,
  NextTaskCompletionDetached,
};

struct chtholly_next_task_v1_completion {
  atomic_uint references;
  atomic_uint state;
  void *owner;
  uint32_t owner_kind;
  chtholly_next_task_v1_completion_fn wake;
  chtholly_next_task_v1_completion_fn release;
  void *context;
  chtholly_next_task_v1_completion *next;
};

enum {
  NextTaskCancellationCauseNone,
  NextTaskCancellationCauseOwnerRequest,
  NextTaskCancellationCauseDeadlineExpired,
};

struct chtholly_next_task_v1_deadline {
  atomic_uint references;
  atomic_uint state;
  chtholly_next_task_v1_executor *executor;
  chtholly_next_task_v1_task *task;
  struct chtholly_next_task_v1_deadline *retired_next;
  int task_reference_released;
  NextTaskInstant instant;
  uint64_t sequence;
  size_t heap_index;
  int in_heap;
};

enum {
  NextTaskDeadlineArmed,
  NextTaskDeadlineSelected,
  NextTaskDeadlineReleased,
};

enum {
  NextTaskCompletionOwnerNone,
  NextTaskCompletionOwnerTask,
  NextTaskCompletionOwnerGroup,
};

struct NextTaskGroupMember {
  chtholly_next_task_v1_task_group *group;
  chtholly_next_task_v1_task *task;
  uint32_t flags;
  int cancellation_sent;
  NextTaskGroupMember *group_next;
  NextTaskGroupMember *task_next;
};

struct chtholly_next_task_v1_task_group {
  atomic_uint references;
  NextTaskMutex mutex;
  NextTaskCondition condition;
  NextTaskCancellationNode *owner_node;
  NextTaskGroupMember *members;
  chtholly_next_task_v1_completion *completions;
  uint64_t active_count;
  int closed;
  int cancellation_requested;
  int implicit_cancelled;
  int implicit_failed;
};

typedef struct NextTaskScopeExecutor {
  chtholly_next_task_v1_executor *executor;
  struct NextTaskScopeExecutor *next;
} NextTaskScopeExecutor;

struct chtholly_next_task_v1_scope {
  atomic_uint references;
  NextTaskMutex mutex;
  NextTaskCondition condition;
  chtholly_next_task_v1_executor *executor;
  NextTaskScopeExecutor *additional_executors;
  chtholly_next_task_v1_task *tasks;
  uint64_t task_count;
  int cancellation_requested;
};

struct chtholly_next_task_v1_executor {
  atomic_uint references;
  NextTaskMutex mutex;
  NextTaskCondition condition;
  NextTaskThread *workers;
  uint32_t worker_count;
  uint32_t scope_count;
  uint64_t active_count;
  chtholly_next_task_v1_task *queue_head;
  chtholly_next_task_v1_task *queue_tail;
  chtholly_next_task_v1_deadline **deadline_heap;
  size_t deadline_count;
  size_t deadline_capacity;
  uint64_t next_deadline_sequence;
#if defined(CHTHOLLY_NEXT_TASK_TESTING) || defined(CHTHOLLY_RUNTIME_TESTING)
  NextTaskInstant testing_now;
  int testing_clock_enabled;
#endif
  int shutdown;
};

#if defined(CHTHOLLY_NEXT_TASK_TESTING)
#if defined(_MSC_VER)
#define CHTHOLLY_NEXT_TASK_THREAD_LOCAL __declspec(thread)
#else
#define CHTHOLLY_NEXT_TASK_THREAD_LOCAL _Thread_local
#endif
static CHTHOLLY_NEXT_TASK_THREAD_LOCAL chtholly_next_task_v1_task
    *next_task_testing_current;
#endif

static void next_task_scope_drop_reference(chtholly_next_task_v1_scope *scope);
static void
next_task_group_drop_reference(chtholly_next_task_v1_task_group *group);
static void next_task_request_cancel_tree(chtholly_next_task_v1_task *task,
                                          int include_root, uint32_t cause);
static int next_task_mark_cancelled(chtholly_next_task_v1_task *task,
                                    uint32_t cause);

static int next_task_instant_compare(NextTaskInstant left,
                                     NextTaskInstant right) {
  if (left.seconds != right.seconds)
    return left.seconds < right.seconds ? -1 : 1;
  if (left.nanoseconds != right.nanoseconds)
    return left.nanoseconds < right.nanoseconds ? -1 : 1;
  return 0;
}

static NextTaskInstant next_task_instant_add(NextTaskInstant instant,
                                             uint64_t nanoseconds) {
  const uint64_t seconds =
      nanoseconds / CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND;
  const uint32_t remainder =
      (uint32_t)(nanoseconds % CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND);
  if (seconds > UINT64_MAX - instant.seconds) {
    instant.seconds = UINT64_MAX;
    instant.nanoseconds = CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND - 1u;
    return instant;
  }
  instant.seconds += seconds;
  if (remainder >=
      CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND - instant.nanoseconds) {
    if (instant.seconds == UINT64_MAX) {
      instant.nanoseconds = CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND - 1u;
      return instant;
    }
    ++instant.seconds;
    instant.nanoseconds =
        remainder -
        (CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND - instant.nanoseconds);
  } else {
    instant.nanoseconds += remainder;
  }
  return instant;
}

static int
next_task_executor_now_locked(chtholly_next_task_v1_executor *executor,
                              NextTaskInstant *out_now) {
#if defined(CHTHOLLY_NEXT_TASK_TESTING) || defined(CHTHOLLY_RUNTIME_TESTING)
  if (executor->testing_clock_enabled) {
    *out_now = executor->testing_now;
    return 1;
  }
#endif
#if defined(CHTHOLLY_NEXT_TASK_TESTING)
  (void)executor;
  (void)out_now;
  return 0;
#else
  return chtholly_next_runtime_v1_monotonic_now(
             &out_now->seconds, &out_now->nanoseconds) == 0;
#endif
}

static void
next_task_executor_wait_until_locked(chtholly_next_task_v1_executor *executor,
                                     NextTaskInstant deadline) {
#if defined(CHTHOLLY_NEXT_TASK_TESTING) || defined(CHTHOLLY_RUNTIME_TESTING)
  if (executor->testing_clock_enabled) {
    next_task_wait(&executor->condition, &executor->mutex);
    return;
  }
#endif
#if defined(_WIN32)
  {
    NextTaskInstant now;
    uint64_t milliseconds;
    uint64_t seconds;
    uint32_t nanoseconds;
    if (!next_task_executor_now_locked(executor, &now) ||
        next_task_instant_compare(deadline, now) <= 0)
      return;
    seconds = deadline.seconds - now.seconds;
    if (deadline.nanoseconds < now.nanoseconds) {
      --seconds;
      nanoseconds = deadline.nanoseconds +
                    CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND - now.nanoseconds;
    } else {
      nanoseconds = deadline.nanoseconds - now.nanoseconds;
    }
    if (seconds >= UINT32_MAX / 1000u)
      milliseconds = UINT32_MAX - 1u;
    else {
      milliseconds = seconds * 1000u + (nanoseconds + 999999u) / 1000000u;
      if (milliseconds == 0)
        milliseconds = 1;
      if (milliseconds >= UINT32_MAX)
        milliseconds = UINT32_MAX - 1u;
    }
    next_task_wait_milliseconds(&executor->condition, &executor->mutex,
                                (uint32_t)milliseconds);
  }
#else
#if defined(__APPLE__)
  {
    NextTaskInstant now;
    uint64_t seconds;
    uint32_t nanoseconds;
    if (!next_task_executor_now_locked(executor, &now) ||
        next_task_instant_compare(deadline, now) <= 0)
      return;
    seconds = deadline.seconds - now.seconds;
    if (deadline.nanoseconds < now.nanoseconds) {
      --seconds;
      nanoseconds = deadline.nanoseconds +
                    CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND - now.nanoseconds;
    } else {
      nanoseconds = deadline.nanoseconds - now.nanoseconds;
    }
    next_task_wait_relative(&executor->condition, &executor->mutex, seconds,
                            nanoseconds);
  }
#else
  next_task_wait_absolute(&executor->condition, &executor->mutex,
                          deadline.seconds, deadline.nanoseconds);
#endif
#endif
}

static int
next_task_deadline_less(const chtholly_next_task_v1_deadline *left,
                        const chtholly_next_task_v1_deadline *right) {
  const int instant_order =
      next_task_instant_compare(left->instant, right->instant);
  return instant_order < 0 ||
         (instant_order == 0 && left->sequence < right->sequence);
}

static void
next_task_deadline_heap_swap(chtholly_next_task_v1_executor *executor,
                             size_t left, size_t right) {
  chtholly_next_task_v1_deadline *temporary = executor->deadline_heap[left];
  executor->deadline_heap[left] = executor->deadline_heap[right];
  executor->deadline_heap[right] = temporary;
  executor->deadline_heap[left]->heap_index = left;
  executor->deadline_heap[right]->heap_index = right;
}

static void
next_task_deadline_heap_sift_up(chtholly_next_task_v1_executor *executor,
                                size_t index) {
  while (index != 0) {
    const size_t parent = (index - 1) / 2;
    if (!next_task_deadline_less(executor->deadline_heap[index],
                                 executor->deadline_heap[parent]))
      break;
    next_task_deadline_heap_swap(executor, index, parent);
    index = parent;
  }
}

static void
next_task_deadline_heap_sift_down(chtholly_next_task_v1_executor *executor,
                                  size_t index) {
  for (;;) {
    size_t child = index * 2 + 1;
    if (child >= executor->deadline_count)
      return;
    if (child + 1 < executor->deadline_count &&
        next_task_deadline_less(executor->deadline_heap[child + 1],
                                executor->deadline_heap[child]))
      ++child;
    if (!next_task_deadline_less(executor->deadline_heap[child],
                                 executor->deadline_heap[index]))
      return;
    next_task_deadline_heap_swap(executor, index, child);
    index = child;
  }
}

static int next_task_deadline_heap_insert_locked(
    chtholly_next_task_v1_executor *executor,
    chtholly_next_task_v1_deadline *deadline) {
  if (executor->deadline_count == executor->deadline_capacity) {
    const size_t capacity =
        executor->deadline_capacity == 0 ? 8 : executor->deadline_capacity * 2;
    chtholly_next_task_v1_deadline **heap;
    if (capacity < executor->deadline_capacity ||
        capacity > SIZE_MAX / sizeof(*heap))
      return 0;
    heap = (chtholly_next_task_v1_deadline **)realloc(executor->deadline_heap,
                                                      capacity * sizeof(*heap));
    if (heap == NULL)
      return 0;
    executor->deadline_heap = heap;
    executor->deadline_capacity = capacity;
  }
  deadline->sequence = executor->next_deadline_sequence++;
  deadline->heap_index = executor->deadline_count;
  deadline->in_heap = 1;
  executor->deadline_heap[executor->deadline_count++] = deadline;
  next_task_deadline_heap_sift_up(executor, deadline->heap_index);
  next_task_notify_all(&executor->condition);
  return 1;
}

static void next_task_deadline_heap_remove_locked(
    chtholly_next_task_v1_executor *executor,
    chtholly_next_task_v1_deadline *deadline) {
  const size_t index = deadline->heap_index;
  const size_t last = --executor->deadline_count;
  deadline->in_heap = 0;
  if (index == last)
    return;
  executor->deadline_heap[index] = executor->deadline_heap[last];
  executor->deadline_heap[index]->heap_index = index;
  if (index != 0 &&
      next_task_deadline_less(executor->deadline_heap[index],
                              executor->deadline_heap[(index - 1) / 2]))
    next_task_deadline_heap_sift_up(executor, index);
  else
    next_task_deadline_heap_sift_down(executor, index);
}

static void
next_task_deadline_drop_reference(chtholly_next_task_v1_deadline *deadline) {
  if (atomic_fetch_sub_explicit(&deadline->references, 1,
                                memory_order_acq_rel) == 1) {
    chtholly_next_task_v1_executor_release(deadline->executor);
    free(deadline);
  }
}

static void next_task_deadline_release_task_reference(
    chtholly_next_task_v1_deadline *deadline) {
  chtholly_next_task_v1_task *task;
  if (deadline == NULL || deadline->task_reference_released)
    return;
  task = deadline->task;
  deadline->task = NULL;
  deadline->task_reference_released = 1;
  if (task != NULL)
    chtholly_next_task_v1_task_release(task);
}

static int next_task_mark_cancelled(chtholly_next_task_v1_task *task,
                                    uint32_t cause) {
  uint32_t observed = NextTaskCancellationCauseNone;
  uint32_t state;
  int marked = 0;
  next_task_lock(&task->mutex);
  state = atomic_load_explicit(&task->scheduler_state, memory_order_acquire);
  if (!atomic_load_explicit(&task->published_terminal, memory_order_acquire) &&
      state != CHTHOLLY_NEXT_TASK_V1_STATE_CANCELLED &&
      state != CHTHOLLY_NEXT_TASK_V1_STATE_COMPLETED &&
      state != CHTHOLLY_NEXT_TASK_V1_STATE_FAILED) {
    (void)atomic_compare_exchange_strong_explicit(
        &task->cancellation_cause, &observed, cause, memory_order_acq_rel,
        memory_order_acquire);
    atomic_store_explicit(&task->cancellation_requested, 1,
                          memory_order_release);
    marked = 1;
  }
  next_task_unlock(&task->mutex);
  if (marked)
    (void)chtholly_next_task_v1_task_wake(task);
  return marked;
}

static void next_task_completion_owner_release(
    chtholly_next_task_v1_completion *completion) {
  if (completion->owner_kind == NextTaskCompletionOwnerTask)
    chtholly_next_task_v1_task_release(
        (chtholly_next_task_v1_task *)completion->owner);
  else if (completion->owner_kind == NextTaskCompletionOwnerGroup)
    next_task_group_drop_reference(
        (chtholly_next_task_v1_task_group *)completion->owner);
}

static void next_task_completion_drop_internal(
    chtholly_next_task_v1_completion *completion) {
  if (atomic_fetch_sub_explicit(&completion->references, 1,
                                memory_order_acq_rel) == 1) {
    next_task_completion_owner_release(completion);
    free(completion);
  }
}

static void
next_task_publish_completions(chtholly_next_task_v1_completion *completions) {
  while (completions != NULL) {
    chtholly_next_task_v1_completion *completion = completions;
    completions = completion->next;
    completion->next = NULL;
    completion->wake(completion->context);
    completion->release(completion->context);
    next_task_completion_drop_internal(completion);
  }
}

static void
next_task_group_publish_terminal_members(NextTaskGroupMember *members,
                                         uint32_t step) {
  while (members != NULL) {
    NextTaskGroupMember *member = members;
    chtholly_next_task_v1_task_group *group = member->group;
    chtholly_next_task_v1_completion *completions = NULL;
    NextTaskGroupMember **cursor;
    members = member->task_next;
    next_task_lock(&group->mutex);
    cursor = &group->members;
    while (*cursor != NULL && *cursor != member)
      cursor = &(*cursor)->group_next;
    if (*cursor == member) {
      *cursor = member->group_next;
      --group->active_count;
      if ((member->flags &
           CHTHOLLY_NEXT_TASK_V1_GROUP_MEMBER_EXPECT_SUCCESS_ONLY) != 0) {
        if (step == CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED)
          group->implicit_cancelled = 1;
        else if (step == CHTHOLLY_NEXT_TASK_V1_STEP_FAILED)
          group->implicit_failed = 1;
      }
      if (group->closed && group->active_count == 0) {
        completions = group->completions;
        group->completions = NULL;
        for (chtholly_next_task_v1_completion *completion = completions;
             completion != NULL; completion = completion->next)
          atomic_store_explicit(&completion->state, NextTaskCompletionReady,
                                memory_order_release);
      }
    }
    next_task_unlock(&group->mutex);
    next_task_publish_completions(completions);
    chtholly_next_task_v1_task_release(member->task);
    next_task_group_drop_reference(group);
    free(member);
  }
}

static NextTaskCancellationNode *
next_task_cancellation_node_create(NextTaskCancellationNode *parent) {
  NextTaskCancellationNode *node =
      (NextTaskCancellationNode *)malloc(sizeof(*node));
  if (node == NULL)
    return NULL;
  atomic_init(&node->references, 1);
  node->parent = parent;
  if (parent != NULL)
    (void)atomic_fetch_add_explicit(&parent->references, 1,
                                    memory_order_relaxed);
  return node;
}

static void
next_task_cancellation_node_release(NextTaskCancellationNode *node) {
  while (node != NULL && atomic_fetch_sub_explicit(&node->references, 1,
                                                   memory_order_acq_rel) == 1) {
    NextTaskCancellationNode *parent = node->parent;
    free(node);
    node = parent;
  }
}

static void next_task_cancellation_node_retain(NextTaskCancellationNode *node) {
  if (node != NULL)
    (void)atomic_fetch_add_explicit(&node->references, 1, memory_order_relaxed);
}

void *chtholly_next_task_v1_frame_allocate(uint64_t size, uint64_t alignment) {
  if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      alignment > (uint64_t)SIZE_MAX || size > (uint64_t)SIZE_MAX)
    return NULL;
  if (alignment < sizeof(void *))
    alignment = sizeof(void *);
#if defined(_WIN32)
  return _aligned_malloc((size_t)size, (size_t)alignment);
#else
  void *result = NULL;
  return posix_memalign(&result, (size_t)alignment, (size_t)size) == 0 ? result
                                                                       : NULL;
#endif
}

void chtholly_next_task_v1_frame_deallocate(void *frame) {
#if defined(_WIN32)
  _aligned_free(frame);
#else
  free(frame);
#endif
}

void chtholly_next_task_v1_executor_retain(
    chtholly_next_task_v1_executor *executor) {
  if (executor != NULL)
    (void)atomic_fetch_add_explicit(&executor->references, 1,
                                    memory_order_relaxed);
}

void chtholly_next_task_v1_scope_retain(chtholly_next_task_v1_scope *scope) {
  if (scope != NULL)
    (void)atomic_fetch_add_explicit(&scope->references, 1,
                                    memory_order_relaxed);
}

void chtholly_next_task_v1_task_retain(chtholly_next_task_v1_task *task) {
  if (task != NULL)
    (void)atomic_fetch_add_explicit(&task->references, 1, memory_order_relaxed);
}

static void next_task_dispose(chtholly_next_task_v1_task *task) {
  chtholly_next_task_v1_executor *executor = task->executor;
  NextTaskCancellationNode *cancellation_node = task->cancellation_node;
  task->descriptor.destroy(task->frame);
  next_task_sync_destroy(&task->mutex, &task->condition);
  free(task);
  chtholly_next_task_v1_executor_release(executor);
  next_task_cancellation_node_release(cancellation_node);
}

void chtholly_next_task_v1_task_release(chtholly_next_task_v1_task *task) {
  if (task != NULL && atomic_fetch_sub_explicit(&task->references, 1,
                                                memory_order_acq_rel) == 1)
    next_task_dispose(task);
}

static void next_task_enqueue_locked(chtholly_next_task_v1_executor *executor,
                                     chtholly_next_task_v1_task *task) {
  task->queue_next = NULL;
  if (executor->queue_tail != NULL)
    executor->queue_tail->queue_next = task;
  else
    executor->queue_head = task;
  executor->queue_tail = task;
  atomic_store_explicit(&task->scheduler_state,
                        CHTHOLLY_NEXT_TASK_V1_STATE_QUEUED,
                        memory_order_release);
  chtholly_next_task_v1_task_retain(task);
  next_task_notify_all(&executor->condition);
}

static void next_task_remove_from_scope(chtholly_next_task_v1_task *task) {
  chtholly_next_task_v1_scope *scope = task->scope;
  chtholly_next_task_v1_task **cursor;
  if (scope == NULL)
    return;
  next_task_lock(&scope->mutex);
  cursor = &scope->tasks;
  while (*cursor != NULL && *cursor != task)
    cursor = &(*cursor)->scope_next;
  if (*cursor == task) {
    *cursor = task->scope_next;
    --scope->task_count;
  }
  next_task_lock(&task->mutex);
  task->scope = NULL;
  next_task_unlock(&task->mutex);
  next_task_notify_all(&scope->condition);
  next_task_unlock(&scope->mutex);
  chtholly_next_task_v1_task_release(task);
  next_task_scope_drop_reference(scope);
}

static void next_task_publish_terminal(chtholly_next_task_v1_task *task,
                                       uint32_t step) {
  chtholly_next_task_v1_executor *executor;
  chtholly_next_task_v1_deadline *retired_deadlines = NULL;
  chtholly_next_task_v1_completion *completions;
  NextTaskGroupMember *group_members;
  if (step == CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED) {
    uint32_t cause =
        atomic_load_explicit(&task->cancellation_cause, memory_order_acquire);
    if (cause == NextTaskCancellationCauseNone)
      cause = NextTaskCancellationCauseOwnerRequest;
    next_task_request_cancel_tree(task, 0, cause);
  }
  next_task_lock(&task->mutex);
  task->terminal_step = step;
  atomic_store_explicit(&task->published_terminal, 1, memory_order_release);
  executor = task->executor;
  task->executor = NULL;
  completions = task->completions;
  task->completions = NULL;
  group_members = task->group_members;
  task->group_members = NULL;
  for (chtholly_next_task_v1_completion *cursor = completions; cursor != NULL;
       cursor = cursor->next)
    atomic_store_explicit(&cursor->state, NextTaskCompletionReady,
                          memory_order_release);
  next_task_notify_all(&task->condition);
  next_task_unlock(&task->mutex);
  next_task_publish_completions(completions);
  next_task_group_publish_terminal_members(group_members, step);
  /* A deadline retains its task until the caller releases the deadline. A
     frame destructor may itself release that handle, so leaving an armed
     deadline in the executor heap at terminal publication would form a
     task/frame/deadline reference cycle. Retire all deadlines owned by this
     task before the frame can be destroyed. The heap references are dropped
     after releasing the executor lock; dropping them can release the task. */
  next_task_lock(&executor->mutex);
  for (size_t index = 0; index < executor->deadline_count;) {
    chtholly_next_task_v1_deadline *deadline =
        executor->deadline_heap[index];
    if (deadline->task != task) {
      ++index;
      continue;
    }
    next_task_deadline_heap_remove_locked(executor, deadline);
    atomic_store_explicit(&deadline->state, NextTaskDeadlineReleased,
                          memory_order_release);
    deadline->retired_next = retired_deadlines;
    retired_deadlines = deadline;
  }
  next_task_notify_all(&executor->condition);
  next_task_unlock(&executor->mutex);
  while (retired_deadlines != NULL) {
    chtholly_next_task_v1_deadline *deadline = retired_deadlines;
    retired_deadlines = deadline->retired_next;
    deadline->retired_next = NULL;
    next_task_deadline_release_task_reference(deadline);
    next_task_deadline_drop_reference(deadline);
  }
  /* The scope still retains every executor used by an attached task. Transfer
     terminal lifetime back to it before detaching, so an executor cannot be
     destroyed by its own worker while releasing the task's final reference. */
  chtholly_next_task_v1_executor_release(executor);
  next_task_remove_from_scope(task);
}

static void next_task_run(chtholly_next_task_v1_executor *executor,
                          chtholly_next_task_v1_task *task) {
#if defined(CHTHOLLY_NEXT_TASK_TESTING)
  chtholly_next_task_v1_task *previous_testing_current =
      next_task_testing_current;
  next_task_testing_current = task;
#endif
  uint32_t step = task->descriptor.resume(task->frame, task);
#if defined(CHTHOLLY_NEXT_TASK_TESTING)
  next_task_testing_current = previous_testing_current;
#endif
  chtholly_next_task_v1_executor *next_executor;
  int terminal = 0;
  if (step > CHTHOLLY_NEXT_TASK_V1_STEP_FAILED)
    step = CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED;
  next_task_lock(&task->mutex);
  next_executor = task->executor;
  next_task_lock(&executor->mutex);
  --executor->active_count;
  if ((step == CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED ||
       step == CHTHOLLY_NEXT_TASK_V1_STEP_FAILED) &&
      atomic_load_explicit(&task->cancellation_requested, memory_order_acquire))
    step = CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED;
  if (step == CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED ||
      step == CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED ||
      step == CHTHOLLY_NEXT_TASK_V1_STEP_FAILED) {
    atomic_store_explicit(&task->scheduler_state,
                          step == CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED
                              ? CHTHOLLY_NEXT_TASK_V1_STATE_COMPLETED
                          : step == CHTHOLLY_NEXT_TASK_V1_STEP_FAILED
                              ? CHTHOLLY_NEXT_TASK_V1_STATE_FAILED
                              : CHTHOLLY_NEXT_TASK_V1_STATE_CANCELLED,
                          memory_order_release);
    task->rerun_requested = 0;
    terminal = 1;
  }
  next_task_notify_all(&executor->condition);
  next_task_unlock(&executor->mutex);
  if (!terminal) {
    next_task_lock(&next_executor->mutex);
    if (step == CHTHOLLY_NEXT_TASK_V1_STEP_RESCHEDULE ||
        task->rerun_requested ||
        atomic_load_explicit(&task->cancellation_requested,
                             memory_order_acquire)) {
      task->rerun_requested = 0;
      next_task_enqueue_locked(next_executor, task);
    } else {
      atomic_store_explicit(&task->scheduler_state,
                            CHTHOLLY_NEXT_TASK_V1_STATE_IDLE,
                            memory_order_release);
    }
    next_task_notify_all(&next_executor->condition);
    next_task_unlock(&next_executor->mutex);
  }
  next_task_unlock(&task->mutex);
  if (terminal)
    next_task_publish_terminal(task, step);
}

#if defined(_WIN32)
static DWORD WINAPI next_task_worker(void *userdata)
#else
static void *next_task_worker(void *userdata)
#endif
{
  chtholly_next_task_v1_executor *executor =
      (chtholly_next_task_v1_executor *)userdata;
  for (;;) {
    chtholly_next_task_v1_task *task = NULL;
    chtholly_next_task_v1_deadline *expired = NULL;
    next_task_lock(&executor->mutex);
    for (;;) {
      if (executor->deadline_count != 0) {
        NextTaskInstant now;
        chtholly_next_task_v1_deadline *candidate = executor->deadline_heap[0];
        if (next_task_executor_now_locked(executor, &now) &&
            next_task_instant_compare(candidate->instant, now) <= 0) {
          next_task_deadline_heap_remove_locked(executor, candidate);
          atomic_store_explicit(&candidate->state, NextTaskDeadlineSelected,
                                memory_order_release);
          expired = candidate;
          break;
        }
      }
      if (executor->queue_head != NULL) {
        task = executor->queue_head;
        executor->queue_head = task->queue_next;
        if (executor->queue_head == NULL)
          executor->queue_tail = NULL;
        task->queue_next = NULL;
        atomic_store_explicit(&task->scheduler_state,
                              CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING,
                              memory_order_release);
        ++executor->active_count;
        break;
      }
      if (executor->shutdown)
        break;
      if (executor->deadline_count == 0)
        next_task_wait(&executor->condition, &executor->mutex);
      else
        next_task_executor_wait_until_locked(
            executor, executor->deadline_heap[0]->instant);
    }
    next_task_unlock(&executor->mutex);
    if (expired != NULL) {
      next_task_request_cancel_tree(expired->task, 1,
                                    NextTaskCancellationCauseDeadlineExpired);
      next_task_deadline_release_task_reference(expired);
      next_task_deadline_drop_reference(expired);
      continue;
    }
    if (task == NULL)
      break;
    next_task_run(executor, task);
    chtholly_next_task_v1_task_release(task);
  }
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

int32_t chtholly_next_task_v1_executor_create(
    const chtholly_next_task_v1_executor_config *config,
    chtholly_next_task_v1_executor **out_executor) {
  chtholly_next_task_v1_executor *executor;
  uint32_t index;
  if (out_executor == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_executor = NULL;
  if (config == NULL || config->struct_size < sizeof(*config) ||
      config->worker_count == 0 || config->reserved != 0)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  executor = (chtholly_next_task_v1_executor *)calloc(1, sizeof(*executor));
  if (executor == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  executor->workers = (NextTaskThread *)calloc(config->worker_count,
                                               sizeof(*executor->workers));
  if (executor->workers == NULL) {
    free(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  atomic_init(&executor->references, 1);
  if (!next_task_sync_initialize(&executor->mutex, &executor->condition)) {
    free(executor->workers);
    free(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  executor->worker_count = config->worker_count;
  for (index = 0; index < config->worker_count; ++index) {
#if defined(_WIN32)
    executor->workers[index] =
        CreateThread(NULL, 0, next_task_worker, executor, 0, NULL);
    if (executor->workers[index] == NULL)
      break;
#else
    if (pthread_create(&executor->workers[index], NULL, next_task_worker,
                       executor) != 0)
      break;
#endif
  }
  if (index != config->worker_count) {
    next_task_lock(&executor->mutex);
    executor->shutdown = 1;
    next_task_notify_all(&executor->condition);
    next_task_unlock(&executor->mutex);
    while (index != 0) {
      --index;
#if defined(_WIN32)
      (void)WaitForSingleObject(executor->workers[index], INFINITE);
      (void)CloseHandle(executor->workers[index]);
#else
      (void)pthread_join(executor->workers[index], NULL);
#endif
    }
    next_task_sync_destroy(&executor->mutex, &executor->condition);
    free(executor->workers);
    free(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  *out_executor = executor;
  return 0;
}

static void
next_task_executor_dispose(chtholly_next_task_v1_executor *executor) {
  uint32_t index;
  next_task_lock(&executor->mutex);
  executor->shutdown = 1;
  next_task_notify_all(&executor->condition);
  next_task_unlock(&executor->mutex);
  for (index = 0; index < executor->worker_count; ++index) {
#if defined(_WIN32)
    (void)WaitForSingleObject(executor->workers[index], INFINITE);
    (void)CloseHandle(executor->workers[index]);
#else
    (void)pthread_join(executor->workers[index], NULL);
#endif
  }
  next_task_sync_destroy(&executor->mutex, &executor->condition);
  free(executor->deadline_heap);
  free(executor->workers);
  free(executor);
}

void chtholly_next_task_v1_executor_release(
    chtholly_next_task_v1_executor *executor) {
  if (executor != NULL && atomic_fetch_sub_explicit(&executor->references, 1,
                                                    memory_order_acq_rel) == 1)
    next_task_executor_dispose(executor);
}

int32_t
chtholly_next_task_v1_scope_create(chtholly_next_task_v1_executor *executor,
                                   chtholly_next_task_v1_scope **out_scope) {
  chtholly_next_task_v1_scope *scope;
  if (out_scope == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_scope = NULL;
  if (executor == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&executor->mutex);
  if (executor->shutdown) {
    next_task_unlock(&executor->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_SHUTDOWN;
  }
  ++executor->scope_count;
  next_task_unlock(&executor->mutex);
  scope = (chtholly_next_task_v1_scope *)calloc(1, sizeof(*scope));
  if (scope == NULL) {
    next_task_lock(&executor->mutex);
    --executor->scope_count;
    next_task_unlock(&executor->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  atomic_init(&scope->references, 1);
  if (!next_task_sync_initialize(&scope->mutex, &scope->condition)) {
    free(scope);
    next_task_lock(&executor->mutex);
    --executor->scope_count;
    next_task_unlock(&executor->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  scope->executor = executor;
  chtholly_next_task_v1_executor_retain(executor);
  *out_scope = scope;
  return 0;
}

int32_t
chtholly_next_task_v1_scope_request_cancel(chtholly_next_task_v1_scope *scope) {
  chtholly_next_task_v1_task *task;
  if (scope == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&scope->mutex);
  scope->cancellation_requested = 1;
  for (task = scope->tasks; task != NULL; task = task->scope_next)
    (void)next_task_mark_cancelled(task, NextTaskCancellationCauseOwnerRequest);
  next_task_unlock(&scope->mutex);
  return 0;
}

int32_t chtholly_next_task_v1_scope_join(chtholly_next_task_v1_scope *scope) {
  if (scope == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&scope->mutex);
  while (scope->task_count != 0)
    next_task_wait(&scope->condition, &scope->mutex);
  next_task_unlock(&scope->mutex);
  return 0;
}

static void next_task_scope_dispose(chtholly_next_task_v1_scope *scope) {
  chtholly_next_task_v1_executor *executor = scope->executor;
  NextTaskScopeExecutor *additional_executors = scope->additional_executors;
  (void)chtholly_next_task_v1_scope_request_cancel(scope);
  (void)chtholly_next_task_v1_scope_join(scope);
  next_task_lock(&executor->mutex);
  --executor->scope_count;
  next_task_notify_all(&executor->condition);
  next_task_unlock(&executor->mutex);
  next_task_sync_destroy(&scope->mutex, &scope->condition);
  free(scope);
  while (additional_executors != NULL) {
    NextTaskScopeExecutor *next = additional_executors->next;
    chtholly_next_task_v1_executor_release(additional_executors->executor);
    free(additional_executors);
    additional_executors = next;
  }
  chtholly_next_task_v1_executor_release(executor);
}

static void next_task_scope_drop_reference(chtholly_next_task_v1_scope *scope) {
  if (scope != NULL && atomic_fetch_sub_explicit(&scope->references, 1,
                                                 memory_order_acq_rel) == 1)
    next_task_scope_dispose(scope);
}

static int
next_task_scope_retain_executor(chtholly_next_task_v1_scope *scope,
                                chtholly_next_task_v1_executor *executor) {
  NextTaskScopeExecutor *entry;
  NextTaskScopeExecutor *cursor;
  if (executor == scope->executor)
    return 1;
  entry = (NextTaskScopeExecutor *)malloc(sizeof(*entry));
  if (entry == NULL)
    return 0;
  next_task_lock(&scope->mutex);
  for (cursor = scope->additional_executors; cursor != NULL;
       cursor = cursor->next) {
    if (cursor->executor == executor) {
      next_task_unlock(&scope->mutex);
      free(entry);
      return 1;
    }
  }
  chtholly_next_task_v1_executor_retain(executor);
  entry->executor = executor;
  entry->next = scope->additional_executors;
  scope->additional_executors = entry;
  next_task_unlock(&scope->mutex);
  return 1;
}

void chtholly_next_task_v1_scope_release(chtholly_next_task_v1_scope *scope) {
  if (scope == NULL)
    return;
  (void)chtholly_next_task_v1_scope_request_cancel(scope);
  (void)chtholly_next_task_v1_scope_join(scope);
  next_task_scope_drop_reference(scope);
}

static int32_t
next_task_create_on(chtholly_next_task_v1_scope *scope,
                    chtholly_next_task_v1_executor *executor,
                    chtholly_next_task_v1_task *parent,
                    const chtholly_next_task_v1_frame_descriptor *descriptor,
                    void *frame, chtholly_next_task_v1_task **out_task) {
  chtholly_next_task_v1_task *task;
  const size_t descriptor_v1_size =
      offsetof(chtholly_next_task_v1_frame_descriptor, move_error);
  size_t descriptor_size;
  if (out_task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_task = NULL;
  if (scope == NULL || executor == NULL || descriptor == NULL ||
      frame == NULL || descriptor->struct_size < descriptor_v1_size ||
      descriptor->abi_version != CHTHOLLY_NEXT_TASK_ABI_V1 ||
      descriptor->reserved != 0 || descriptor->resume == NULL ||
      descriptor->destroy == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  task = (chtholly_next_task_v1_task *)calloc(1, sizeof(*task));
  if (task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  task->cancellation_node = next_task_cancellation_node_create(
      parent == NULL ? NULL : parent->cancellation_node);
  if (task->cancellation_node == NULL) {
    free(task);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  atomic_init(&task->references, 1);
  atomic_init(&task->cancellation_requested, 0);
  atomic_init(&task->cancellation_cause, NextTaskCancellationCauseNone);
  atomic_init(&task->published_terminal, 0);
  atomic_init(&task->result_taken, 0);
  atomic_init(&task->error_taken, 0);
  atomic_init(&task->scheduler_state, CHTHOLLY_NEXT_TASK_V1_STATE_IDLE);
  if (!next_task_sync_initialize(&task->mutex, &task->condition)) {
    next_task_cancellation_node_release(task->cancellation_node);
    free(task);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  memset(&task->descriptor, 0, sizeof(task->descriptor));
  descriptor_size = descriptor->struct_size >= sizeof(task->descriptor)
                        ? sizeof(task->descriptor)
                        : descriptor_v1_size;
  memcpy(&task->descriptor, descriptor, descriptor_size);
  task->frame = frame;
  task->scope = scope;
  task->executor = executor;

  next_task_lock(&scope->mutex);
  if (parent != NULL) {
    int parent_running;
    next_task_lock(&parent->mutex);
    parent_running =
        atomic_load_explicit(&parent->scheduler_state, memory_order_acquire) ==
            CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING &&
        !atomic_load_explicit(&parent->published_terminal,
                              memory_order_acquire) &&
        parent->scope == scope;
    next_task_unlock(&parent->mutex);
    if (!parent_running) {
      next_task_unlock(&scope->mutex);
      task->frame = NULL;
      next_task_sync_destroy(&task->mutex, &task->condition);
      next_task_cancellation_node_release(task->cancellation_node);
      free(task);
      return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
    }
  }
  next_task_lock(&executor->mutex);
  if (executor->shutdown) {
    next_task_unlock(&executor->mutex);
    next_task_unlock(&scope->mutex);
    task->frame = NULL;
    next_task_sync_destroy(&task->mutex, &task->condition);
    next_task_cancellation_node_release(task->cancellation_node);
    free(task);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_SHUTDOWN;
  }
  if (scope->cancellation_requested ||
      (parent != NULL && atomic_load_explicit(&parent->cancellation_requested,
                                              memory_order_acquire))) {
    uint32_t cause = NextTaskCancellationCauseOwnerRequest;
    if (!scope->cancellation_requested && parent != NULL) {
      cause = atomic_load_explicit(&parent->cancellation_cause,
                                   memory_order_acquire);
      if (cause == NextTaskCancellationCauseNone)
        cause = NextTaskCancellationCauseOwnerRequest;
    }
    atomic_store_explicit(&task->cancellation_cause, cause,
                          memory_order_release);
    atomic_store_explicit(&task->cancellation_requested, 1,
                          memory_order_release);
  }
  chtholly_next_task_v1_scope_retain(scope);
  chtholly_next_task_v1_executor_retain(executor);
  chtholly_next_task_v1_task_retain(task);
  task->scope_next = scope->tasks;
  scope->tasks = task;
  ++scope->task_count;
  next_task_enqueue_locked(executor, task);
  next_task_unlock(&executor->mutex);
  next_task_unlock(&scope->mutex);
  *out_task = task;
  return 0;
}

int32_t chtholly_next_task_v1_task_create(
    chtholly_next_task_v1_scope *scope,
    const chtholly_next_task_v1_frame_descriptor *descriptor, void *frame,
    chtholly_next_task_v1_task **out_task) {
  return next_task_create_on(scope, scope == NULL ? NULL : scope->executor,
                             NULL, descriptor, frame, out_task);
}

int32_t chtholly_next_task_v1_task_create_child(
    chtholly_next_task_v1_task *parent,
    const chtholly_next_task_v1_frame_descriptor *descriptor, void *frame,
    chtholly_next_task_v1_task **out_task) {
  chtholly_next_task_v1_scope *scope;
  chtholly_next_task_v1_executor *executor;
  if (parent == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&parent->mutex);
  if (atomic_load_explicit(&parent->scheduler_state, memory_order_acquire) !=
          CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING ||
      parent->scope == NULL) {
    next_task_unlock(&parent->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  scope = parent->scope;
  executor = parent->executor;
  chtholly_next_task_v1_scope_retain(scope);
  chtholly_next_task_v1_executor_retain(executor);
  next_task_unlock(&parent->mutex);
  {
    const int32_t status = next_task_create_on(scope, executor, parent,
                                               descriptor, frame, out_task);
    next_task_scope_drop_reference(scope);
    chtholly_next_task_v1_executor_release(executor);
    return status;
  }
}

int32_t chtholly_next_task_v1_task_wake(chtholly_next_task_v1_task *task) {
  chtholly_next_task_v1_executor *executor;
  uint32_t state;
  if (task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&task->mutex);
  if (atomic_load_explicit(&task->published_terminal, memory_order_acquire)) {
    next_task_unlock(&task->mutex);
    return 0;
  }
  /* The task mutex prevents terminal publication and scope detachment, so the
     scope keeps executor alive until this scheduling operation completes. */
  executor = task->executor;
  next_task_lock(&executor->mutex);
  state = atomic_load_explicit(&task->scheduler_state, memory_order_acquire);
  if (state == CHTHOLLY_NEXT_TASK_V1_STATE_IDLE)
    next_task_enqueue_locked(executor, task);
  else if (state == CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING)
    task->rerun_requested = 1;
  next_task_unlock(&executor->mutex);
  next_task_unlock(&task->mutex);
  return 0;
}

static int next_task_cancellation_node_descends_from(
    const NextTaskCancellationNode *node,
    const NextTaskCancellationNode *ancestor) {
  for (node = node == NULL ? NULL : node->parent; node != NULL;
       node = node->parent)
    if (node == ancestor)
      return 1;
  return 0;
}

static void next_task_request_cancel_tree(chtholly_next_task_v1_task *task,
                                          int include_root, uint32_t cause) {
  chtholly_next_task_v1_scope *scope = NULL;
  chtholly_next_task_v1_task *cursor;
  if (include_root && !next_task_mark_cancelled(task, cause))
    return;

  next_task_lock(&task->mutex);
  if (!atomic_load_explicit(&task->published_terminal, memory_order_acquire) &&
      task->scope != NULL) {
    scope = task->scope;
    chtholly_next_task_v1_scope_retain(scope);
  }
  next_task_unlock(&task->mutex);

  if (scope == NULL) {
    return;
  }
  next_task_lock(&scope->mutex);
  for (cursor = scope->tasks; cursor != NULL; cursor = cursor->scope_next) {
    if (cursor != task &&
        next_task_cancellation_node_descends_from(cursor->cancellation_node,
                                                  task->cancellation_node))
      (void)next_task_mark_cancelled(cursor, cause);
  }
  next_task_unlock(&scope->mutex);
  next_task_scope_drop_reference(scope);
}

int32_t chtholly_next_task_v1_task_rebind_executor(
    chtholly_next_task_v1_task *task,
    chtholly_next_task_v1_executor *executor) {
  chtholly_next_task_v1_executor *old_executor;
  chtholly_next_task_v1_scope *scope;
  if (task == NULL || executor == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  chtholly_next_task_v1_executor_retain(executor);
  next_task_lock(&task->mutex);
  if (atomic_load_explicit(&task->scheduler_state, memory_order_acquire) !=
          CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING ||
      task->scope == NULL) {
    next_task_unlock(&task->mutex);
    chtholly_next_task_v1_executor_release(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  scope = task->scope;
  chtholly_next_task_v1_scope_retain(scope);
  next_task_unlock(&task->mutex);
  next_task_lock(&executor->mutex);
  if (executor->shutdown) {
    next_task_unlock(&executor->mutex);
    next_task_scope_drop_reference(scope);
    chtholly_next_task_v1_executor_release(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_SHUTDOWN;
  }
  next_task_unlock(&executor->mutex);
  if (!next_task_scope_retain_executor(scope, executor)) {
    next_task_scope_drop_reference(scope);
    chtholly_next_task_v1_executor_release(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  next_task_lock(&task->mutex);
  if (atomic_load_explicit(&task->scheduler_state, memory_order_acquire) !=
          CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING ||
      task->scope != scope) {
    next_task_unlock(&task->mutex);
    next_task_scope_drop_reference(scope);
    chtholly_next_task_v1_executor_release(executor);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  old_executor = task->executor;
  if (old_executor == executor) {
    next_task_unlock(&task->mutex);
    next_task_scope_drop_reference(scope);
    chtholly_next_task_v1_executor_release(executor);
    return 0;
  }
  task->executor = executor;
  next_task_unlock(&task->mutex);
  chtholly_next_task_v1_executor_release(old_executor);
  next_task_scope_drop_reference(scope);
  return 0;
}

int32_t
chtholly_next_task_v1_task_request_cancel(chtholly_next_task_v1_task *task) {
  if (task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_request_cancel_tree(task, 1, NextTaskCancellationCauseOwnerRequest);
  return 0;
}

uint8_t chtholly_next_task_v1_task_cancellation_requested(
    const chtholly_next_task_v1_task *task) {
  return task == NULL ? 1
                      : atomic_load_explicit(&task->cancellation_requested,
                                             memory_order_acquire);
}

int32_t chtholly_next_task_v1_task_deadline_after(
    chtholly_next_task_v1_task *task, uint64_t timeout_nanoseconds,
    chtholly_next_task_v1_deadline **out_deadline) {
  chtholly_next_task_v1_deadline *deadline;
  chtholly_next_task_v1_executor *executor;
  NextTaskInstant now;
  if (out_deadline == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_deadline = NULL;
  if (task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  deadline = (chtholly_next_task_v1_deadline *)calloc(1, sizeof(*deadline));
  if (deadline == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  atomic_init(&deadline->references, 1);
  atomic_init(&deadline->state, NextTaskDeadlineArmed);

  next_task_lock(&task->mutex);
  if (atomic_load_explicit(&task->scheduler_state, memory_order_acquire) !=
          CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING ||
      task->executor == NULL || task->scope == NULL ||
      atomic_load_explicit(&task->published_terminal, memory_order_acquire)) {
    next_task_unlock(&task->mutex);
    free(deadline);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  executor = task->executor;
  chtholly_next_task_v1_executor_retain(executor);
  chtholly_next_task_v1_task_retain(task);
  next_task_unlock(&task->mutex);

  deadline->executor = executor;
  deadline->task = task;
  next_task_lock(&executor->mutex);
  if (executor->shutdown) {
    next_task_unlock(&executor->mutex);
    next_task_deadline_release_task_reference(deadline);
    next_task_deadline_drop_reference(deadline);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_SHUTDOWN;
  }
  if (!next_task_executor_now_locked(executor, &now)) {
    next_task_unlock(&executor->mutex);
    next_task_deadline_release_task_reference(deadline);
    next_task_deadline_drop_reference(deadline);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_CLOCK_FAILURE;
  }
  deadline->instant = next_task_instant_add(now, timeout_nanoseconds);
  if (!next_task_deadline_heap_insert_locked(executor, deadline)) {
    next_task_unlock(&executor->mutex);
    next_task_deadline_release_task_reference(deadline);
    next_task_deadline_drop_reference(deadline);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  (void)atomic_fetch_add_explicit(&deadline->references, 1,
                                  memory_order_relaxed);
  next_task_unlock(&executor->mutex);
  *out_deadline = deadline;
  return 0;
}

void chtholly_next_task_v1_deadline_release(
    chtholly_next_task_v1_deadline *deadline) {
  int removed = 0;
  if (deadline == NULL)
    return;
  next_task_lock(&deadline->executor->mutex);
  if (atomic_load_explicit(&deadline->state, memory_order_acquire) ==
      NextTaskDeadlineArmed) {
    next_task_deadline_heap_remove_locked(deadline->executor, deadline);
    atomic_store_explicit(&deadline->state, NextTaskDeadlineReleased,
                          memory_order_release);
    next_task_notify_all(&deadline->executor->condition);
    removed = 1;
  }
  next_task_unlock(&deadline->executor->mutex);
  if (removed) {
    next_task_deadline_release_task_reference(deadline);
    next_task_deadline_drop_reference(deadline);
  }
  next_task_deadline_drop_reference(deadline);
}

#if defined(CHTHOLLY_NEXT_TASK_TESTING) || defined(CHTHOLLY_RUNTIME_TESTING)
int32_t chtholly_next_task_v1_testing_executor_set_time(
    chtholly_next_task_v1_executor *executor, uint64_t seconds,
    uint32_t nanoseconds) {
  if (executor == NULL ||
      nanoseconds >= CHTHOLLY_NEXT_TASK_NANOSECONDS_PER_SECOND)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&executor->mutex);
  if (executor->deadline_count != 0) {
    next_task_unlock(&executor->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  executor->testing_now.seconds = seconds;
  executor->testing_now.nanoseconds = nanoseconds;
  executor->testing_clock_enabled = 1;
  next_task_notify_all(&executor->condition);
  next_task_unlock(&executor->mutex);
  return 0;
}

int32_t chtholly_next_task_v1_testing_executor_advance_time(
    chtholly_next_task_v1_executor *executor, uint64_t nanoseconds) {
  if (executor == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&executor->mutex);
  if (!executor->testing_clock_enabled) {
    next_task_unlock(&executor->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  executor->testing_now =
      next_task_instant_add(executor->testing_now, nanoseconds);
  next_task_notify_all(&executor->condition);
  next_task_unlock(&executor->mutex);
  return 0;
}

uint32_t chtholly_next_task_v1_testing_cancellation_cause(
    const chtholly_next_task_v1_task *task) {
  return task == NULL ? NextTaskCancellationCauseNone
                      : atomic_load_explicit(&task->cancellation_cause,
                                             memory_order_acquire);
}
#endif

#if defined(CHTHOLLY_NEXT_TASK_TESTING)
static atomic_int next_task_testing_trace;

void chtholly_next_task_v1_testing_request_current_cancel(void) {
  if (next_task_testing_current == NULL)
    abort();
  if (chtholly_next_task_v1_task_request_cancel(next_task_testing_current) != 0)
    abort();
}

void chtholly_next_task_v1_testing_trace_reset(void) {
  atomic_store_explicit(&next_task_testing_trace, 0, memory_order_release);
}

void chtholly_next_task_v1_testing_trace_append(int32_t event) {
  int observed =
      atomic_load_explicit(&next_task_testing_trace, memory_order_acquire);
  while (!atomic_compare_exchange_weak_explicit(
      &next_task_testing_trace, &observed, observed * 10 + event,
      memory_order_acq_rel, memory_order_acquire)) {}
}

int32_t chtholly_next_task_v1_testing_trace_value(void) {
  return atomic_load_explicit(&next_task_testing_trace, memory_order_acquire);
}
#endif

int32_t chtholly_next_task_v1_task_join(chtholly_next_task_v1_task *task) {
  if (task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&task->mutex);
  while (!atomic_load_explicit(&task->published_terminal, memory_order_acquire))
    next_task_wait(&task->condition, &task->mutex);
  next_task_unlock(&task->mutex);
  return 0;
}

int32_t
chtholly_next_task_v1_task_query(const chtholly_next_task_v1_task *task,
                                 chtholly_next_task_v1_task_info *out_info) {
  const size_t task_info_v1_size =
      offsetof(chtholly_next_task_v1_task_info, error_available);
  if (task == NULL || out_info == NULL ||
      out_info->struct_size < task_info_v1_size)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  out_info->state =
      atomic_load_explicit(&task->scheduler_state, memory_order_acquire);
  out_info->cancellation_requested =
      atomic_load_explicit(&task->cancellation_requested, memory_order_acquire);
  out_info->result_available =
      atomic_load_explicit(&task->published_terminal, memory_order_acquire) &&
      task->terminal_step == CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED &&
      task->descriptor.move_result != NULL;
  out_info->result_taken =
      atomic_load_explicit(&task->result_taken, memory_order_acquire);
  out_info->reserved = 0;
  if (out_info->struct_size >= sizeof(*out_info)) {
    out_info->error_available =
        atomic_load_explicit(&task->published_terminal, memory_order_acquire) &&
        task->terminal_step == CHTHOLLY_NEXT_TASK_V1_STEP_FAILED &&
        task->descriptor.move_error != NULL;
    out_info->error_taken =
        atomic_load_explicit(&task->error_taken, memory_order_acquire);
    memset(out_info->reserved2, 0, sizeof(out_info->reserved2));
  }
  return 0;
}

int32_t chtholly_next_task_v1_task_take_result(chtholly_next_task_v1_task *task,
                                               void *out_result) {
  if (task == NULL || out_result == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&task->mutex);
  if (!atomic_load_explicit(&task->published_terminal, memory_order_acquire) ||
      task->terminal_step != CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED ||
      task->descriptor.move_result == NULL) {
    next_task_unlock(&task->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  if (atomic_load_explicit(&task->result_taken, memory_order_acquire)) {
    next_task_unlock(&task->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_RESULT_TAKEN;
  }
  atomic_store_explicit(&task->result_taken, 1, memory_order_release);
  next_task_unlock(&task->mutex);
  task->descriptor.move_result(task->frame, out_result);
  return 0;
}

int32_t chtholly_next_task_v1_task_take_error(chtholly_next_task_v1_task *task,
                                              void *out_error) {
  if (task == NULL || out_error == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&task->mutex);
  if (!atomic_load_explicit(&task->published_terminal, memory_order_acquire) ||
      task->terminal_step != CHTHOLLY_NEXT_TASK_V1_STEP_FAILED ||
      task->descriptor.move_error == NULL) {
    next_task_unlock(&task->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  if (atomic_load_explicit(&task->error_taken, memory_order_acquire)) {
    next_task_unlock(&task->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_RESULT_TAKEN;
  }
  atomic_store_explicit(&task->error_taken, 1, memory_order_release);
  next_task_unlock(&task->mutex);
  task->descriptor.move_error(task->frame, out_error);
  return 0;
}

static void next_task_group_dispose(chtholly_next_task_v1_task_group *group) {
  NextTaskCancellationNode *owner_node = group->owner_node;
  next_task_sync_destroy(&group->mutex, &group->condition);
  free(group);
  next_task_cancellation_node_release(owner_node);
}

static void
next_task_group_drop_reference(chtholly_next_task_v1_task_group *group) {
  if (group != NULL && atomic_fetch_sub_explicit(&group->references, 1,
                                                 memory_order_acq_rel) == 1)
    next_task_group_dispose(group);
}

void chtholly_next_task_v1_task_group_retain(
    chtholly_next_task_v1_task_group *group) {
  if (group != NULL)
    (void)atomic_fetch_add_explicit(&group->references, 1,
                                    memory_order_relaxed);
}

int32_t chtholly_next_task_v1_task_group_create(
    chtholly_next_task_v1_task *owner,
    chtholly_next_task_v1_task_group **out_group) {
  chtholly_next_task_v1_task_group *group;
  NextTaskCancellationNode *owner_node;
  if (out_group == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_group = NULL;
  if (owner == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&owner->mutex);
  if (atomic_load_explicit(&owner->scheduler_state, memory_order_acquire) !=
          CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING ||
      atomic_load_explicit(&owner->published_terminal, memory_order_acquire)) {
    next_task_unlock(&owner->mutex);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  owner_node = owner->cancellation_node;
  next_task_cancellation_node_retain(owner_node);
  next_task_unlock(&owner->mutex);
  group = (chtholly_next_task_v1_task_group *)calloc(1, sizeof(*group));
  if (group == NULL) {
    next_task_cancellation_node_release(owner_node);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  if (!next_task_sync_initialize(&group->mutex, &group->condition)) {
    free(group);
    next_task_cancellation_node_release(owner_node);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  }
  atomic_init(&group->references, 1);
  group->owner_node = owner_node;
  *out_group = group;
  return 0;
}

int32_t
chtholly_next_task_v1_task_group_attach(chtholly_next_task_v1_task_group *group,
                                        chtholly_next_task_v1_task *child,
                                        uint32_t flags) {
  NextTaskGroupMember *member;
  int request_cancel = 0;
  if (group == NULL || child == NULL ||
      (flags & ~CHTHOLLY_NEXT_TASK_V1_GROUP_MEMBER_EXPECT_SUCCESS_ONLY) != 0)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  member = (NextTaskGroupMember *)calloc(1, sizeof(*member));
  if (member == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  member->group = group;
  member->task = child;
  member->flags = flags;
  next_task_lock(&child->mutex);
  if (child->cancellation_node == NULL ||
      child->cancellation_node->parent != group->owner_node) {
    next_task_unlock(&child->mutex);
    free(member);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  }
  next_task_lock(&group->mutex);
  if (group->closed) {
    next_task_unlock(&group->mutex);
    next_task_unlock(&child->mutex);
    free(member);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  if (atomic_load_explicit(&child->published_terminal, memory_order_acquire)) {
    if ((flags & CHTHOLLY_NEXT_TASK_V1_GROUP_MEMBER_EXPECT_SUCCESS_ONLY) != 0) {
      if (child->terminal_step == CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED)
        group->implicit_cancelled = 1;
      else if (child->terminal_step == CHTHOLLY_NEXT_TASK_V1_STEP_FAILED)
        group->implicit_failed = 1;
    }
    next_task_unlock(&group->mutex);
    next_task_unlock(&child->mutex);
    free(member);
    return 0;
  }
  chtholly_next_task_v1_task_retain(child);
  chtholly_next_task_v1_task_group_retain(group);
  member->group_next = group->members;
  group->members = member;
  member->task_next = child->group_members;
  child->group_members = member;
  ++group->active_count;
  request_cancel = group->cancellation_requested;
  member->cancellation_sent = request_cancel;
  next_task_unlock(&group->mutex);
  next_task_unlock(&child->mutex);
  if (request_cancel)
    (void)chtholly_next_task_v1_task_request_cancel(child);
  return 0;
}

int32_t chtholly_next_task_v1_task_group_request_cancel(
    chtholly_next_task_v1_task_group *group) {
  if (group == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&group->mutex);
  group->cancellation_requested = 1;
  next_task_unlock(&group->mutex);
  for (;;) {
    chtholly_next_task_v1_task *task = NULL;
    next_task_lock(&group->mutex);
    for (NextTaskGroupMember *member = group->members; member != NULL;
         member = member->group_next) {
      if (member->cancellation_sent)
        continue;
      member->cancellation_sent = 1;
      task = member->task;
      chtholly_next_task_v1_task_retain(task);
      break;
    }
    next_task_unlock(&group->mutex);
    if (task == NULL)
      break;
    (void)chtholly_next_task_v1_task_request_cancel(task);
    chtholly_next_task_v1_task_release(task);
  }
  return 0;
}

int32_t
chtholly_next_task_v1_task_group_close(chtholly_next_task_v1_task_group *group,
                                       uint8_t request_cancel) {
  if (group == NULL || request_cancel > 1)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&group->mutex);
  group->closed = 1;
  next_task_unlock(&group->mutex);
  return request_cancel ? chtholly_next_task_v1_task_group_request_cancel(group)
                        : 0;
}

int32_t chtholly_next_task_v1_task_group_query(
    const chtholly_next_task_v1_task_group *group,
    chtholly_next_task_v1_task_group_info *out_info) {
  chtholly_next_task_v1_task_group *mutable_group =
      (chtholly_next_task_v1_task_group *)group;
  if (group == NULL || out_info == NULL ||
      out_info->struct_size < sizeof(*out_info))
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  next_task_lock(&mutable_group->mutex);
  out_info->active_count = group->active_count;
  out_info->closed = (uint8_t)group->closed;
  out_info->cancellation_requested = (uint8_t)group->cancellation_requested;
  out_info->implicit_cancelled = (uint8_t)group->implicit_cancelled;
  out_info->implicit_failed = (uint8_t)group->implicit_failed;
  memset(out_info->reserved, 0, sizeof(out_info->reserved));
  next_task_unlock(&mutable_group->mutex);
  return 0;
}

int32_t chtholly_next_task_v1_task_group_completion_arm(
    chtholly_next_task_v1_task_group *group,
    chtholly_next_task_v1_completion_fn wake,
    chtholly_next_task_v1_completion_fn release, void *context,
    chtholly_next_task_v1_completion **out_completion,
    uint32_t *out_disposition) {
  chtholly_next_task_v1_completion *completion;
  if (out_completion == NULL || out_disposition == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_completion = NULL;
  *out_disposition = CHTHOLLY_NEXT_TASK_V1_COMPLETION_READY;
  if (group == NULL || wake == NULL || release == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  completion =
      (chtholly_next_task_v1_completion *)calloc(1, sizeof(*completion));
  if (completion == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  atomic_init(&completion->references, 1);
  atomic_init(&completion->state, NextTaskCompletionReady);
  completion->wake = wake;
  completion->release = release;
  completion->context = context;
  next_task_lock(&group->mutex);
  if (!group->closed) {
    next_task_unlock(&group->mutex);
    free(completion);
    return CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY;
  }
  if (group->active_count != 0) {
    chtholly_next_task_v1_task_group_retain(group);
    completion->owner = group;
    completion->owner_kind = NextTaskCompletionOwnerGroup;
    atomic_store_explicit(&completion->references, 2, memory_order_relaxed);
    atomic_store_explicit(&completion->state, NextTaskCompletionArmed,
                          memory_order_release);
    completion->next = group->completions;
    group->completions = completion;
    *out_disposition = CHTHOLLY_NEXT_TASK_V1_COMPLETION_ARMED;
  }
  next_task_unlock(&group->mutex);
  *out_completion = completion;
  return 0;
}

void chtholly_next_task_v1_task_group_release(
    chtholly_next_task_v1_task_group *group) {
  if (group == NULL)
    return;
  (void)chtholly_next_task_v1_task_group_close(group, 1);
  next_task_group_drop_reference(group);
}

int32_t chtholly_next_task_v1_completion_arm(
    chtholly_next_task_v1_task *task, chtholly_next_task_v1_completion_fn wake,
    chtholly_next_task_v1_completion_fn release, void *context,
    chtholly_next_task_v1_completion **out_completion,
    uint32_t *out_disposition) {
  chtholly_next_task_v1_completion *completion;
  if (out_completion == NULL || out_disposition == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  *out_completion = NULL;
  *out_disposition = CHTHOLLY_NEXT_TASK_V1_COMPLETION_READY;
  if (task == NULL || wake == NULL || release == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  completion =
      (chtholly_next_task_v1_completion *)calloc(1, sizeof(*completion));
  if (completion == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY;
  atomic_init(&completion->references, 1);
  atomic_init(&completion->state, NextTaskCompletionReady);
  completion->wake = wake;
  completion->release = release;
  completion->context = context;

  next_task_lock(&task->mutex);
  if (!atomic_load_explicit(&task->published_terminal, memory_order_acquire)) {
    chtholly_next_task_v1_task_retain(task);
    completion->owner = task;
    completion->owner_kind = NextTaskCompletionOwnerTask;
    atomic_store_explicit(&completion->references, 2, memory_order_relaxed);
    atomic_store_explicit(&completion->state, NextTaskCompletionArmed,
                          memory_order_release);
    completion->next = task->completions;
    task->completions = completion;
    *out_disposition = CHTHOLLY_NEXT_TASK_V1_COMPLETION_ARMED;
  }
  next_task_unlock(&task->mutex);
  *out_completion = completion;
  return 0;
}

uint8_t chtholly_next_task_v1_completion_ready(
    const chtholly_next_task_v1_completion *completion) {
  return completion != NULL &&
         atomic_load_explicit(&completion->state, memory_order_acquire) ==
             NextTaskCompletionReady;
}

int32_t chtholly_next_task_v1_completion_detach(
    chtholly_next_task_v1_completion *completion) {
  chtholly_next_task_v1_task *task = NULL;
  chtholly_next_task_v1_task_group *group = NULL;
  chtholly_next_task_v1_completion **cursor = NULL;
  int detached = 0;
  if (completion == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT;
  if (completion->owner_kind == NextTaskCompletionOwnerTask)
    task = (chtholly_next_task_v1_task *)completion->owner;
  else if (completion->owner_kind == NextTaskCompletionOwnerGroup)
    group = (chtholly_next_task_v1_task_group *)completion->owner;
  if (task == NULL && group == NULL)
    return 0;
  if (task != NULL)
    next_task_lock(&task->mutex);
  else
    next_task_lock(&group->mutex);
  if (atomic_load_explicit(&completion->state, memory_order_acquire) ==
      NextTaskCompletionArmed) {
    cursor = task != NULL ? &task->completions : &group->completions;
    while (*cursor != NULL && *cursor != completion)
      cursor = &(*cursor)->next;
    if (*cursor == completion) {
      *cursor = completion->next;
      completion->next = NULL;
      atomic_store_explicit(&completion->state, NextTaskCompletionDetached,
                            memory_order_release);
      detached = 1;
    }
  }
  if (task != NULL)
    next_task_unlock(&task->mutex);
  else
    next_task_unlock(&group->mutex);
  if (detached) {
    completion->release(completion->context);
    if (atomic_fetch_sub_explicit(&completion->references, 1,
                                  memory_order_acq_rel) == 1) {
      next_task_completion_owner_release(completion);
      free(completion);
    }
  }
  return 0;
}

void chtholly_next_task_v1_completion_release(
    chtholly_next_task_v1_completion *completion) {
  if (completion == NULL)
    return;
  (void)chtholly_next_task_v1_completion_detach(completion);
  if (atomic_fetch_sub_explicit(&completion->references, 1,
                                memory_order_acq_rel) == 1) {
    next_task_completion_owner_release(completion);
    free(completion);
  }
}
