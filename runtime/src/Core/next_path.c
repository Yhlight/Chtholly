#include "next_path.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

int32_t next_runtime_resolve_path(const uint8_t *path, uint64_t size,
                                NextRuntimePathChar **out_path) {
  NextRuntimePathChar *result;
  if (out_path == NULL)
    return NEXT_RUNTIME_PATH_INVALID_ARGUMENT;
  *out_path = NULL;
  if (path == NULL || size == 0 || size > (uint64_t)SIZE_MAX - 1u)
    return NEXT_RUNTIME_PATH_INVALID_ARGUMENT;
#if defined(_WIN32)
  if (size > (uint64_t)INT_MAX)
    return NEXT_RUNTIME_PATH_INVALID_ARGUMENT;
#endif
  if (memchr(path, 0, (size_t)size) != NULL)
    return NEXT_RUNTIME_PATH_INVALID_ARGUMENT;
#if defined(_WIN32)
  int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 (const char *)path, (int)size, NULL, 0);
  if (units <= 0 || (size_t)units > SIZE_MAX / sizeof(*result) - 1u)
    return NEXT_RUNTIME_PATH_INVALID_ARGUMENT;
  result = (NextRuntimePathChar *)malloc(((size_t)units + 1u) * sizeof(*result));
  if (result == NULL)
    return NEXT_RUNTIME_PATH_OUT_OF_MEMORY;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        (const char *)path, (int)size, result, units) != units) {
    free(result);
    return NEXT_RUNTIME_PATH_INVALID_ARGUMENT;
  }
  result[units] = L'\0';
#else
  result = (NextRuntimePathChar *)malloc((size_t)size + 1u);
  if (result == NULL)
    return NEXT_RUNTIME_PATH_OUT_OF_MEMORY;
  memcpy(result, path, (size_t)size);
  result[size] = '\0';
#endif
  *out_path = result;
  return 0;
}
