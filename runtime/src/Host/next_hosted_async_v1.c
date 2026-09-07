#include "chtholly/next_hosted_async_v1.h"

#include "chtholly/next_task_v1.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct NextHostedAsyncSubscription {
  chtholly_next_hosted_async_v1_subscription_fn callback;
  void *context;
  atomic_uchar cancelled;
} NextHostedAsyncSubscription;

typedef struct NextHostedAsyncCompletion {
  atomic_uchar ready;
} NextHostedAsyncCompletion;

void *chtholly_next_hosted_async_v1_subscription_register(
    chtholly_next_hosted_async_v1_subscription_fn callback, void *context,
    chtholly_next_hosted_async_v1_release_fn release) {
  NextHostedAsyncSubscription *subscription;
  (void)release;
  if (callback == NULL)
    return NULL;
  subscription =
      (NextHostedAsyncSubscription *)calloc(1, sizeof(*subscription));
  if (subscription == NULL)
    return NULL;
  subscription->callback = callback;
  subscription->context = context;
  atomic_init(&subscription->cancelled, 0);
  return subscription;
}

void chtholly_next_hosted_async_v1_subscription_unregister(void *handle) {
  free(handle);
}

void chtholly_next_hosted_async_v1_subscription_cancel(void *handle) {
  NextHostedAsyncSubscription *subscription =
      (NextHostedAsyncSubscription *)handle;
  if (subscription == NULL)
    return;
  atomic_store_explicit(&subscription->cancelled, 1, memory_order_release);
  free(subscription);
}

void *chtholly_next_hosted_async_v1_subscription_cancel_async(void *handle) {
  NextHostedAsyncSubscription *subscription =
      (NextHostedAsyncSubscription *)handle;
  NextHostedAsyncCompletion *completion;
  if (subscription == NULL)
    return NULL;
  atomic_store_explicit(&subscription->cancelled, 1, memory_order_release);
  completion = (NextHostedAsyncCompletion *)calloc(1, sizeof(*completion));
  free(subscription);
  if (completion == NULL)
    return NULL;
  atomic_init(&completion->ready, 1);
  return completion;
}

void chtholly_next_hosted_async_v1_completion_wait(void *completion) {
  free(completion);
}

bool chtholly_next_hosted_async_v1_completion_poll(void *completion) {
  const NextHostedAsyncCompletion *value =
      (const NextHostedAsyncCompletion *)completion;
  return value != NULL &&
         atomic_load_explicit(&value->ready, memory_order_acquire) != 0;
}

bool chtholly_next_hosted_async_v1_completion_arm(
    void *completion, chtholly_next_hosted_async_v1_wake_fn wake, void *context,
    chtholly_next_hosted_async_v1_release_fn release) {
  (void)wake;
  if (release != NULL)
    release(context);
  return completion != NULL &&
         !chtholly_next_hosted_async_v1_completion_poll(completion);
}

void chtholly_next_hosted_async_v1_completion_detach(
    void *completion, void *context,
    chtholly_next_hosted_async_v1_release_fn release) {
  if (release != NULL)
    release(context);
  free(completion);
}

int32_t chtholly_next_hosted_async_v1_scheduler_resume(void *task_identity) {
  chtholly_next_task_v1_task *task =
      (chtholly_next_task_v1_task *)task_identity;
  chtholly_next_task_v1_task_info info = {sizeof(info), 0, 0, 0, 0, 0, 0, 0,
                                          {0}};
  if (task == NULL)
    return CHTHOLLY_NEXT_TASK_V1_STEP_FAILED;
  (void)chtholly_next_task_v1_task_wake(task);
  if (chtholly_next_task_v1_task_query(task, &info) != 0)
    return CHTHOLLY_NEXT_TASK_V1_STEP_FAILED;
  if (info.state == CHTHOLLY_NEXT_TASK_V1_STATE_CANCELLED)
    return CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED;
  if (info.state == CHTHOLLY_NEXT_TASK_V1_STATE_COMPLETED)
    return CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED;
  if (info.state == CHTHOLLY_NEXT_TASK_V1_STATE_FAILED)
    return CHTHOLLY_NEXT_TASK_V1_STEP_FAILED;
  return CHTHOLLY_NEXT_TASK_V1_STEP_SUSPENDED;
}

void chtholly_next_hosted_async_v1_task_request_cancel(void *task_identity) {
  (void)chtholly_next_task_v1_task_request_cancel(
      (chtholly_next_task_v1_task *)task_identity);
}

bool chtholly_next_hosted_async_v1_task_is_cancelled(void *task_identity) {
  return chtholly_next_task_v1_task_cancellation_requested(
             (const chtholly_next_task_v1_task *)task_identity) != 0;
}

void chtholly_next_hosted_async_v1_scope_request_cancel(void *scope_identity) {
  (void)chtholly_next_task_v1_scope_request_cancel(
      (chtholly_next_task_v1_scope *)scope_identity);
}
