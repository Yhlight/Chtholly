#include "chtholly/next_task_v1.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace {

constexpr std::uint32_t kCancellationCauseOwnerRequest = 1;
constexpr std::uint32_t kCancellationCauseDeadlineExpired = 2;

struct Frame {
  std::atomic<int> calls{0};
  int value = 0;
  bool suspend_once = false;
  bool arm_deadline = false;
  chtholly_next_task_v1_deadline *deadline = nullptr;
};

std::atomic<int> destroyed_frames{0};
std::atomic<int> destroyed_values{0};
std::atomic<int> completion_wakes{0};
std::atomic<int> completion_releases{0};

extern "C" int32_t chtholly_next_task_v1_testing_executor_set_time(
    chtholly_next_task_v1_executor *, std::uint64_t, std::uint32_t);
extern "C" int32_t chtholly_next_task_v1_testing_executor_advance_time(
    chtholly_next_task_v1_executor *, std::uint64_t);
extern "C" std::uint32_t chtholly_next_task_v1_testing_cancellation_cause(
    const chtholly_next_task_v1_task *);

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "task runtime test failed: %s\n", message);
  std::abort();
}

void check(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

std::uint32_t resume(void *opaque, chtholly_next_task_v1_task *task) {
  auto *frame = static_cast<Frame *>(opaque);
  const int call = frame->calls.fetch_add(1, std::memory_order_acq_rel);
  if (frame->arm_deadline && call == 0) {
    check(chtholly_next_task_v1_task_deadline_after(task, 1'000'000,
                                                    &frame->deadline) == 0,
          "deadline registration");
  }
  if (chtholly_next_task_v1_task_cancellation_requested(task) != 0)
    return CHTHOLLY_NEXT_TASK_V1_STEP_CANCELLED;
  if (frame->suspend_once && call == 0)
    return CHTHOLLY_NEXT_TASK_V1_STEP_SUSPENDED;
  return CHTHOLLY_NEXT_TASK_V1_STEP_COMPLETED;
}

void destroy(void *opaque) {
  auto *frame = static_cast<Frame *>(opaque);
  if (frame->deadline != nullptr) {
    chtholly_next_task_v1_deadline_release(frame->deadline);
    frame->deadline = nullptr;
  }
  ++destroyed_frames;
  destroyed_values.fetch_add(frame->value, std::memory_order_acq_rel);
  frame->~Frame();
  chtholly_next_task_v1_frame_deallocate(frame);
}

void move_result(void *opaque, void *out) {
  *static_cast<int *>(out) = static_cast<Frame *>(opaque)->value;
}

void wake(void *) { ++completion_wakes; }
void release(void *) { ++completion_releases; }

chtholly_next_task_v1_frame_descriptor descriptor() {
  chtholly_next_task_v1_frame_descriptor result{};
  result.struct_size = sizeof(result);
  result.abi_version = CHTHOLLY_NEXT_TASK_ABI_V1;
  result.resume = &resume;
  result.destroy = &destroy;
  result.move_result = &move_result;
  return result;
}

Frame *make_frame(int value, bool suspend_once = false,
                  bool arm_deadline = false) {
  auto *frame = static_cast<Frame *>(
      chtholly_next_task_v1_frame_allocate(sizeof(Frame), alignof(Frame)));
  check(frame != nullptr, "frame allocation");
  new (frame) Frame{};
  frame->value = value;
  frame->suspend_once = suspend_once;
  frame->arm_deadline = arm_deadline;
  return frame;
}

void wait_for_state(chtholly_next_task_v1_task *task, std::uint32_t state) {
  chtholly_next_task_v1_task_info info{};
  info.struct_size = sizeof(info);
  for (int attempt = 0; attempt != 2000; ++attempt) {
    check(chtholly_next_task_v1_task_query(task, &info) == 0, "task query");
    if (info.state == state)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  fail("task state timeout");
}

void test_suspend_wake_and_completion(chtholly_next_task_v1_scope *scope) {
  auto *frame = make_frame(42, true);
  auto desc = descriptor();
  chtholly_next_task_v1_task *task = nullptr;
  check(chtholly_next_task_v1_task_create(scope, &desc, frame, &task) == 0,
        "suspended task create");
  wait_for_state(task, CHTHOLLY_NEXT_TASK_V1_STATE_IDLE);

  chtholly_next_task_v1_completion *completion = nullptr;
  std::uint32_t disposition = 99;
  check(chtholly_next_task_v1_completion_arm(
            task, &wake, &release, nullptr, &completion, &disposition) == 0,
        "completion arm");
  check(disposition == CHTHOLLY_NEXT_TASK_V1_COMPLETION_ARMED,
        "completion must arm for suspended task");
  check(chtholly_next_task_v1_task_wake(task) == 0, "task wake");
  check(chtholly_next_task_v1_task_join(task) == 0, "task join");
  check(chtholly_next_task_v1_completion_ready(completion) != 0,
        "completion ready");
  check(completion_wakes.load() == 1, "completion wake callback");
  check(completion_releases.load() == 1, "completion release callback");
  chtholly_next_task_v1_completion_release(completion);
  check(completion_releases.load() == 1, "completion release callback");

  int result = 0;
  check(chtholly_next_task_v1_task_take_result(task, &result) == 0,
        "take result");
  check(result == 42, "result payload");
  check(chtholly_next_task_v1_task_take_result(task, &result) ==
            CHTHOLLY_NEXT_TASK_V1_STATUS_RESULT_TAKEN,
        "result is one shot");
  chtholly_next_task_v1_task_release(task);
}

void test_cancel_and_deadline(chtholly_next_task_v1_executor *executor,
                              chtholly_next_task_v1_scope *scope) {
  auto *frame = make_frame(7, true);
  auto desc = descriptor();
  chtholly_next_task_v1_task *task = nullptr;
  check(chtholly_next_task_v1_task_create(scope, &desc, frame, &task) == 0,
        "cancel task create");
  wait_for_state(task, CHTHOLLY_NEXT_TASK_V1_STATE_IDLE);
  check(chtholly_next_task_v1_task_request_cancel(task) == 0,
        "task cancellation request");
  check(chtholly_next_task_v1_task_wake(task) == 0, "cancelled task wake");
  check(chtholly_next_task_v1_task_join(task) == 0, "cancelled task join");
  chtholly_next_task_v1_task_info info{};
  info.struct_size = sizeof(info);
  check(chtholly_next_task_v1_task_query(task, &info) == 0,
        "cancelled task query");
  check(info.state == CHTHOLLY_NEXT_TASK_V1_STATE_CANCELLED,
        "cancelled task state");
  check(chtholly_next_task_v1_testing_cancellation_cause(task) ==
            kCancellationCauseOwnerRequest,
        "owner cancellation cause");
  int result = 0;
  check(chtholly_next_task_v1_task_take_result(task, &result) ==
            CHTHOLLY_NEXT_TASK_V1_STATUS_NOT_READY,
        "cancelled task has no result");
  chtholly_next_task_v1_task_release(task);

  check(chtholly_next_task_v1_testing_executor_set_time(executor, 10, 0) ==
            0,
        "testing clock setup");
  frame = make_frame(9, true, true);
  task = nullptr;
  check(chtholly_next_task_v1_task_create(scope, &desc, frame, &task) == 0,
        "deadline task create");
  wait_for_state(task, CHTHOLLY_NEXT_TASK_V1_STATE_IDLE);
  check(chtholly_next_task_v1_testing_executor_advance_time(executor,
                                                              2'000'000) == 0,
        "testing clock advance");
  check(chtholly_next_task_v1_task_join(task) == 0, "deadline task join");
  check(chtholly_next_task_v1_task_query(task, &info) == 0,
        "deadline task query");
  check(info.state == CHTHOLLY_NEXT_TASK_V1_STATE_CANCELLED,
        "deadline cancellation state");
  check(chtholly_next_task_v1_testing_cancellation_cause(task) ==
            kCancellationCauseDeadlineExpired,
        "deadline cancellation cause");
  chtholly_next_task_v1_task_release(task);

  frame = make_frame(11, false, true);
  task = nullptr;
  check(chtholly_next_task_v1_task_create(scope, &desc, frame, &task) == 0,
        "terminal deadline task create");
  check(chtholly_next_task_v1_task_join(task) == 0,
        "terminal deadline task join");
  check(chtholly_next_task_v1_task_query(task, &info) == 0,
        "terminal deadline task query");
  check(info.state == CHTHOLLY_NEXT_TASK_V1_STATE_COMPLETED,
        "terminal deadline task state");
  chtholly_next_task_v1_task_release(task);
}

} // namespace

int main() {
  chtholly_next_task_v1_executor_config config{};
  config.struct_size = sizeof(config);
  config.worker_count = 2;
  chtholly_next_task_v1_executor *executor = nullptr;
  check(chtholly_next_task_v1_executor_create(&config, &executor) == 0,
        "executor create");
  chtholly_next_task_v1_scope *scope = nullptr;
  check(chtholly_next_task_v1_scope_create(executor, &scope) == 0,
        "scope create");
  test_suspend_wake_and_completion(scope);
  test_cancel_and_deadline(executor, scope);
  check(chtholly_next_task_v1_scope_join(scope) == 0, "scope join");
  chtholly_next_task_v1_scope_release(scope);
  chtholly_next_task_v1_executor_release(executor);
  if (destroyed_frames.load() != 4) {
    std::fprintf(stderr, "destroyed frame count: %d values: %d\n",
                 destroyed_frames.load(), destroyed_values.load());
    fail("all task frames destroyed");
  }
  return 0;
}
