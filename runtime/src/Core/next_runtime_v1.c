#include "chtholly/next_runtime_v1.h"
#include "next_path.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#endif

#define CHTHOLLY_NEXT_RUNTIME_NANOSECONDS_PER_SECOND 1000000000u

static uint8_t **next_runtime_args;
static uint64_t *next_runtime_arg_sizes;
static uint64_t next_runtime_arg_count;

#if defined(CHTHOLLY_RUNTIME_TESTING)
static uint64_t next_runtime_allocation_limit = UINT64_MAX;
static uint64_t next_runtime_allocation_count;
static uint64_t next_runtime_lifecycle_construct_count;
static uint64_t next_runtime_lifecycle_drop_count;
static uint64_t next_runtime_lifecycle_expected_drops = UINT64_MAX;
#endif

static int next_runtime_valid_alignment(uint64_t alignment) {
  return alignment != 0 && (alignment & (alignment - 1u)) == 0 &&
         alignment <= (uint64_t)SIZE_MAX &&
         alignment >= (uint64_t)sizeof(void *);
}

static void next_runtime_clear_args(void) {
  uint64_t index;
  for (index = 0; index < next_runtime_arg_count; ++index)
    free(next_runtime_args[index]);
  free(next_runtime_args);
  free(next_runtime_arg_sizes);
  next_runtime_args = NULL;
  next_runtime_arg_sizes = NULL;
  next_runtime_arg_count = 0;
}

static void next_runtime_free_pending(uint8_t **args, uint64_t count,
                                      uint64_t *sizes) {
  uint64_t index;
  for (index = 0; index < count; ++index)
    free(args[index]);
  free(args);
  free(sizes);
}

static int32_t next_runtime_commit_args(uint8_t **args, uint64_t *sizes,
                                        uint64_t count) {
  next_runtime_clear_args();
  next_runtime_args = args;
  next_runtime_arg_sizes = sizes;
  next_runtime_arg_count = count;
  return 0;
}

static int next_runtime_append_utf8(uint32_t scalar, uint8_t *output,
                                    uint64_t *size) {
  if (scalar <= 0x7fu) {
    output[(*size)++] = (uint8_t)scalar;
  } else if (scalar <= 0x7ffu) {
    output[(*size)++] = (uint8_t)(0xc0u | (scalar >> 6));
    output[(*size)++] = (uint8_t)(0x80u | (scalar & 0x3fu));
  } else if (scalar <= 0xffffu) {
    output[(*size)++] = (uint8_t)(0xe0u | (scalar >> 12));
    output[(*size)++] = (uint8_t)(0x80u | ((scalar >> 6) & 0x3fu));
    output[(*size)++] = (uint8_t)(0x80u | (scalar & 0x3fu));
  } else if (scalar <= 0x10ffffu) {
    output[(*size)++] = (uint8_t)(0xf0u | (scalar >> 18));
    output[(*size)++] = (uint8_t)(0x80u | ((scalar >> 12) & 0x3fu));
    output[(*size)++] = (uint8_t)(0x80u | ((scalar >> 6) & 0x3fu));
    output[(*size)++] = (uint8_t)(0x80u | (scalar & 0x3fu));
  } else {
    return 0;
  }
  return 1;
}

static int next_runtime_utf16_to_utf8(const uint16_t *source,
                                      uint8_t **out_data,
                                      uint64_t *out_size) {
  uint64_t units = 0;
  uint64_t index = 0;
  uint64_t size = 0;
  uint8_t *result;
  while (source[units] != 0)
    ++units;
  if (units > (UINT64_MAX - 1u) / 3u || units > (uint64_t)SIZE_MAX / 3u)
    return 0;
  result = (uint8_t *)malloc((size_t)(units * 3u + 1u));
  if (result == NULL)
    return 0;
  while (index < units) {
    uint32_t scalar = source[index++];
    if (scalar >= 0xd800u && scalar <= 0xdbffu) {
      uint32_t low;
      if (index >= units) {
        free(result);
        return 0;
      }
      low = source[index++];
      if (low < 0xdc00u || low > 0xdfffu) {
        free(result);
        return 0;
      }
      scalar = 0x10000u + ((scalar - 0xd800u) << 10) + (low - 0xdc00u);
    } else if (scalar >= 0xdc00u && scalar <= 0xdfffu) {
      free(result);
      return 0;
    }
    if (!next_runtime_append_utf8(scalar, result, &size)) {
      free(result);
      return 0;
    }
  }
  result[size] = 0;
  *out_data = result;
  *out_size = size;
  return 1;
}

void chtholly_next_runtime_v1_init(void) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
}

void chtholly_next_runtime_v1_shutdown(void) {
  (void)fflush(stdout);
  (void)fflush(stderr);
  next_runtime_clear_args();
#if defined(CHTHOLLY_RUNTIME_TESTING)
  if (next_runtime_lifecycle_expected_drops != UINT64_MAX &&
      next_runtime_lifecycle_drop_count !=
          next_runtime_lifecycle_expected_drops)
    abort();
#endif
}

int32_t chtholly_next_runtime_v1_set_process_args_utf8(
    int32_t argc, const uint8_t *const *argv) {
  uint8_t **args;
  uint64_t *sizes;
  uint64_t index;
  if (argc < 0 || (argc != 0 && argv == NULL))
    return -1;
  if (argc == 0) {
    next_runtime_clear_args();
    return 0;
  }
  args = (uint8_t **)calloc((size_t)argc, sizeof(*args));
  sizes = (uint64_t *)calloc((size_t)argc, sizeof(*sizes));
  if (args == NULL || sizes == NULL) {
    free(args);
    free(sizes);
    return -1;
  }
  for (index = 0; index < (uint64_t)argc; ++index) {
    size_t size;
    if (argv[index] == NULL) {
      next_runtime_free_pending(args, index, sizes);
      return -1;
    }
    size = strlen((const char *)argv[index]);
    args[index] = (uint8_t *)malloc(size + 1u);
    if (args[index] == NULL) {
      next_runtime_free_pending(args, index, sizes);
      return -1;
    }
    memcpy(args[index], argv[index], size + 1u);
    sizes[index] = (uint64_t)size;
  }
  return next_runtime_commit_args(args, sizes, (uint64_t)argc);
}

int32_t chtholly_next_runtime_v1_set_process_args_utf16(
    int32_t argc, const uint16_t *const *argv) {
  uint8_t **args;
  uint64_t *sizes;
  uint64_t index;
  if (argc < 0 || (argc != 0 && argv == NULL))
    return -1;
  if (argc == 0) {
    next_runtime_clear_args();
    return 0;
  }
  args = (uint8_t **)calloc((size_t)argc, sizeof(*args));
  sizes = (uint64_t *)calloc((size_t)argc, sizeof(*sizes));
  if (args == NULL || sizes == NULL) {
    free(args);
    free(sizes);
    return -1;
  }
  for (index = 0; index < (uint64_t)argc; ++index) {
    if (argv[index] == NULL ||
        !next_runtime_utf16_to_utf8(argv[index], &args[index], &sizes[index])) {
      next_runtime_free_pending(args, index, sizes);
      return -1;
    }
  }
  return next_runtime_commit_args(args, sizes, (uint64_t)argc);
}

uint64_t chtholly_next_runtime_v1_arg_count(void) {
  return next_runtime_arg_count;
}

const uint8_t *chtholly_next_runtime_v1_arg_data(uint64_t index) {
  return index < next_runtime_arg_count ? next_runtime_args[index] : NULL;
}

uint64_t chtholly_next_runtime_v1_arg_size(uint64_t index) {
  return index < next_runtime_arg_count ? next_runtime_arg_sizes[index] : 0;
}

int64_t chtholly_next_runtime_v1_console_write(uint32_t stream,
                                               const uint8_t *data,
                                               uint64_t size) {
  FILE *output;
  size_t written;
  if ((stream != 1u && stream != 2u) || (data == NULL && size != 0u) ||
      size > (uint64_t)SIZE_MAX)
    return -1;
  output = stream == 1u ? stdout : stderr;
  if (size == 0u)
    return fflush(output) == 0 ? 0 : -1;
  written = fwrite(data, 1, (size_t)size, output);
  if (written != (size_t)size || fflush(output) != 0)
    return -1;
  return (int64_t)written;
}

int32_t chtholly_next_runtime_v1_fs_exists(const uint8_t *path,
                                           uint64_t path_size) {
  NextRuntimePathChar *resolved = NULL;
  if (next_runtime_resolve_path(path, path_size, &resolved) != 0)
    return 0;
#if defined(_WIN32)
  int result;
  result = _waccess(resolved, 0) == 0;
#else
  int result;
  result = access(resolved, F_OK) == 0;
#endif
  free(resolved);
  return result;
}

int64_t chtholly_next_runtime_v1_fs_write(const uint8_t *path,
                                          uint64_t path_size,
                                          const uint8_t *data,
                                          uint64_t data_size) {
  FILE *file;
  size_t written;
  int flush_result;
  int close_result;
  NextRuntimePathChar *resolved = NULL;
  if ((data == NULL && data_size != 0) || data_size > (uint64_t)SIZE_MAX)
    return -1;
#if defined(_WIN32)
  if (next_runtime_resolve_path(path, path_size, &resolved) != 0)
    return -1;
  file = NULL;
  (void)_wfopen_s(&file, resolved, L"wb");
#else
  if (next_runtime_resolve_path(path, path_size, &resolved) != 0)
    return -1;
  file = fopen(resolved, "wb");
#endif
  free(resolved);
  if (file == NULL)
    return -1;
  written = fwrite(data, 1, (size_t)data_size, file);
  flush_result = fflush(file);
  close_result = fclose(file);
  if (written != (size_t)data_size || flush_result != 0 || close_result != 0)
    return -1;
  return (int64_t)written;
}

int32_t chtholly_next_runtime_v1_fs_remove(const uint8_t *path,
                                           uint64_t path_size) {
  NextRuntimePathChar *resolved = NULL;
  int32_t resolve_status = next_runtime_resolve_path(path, path_size, &resolved);
  if (resolve_status != 0)
    return -1;
#if defined(_WIN32)
  int result;
  result = _wremove(resolved);
#else
  int result;
  result = remove(resolved);
#endif
  free(resolved);
  return result == 0 ? 0 : -1;
}

void *chtholly_next_runtime_v1_allocate(uint64_t size, uint64_t alignment) {
  size_t allocation_size;
  void *result = NULL;
  if (!next_runtime_valid_alignment(alignment) || size > (uint64_t)SIZE_MAX)
    return NULL;
  allocation_size = (size_t)(size == 0 ? 1u : size);
#if defined(CHTHOLLY_RUNTIME_TESTING)
  if (allocation_size > next_runtime_allocation_limit)
    return NULL;
#endif
#if defined(_WIN32)
  result = _aligned_malloc(allocation_size, (size_t)alignment);
#else
  if (posix_memalign(&result, (size_t)alignment, allocation_size) != 0)
    result = NULL;
#endif
#if defined(CHTHOLLY_RUNTIME_TESTING)
  if (result != NULL) {
    ++next_runtime_allocation_count;
    next_runtime_allocation_limit -= allocation_size;
  }
#endif
  return result;
}

void chtholly_next_runtime_v1_deallocate(void *ptr, uint64_t size,
                                         uint64_t alignment) {
  (void)size;
  (void)alignment;
  if (ptr == NULL)
    return;
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

#if defined(CHTHOLLY_RUNTIME_TESTING)
void chtholly_next_runtime_v1_testing_set_allocation_limit(uint64_t bytes) {
  next_runtime_allocation_limit = bytes;
}

uint64_t chtholly_next_runtime_v1_testing_allocation_count(void) {
  return next_runtime_allocation_count;
}

void chtholly_next_runtime_v1_testing_lifecycle_reset(void) {
  next_runtime_lifecycle_construct_count = 0;
  next_runtime_lifecycle_drop_count = 0;
  next_runtime_lifecycle_expected_drops = UINT64_MAX;
}

void chtholly_next_runtime_v1_testing_lifecycle_construct(void) {
  ++next_runtime_lifecycle_construct_count;
}

void chtholly_next_runtime_v1_testing_lifecycle_drop(void) {
  ++next_runtime_lifecycle_drop_count;
}

void chtholly_next_runtime_v1_testing_lifecycle_expect_drops(uint64_t count) {
  next_runtime_lifecycle_expected_drops = count;
}

uint64_t chtholly_next_runtime_v1_testing_lifecycle_construct_count(void) {
  return next_runtime_lifecycle_construct_count;
}

uint64_t chtholly_next_runtime_v1_testing_lifecycle_drop_count(void) {
  return next_runtime_lifecycle_drop_count;
}
#endif

int32_t chtholly_next_runtime_v1_monotonic_now(uint64_t *out_seconds,
                                               uint32_t *out_nanoseconds) {
  if (out_seconds == NULL || out_nanoseconds == NULL)
    return -1;
#if defined(_WIN32)
  {
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t ticks;
    uint64_t ticks_per_second;
    uint64_t remainder;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
      return -1;
    ticks = (uint64_t)counter.QuadPart;
    ticks_per_second = (uint64_t)frequency.QuadPart;
    remainder = ticks % ticks_per_second;
    *out_seconds = ticks / ticks_per_second;
    *out_nanoseconds = (uint32_t)(
        (long double)remainder *
        (long double)CHTHOLLY_NEXT_RUNTIME_NANOSECONDS_PER_SECOND /
        (long double)ticks_per_second);
    return 0;
  }
#else
  {
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0 || current.tv_sec < 0 ||
        current.tv_nsec < 0 ||
        current.tv_nsec >=
            (long)CHTHOLLY_NEXT_RUNTIME_NANOSECONDS_PER_SECOND)
      return -1;
    *out_seconds = (uint64_t)current.tv_sec;
    *out_nanoseconds = (uint32_t)current.tv_nsec;
    return 0;
  }
#endif
}

void chtholly_next_runtime_v1_trap_arithmetic(uint32_t reason) {
  (void)reason;
  abort();
}

void chtholly_next_runtime_v1_trap_failure(uint32_t reason) {
  (void)reason;
  abort();
}

void chtholly_next_runtime_v1_trap_coroutine(uint32_t reason) {
  (void)reason;
  abort();
}

void chtholly_next_runtime_v1_drain_thread_static_drops(void) {}

void chtholly_next_runtime_v1_drain_program_static_drops(void) {}
