#include "chtholly/next_host_v1.h"
#include "chtholly/next_runtime_v1.h"
#include "../Core/next_path.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NextHostSocket;
#define NEXT_HOST_INVALID_SOCKET INVALID_SOCKET
#define next_host_socket_close closesocket
#define next_host_socket_error WSAGetLastError()
#define NEXT_HOST_EINTR WSAEINTR
#define NEXT_HOST_ECONNRESET WSAECONNRESET
#define NEXT_HOST_SHUT_RDWR SD_BOTH
#else
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int NextHostSocket;
#define NEXT_HOST_INVALID_SOCKET (-1)
#define next_host_socket_close close
#define next_host_socket_error errno
#define NEXT_HOST_EINTR EINTR
#define NEXT_HOST_ECONNRESET ECONNRESET
#define NEXT_HOST_SHUT_RDWR SHUT_RDWR
#endif

typedef struct NextHostHandle {
  FILE *file;
  uint32_t magic;
  uint8_t closed;
} NextHostHandle;

typedef struct NextHostTask {
  const void *entry;
  uint32_t magic;
  uint8_t cancelled;
  uint8_t completed;
  uint8_t joined;
} NextHostTask;

typedef struct NextHostSocketHandle {
  NextHostSocket socket;
  uint32_t magic;
} NextHostSocketHandle;

#define NEXT_HOST_HANDLE_MAGIC 0x48444c31u
#define NEXT_HOST_TASK_MAGIC 0x54415331u
#define NEXT_HOST_SOCKET_MAGIC 0x534f4331u

#if defined(_WIN32)
static int next_host_socket_startup(void) {
  static int initialized = 0;
  WSADATA data;
  if (initialized)
    return 0;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return -1;
  initialized = 1;
  return 0;
}
#else
static int next_host_socket_startup(void) { return 0; }
#endif

static int32_t next_host_socket_status(int error) {
  if (error == NEXT_HOST_ECONNRESET)
    return CHTHOLLY_NEXT_HOST_STATUS_RESET;
  if (error == NEXT_HOST_EINTR)
    return CHTHOLLY_NEXT_HOST_STATUS_CANCELLED;
  return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
}

static NextHostSocketHandle *next_host_valid_socket(void *opaque) {
  NextHostSocketHandle *handle = (NextHostSocketHandle *)opaque;
  if (handle == NULL || handle->magic != NEXT_HOST_SOCKET_MAGIC ||
      handle->socket == NEXT_HOST_INVALID_SOCKET)
    return NULL;
  return handle;
}

static int32_t next_host_new_socket(NextHostSocket socket, void **out) {
  NextHostSocketHandle *handle;
  if (out == NULL || socket == NEXT_HOST_INVALID_SOCKET)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out = NULL;
  handle = (NextHostSocketHandle *)calloc(1, sizeof(*handle));
  if (handle == NULL) {
    next_host_socket_close(socket);
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  }
  handle->socket = socket;
  handle->magic = NEXT_HOST_SOCKET_MAGIC;
  *out = handle;
  return 0;
}

static int next_host_valid_handle(const NextHostHandle *handle) {
  return handle != NULL && handle->magic == NEXT_HOST_HANDLE_MAGIC &&
         !handle->closed && handle->file != NULL;
}

static int next_host_valid_task(const NextHostTask *task) {
  return task != NULL && task->magic == NEXT_HOST_TASK_MAGIC && !task->joined;
}

static FILE *next_host_open_file(const NextRuntimePathChar *path, int create) {
#if defined(_WIN32)
  FILE *file = NULL;
  return _wfopen_s(&file, path, create ? L"w+b" : L"r+b") == 0 ? file : NULL;
#else
  return fopen(path, create ? "w+b" : "r+b");
#endif
}

static int32_t next_host_open_mode(const uint8_t *path, uint64_t path_size,
                                   void **out_handle, int mode) {
  NextHostHandle *handle;
  NextRuntimePathChar *path_copy;
  int32_t status;
  FILE *file;
  if (out_handle == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_handle = NULL;
  status = next_runtime_resolve_path(path, path_size, &path_copy);
  if (status != 0)
    return status;
  if (mode == 1) {
#if defined(_WIN32)
    file = NULL;
    (void)_wfopen_s(&file, path_copy, L"rb");
#else
    file = fopen(path_copy, "rb");
#endif
  } else {
    file = next_host_open_file(path_copy, mode == 2);
    if (file == NULL && mode == 0)
      file = next_host_open_file(path_copy, 1);
  }
  free(path_copy);
  if (file == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  handle = (NextHostHandle *)calloc(1, sizeof(*handle));
  if (handle == NULL) {
    fclose(file);
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  }
  handle->file = file;
  handle->magic = NEXT_HOST_HANDLE_MAGIC;
  *out_handle = handle;
  return 0;
}

int32_t chtholly_next_host_v1_open(const uint8_t *path, uint64_t size, void **out) {
  return next_host_open_mode(path, size, out, 0);
}
int32_t chtholly_next_host_v1_open_read(const uint8_t *path, uint64_t size, void **out) {
  return next_host_open_mode(path, size, out, 1);
}
int32_t chtholly_next_host_v1_create(const uint8_t *path, uint64_t size, void **out) {
  return next_host_open_mode(path, size, out, 2);
}

int64_t chtholly_next_host_v1_read(void *opaque, uint8_t *buffer,
                                   uint64_t count) {
  NextHostHandle *handle = (NextHostHandle *)opaque;
  size_t read_count;
  if (!next_host_valid_handle(handle) || (buffer == NULL && count != 0) ||
      count > (uint64_t)SIZE_MAX || count > (uint64_t)INT64_MAX)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  if (count == 0)
    return 0;
  read_count = fread(buffer, 1, (size_t)count, handle->file);
  if (read_count == 0 && ferror(handle->file))
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  return (int64_t)read_count;
}

int64_t chtholly_next_host_v1_write(void *opaque, const uint8_t *buffer,
                                    uint64_t count) {
  NextHostHandle *handle = (NextHostHandle *)opaque;
  size_t write_count;
  if (!next_host_valid_handle(handle) || (buffer == NULL && count != 0) ||
      count > (uint64_t)SIZE_MAX || count > (uint64_t)INT64_MAX)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  if (count == 0)
    return fflush(handle->file) == 0 ? 0 : CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  write_count = fwrite(buffer, 1, (size_t)count, handle->file);
  if (write_count != (size_t)count || fflush(handle->file) != 0)
    return write_count == (size_t)count ? CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE
                                        : (int64_t)write_count;
  return (int64_t)write_count;
}

/* CFDL ref_mut Handle is a pointer to the handle slot, not the handle bits. */
int64_t chtholly_next_host_v1_read_ref(void **handle, uint8_t *buffer, uint64_t count) {
  return handle ? chtholly_next_host_v1_read(*handle, buffer, count)
                : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}
int64_t chtholly_next_host_v1_write_ref(void **handle, const uint8_t *buffer, uint64_t count) {
  return handle ? chtholly_next_host_v1_write(*handle, buffer, count)
                : CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
}

int32_t chtholly_next_host_v1_close(void *opaque) {
  NextHostHandle *handle = (NextHostHandle *)opaque;
  int result;
  if (handle == NULL || handle->magic != NEXT_HOST_HANDLE_MAGIC)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  if (handle->closed)
    return CHTHOLLY_NEXT_HOST_STATUS_ALREADY_CLOSED;
  result = fclose(handle->file);
  handle->closed = 1;
  handle->file = NULL;
  handle->magic = 0;
  free(handle);
  return result == 0 ? 0 : CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
}

int32_t chtholly_next_host_v1_monotonic_now(
    chtholly_next_host_v1_instant *out_value) {
  if (out_value == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  out_value->reserved = 0;
  return chtholly_next_runtime_v1_monotonic_now(&out_value->seconds,
                                                &out_value->nanoseconds);
}

int32_t chtholly_next_host_v1_task_spawn(const void *entry, void **out_task) {
  NextHostTask *task;
  if (out_task == NULL || entry == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_task = NULL;
  task = (NextHostTask *)calloc(1, sizeof(*task));
  if (task == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_OUT_OF_MEMORY;
  task->entry = entry;
  task->magic = NEXT_HOST_TASK_MAGIC;
  *out_task = task;
  return 0;
}

int32_t chtholly_next_host_v1_task_poll(void *opaque) {
  NextHostTask *task = (NextHostTask *)opaque;
  if (!next_host_valid_task(task))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  if (task->cancelled)
    return CHTHOLLY_NEXT_HOST_STATUS_CANCELLED;
  if (!task->completed) {
    task->completed = 1;
    return CHTHOLLY_NEXT_HOST_STATUS_NOT_READY;
  }
  return 0;
}

int32_t chtholly_next_host_v1_task_cancel(void *opaque) {
  NextHostTask *task = (NextHostTask *)opaque;
  if (!next_host_valid_task(task))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  task->cancelled = 1;
  return 0;
}

int32_t chtholly_next_host_v1_task_wake(void *opaque) {
  NextHostTask *task = (NextHostTask *)opaque;
  if (!next_host_valid_task(task))
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  if (task->cancelled)
    return CHTHOLLY_NEXT_HOST_STATUS_CANCELLED;
  task->completed = 1;
  return 0;
}

int32_t chtholly_next_host_v1_task_join(void *opaque) {
  NextHostTask *task = (NextHostTask *)opaque;
  int32_t result;
  if (task == NULL || task->magic != NEXT_HOST_TASK_MAGIC)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  if (task->joined)
    return CHTHOLLY_NEXT_HOST_STATUS_ALREADY_CLOSED;
  result = task->cancelled ? CHTHOLLY_NEXT_HOST_STATUS_CANCELLED : 0;
  task->joined = 1;
  task->magic = 0;
  free(task);
  return result;
}

int32_t chtholly_next_host_v1_net_listen(uint16_t port, void **out_listener) {
  NextHostSocket socket_handle;
  struct sockaddr_in address;
  int reuse = 1;
  if (out_listener == NULL || port == 0)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_listener = NULL;
  if (next_host_socket_startup() != 0)
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  socket_handle = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_handle == NEXT_HOST_INVALID_SOCKET)
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
#if defined(_WIN32)
  (void)setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, (int)sizeof(reuse));
#else
  (void)setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   (socklen_t)sizeof(reuse));
#endif
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (bind(socket_handle, (const struct sockaddr *)&address,
           (socklen_t)sizeof(address)) != 0 || listen(socket_handle, 16) != 0) {
    next_host_socket_close(socket_handle);
    return next_host_socket_status(next_host_socket_error);
  }
  return next_host_new_socket(socket_handle, out_listener);
}

int32_t chtholly_next_host_v1_net_accept(void *opaque, void **out_stream) {
  NextHostSocketHandle *listener = next_host_valid_socket(opaque);
  NextHostSocket accepted;
  if (listener == NULL || out_stream == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  *out_stream = NULL;
  accepted = accept(listener->socket, NULL, NULL);
  if (accepted == NEXT_HOST_INVALID_SOCKET)
    return next_host_socket_status(next_host_socket_error);
  return next_host_new_socket(accepted, out_stream);
}

int64_t chtholly_next_host_v1_net_read(void *opaque, uint8_t *buffer,
                                       uint64_t capacity) {
  NextHostSocketHandle *stream = next_host_valid_socket(opaque);
  if (stream == NULL || (buffer == NULL && capacity != 0) ||
      capacity > (uint64_t)INT_MAX)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  if (capacity == 0)
    return 0;
#if defined(_WIN32)
  {
    const int result = recv(stream->socket, (char *)buffer, (int)capacity, 0);
    if (result == 0)
      return 0;
    if (result < 0)
      return next_host_socket_status(next_host_socket_error);
    return result;
  }
#else
  {
    ssize_t result = recv(stream->socket, buffer, (size_t)capacity, 0);
    if (result == 0)
      return 0;
    if (result < 0)
      return next_host_socket_status(next_host_socket_error);
    return (int64_t)result;
  }
#endif
}

int64_t chtholly_next_host_v1_net_write(void *opaque, const uint8_t *buffer,
                                        uint64_t size) {
  NextHostSocketHandle *stream = next_host_valid_socket(opaque);
  if (stream == NULL || (buffer == NULL && size != 0) ||
      size > (uint64_t)INT_MAX)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_ARGUMENT;
  if (size == 0)
    return 0;
#if defined(_WIN32)
  {
    const int result = send(stream->socket, (const char *)buffer, (int)size, 0);
    if (result < 0)
      return next_host_socket_status(next_host_socket_error);
    return result;
  }
#else
  {
    ssize_t result = send(stream->socket, buffer, (size_t)size, 0);
    if (result < 0)
      return next_host_socket_status(next_host_socket_error);
    return (int64_t)result;
  }
#endif
}

int32_t chtholly_next_host_v1_net_close(void *opaque) {
  NextHostSocketHandle *handle = next_host_valid_socket(opaque);
  if (handle == NULL)
    return CHTHOLLY_NEXT_HOST_STATUS_INVALID_HANDLE;
  if (next_host_socket_close(handle->socket) != 0) {
    handle->magic = 0;
    free(handle);
    return CHTHOLLY_NEXT_HOST_STATUS_IO_FAILURE;
  }
  handle->magic = 0;
  handle->socket = NEXT_HOST_INVALID_SOCKET;
  free(handle);
  return 0;
}
