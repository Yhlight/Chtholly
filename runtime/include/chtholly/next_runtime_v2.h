#ifndef CHTHOLLY_NEXT_RUNTIME_V2_H
#define CHTHOLLY_NEXT_RUNTIME_V2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_RUNTIME_ABI_V2 2u
#define CHTHOLLY_NEXT_RUNTIME_V2_OK 0
#define CHTHOLLY_NEXT_RUNTIME_V2_INVALID_ARGUMENT (-2801)
#define CHTHOLLY_NEXT_RUNTIME_V2_OUT_OF_MEMORY (-2803)

// Fallible allocation boundary used by transactional containers. On failure
// `*out` is always set to NULL and the caller retains ownership of its old
// storage.
int32_t chtholly_next_runtime_v2_allocate(uint64_t size, uint64_t alignment,
                                           void **out);
void chtholly_next_runtime_v2_deallocate(void *ptr, uint64_t size,
                                         uint64_t alignment);

#ifdef __cplusplus
}
#endif

#endif
