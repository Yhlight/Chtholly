#ifndef CHTHOLLY_NEXT_TASK_V1_H
#define CHTHOLLY_NEXT_TASK_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_TASK_ABI_V1 1u

#define CHTHOLLY_NEXT_TASK_V1_STATUS_INVALID_ARGUMENT (-2601)
#define CHTHOLLY_NEXT_TASK_V1_STATUS_OUT_OF_MEMORY (-2602)
#define CHTHOLLY_NEXT_TASK_V1_STATUS_SHUTDOWN (-2603)
#define CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY (-2604)
#define CHTHOLLY_NEXT_TASK_V1_STATUS_RESULT_TAKEN (-2605)
#define CHTHOLLY_NEXT_TASK_V1_STATUS_CLOCK_FAILURE (-2606)

#define CHTHOLLY_NEXT_TASK_V1_STEP_SUSPENDED 0u
#define CHTHOLLY_NEXT_TASK_V1_STEP_RESCHEDULE 1u
#define CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED 2u
#define CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED 3u
#define CHTHOLLY_NEXT_TASK_V1_STEP_FAILED 4u

#define CHTHOLLY_NEXT_TASK_V1_STATE_IDLE 0u
#define CHTHOLLY_NEXT_TASK_V1_STATE_QUEUED 1u
#define CHTHOLLY_NEXT_TASK_V1_STATE_RUNNING 2u
#define CHTHOLLY_NEXT_TASK_V1_STATE_CANCELLED 3u
#define CHTHOLLY_NEXT_TASK_V1_STATE_COMPLETED 4u
#define CHTHOLLY_NEXT_TASK_V1_STATE_FAILED 5u

typedef struct chtholly_next_task_v1_executor chtholly_next_task_v1_executor;
typedef struct chtholly_next_task_v1_scope chtholly_next_task_v1_scope;
typedef struct chtholly_next_task_v1_task chtholly_next_task_v1_task;
typedef struct chtholly_next_task_v1_task_group
    chtholly_next_task_v1_task_group;
typedef struct chtholly_next_task_v1_completion
    chtholly_next_task_v1_completion;
typedef struct chtholly_next_task_v1_deadline chtholly_next_task_v1_deadline;

typedef uint32_t (*chtholly_next_task_v1_resume_fn)(
    void *frame, chtholly_next_task_v1_task *task);
typedef void (*chtholly_next_task_v1_destroy_fn)(void *frame);
typedef void (*chtholly_next_task_v1_move_result_fn)(void *frame,
                                                     void *out_result);
typedef void (*chtholly_next_task_v1_completion_fn)(void *context);

#define CHTHOLLY_NEXT_TASK_V1_COMPLETION_READY 0u
#define CHTHOLLY_NEXT_TASK_V1_COMPLETION_ARMED 1u

typedef struct chtholly_next_task_v1_frame_descriptor {
  uint64_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  chtholly_next_task_v1_resume_fn resume;
  chtholly_next_task_v1_destroy_fn destroy;
  chtholly_next_task_v1_move_result_fn move_result;
  /* Optional append-only v1 field. A descriptor ending before this field is
     a success-only descriptor. */
  chtholly_next_task_v1_move_result_fn move_error;
} chtholly_next_task_v1_frame_descriptor;

typedef struct chtholly_next_task_v1_executor_config {
  uint64_t struct_size;
  uint32_t worker_count;
  uint32_t reserved;
} chtholly_next_task_v1_executor_config;

typedef struct chtholly_next_task_v1_task_info {
  uint64_t struct_size;
  uint32_t state;
  uint8_t cancellation_requested;
  uint8_t result_available;
  uint8_t result_taken;
  uint8_t reserved;
  /* Optional append-only v1 fields, written only when struct_size includes
     them. */
  uint8_t error_available;
  uint8_t error_taken;
  uint8_t reserved2[6];
} chtholly_next_task_v1_task_info;

#define CHTHOLLY_NEXT_TASK_V1_GROUP_MEMBER_EXPECT_SUCCESS_ONLY 1u

typedef struct chtholly_next_task_v1_task_group_info {
  uint64_t struct_size;
  uint64_t active_count;
  uint8_t closed;
  uint8_t cancellation_requested;
  uint8_t implicit_cancelled;
  uint8_t implicit_failed;
  uint8_t reserved[4];
} chtholly_next_task_v1_task_group_info;

void *chtholly_next_task_v1_frame_allocate(uint64_t size, uint64_t alignment);
void chtholly_next_task_v1_frame_deallocate(void *frame);

int32_t chtholly_next_task_v1_executor_create(
    const chtholly_next_task_v1_executor_config *config,
    chtholly_next_task_v1_executor **out_executor);
void chtholly_next_task_v1_executor_retain(
    chtholly_next_task_v1_executor *executor);
void chtholly_next_task_v1_executor_release(
    chtholly_next_task_v1_executor *executor);

int32_t
chtholly_next_task_v1_scope_create(chtholly_next_task_v1_executor *executor,
                                   chtholly_next_task_v1_scope **out_scope);
void chtholly_next_task_v1_scope_retain(chtholly_next_task_v1_scope *scope);
/* Releasing a public scope handle first requests cancellation and joins all
   children. Retained aliases observe the same closed scope. */
void chtholly_next_task_v1_scope_release(chtholly_next_task_v1_scope *scope);
int32_t
chtholly_next_task_v1_scope_request_cancel(chtholly_next_task_v1_scope *scope);
int32_t chtholly_next_task_v1_scope_join(chtholly_next_task_v1_scope *scope);

/* The descriptor is copied. On success task_create consumes frame and queues
   it immediately. On failure frame ownership is unchanged. The task passed to
   resume is borrowed for the duration of that call. */
int32_t chtholly_next_task_v1_task_create(
    chtholly_next_task_v1_scope *scope,
    const chtholly_next_task_v1_frame_descriptor *descriptor, void *frame,
    chtholly_next_task_v1_task **out_task);
/* Creates an eager child on the parent's current executor and cancellation
   scope. The child joins the parent's cancellation ancestry: parent
   cancellation propagates downward, while child cancellation does not affect
   its parent or siblings. This is valid only while parent is running. */
int32_t chtholly_next_task_v1_task_create_child(
    chtholly_next_task_v1_task *parent,
    const chtholly_next_task_v1_frame_descriptor *descriptor, void *frame,
    chtholly_next_task_v1_task **out_task);
void chtholly_next_task_v1_task_retain(chtholly_next_task_v1_task *task);
void chtholly_next_task_v1_task_release(chtholly_next_task_v1_task *task);
/* Cancellation is sticky and propagates to every currently attached
   descendant. A child attached concurrently either participates in that
   propagation or inherits the already-requested state. */
int32_t
chtholly_next_task_v1_task_request_cancel(chtholly_next_task_v1_task *task);
int32_t chtholly_next_task_v1_task_wake(chtholly_next_task_v1_task *task);
/* Rebinds future wake/resume scheduling. The running continuation must return
   STEP_RESCHEDULE after a successful rebind. */
int32_t chtholly_next_task_v1_task_rebind_executor(
    chtholly_next_task_v1_task *task, chtholly_next_task_v1_executor *executor);
uint8_t chtholly_next_task_v1_task_cancellation_requested(
    const chtholly_next_task_v1_task *task);
/* Registers a relative timeout against the hosted monotonic clock. This is
   valid only while task is running. Zero is an immediate deadline. Release
   is nonblocking and must occur after structured children are drained. */
int32_t chtholly_next_task_v1_task_deadline_after(
    chtholly_next_task_v1_task *task, uint64_t timeout_nanoseconds,
    chtholly_next_task_v1_deadline **out_deadline);
void chtholly_next_task_v1_deadline_release(
    chtholly_next_task_v1_deadline *deadline);
int32_t chtholly_next_task_v1_task_join(chtholly_next_task_v1_task *task);
int32_t
chtholly_next_task_v1_task_query(const chtholly_next_task_v1_task *task,
                                 chtholly_next_task_v1_task_info *out_info);
int32_t chtholly_next_task_v1_task_take_result(chtholly_next_task_v1_task *task,
                                               void *out_result);
int32_t chtholly_next_task_v1_task_take_error(chtholly_next_task_v1_task *task,
                                              void *out_error);

/* A task group is a compiler-owned lexical child set. It never blocks during
   release. Attach is linearized with task terminal publication; cancellation
   reaches attached children and their existing cancellation descendants. */
int32_t chtholly_next_task_v1_task_group_create(
    chtholly_next_task_v1_task *owner,
    chtholly_next_task_v1_task_group **out_group);
void chtholly_next_task_v1_task_group_retain(
    chtholly_next_task_v1_task_group *group);
void chtholly_next_task_v1_task_group_release(
    chtholly_next_task_v1_task_group *group);
int32_t
chtholly_next_task_v1_task_group_attach(chtholly_next_task_v1_task_group *group,
                                        chtholly_next_task_v1_task *child,
                                        uint32_t flags);
int32_t chtholly_next_task_v1_task_group_request_cancel(
    chtholly_next_task_v1_task_group *group);
int32_t
chtholly_next_task_v1_task_group_close(chtholly_next_task_v1_task_group *group,
                                       uint8_t request_cancel);
int32_t chtholly_next_task_v1_task_group_query(
    const chtholly_next_task_v1_task_group *group,
    chtholly_next_task_v1_task_group_info *out_info);
int32_t chtholly_next_task_v1_task_group_completion_arm(
    chtholly_next_task_v1_task_group *group,
    chtholly_next_task_v1_completion_fn wake,
    chtholly_next_task_v1_completion_fn release, void *context,
    chtholly_next_task_v1_completion **out_completion,
    uint32_t *out_disposition);

/* Atomically observes terminal publication or installs a one-shot waiter.
   READY leaves callback ownership with the caller. ARMED transfers it until
   terminal publication or detach, when release is called exactly once.
   Callbacks are never invoked while the task mutex is held. */
int32_t chtholly_next_task_v1_completion_arm(
    chtholly_next_task_v1_task *task, chtholly_next_task_v1_completion_fn wake,
    chtholly_next_task_v1_completion_fn release, void *context,
    chtholly_next_task_v1_completion **out_completion,
    uint32_t *out_disposition);
uint8_t chtholly_next_task_v1_completion_ready(
    const chtholly_next_task_v1_completion *completion);
int32_t chtholly_next_task_v1_completion_detach(
    chtholly_next_task_v1_completion *completion);
/* Release also detaches an armed completion. */
void chtholly_next_task_v1_completion_release(
    chtholly_next_task_v1_completion *completion);

#ifdef __cplusplus
}
#endif

#endif
