#include "chtholly/next_runtime_v2.h"

#include <stddef.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <malloc.h>
#endif

static int v2_valid_alignment(uint64_t alignment) {
  return alignment != 0 && (alignment & (alignment - 1u)) == 0 &&
         alignment >= sizeof(void *);
}

int32_t chtholly_next_runtime_v2_allocate(uint64_t size, uint64_t alignment,
                                           void **out) {
  if (out == NULL || !v2_valid_alignment(alignment) ||
      size > (uint64_t)SIZE_MAX) {
    if (out != NULL)
      *out = NULL;
    return CHTHOLLY_NEXT_RUNTIME_V2_INVALID_ARGUMENT;
  }
  *out = NULL;
#if defined(_WIN32)
  *out = _aligned_malloc((size_t)(size == 0 ? 1u : size),
                         (size_t)alignment);
#else
  if (posix_memalign(out, (size_t)alignment,
                     (size_t)(size == 0 ? 1u : size)) != 0)
    *out = NULL;
#endif
  return *out == NULL ? CHTHOLLY_NEXT_RUNTIME_V2_OUT_OF_MEMORY
                      : CHTHOLLY_NEXT_RUNTIME_V2_OK;
}

void chtholly_next_runtime_v2_deallocate(void *ptr, uint64_t size,
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
