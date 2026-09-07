#include "chtholly/component_loader_v2.h"
#include <atomic>
#include "test_check.h"
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
  CHTHOLLY_TEST_CHECK(argc >= 2);
  int minimum_cycles = 4, minimum_seconds = 0;
  unsigned seed = 1;
  for (int index = 2; index + 1 < argc; index += 2) {
    const std::string option = argv[index];
    if (option == "--cycles") minimum_cycles = std::stoi(argv[index + 1]);
    else if (option == "--seconds") minimum_seconds = std::stoi(argv[index + 1]);
    else if (option == "--seed") seed = static_cast<unsigned>(std::stoul(argv[index + 1]));
    else CHTHOLLY_TEST_CHECK(false);
  }
  CHTHOLLY_TEST_CHECK(minimum_cycles > 0 && minimum_seconds >= 0);
  const auto started = std::chrono::steady_clock::now();
  for (int cycle = 0; cycle < minimum_cycles ||
       std::chrono::steady_clock::now() - started < std::chrono::seconds(minimum_seconds); ++cycle) {
    std::fprintf(stderr, "{\"event\":\"load\",\"cycle\":%d,\"seed\":%u}\n", cycle, seed);
    chtholly_component_module_v2 *module = nullptr;
    char diagnostic[256]{}; uint64_t diagnostic_size = 0;
    CHTHOLLY_TEST_CHECK(chtholly_component_load_v2(argv[1], &module, diagnostic,
                                      sizeof(diagnostic), &diagnostic_size) ==
           CHTHOLLY_COMPONENT_LOADER_V2_OK);
    std::atomic<bool> stop{false};
    std::atomic<unsigned> calls{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&, i] {
      unsigned schedule = seed ^ static_cast<unsigned>(i + 1);
      while (!stop.load()) {
        schedule = schedule * 1664525U + 1013904223U;
        if ((schedule & 7U) == 0) std::this_thread::yield();
        chtholly_next_resource_operation_v2 *operation = nullptr;
        const auto status = chtholly_component_invoke_v2(
            module, 1, &operation, nullptr, 0, nullptr);
        if (status == CHTHOLLY_COMPONENT_LOADER_V2_OK && operation) {
          CHTHOLLY_TEST_CHECK(chtholly_next_resource_operation_v2_destroy(operation) ==
                 CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
          ++calls;
        } else if (status != CHTHOLLY_COMPONENT_LOADER_V2_CLOSING) {
          std::this_thread::yield();
        }
      }
    });
    // Establish a non-vacuous concurrent invocation phase before asking the
    // closer to transition the module. Without this handoff, a fast closer
    // can legitimately set `closing` before any worker acquires the mutex,
    // making the evidence depend on scheduler luck.
    for (int attempt = 0; attempt < 1000 && calls.load() == 0; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHTHOLLY_TEST_CHECK(calls.load() > 0);
    std::atomic<uint32_t> close_status{UINT32_MAX};
    std::thread closer([&] {
      // ABI-2 close is retryable while worker capabilities are armed. Keep
      // retrying until the lease reaches quiescence rather than turning the
      // expected NOT_READY response into a flaky assertion failure.
      for (;;) {
        const auto status =
            chtholly_component_close_v2(module, nullptr, 0, nullptr);
        if (status == CHTHOLLY_COMPONENT_LOADER_V2_OK) {
          close_status = status;
          break;
        }
        CHTHOLLY_TEST_CHECK(status == CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY ||
               status == CHTHOLLY_COMPONENT_LOADER_V2_CLOSING);
        std::this_thread::yield();
      }
    });
    closer.join();
    std::fprintf(stderr, "{\"event\":\"close\",\"cycle\":%d,\"status\":%u}\n", cycle, close_status.load());
    stop = true;
    for (auto &worker : workers) worker.join();
    std::fprintf(stderr, "{\"event\":\"joined\",\"cycle\":%d,\"calls\":%u,\"workers\":4}\n", cycle, calls.load());
    CHTHOLLY_TEST_CHECK(close_status == CHTHOLLY_COMPONENT_LOADER_V2_OK);
    CHTHOLLY_TEST_CHECK(calls.load() > 0);
    CHTHOLLY_TEST_CHECK(chtholly_component_unload_v2(module, nullptr, 0, nullptr) ==
           CHTHOLLY_COMPONENT_LOADER_V2_OK);
    std::fprintf(stderr, "{\"event\":\"unloaded\",\"cycle\":%d}\n", cycle);
  }
  return 0;
}
