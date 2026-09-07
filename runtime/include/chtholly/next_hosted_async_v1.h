#ifndef CHTHOLLY_NEXT_HOSTED_ASYNC_V1_H
#define CHTHOLLY_NEXT_HOSTED_ASYNC_V1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_HOSTED_ASYNC_ABI_V1 1u

typedef int32_t (*chtholly_next_hosted_async_v1_subscription_fn)(int32_t value,
                                                                 void *context);
typedef void (*chtholly_next_hosted_async_v1_wake_fn)(void *context);
typedef void (*chtholly_next_hosted_async_v1_release_fn)(void *context);

void *chtholly_next_hosted_async_v1_subscription_register(
    chtholly_next_hosted_async_v1_subscription_fn callback, void *context,
    chtholly_next_hosted_async_v1_release_fn release);
void chtholly_next_hosted_async_v1_subscription_unregister(void *handle);
void chtholly_next_hosted_async_v1_subscription_cancel(void *handle);
void *chtholly_next_hosted_async_v1_subscription_cancel_async(void *handle);

void chtholly_next_hosted_async_v1_completion_wait(void *completion);
bool chtholly_next_hosted_async_v1_completion_poll(void *completion);
bool chtholly_next_hosted_async_v1_completion_arm(
    void *completion, chtholly_next_hosted_async_v1_wake_fn wake, void *context,
    chtholly_next_hosted_async_v1_release_fn release);
void chtholly_next_hosted_async_v1_completion_detach(
    void *completion, void *context,
    chtholly_next_hosted_async_v1_release_fn release);

int32_t chtholly_next_hosted_async_v1_scheduler_resume(void *task_identity);
void chtholly_next_hosted_async_v1_task_request_cancel(void *task_identity);
bool chtholly_next_hosted_async_v1_task_is_cancelled(void *task_identity);
void chtholly_next_hosted_async_v1_scope_request_cancel(void *scope_identity);

#ifdef __cplusplus
}
#endif

#endif
