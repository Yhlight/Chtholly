#include "chtholly/next_host_v1.h"
#include "chtholly/next_runtime_v1.h"

#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
struct TypedPayload {
  std::int32_t value = 0;
  std::int32_t marker = 0;
};
std::atomic<int> typed_drop_count = 0;
void typed_move(void *destination, void *source) {
  auto *to = static_cast<TypedPayload *>(destination);
  auto *from = static_cast<TypedPayload *>(source);
  *to = *from;
  from->value = 0;
  from->marker = 0;
}
void typed_drop(void *value) {
  auto *payload = static_cast<TypedPayload *>(value);
  ++typed_drop_count;
  payload->value = 0;
  payload->marker = 0;
}

void checkAt(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "host runtime check failed at line %d: %s\n", line,
                 expression);
    std::abort();
  }
}
#define check(condition) checkAt((condition), #condition, __LINE__)
} // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    std::filesystem::path(u8"chtholly-next-host-文件-📦.bin");
  const auto path_utf8 = path.u8string();
  const std::string path_text(path_utf8.begin(), path_utf8.end());
  void *handle = nullptr;
  const std::uint8_t payload[] = {'C', 'h', 't', 'h', 'o', 'l', 'l', 'y'};
  std::uint8_t readback[sizeof(payload)] = {};
  chtholly_next_host_v1_instant first{}, second{};
  void *task = nullptr;
  const std::uint8_t argument_zero[] = "chtholly-test";
  const std::uint8_t argument_one[] = "preview-argument";
  const std::uint8_t *arguments[] = {argument_zero, argument_one};

  check(chtholly_next_runtime_v1_set_process_args_utf8(2, arguments) == 0);
  check(chtholly_next_runtime_v1_arg_count() == 2);
  check(chtholly_next_runtime_v1_arg_size(1) == 16);
  check(std::string(reinterpret_cast<const char *>(
                        chtholly_next_runtime_v1_arg_data(1)),
                    chtholly_next_runtime_v1_arg_size(1)) ==
        "preview-argument");
  check(chtholly_next_runtime_v1_arg_data(2) == nullptr);
  check(chtholly_next_runtime_v1_console_write(1, nullptr, 0) == 0);

  void *aligned = chtholly_next_runtime_v1_allocate(37, 64);
  check(aligned != nullptr);
  check(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
  chtholly_next_runtime_v1_deallocate(aligned, 37, 64);
  check(chtholly_next_runtime_v1_allocate(8, 3) == nullptr);
  chtholly_next_runtime_v1_testing_set_allocation_limit(7);
  check(chtholly_next_runtime_v1_allocate(8, 8) == nullptr);
  chtholly_next_runtime_v1_testing_set_allocation_limit(UINT64_MAX);
  chtholly_next_runtime_v1_testing_lifecycle_reset();
  chtholly_next_runtime_v1_testing_lifecycle_construct();
  chtholly_next_runtime_v1_testing_lifecycle_construct();
  chtholly_next_runtime_v1_testing_lifecycle_drop();
  chtholly_next_runtime_v1_testing_lifecycle_expect_drops(1);
  check(chtholly_next_runtime_v1_testing_lifecycle_construct_count() == 2);
  check(chtholly_next_runtime_v1_testing_lifecycle_drop_count() == 1);

  std::filesystem::remove(path);
  check(chtholly_next_host_v1_open(
            reinterpret_cast<const std::uint8_t *>(path_text.data()),
            path_text.size(), &handle) == 0);
  check(handle != nullptr);
  check(chtholly_next_host_v1_write(handle, payload, sizeof(payload)) ==
        static_cast<std::int64_t>(sizeof(payload)));
  check(chtholly_next_host_v1_close(handle) == 0);
  const std::uint8_t embedded_nul[] = {'b', 'a', 0, 'd'};
  handle = nullptr;
  check(chtholly_next_host_v1_open(embedded_nul, sizeof(embedded_nul),
                                  &handle) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(handle == nullptr);

  handle = nullptr;
  check(chtholly_next_host_v1_open(
            reinterpret_cast<const std::uint8_t *>(path_text.data()),
            path_text.size(), &handle) == 0);
  check(chtholly_next_host_v1_read(handle, readback, sizeof(readback)) ==
        static_cast<std::int64_t>(sizeof(readback)));
  check(std::string(reinterpret_cast<char *>(readback), sizeof(readback)) ==
        "Chtholly");
  check(chtholly_next_host_v1_close(handle) == 0);
  check(chtholly_next_host_v1_close(nullptr) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE);
  check(chtholly_next_host_v1_read(nullptr, readback, sizeof(readback)) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(chtholly_next_host_v1_write(nullptr, payload, sizeof(payload)) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);

  const std::uint8_t replacement[] = {'s', 'h', 'o', 'r', 't'};
  check(chtholly_next_runtime_v1_fs_exists(
            reinterpret_cast<const std::uint8_t *>(path_text.data()),
            path_text.size()) == 1);
  check(chtholly_next_runtime_v1_fs_write(
            reinterpret_cast<const std::uint8_t *>(path_text.data()),
            path_text.size(), replacement, sizeof(replacement)) ==
        static_cast<std::int64_t>(sizeof(replacement)));
  check(std::filesystem::file_size(path) == sizeof(replacement));
  std::ifstream replaced(path, std::ios::binary);
  check(std::string(std::istreambuf_iterator<char>(replaced), {}) == "short");
  replaced.close();
  check(chtholly_next_runtime_v1_fs_remove(
            reinterpret_cast<const std::uint8_t *>(path_text.data()),
            path_text.size()) == 0);
  check(chtholly_next_runtime_v1_fs_exists(
            reinterpret_cast<const std::uint8_t *>(path_text.data()),
            path_text.size()) == 0);

  check(chtholly_next_host_v1_monotonic_now(&first) == 0);
  check(chtholly_next_host_v1_monotonic_now(&second) == 0);
  check(second.seconds > first.seconds ||
        (second.seconds == first.seconds &&
         second.nanoseconds >= first.nanoseconds));

  int entry_marker = 0;
  check(chtholly_next_host_v1_task_spawn(nullptr, &task) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(chtholly_next_host_v1_task_spawn(&entry_marker, nullptr) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(chtholly_next_host_v1_task_spawn(&entry_marker, &task) == 0);
  check(chtholly_next_host_v1_task_poll(task) ==
        CHTHOLLY_NEXT_HOST_STATUS_NOT_READY);
  check(chtholly_next_host_v1_task_cancel(task) == 0);
  check(chtholly_next_host_v1_task_poll(task) ==
        CHTHOLLY_NEXT_HOST_STATUS_CANCELLED);
  check(chtholly_next_host_v1_task_join(task) ==
        CHTHOLLY_NEXT_HOST_STATUS_CANCELLED);

  task = nullptr;
  check(chtholly_next_host_v1_task_spawn(&entry_marker, &task) == 0);
  check(chtholly_next_host_v1_task_wake(task) == 0);
  check(chtholly_next_host_v1_task_poll(task) == 0);
  check(chtholly_next_host_v1_task_join(task) == 0);

  void *listener = nullptr;
  check(chtholly_next_host_v1_net_listen(39231, &listener) == 0);
  check(listener != nullptr);
  std::thread client([&] {
#if defined(_WIN32)
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET)
      return;
#else
    int socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0)
      return;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(39231);
#if defined(_WIN32)
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_handle, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == 0) {
      const std::uint8_t bytes[] = {'n', 'e', 't'};
      (void)send(socket_handle, reinterpret_cast<const char *>(bytes),
                 sizeof(bytes), 0);
    }
    closesocket(socket_handle);
#else
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_handle, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == 0) {
      const std::uint8_t bytes[] = {'n', 'e', 't'};
      (void)send(socket_handle, bytes, sizeof(bytes), 0);
    }
    close(socket_handle);
#endif
  });
  void *stream = nullptr;
  check(chtholly_next_host_v1_net_accept(listener, &stream) == 0);
  std::uint8_t network_bytes[3] = {};
  check(chtholly_next_host_v1_net_read(stream, network_bytes,
                                       sizeof(network_bytes)) == 3);
  check(std::string(reinterpret_cast<char *>(network_bytes), 3) == "net");
  check(chtholly_next_host_v1_net_read(stream, network_bytes, 0) == 0);
  client.join();
  check(chtholly_next_host_v1_net_close(stream) == 0);
  check(chtholly_next_host_v1_net_close(listener) == 0);
  check(chtholly_next_host_v1_net_close(nullptr) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE);

  void *mutex = nullptr;
  check(chtholly_next_host_v1_sync_mutex_init(&mutex) == 0);
  check(mutex != nullptr);
  check(chtholly_next_host_v1_sync_mutex_lock(mutex) == 0);
  std::atomic<bool> waiter_started = false;
  std::atomic<bool> waiter_acquired = false;
  std::thread waiter([&] {
    waiter_started.store(true);
    if (chtholly_next_host_v1_sync_mutex_lock(mutex) == 0) {
      waiter_acquired.store(true);
      (void)chtholly_next_host_v1_sync_mutex_unlock(mutex);
    }
  });
  while (!waiter_started.load()) {}
  check(!waiter_acquired.load());
  check(chtholly_next_host_v1_sync_mutex_unlock(mutex) == 0);
  waiter.join();
  check(waiter_acquired.load());
  check(chtholly_next_host_v1_sync_mutex_close(mutex) == 0);
  check(chtholly_next_host_v1_sync_mutex_close(nullptr) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE);

  void *guard_mutex = nullptr;
  check(chtholly_next_host_v1_sync_mutex_init(&guard_mutex) == 0);
  chtholly_next_sync_guard guard{};
  check(chtholly_next_host_v1_sync_guard_acquire(guard_mutex, &guard) == 0);
  check(chtholly_next_host_v1_sync_guard_release(&guard) == 0);
  check(chtholly_next_host_v1_sync_guard_release(&guard) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(chtholly_next_host_v1_sync_mutex_close(guard_mutex) == 0);

  void *condition_mutex = nullptr;
  void *condition = nullptr;
  check(chtholly_next_host_v1_sync_mutex_init(&condition_mutex) == 0);
  check(chtholly_next_host_v1_sync_condvar_init(&condition) == 0);
  check(chtholly_next_host_v1_sync_mutex_lock(condition_mutex) == 0);
  std::atomic<bool> condition_started = false;
  std::atomic<bool> condition_ready = false;
  std::atomic<int32_t> condition_status = -1;
  std::thread condition_waiter([&] {
    check(chtholly_next_host_v1_sync_mutex_lock(condition_mutex) == 0);
    condition_started.store(true);
    while (!condition_ready.load()) {
      const auto status = chtholly_next_host_v1_sync_condvar_wait(
          condition, condition_mutex);
      if (status != 0) {
        condition_status.store(status);
        (void)chtholly_next_host_v1_sync_mutex_unlock(condition_mutex);
        return;
      }
    }
    condition_status.store(0);
    check(chtholly_next_host_v1_sync_mutex_unlock(condition_mutex) == 0);
  });
  check(chtholly_next_host_v1_sync_mutex_unlock(condition_mutex) == 0);
  while (!condition_started.load()) {}
  check(chtholly_next_host_v1_sync_mutex_lock(condition_mutex) == 0);
  condition_ready.store(true);
  check(chtholly_next_host_v1_sync_condvar_notify_one(condition) == 0);
  check(chtholly_next_host_v1_sync_mutex_unlock(condition_mutex) == 0);
  condition_waiter.join();
  check(condition_status.load() == 0);
  check(chtholly_next_host_v1_sync_condvar_close(condition) == 0);
  check(chtholly_next_host_v1_sync_mutex_close(condition_mutex) == 0);

  void *channel = nullptr;
  check(chtholly_next_host_v1_channel_init(8, &channel) == 0);
  check(channel != nullptr);
  const std::uint8_t channel_payload[] = {'h', 'e', 'l', 'l', 'o'};
  std::thread producer([&] {
    check(chtholly_next_host_v1_channel_send(
              channel, channel_payload, sizeof(channel_payload)) == 0);
  });
  std::uint8_t channel_readback[8] = {};
  std::uint64_t channel_size = 0;
  check(chtholly_next_host_v1_channel_receive(
            channel, channel_readback, sizeof(channel_readback),
            &channel_size) == 0);
  producer.join();
  check(channel_size == sizeof(channel_payload));
  check(std::string(reinterpret_cast<char *>(channel_readback), channel_size) ==
        "hello");
  check(chtholly_next_host_v1_channel_close(channel) == 0);
  channel = nullptr;
  check(chtholly_next_host_v1_channel_send(channel, channel_payload,
                                           sizeof(channel_payload)) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(chtholly_next_host_v1_channel_close(nullptr) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE);

  void *backpressure_channel = nullptr;
  check(chtholly_next_host_v1_channel_init(4, &backpressure_channel) == 0);
  const std::uint8_t full_payload[] = {'1', '2', '3', '4'};
  const std::uint8_t one_payload[] = {'x'};
  check(chtholly_next_host_v1_channel_send(
            backpressure_channel, full_payload, sizeof(full_payload)) == 0);
  std::atomic<bool> blocked_sender_started = false;
  std::atomic<int32_t> blocked_sender_status = -1;
  std::thread blocked_sender([&] {
    blocked_sender_started.store(true);
    blocked_sender_status.store(chtholly_next_host_v1_channel_send(
        backpressure_channel, one_payload, sizeof(one_payload)));
  });
  while (!blocked_sender_started.load()) {}
  std::uint8_t drained[4] = {};
  std::uint64_t drained_size = 0;
  check(chtholly_next_host_v1_channel_receive(
            backpressure_channel, drained, sizeof(drained), &drained_size) == 0);
  blocked_sender.join();
  check(blocked_sender_status.load() == 0);
  check(chtholly_next_host_v1_channel_close(backpressure_channel) == 0);

  void *wake_channel = nullptr;
  check(chtholly_next_host_v1_channel_init(4, &wake_channel) == 0);
  std::atomic<bool> blocked_receiver_started = false;
  std::atomic<int32_t> blocked_receiver_status = -1;
  std::thread blocked_receiver([&] {
    blocked_receiver_started.store(true);
    std::uint8_t byte = 0;
    std::uint64_t size = 0;
    blocked_receiver_status.store(chtholly_next_host_v1_channel_receive(
        wake_channel, &byte, 1, &size));
  });
  while (!blocked_receiver_started.load()) {}
  check(chtholly_next_host_v1_channel_close(wake_channel) == 0);
  blocked_receiver.join();
  check(blocked_receiver_status.load() == CHTHOLLY_NEXT_HOST_STATUS_CLOSED);

  chtholly_next_typed_channel_descriptor typed_descriptor{
      CHTHOLLY_NEXT_TYPED_CHANNEL_ABI_V1,
      CHTHOLLY_NEXT_TYPED_CHANNEL_SEND | CHTHOLLY_NEXT_TYPED_CHANNEL_SYNC,
      sizeof(TypedPayload), alignof(TypedPayload), typed_move, typed_drop};
  auto invalid_typed_descriptor = typed_descriptor;
  invalid_typed_descriptor.capabilities = 0;
  void *invalid_typed_channel = nullptr;
  check(chtholly_next_host_v1_typed_channel_init(
            2, &invalid_typed_descriptor, &invalid_typed_channel) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(invalid_typed_channel == nullptr);
  auto invalid_typed_alignment = typed_descriptor;
  invalid_typed_alignment.alignment = 3;
  check(chtholly_next_host_v1_typed_channel_init(
            2, &invalid_typed_alignment, &invalid_typed_channel) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(invalid_typed_channel == nullptr);
  auto invalid_typed_size = typed_descriptor;
  invalid_typed_size.size = 0;
  check(chtholly_next_host_v1_typed_channel_init(
            2, &invalid_typed_size, &invalid_typed_channel) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(invalid_typed_channel == nullptr);
  void *typed_channel = nullptr;
  check(chtholly_next_host_v1_typed_channel_init(
            2, &typed_descriptor, &typed_channel) == 0);
  TypedPayload typed_first{17, 91};
  chtholly_next_typed_channel_token send_token{};
  check(chtholly_next_host_v1_typed_channel_send_prepare(
            typed_channel, &typed_first, &send_token) == 0);
  check(typed_first.value == 17 && typed_first.marker == 91);
  check(chtholly_next_host_v1_typed_channel_send_commit(&send_token) == 0);
  check(chtholly_next_host_v1_typed_channel_send_commit(&send_token) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(typed_first.value == 0 && typed_first.marker == 0);
  TypedPayload received{};
  chtholly_next_typed_channel_token receive_token{};
  check(chtholly_next_host_v1_typed_channel_receive_acquire(
            typed_channel, &receive_token) == 0);
  check(chtholly_next_host_v1_typed_channel_receive_commit(
            &receive_token, &received) == 0);
  check(chtholly_next_host_v1_typed_channel_receive_commit(
            &receive_token, &received) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(received.value == 17 && received.marker == 91);
  check(typed_drop_count.load() == 0);

  TypedPayload cancelled{23, 7};
  check(chtholly_next_host_v1_typed_channel_send_prepare(
            typed_channel, &cancelled, &send_token) == 0);
  check(chtholly_next_host_v1_typed_channel_send_cancel(&send_token) == 0);
  check(chtholly_next_host_v1_typed_channel_send_cancel(&send_token) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  check(cancelled.value == 23 && cancelled.marker == 7);
  TypedPayload queued{31, 8};
  check(chtholly_next_host_v1_typed_channel_send_prepare(
            typed_channel, &queued, &send_token) == 0);
  check(chtholly_next_host_v1_typed_channel_send_commit(&send_token) == 0);
  check(chtholly_next_host_v1_typed_channel_receive_acquire(
            typed_channel, &receive_token) == 0);
  check(chtholly_next_host_v1_typed_channel_receive_cancel(&receive_token) == 0);
  check(typed_drop_count.load() == 1);
  check(chtholly_next_host_v1_typed_channel_close(typed_channel) == 0);
  typed_channel = nullptr;
  check(chtholly_next_host_v1_typed_channel_send_prepare(
            typed_channel, &typed_first, &send_token) ==
        CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);

  void *blocked_typed_channel = nullptr;
  check(chtholly_next_host_v1_typed_channel_init(
            1, &typed_descriptor, &blocked_typed_channel) == 0);
  TypedPayload occupied{41, 9};
  check(chtholly_next_host_v1_typed_channel_send_prepare(
            blocked_typed_channel, &occupied, &send_token) == 0);
  check(chtholly_next_host_v1_typed_channel_send_commit(&send_token) == 0);
  TypedPayload waiting{42, 10};
  std::atomic<bool> typed_waiting_started = false;
  std::atomic<int32_t> typed_waiting_status = 0;
  std::thread typed_waiter([&] {
    typed_waiting_started.store(true);
    typed_waiting_status.store(
        chtholly_next_host_v1_typed_channel_send_prepare(
            blocked_typed_channel, &waiting, &send_token));
  });
  while (!typed_waiting_started.load()) {
  }
  while (chtholly_next_host_v1_typed_channel_test_active(blocked_typed_channel) == 0)
    std::this_thread::yield();
  check(chtholly_next_host_v1_typed_channel_close(blocked_typed_channel) == 0);
  typed_waiter.join();
  check(typed_waiting_status.load() == CHTHOLLY_NEXT_HOST_STATUS_CLOSED);
  check(waiting.value == 42 && waiting.marker == 10);
  check(typed_drop_count.load() == 2);

  void *failing_channel = nullptr;
  check(chtholly_next_host_v1_typed_channel_init(1, &typed_descriptor, &failing_channel) == 0);
  TypedPayload preserved{99, 18};
  check(chtholly_next_host_v1_typed_channel_send_prepare(failing_channel, &preserved, &send_token) == 0);
  auto copied_token = send_token;
  check(chtholly_next_host_v1_typed_channel_send_cancel(&copied_token) == CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT);
  chtholly_next_host_v1_typed_channel_test_fail_allocate();
  check(chtholly_next_host_v1_typed_channel_send_commit(&send_token) == CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY);
  check(preserved.value == 99 && preserved.marker == 18);
  check(typed_drop_count.load() == 2);
  check(chtholly_next_host_v1_typed_channel_close(failing_channel) == 0);

  void *empty_channel = nullptr;
  check(chtholly_next_host_v1_typed_channel_init(1, &typed_descriptor, &empty_channel) == 0);
  std::atomic<int32_t> receive_status{0};
  std::thread receiver([&] {
    chtholly_next_typed_channel_token pending{};
    receive_status = chtholly_next_host_v1_typed_channel_receive_acquire(empty_channel, &pending);
  });
  while (chtholly_next_host_v1_typed_channel_test_active(empty_channel) == 0)
    std::this_thread::yield();
  check(chtholly_next_host_v1_typed_channel_close(empty_channel) == 0);
  receiver.join();
  check(receive_status == CHTHOLLY_NEXT_HOST_STATUS_CLOSED);

  std::filesystem::remove(path);
  chtholly_next_runtime_v1_shutdown();
  return 0;
}
