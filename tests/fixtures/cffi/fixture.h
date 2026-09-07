#ifndef CHTHOLLY_CFFI_FIXTURE_H
#define CHTHOLLY_CFFI_FIXTURE_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(CHTHOLLY_CFFI_PROVIDER_IMPL)
#include <windows.h>
#include <bcrypt.h>
#else
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef DWORD *LPDWORD;
typedef struct _OVERLAPPED *LPOVERLAPPED;
typedef int BOOL;
typedef long NTSTATUS;
BOOL __stdcall ReadFile(HANDLE hFile, void *lpBuffer,
                        DWORD nNumberOfBytesToRead, DWORD *lpNumberOfBytesRead,
                        void *lpOverlapped);
#endif
NTSTATUS c_bcrypt_random(void *buffer, unsigned long length);
void *c_bcrypt_buffer(void);
NTSTATUS c_bcrypt_get_property(void *buffer, unsigned long capacity,
                               unsigned long *result);
NTSTATUS c_bcrypt_get_property_invalid(void *buffer, unsigned long capacity,
                                        unsigned long *result);
NTSTATUS c_bcrypt_get_property_small(void *buffer, unsigned long capacity,
                                      unsigned long *result);
void *c_bcrypt_property_buffer(void);
#endif

#if !defined(_WIN32)
#if defined(CHTHOLLY_CFFI_PROVIDER_IMPL)
#include <stdio.h>
#define CHTHOLLY_CFFI_STREAM FILE *
#else
#define CHTHOLLY_CFFI_STREAM void *
#endif
typedef long chtholly_cffi_ssize_t;
typedef unsigned long chtholly_cffi_size_t;
chtholly_cffi_ssize_t recv(int sockfd, void *buffer,
                           chtholly_cffi_size_t capacity, int flags);
int c_posix_recv_data_socket(void);
int c_posix_recv_eof_socket(void);
int c_posix_recv_invalid_socket(void);
void *c_posix_recv_buffer(void);
int c_posix_recv_close(int socket_fd);
chtholly_cffi_size_t fread(void *buffer, chtholly_cffi_size_t element_size,
                           chtholly_cffi_size_t element_count,
                           CHTHOLLY_CFFI_STREAM stream);
int feof(CHTHOLLY_CFFI_STREAM stream);
int ferror(CHTHOLLY_CFFI_STREAM stream);
CHTHOLLY_CFFI_STREAM c_fread_data_stream(void);
CHTHOLLY_CFFI_STREAM c_fread_eof_stream(void);
CHTHOLLY_CFFI_STREAM c_fread_error_stream(void);
void *c_fread_buffer(void);
int c_fread_close(CHTHOLLY_CFFI_STREAM stream);
#undef CHTHOLLY_CFFI_STREAM
#endif

#define CHTHOLLY_CFFI_LIMIT (1 + 2 * 3)
#define CHTHOLLY_CFFI_CHAIN CHTHOLLY_CFFI_LIMIT
#define CHTHOLLY_CFFI_FUNCTION(value) ((value) + 1)
#define CHTHOLLY_CFFI_SOFT_ERROR 2
#define CHTHOLLY_CFFI_HARD_BEGIN 4
#define CHTHOLLY_CFFI_HARD_END 6

int c_sqlite_open_invalid(void);
int c_zlib_corrupt(void);
int c_curl_perform_invalid(void);

typedef enum chtholly_cffi_status {
  CHTHOLLY_CFFI_OK = 0,
  CHTHOLLY_CFFI_FAILED = -1,
} chtholly_cffi_status;

typedef union chtholly_cffi_number {
  long integer;
  double real;
} chtholly_cffi_number;

typedef struct {
  int32_t value;
} chtholly_cffi_typedef_struct;

typedef union {
  int32_t integer;
  double real;
} chtholly_cffi_typedef_union;

typedef void (*chtholly_cffi_callback)(int32_t value);
typedef void *chtholly_cffi_handle;

typedef struct chtholly_cffi_bits {
  unsigned value : 3;
} chtholly_cffi_bits;

chtholly_cffi_status c_number_make(chtholly_cffi_number *value);
int32_t c_number_sum(chtholly_cffi_number value);
int32_t c_status_is_ok(chtholly_cffi_status value);
int32_t c_set_callback(chtholly_cffi_callback callback);
int32_t c_add_one(int32_t value);
int32_t c_errno_probe(int32_t fail);
int32_t c_code_probe(int32_t code);
chtholly_cffi_status c_status_code(chtholly_cffi_status code);
void *c_errno_pointer(int32_t fail);
int32_t c_win32_probe(int32_t fail);
void *c_fopen_missing(void);
int32_t c_code_set_probe(int32_t code);
int32_t c_code_allowed_probe(int32_t code);
uint32_t c_unsigned_set_probe(uint32_t code);
chtholly_cffi_handle c_errno_sentinel(void);
chtholly_cffi_handle c_win32_handle(int32_t fail);
int32_t c_win32_close_handle(chtholly_cffi_handle handle);
#if defined(_WIN32)
HANDLE c_win32_read_data_handle(void);
HANDLE c_win32_read_eof_handle(void);
HANDLE c_win32_read_invalid_handle(void);
HANDLE c_win32_read_probe_handle(uint32_t mode);
void *c_win32_read_buffer(void);
int32_t c_win32_read_close(HANDLE handle);
BOOL c_win32_read_contract_probe(HANDLE handle, void *buffer, DWORD capacity,
                                 DWORD *count, void *overlapped);
#endif
int32_t c_posix_read_probe(int32_t mode);
void *c_posix_buffer(void);
int64_t c_posix_read_into(void *buffer, uint64_t capacity, int32_t mode);
int32_t c_variadic(int32_t fixed, ...);

#endif
