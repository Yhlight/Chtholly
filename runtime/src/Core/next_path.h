#ifndef CHTHOLLY_RUNTIME_INTERNAL_PATH_H
#define CHTHOLLY_RUNTIME_INTERNAL_PATH_H

#include <stddef.h>
#include <stdint.h>

#define NEXT_RUNTIME_PATH_INVALID_ARGUMENT (-2701)
#define NEXT_RUNTIME_PATH_OUT_OF_MEMORY (-2703)

#if defined(_WIN32)
typedef wchar_t NextRuntimePathChar;
#else
typedef char NextRuntimePathChar;
#endif

/* Resolve a length-delimited path once at the native boundary. The caller
   owns the returned allocation and frees it with free(). Errors leave it null.
   Windows uses strict UTF-8 conversion; POSIX preserves non-NUL path bytes. */
int32_t next_runtime_resolve_path(const uint8_t *path, uint64_t size,
                                NextRuntimePathChar **out_path);

#endif
