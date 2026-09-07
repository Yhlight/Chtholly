#define CHTHOLLY_CFFI_PROVIDER_IMPL 1
#include "fixtures/cffi/fixture.h"

#include <errno.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

chtholly_cffi_status c_number_make(chtholly_cffi_number *value) {
  if (!value)
    return CHTHOLLY_CFFI_FAILED;
  value->integer = 40;
  return CHTHOLLY_CFFI_OK;
}

int32_t c_number_sum(chtholly_cffi_number value) {
  return (int32_t)value.integer + 2;
}

int32_t c_status_is_ok(chtholly_cffi_status value) {
  return value == CHTHOLLY_CFFI_OK ? 1 : 0;
}

int32_t c_set_callback(chtholly_cffi_callback callback) {
  if (callback)
    callback(7);
  return 0;
}

int32_t c_add_one(int32_t value) {
  return value + 1;
}

int32_t c_errno_probe(int32_t fail) {
  if (fail) {
    errno = EINVAL;
    return -1;
  }
  return 41;
}

int32_t c_code_probe(int32_t code) {
  return code;
}

chtholly_cffi_status c_status_code(chtholly_cffi_status code) {
  return code;
}

void *c_errno_pointer(int32_t fail) {
  static int value = 73;
  if (fail) {
    errno = EINVAL;
    return NULL;
  }
  return &value;
}

int32_t c_win32_probe(int32_t fail) {
#if defined(_WIN32)
  if (fail) {
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
  }
  SetLastError(ERROR_SUCCESS);
#else
  (void)fail;
#endif
  return 1;
}

void *c_fopen_missing(void) {
#if defined(_WIN32)
  errno = ENOENT;
  return NULL;
#else
  return fopen("chtholly-cffi-file-that-must-not-exist.tmp", "rb");
#endif
}

int32_t c_code_set_probe(int32_t code) {
  return code;
}

int32_t c_code_allowed_probe(int32_t code) {
  return code;
}

uint32_t c_unsigned_set_probe(uint32_t code) {
  return code;
}

chtholly_cffi_handle c_errno_sentinel(void) {
#if defined(_WIN32)
  errno = EINVAL;
  return (chtholly_cffi_handle)(intptr_t)-1;
#else
  return mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, -1, 0);
#endif
}

chtholly_cffi_handle c_win32_handle(int32_t fail) {
#if defined(_WIN32)
  const wchar_t *path =
      fail ? L"chtholly-cffi-file-that-must-not-exist.tmp" : L"NUL";
  return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
#else
  (void)fail;
  return (chtholly_cffi_handle)(intptr_t)-1;
#endif
}

int32_t c_win32_close_handle(chtholly_cffi_handle handle) {
#if defined(_WIN32)
  return CloseHandle((HANDLE)handle) ? 1 : 0;
#else
  (void)handle;
  return 1;
#endif
}

#if defined(_WIN32)
void *c_bcrypt_buffer(void) {
  static unsigned char buffer[16];
  return buffer;
}

NTSTATUS c_bcrypt_random(void *buffer, unsigned long length) {
  return BCryptGenRandom(NULL, (PUCHAR)buffer, (ULONG)length,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

void *c_bcrypt_property_buffer(void) {
  static unsigned char buffer[128];
  return buffer;
}

NTSTATUS c_bcrypt_get_property(void *buffer, unsigned long capacity,
                               unsigned long *result) {
  BCRYPT_ALG_HANDLE algorithm = NULL;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_RNG_ALGORITHM, NULL, 0);
  if (status < 0)
    return status;
  status = BCryptGetProperty(algorithm, BCRYPT_ALGORITHM_NAME,
                             (PUCHAR)buffer, (ULONG)capacity, (ULONG *)result,
                             0);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status;
}

NTSTATUS c_bcrypt_get_property_invalid(void *buffer, unsigned long capacity,
                                        unsigned long *result) {
  BCRYPT_ALG_HANDLE algorithm = NULL;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_RNG_ALGORITHM, NULL, 0);
  if (status < 0)
    return status;
  status = BCryptGetProperty(algorithm, L"ChthollyInvalidProperty",
                             (PUCHAR)buffer, (ULONG)capacity, (ULONG *)result,
                             0);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status;
}

NTSTATUS c_bcrypt_get_property_small(void *buffer, unsigned long capacity,
                                      unsigned long *result) {
  BCRYPT_ALG_HANDLE algorithm = NULL;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_RNG_ALGORITHM, NULL, 0);
  if (status < 0)
    return status;
  status = BCryptGetProperty(algorithm, BCRYPT_ALGORITHM_NAME,
                             (PUCHAR)buffer, (ULONG)capacity, (ULONG *)result,
                             0);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status;
}
#endif

#if defined(_WIN32)
static HANDLE c_win32_read_pipe(const char *payload, DWORD size) {
  HANDLE read_handle = INVALID_HANDLE_VALUE;
  HANDLE write_handle = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&read_handle, &write_handle, NULL, 0))
    return INVALID_HANDLE_VALUE;
  if (size != 0) {
    DWORD written = 0;
    if (!WriteFile(write_handle, payload, size, &written, NULL) ||
        written != size) {
      CloseHandle(read_handle);
      CloseHandle(write_handle);
      return INVALID_HANDLE_VALUE;
    }
  }
  CloseHandle(write_handle);
  return read_handle;
}

HANDLE c_win32_read_data_handle(void) {
  return c_win32_read_pipe("123", 3);
}

HANDLE c_win32_read_eof_handle(void) {
  return CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

HANDLE c_win32_read_invalid_handle(void) {
  return INVALID_HANDLE_VALUE;
}

HANDLE c_win32_read_probe_handle(uint32_t mode) {
  return (HANDLE)(uintptr_t)mode;
}

void *c_win32_read_buffer(void) {
  static unsigned char buffer[8];
  return buffer;
}

int32_t c_win32_read_close(HANDLE handle) {
  return CloseHandle(handle) ? 1 : 0;
}

BOOL c_win32_read_contract_probe(HANDLE handle, void *buffer, DWORD capacity,
                                 DWORD *count, void *overlapped) {
  const uintptr_t mode = (uintptr_t)handle;
  (void)buffer;
  (void)overlapped;
  if (mode == 1) {
    *count = capacity + 1;
    return TRUE;
  }
  if (mode == 2) {
    *count = 1;
    return TRUE;
  }
  SetLastError(ERROR_BAD_COMMAND);
  return FALSE;
}
#endif

int32_t c_posix_read_probe(int32_t mode) {
#if defined(_WIN32)
  if (mode == 2) {
    errno = EBADF;
    return -1;
  }
  return mode == 0 ? 0 : mode == 1 ? 3 : 8;
#else
  char buffer[8];
  if (mode == 2)
    return (int32_t)read(-1, buffer, sizeof(buffer));
  int descriptors[2];
  if (pipe(descriptors) != 0)
    return -1;
  if (mode != 0) {
    const char payload[] = "12345678";
    const size_t size = mode == 1 ? 3 : sizeof(payload) - 1;
    if (write(descriptors[1], payload, size) != (ssize_t)size) {
      close(descriptors[0]);
      close(descriptors[1]);
      return -1;
    }
  }
  close(descriptors[1]);
  const ssize_t result = read(descriptors[0], buffer, sizeof(buffer));
  close(descriptors[0]);
  return (int32_t)result;
#endif
}

void *c_posix_buffer(void) {
  static unsigned char buffer[8];
  return buffer;
}

int64_t c_posix_read_into(void *buffer, uint64_t capacity, int32_t mode) {
  if (mode == 4)
    return (int64_t)capacity + 1;
  if (mode == 5)
    return -2;
  if (mode == 6)
    return 1;
#if defined(_WIN32)
  if (mode == 2) {
    errno = EBADF;
    return -1;
  }
  if (mode == 0 || capacity == 0)
    return 0;
  const unsigned char payload[] = "12345678";
  const uint64_t count = mode == 1 && capacity > 3 ? 3 : capacity;
  for (uint64_t index = 0; index < count; ++index)
    ((unsigned char *)buffer)[index] = payload[index];
  return (int64_t)count;
#else
  if (mode == 2)
    return (int64_t)read(-1, buffer, (size_t)capacity);
  int descriptors[2];
  if (pipe(descriptors) != 0)
    return -1;
  if (mode != 0 && capacity != 0) {
    const char payload[] = "12345678";
    const size_t requested = (size_t)capacity;
    const size_t size = mode == 1 && requested > 3 ? 3 : requested;
    if (write(descriptors[1], payload, size) != (ssize_t)size) {
      close(descriptors[0]);
      close(descriptors[1]);
      return -1;
    }
  }
  close(descriptors[1]);
  const ssize_t result = read(descriptors[0], buffer, (size_t)capacity);
  close(descriptors[0]);
  return (int64_t)result;
#endif
}

#if !defined(_WIN32)
static int c_posix_recv_peer = -1;

static int c_posix_recv_socket(const char *payload, size_t size) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    return -1;
  if (size != 0 && send(sockets[1], payload, size, 0) != (ssize_t)size) {
    close(sockets[0]);
    close(sockets[1]);
    return -1;
  }
  shutdown(sockets[1], SHUT_WR);
  c_posix_recv_peer = sockets[1];
  return sockets[0];
}

int c_posix_recv_data_socket(void) {
  return c_posix_recv_socket("123", 3);
}

int c_posix_recv_eof_socket(void) {
  return c_posix_recv_socket(NULL, 0);
}

int c_posix_recv_invalid_socket(void) {
  return -1;
}

void *c_posix_recv_buffer(void) {
  static unsigned char buffer[8];
  return buffer;
}

int c_posix_recv_close(int socket_fd) {
  const int result = close(socket_fd) == 0 ? 1 : 0;
  if (c_posix_recv_peer >= 0) {
    close(c_posix_recv_peer);
    c_posix_recv_peer = -1;
  }
  return result;
}

static FILE *c_fread_temp(const char *payload) {
  FILE *stream = tmpfile();
  if (!stream)
    return NULL;
  if (payload != NULL) {
    fputs(payload, stream);
    fflush(stream);
  }
  rewind(stream);
  return stream;
}

FILE *c_fread_data_stream(void) {
  return c_fread_temp("12345678");
}

FILE *c_fread_eof_stream(void) {
  return c_fread_temp("");
}

FILE *c_fread_error_stream(void) {
  return fopen("/dev/null", "w");
}

void *c_fread_buffer(void) {
  static unsigned char buffer[16];
  return buffer;
}

int c_fread_close(FILE *stream) {
  return stream != NULL && fclose(stream) == 0 ? 1 : 0;
}
#endif
