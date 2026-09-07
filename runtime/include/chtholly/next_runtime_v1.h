#ifndef CHTHOLLY_NEXT_RUNTIME_V1_H
#define CHTHOLLY_NEXT_RUNTIME_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void chtholly_next_runtime_v1_init(void);
void chtholly_next_runtime_v1_shutdown(void);
int32_t chtholly_next_runtime_v1_set_process_args_utf8(
    int32_t argc, const uint8_t *const *argv);
int32_t chtholly_next_runtime_v1_set_process_args_utf16(
    int32_t argc, const uint16_t *const *argv);
uint64_t chtholly_next_runtime_v1_arg_count(void);
const uint8_t *chtholly_next_runtime_v1_arg_data(uint64_t index);
uint64_t chtholly_next_runtime_v1_arg_size(uint64_t index);
int64_t chtholly_next_runtime_v1_console_write(uint32_t stream,
                                               const uint8_t *data,
                                               uint64_t size);
int32_t chtholly_next_runtime_v1_fs_exists(const uint8_t *path,
                                           uint64_t path_size);
int64_t chtholly_next_runtime_v1_fs_write(const uint8_t *path,
                                          uint64_t path_size,
                                          const uint8_t *data,
                                          uint64_t data_size);
int32_t chtholly_next_runtime_v1_fs_remove(const uint8_t *path,
                                           uint64_t path_size);
void *chtholly_next_runtime_v1_allocate(uint64_t size, uint64_t alignment);
void chtholly_next_runtime_v1_deallocate(void *ptr, uint64_t size,
                                         uint64_t alignment);

#if defined(CHTHOLLY_RUNTIME_TESTING)
void chtholly_next_runtime_v1_testing_set_allocation_limit(uint64_t bytes);
uint64_t chtholly_next_runtime_v1_testing_allocation_count(void);
void chtholly_next_runtime_v1_testing_lifecycle_reset(void);
void chtholly_next_runtime_v1_testing_lifecycle_construct(void);
void chtholly_next_runtime_v1_testing_lifecycle_drop(void);
void chtholly_next_runtime_v1_testing_lifecycle_expect_drops(uint64_t count);
uint64_t chtholly_next_runtime_v1_testing_lifecycle_construct_count(void);
uint64_t chtholly_next_runtime_v1_testing_lifecycle_drop_count(void);
#endif

int32_t chtholly_next_runtime_v1_monotonic_now(uint64_t *out_seconds,
                                               uint32_t *out_nanoseconds);
void chtholly_next_runtime_v1_trap_arithmetic(uint32_t reason);
void chtholly_next_runtime_v1_trap_failure(uint32_t reason);
void chtholly_next_runtime_v1_trap_coroutine(uint32_t reason);
void chtholly_next_runtime_v1_drain_thread_static_drops(void);
void chtholly_next_runtime_v1_drain_program_static_drops(void);

#ifdef __cplusplus
}
#endif

#endif
